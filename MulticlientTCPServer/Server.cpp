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


void ReceiveMessages(int client_fd)
{
    char buffer[1024];
    
    while (server_running)
    {
        ssize_t bytes = recv(client_fd, buffer, sizeof(buffer)-1, 0);
        
        if (bytes > 0)
        {
            buffer[bytes] = '\0';
            printf("Client %d: %s\n", client_fd, buffer);
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

void HandleCommand(const std::string& command)
{
    if(command.find("/whisper") == 0)
    {
        // Template the message like this: /whisper <client_id>
        std::istringstream iss(command.substr(8));
        int target_fd;
        if(iss >> target_fd){
            
            // Check if the target is valid
            if(target_fd <= 0)
            {
                printf("Error: HandleCommand: Whisper Command: Target ID is zero");
                return;
            }

            // Whisper to the valid target
            const std::string message = "Server whispered you"; 
            ssize_t send_bytes = send(target_fd,message.c_str(),message.length(),0);
            if(send_bytes > 0)
            {
                printf("Message successfully sent");
            }
            else {
                printf("Error HandleCommand: Whisper Command: Message to send is empty");
            }
        }
    }
    else if(command.find("/getall") == 0){
        if(clients_map.size() == 0){
            printf("There is no client \n");
            return;    
        }
        
        for(const auto& [id,client] : clients_map){
            client.PrintInfo();
        }
    }
    else if(command.find("/ban") == 0){
        std::istringstream iss(command.substr(4));
        int target_id;

        if(iss >> target_id){
            // Check if the client allready exists
            if(clients_map.count(target_id) == false){
                return;
            }

            ClientData& client = clients_map.at(target_id);
            bool bBanWasSuccess = client.SetIsBanned(true);
            if(bBanWasSuccess){
                printf("Client with id: %d was banned successfully\n",target_id);
                const char* banMSG = "You have been banned by ther server\n";
                send(target_id,banMSG,strlen(banMSG),0); 
                shutdown(target_id,SHUT_WR); // inform client the we are done sending
                close(target_id); // close the connect for the client
            }else{
                printf("Client with id: %d was not banned\n",target_id);
            }
            
        }
    }
}

void BroadcastMessages()
{
    std::string message;

    while(server_running){
        std::cout << "Server write a message \n";
        std::getline(std::cin, message);
        if(message.length() != 0)
        {
            HandleCommand(message);
        }
        else
        {
            printf("Message counldnt sent because Message to broadcast is empty");
        }
    }

    
    // while (server_running)
    // {
    //     std::cout << "Server: Enter message to broadcast: ";
    //     std::getline(std::cin, message);

    //     if (message.empty()) continue;

    //     std::lock_guard<std::mutex> lock(clients_mutex);
        
    //     for (int fd : clients)
    //     {
    //         ssize_t sent = send(fd, message.c_str(), message.length(), 0);
    //         if (sent == -1)
    //         {
    //             printf("Failed to send to client %d\n", fd);
    //         }
    //     }
    // }
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
        char buffer[64] = {0};
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