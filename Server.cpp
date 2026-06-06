#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <iostream>
#include <thread>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>
#include <unordered_map>
#include <cstring>
#include <sstream>
#include "ClientData.cpp"

//For an interview, you could say:
//I deleted the copy constructor and copy assignment because my Socket class has unique ownership of an OS socket descriptor.
// If it were copied, two objects would close the same descriptor in their destructors.
// I allow moving because ownership can safely transfer from one Socket object to another.

// Workthrough of creating a tcp server
// Create a socket for the server
// Create a socket address for the server
// Bind the socket to the address
// Listen for incoming connections
// Accept incoming connections
// Receive messages from clients

class Socket {
public:
 // Constructor and Destructor
    Socket() {}

    ~Socket() {
        // Close the socket when this object is destroyed
        Close();
    }

    // The ownership of the socket should not be copied, if copied is enabled that there would be issues like:
    // DoubleClose, sending and receving from a closed socket, accidentally closing a socket that is still in use, etc.
    // Copy constructor
    Socket(const Socket& other) = delete;
    // Copy assignment operator
    Socket& operator=(const Socket& other) = delete;

    // Move operator is enabled because we might transfer the ownership of the socket to another object,
    // for example when we want to return a socket from a function or when we want to store a socket in a container that requires move semantics.
    // Move constructor and assignment operator
    Socket(Socket&& other) noexcept : fd{other.fd} {
        other.fd = -1; // Invalidate the source socket
    };
    Socket& operator=(Socket&& other) noexcept{
        if(this != &other) {
            Close(); // Close the current socket if it's open
            fd = other.fd; // Transfer ownership of the socket
            other.fd = -1; // Invalidate the source socket
        }
        return *this;
    };

    bool CreateSocket(){
        fd = socket(AF_INET, SOCK_STREAM, 0);

        if(fd == -1) {
            printf("Failed to create socket\n");
        }
        else{
            printf("Socket created successfully\n");
        }

        return fd != -1;
    }

    int32_t GetFD() const {
        return fd;
    }

private:

    // Close the socket if it's open
    void Close(){
        if (fd != -1) {
            close(fd);
            fd = -1;
        }
    }

    int fd = -1;
};

class TCPServer {

public:

    TCPServer(uint32_t mcr = 10) : max_connection_request(mcr) {

    }
    // A tcp port is a 16-bit unsigned integer
    bool StartServer(uint16_t port = 8080) {
        // Create server socket
        if(!server_socket.CreateSocket()) {
            return false;
        }

        // Create server address
        // Server address or socket address has these things: IP address,port number, and address family (IPv4 or IPv6)
        sockaddr_in server_address;
        server_address.sin_family = AF_INET;
        server_address.sin_addr.s_addr = INADDR_ANY;
        server_address.sin_port = htons(port);

        int option = 1;
        setsockopt(
            server_socket.GetFD(),
            SOL_SOCKET,
            SO_REUSEADDR,
            &option,
            sizeof(option)
        );

        // After server/socket address is created bind the socket to the address
        if (bind(server_socket.GetFD(), (struct sockaddr *)&server_address, sizeof(server_address)) == -1) {
                printf("Failed to bind socket\n");
                return false;
        }

        // Listen for incoming connections
        if (listen(server_socket.GetFD(), max_connection_request) == -1) {
            printf("Failed to listen on socket\n");
            return false;
        }

        printf("Server listening on port %u\n", port);
        return true;
    }


    Socket server_socket;

private:

    uint16_t max_connection_request = 10;

};

// Global list of clients + mutex for thread safety
std::vector<int> clients;
std::mutex clients_mutex;
std::atomic<bool> server_running{true};

// Key = fd and value is client, so we get a client from fd its fd
std::unordered_map<int, ClientData> clients_map;
std::mutex clients_map_mutex;

ClientData* GetClient(int client_fd) {
    auto it = clients_map.find(client_fd);
    if (it == clients_map.end()) {
        return nullptr;
    }
    return &it->second;        // Return pointer to the actual object in the map
}

// ─────────────────────────────────────────────────────────────────────────────
// SendToClient  –  Thin wrapper around send() with unified error logging.
//
// @param target_fd   Destination socket fd.
// @param message     Message string to transmit.
// @param context     Caller label used in error output (e.g. "Whisper").
// ─────────────────────────────────────────────────────────────────────────────
static void SendToClient(int target_fd, const std::string& message, const char* context)
{
    ssize_t bytes_sent = send(target_fd, message.c_str(), message.size(), 0);
    if (bytes_sent > 0)
        printf("[%s] Message sent to fd %d.\n", context, target_fd);
    else{
        printf("[%s] Error: Failed to send message to fd %d.\n", context, target_fd);
    }
}

// Whether the sender id has authority to use a command
static bool HasAuthority(int sender_id){

    ClientData* client =  GetClient(sender_id);
    if(sender_id == 0){
        return true;
    }    

    if(client != nullptr && client->GetIsAdmin()){
        return true;
    }
    
    std::string errorMsg = "You dont have authority to use this command";
    SendToClient(sender_id,errorMsg.c_str(),"");

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// ParseTargetFd  –  Extracts an integer fd/id from a command string.
//
// @param command   Full command string.
// @param offset    Number of characters to skip (e.g. 8 for "/whisper").
// @param out_fd    Output parameter; populated on success.
// @returns True if parsing succeeded and the value is positive.
// ─────────────────────────────────────────────────────────────────────────────
static bool ParseTargetFd(const std::string& command, int offset, int& out_fd)
{
    std::istringstream iss(command.substr(offset));
    if (!(iss >> out_fd))
    {
        printf("[Server] Error: Could not parse target id from command.\n");
        return false;
    }
    if (out_fd <= 0)
    {
        printf("[Server] Error: Target id must be a positive integer.\n");
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// HandleWhisper  –  Sends a private message to a single client.
//
// Format:  /whisper <client_fd>
// The message body is currently fixed; extend the substr offset & format as
// needed once you add free-form text to the protocol.
// ─────────────────────────────────────────────────────────────────────────────
void HandleWhisper(int sender_fd, const std::string& command)
{
    int target_fd;
    // "/whisper " is 9 characters; offset 9 skips the command word + space
    if (!ParseTargetFd(command, 9, target_fd)) return;

    // Verify the target socket is a tracked, connected client
    ClientData* target_client = GetClient(target_fd);
    if (!target_client)
    {
        printf("[Whisper] Error: Client fd %d not found.\n", target_fd);
        std::string errorMessage = std::string("[Whisper] Error: Client fd ") + std::to_string(target_fd) + " not found.\n";
        send(sender_fd,errorMessage.c_str(),errorMessage.length(),0);
        return;
    }

    // sender_fd == 0  →  message originates from the server console
    if (sender_fd == 0)
    {
        SendToClient(target_fd, "Server whispered you.", "Whisper");
        return;
    }

    // sender_fd > 0  →  message originates from another connected client
    ClientData* sender_client = GetClient(sender_fd);
    if (!sender_client)
    {
        printf("[Whisper] Error: Sender fd %d not found.\n", sender_fd);
        return;
    }

    const std::string message = sender_client->get_name() + " whispered you.";
    SendToClient(target_fd, message, "Whisper");
}

// ─────────────────────────────────────────────────────────────────────────────
// HandleGetAll  –  Prints connection info for every client in the map.
// ─────────────────────────────────────────────────────────────────────────────
void HandleGetAll(int sender_id)
{
    if(HasAuthority(sender_id) == false){
        return;
    }   

    if (clients_map.empty())
    {
        printf("[GetAll] No clients are currently connected.\n");
        return;
    }

    printf("[GetAll] Connected clients (%zu):\n", clients_map.size());
    for (const auto& [fd, client] : clients_map)
        client.PrintInfo();
}

// ─────────────────────────────────────────────────────────────────────────────
// HandleBan  –  Bans a client, notifies them, and closes their socket.
//
// Format:  /ban <client_fd>
//
// Shutdown sequence:
//   0. Only server or a client that is an admin can use this command, otherwise print that the user cannot use this command
//   1. Mark the client as banned in its data structure.
//   2. Send a notification so the client knows why they were disconnected.
//   3. shutdown(SHUT_WR) signals EOF on our side (half-close).
//   4. close() releases the file descriptor.
// ─────────────────────────────────────────────────────────────────────────────
void HandleBan(int sender_id,const std::string& command)
{
    // Whether the server used this command, or a client that is an admin
    if(HasAuthority(sender_id) == false){
        return;
    }   

    int target_fd;
    // "/ban " is 5 characters
    if (!ParseTargetFd(command, 5, target_fd)) return;

    // count() returns size_t; compare against 0, not false
    if (clients_map.count(target_fd) == 0)
    {
        printf("[Ban] Error: Client fd %d not found.\n", target_fd);
        return;
    }

    ClientData& client = clients_map.at(target_fd);
    if (!client.SetIsBanned(true))
    {
        printf("[Ban] Client fd %d could not be banned (already banned?).\n", target_fd);
        return;
    }

    printf("[Ban] Client fd %d banned successfully.\n", target_fd);

    // Inform the client before cutting the connection
    const std::string ban_msg = "You have been banned by the server.\n";
    SendToClient(target_fd, ban_msg, "Ban");

    shutdown(target_fd, SHUT_WR); // Signal EOF — no more data from our side
    close(target_fd);             // Release the file descriptor

     clients_map.erase(target_fd);
}

void HandlePrivateMessage(int sender_fd,const std::string& command){
    // /msg is 5 characters
    int target_fd;
    if (!ParseTargetFd(command, 5, target_fd)) return;

    // Check if the client exists
    ClientData* client = GetClient(target_fd);
    if(client == nullptr){
        printf("[MSG] Failed to send a private msg to client: %d, from client: %d ",target_fd,sender_fd);

        std::string errorMsg = "Failed to send a private msg to client %d" + std::to_string(target_fd);
        SendToClient(sender_fd,errorMsg,"MSG");
        return;
    }

    // send only the message to the target
    // template the message like this /msg 5 <private message>
    // So send only the part after the 6th char
    std::istringstream iss(command.substr(6));
    std::string privateMessage;
    if(iss >> privateMessage){
        printf("privateeee messageeeee");
    }

    std::string message = "A private message from : " + client->get_name() + " :" + command;
    SendToClient(target_fd,message,"MSG");
}

void MuteClient(int sender_fd,const std::string& message){
    // /mute is 5 char
    std::istringstream iss(message.substr(5));
    int target_id;
    if(iss >> target_id){
        if(clients_map.count(target_id) == false){
            printf("Mute failed, Client with id: %d does not exist\n",target_id);
            return;
        }

        // Get the target client data
        ClientData* target_client = GetClient(target_id);

        // Get the sender client and add the target client to its muted client list
        ClientData* senderClient = GetClient(sender_fd);
        if(senderClient && target_client){
            senderClient->MuteClient(target_id);
            printf("Client: %s successfully muted client %d\n",senderClient->get_name().c_str(),target_id);
        }
        else{
            printf("Mute failed: Sender client or target client is not valid\n");
        }
    }
}

void UnmuteClient(int sender_fd,const std::string& message){
    // /unmute is 7 char
    std::istringstream iss(message.substr(5));
    int target_id;
    if(iss >> target_id){
        if(clients_map.count(target_id) == false){
            printf("Unmute failed, Client with id: %d does not exist\n",target_id);
            return;
        }

        // Get the target client data
        ClientData* target_client = GetClient(target_id);

        // Get the sender client and remove the target client to its muted client list
        ClientData* senderClient = GetClient(sender_fd);
        if(senderClient && target_client){
            senderClient->UnmuteClient(target_id);
            printf("Client: %s successfully unmuted client %d\n",senderClient->get_name().c_str(),target_id);
        }
        else{
            printf("Unmute failed: Sender client or target client is not valid\n");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// HandleCommand  –  Dispatches server/client commands parsed from raw input.
//
// Commands supported:
//   /whisper <client_id>  →  Send a private message to a specific client
//   /getall               →  Print info of every connected client
//   /ban    <client_id>   →  Ban a client and close their connection
//
// @param sender_fd   File descriptor of the sender. 0 means the server itself.
// @param command     Raw command string (e.g. "/whisper 5").
// ─────────────────────────────────────────────────────────────────────────────
void HandleCommand(int sender_fd, const std::string& command)
{
    if      (command.rfind("/whisper", 0) == 0) HandleWhisper(sender_fd, command);
    else if (command.rfind("/getall",  0) == 0) HandleGetAll(sender_fd);
    else if (command.rfind("/ban",     0) == 0) HandleBan(sender_fd,command);
    else if (command.rfind("/msg",     0) == 0) HandlePrivateMessage(sender_fd,command);
    else if (command.rfind("/mute",    0) == 0) MuteClient(sender_fd,command);
    else if (command.rfind("/unmute",  0) == 0) UnmuteClient(sender_fd,command);
    else    printf("[Server] Unknown command: '%s'\n", command.c_str());
}

void BroadcastToEveryClient(int sender_id,const std::string& message){

    // Get the sender client data
    ClientData* sender_client = GetClient(sender_id);
    if(sender_client == nullptr){
        printf("Error: BroadcastToEveryClient: Clientdata is invalid for client with fd: %d",sender_id);
        return;
    }

    // Send message to every single client
    for(const auto& [fd, client] : clients_map){
        // Prevent sending the message to the client that sent the message
        if(fd == sender_id){
            continue;
        }

        // Check if the RECIPIENT (client) has muted the SENDER (sender_id)
        if (client.muted_clients.count(sender_id)) { 
            printf("Log: Skipping: Recipient %s has muted Sender %d\n", 
                    client.get_name().c_str(), sender_id);
            continue;
        }
        // Dont send message to muted clients
        // if(sender_client->muted_clients.count(fd)){
        //     printf("Log: BroadcastToEveryClient: Client with id %d is muted for client %s\n", fd, client.get_name().c_str());
        //     continue;
        // }

        SendToClient(fd,message,"");
    }
}

// Message that the server receives, server should receive commands through other clients, like a client sending a private message to another client
void ReceiveMessages(int client_fd)
{
    char buffer[1024];
    
    while (server_running)
    {
        ssize_t bytes = recv(client_fd, buffer, sizeof(buffer)-1, 0);
        
        if (bytes > 0)
        {
            buffer[bytes] = '\0';
            printf("Received message from Client: %d saying: %s\n", client_fd, buffer);
            const std::string message(buffer);
            // Check if the received message is a command, the command starts with "/" for example /whisper <client_fd>
            if(buffer[0] == '/'){
                HandleCommand(client_fd,message);
            }else{
                BroadcastToEveryClient(client_fd,message);
            }
        }
        else if (bytes == 0)
        {
            printf("Client %s disconnected: with clientID: %d\n", clients_map.at(client_fd).get_name().c_str() ,client_fd);
            
            // Check if the clients exists, if true remove it from the map
            if(clients_map.count(client_fd)){
                std::lock_guard<std::mutex> lock(clients_map_mutex);
                clients_map.erase(client_fd);
            }

            break;
        }
        else
        {
            // Error
            if (errno != EINTR) 
                printf("Receive error on client %d\n", client_fd);
            break;
        }
    }

    close(client_fd);

    // Remove client from list
    std::lock_guard<std::mutex> lock(clients_mutex);
    clients.erase(std::remove(clients.begin(), clients.end(), client_fd), clients.end());
}

// Server sends/broadcasts a message to clients
void BroadcastMessages()
{
    std::string message;

    while(server_running){
        std::cout << "Server write a message \n";
        std::getline(std::cin, message);
        if(message.length() != 0)
        {
            HandleCommand(0,message);
        }
        else
        {
            printf("Message counldnt sent because Message to broadcast is empty");
        }
    }
}

int main()
{
    TCPServer server(10);
    
    if (!server.StartServer(8080))
    {
        printf("Server failed to start\n");
        return 1;
    }

    printf("Server is running on port 8080...\n");

    // Start taking input from the server
    std::thread broadcast_thread(BroadcastMessages);
    broadcast_thread.detach();

    while (true)
    {
        int client_fd = accept(server.server_socket.GetFD(), nullptr, nullptr);
        
        if (client_fd == -1)
        {
            printf("Failed to accept connection\n");
            continue;
        }

        // Send welcome message to the connect client and request their name.
        std::string welcome_message = "Welcome to the server! Pls Write you name downbelow: \n";
        send(client_fd, welcome_message.c_str(), welcome_message.length(), 0);

        // Receive clients name
        char buffer[1024] = {0};
        ssize_t bytes = recv(client_fd, buffer, sizeof(buffer)-1, 0);
        
        std::string client_name = (bytes > 0) ? buffer : "Anonimus";
        
        printf("New client connected: %s (FD: %d)\n", client_name.c_str(), client_fd);

        // Create a ClientData object and store it in the clients map
        {
            std::lock_guard<std::mutex> lock(clients_map_mutex);
            clients_map.emplace(client_fd, ClientData(client_fd, client_name));
        }

        // Start receive thread for this client
        std::thread receive_thread(ReceiveMessages, client_fd);
        receive_thread.detach();
    }

    return 0;
}