#include <iostream>
#include <string>
#include <thread>
#include <cstdio>      // for printf
#include <unistd.h>    // for close()
#include <sys/socket.h>
#include <arpa/inet.h>


void SendMessage(int client_fd)
{
std::string message;

    while (true)
    {
        std::cout << "Enter message to send: \n";
        std::getline(std::cin, message);

        if (message.empty())
        {
            std::cout << "Message cannot be empty!\n";
            continue;
        }

        ssize_t bytes_sent = send(client_fd, message.c_str(), message.length(), 0);

        if (bytes_sent == -1)
        {
            std::cout << "Failed to send message to server\n";
        }
        else
        {
            std::cout << "Sent: " << message << std::endl;
        }
    }
}

void ReceiveMessages(int client_fd)
{
    char buffer[1024];
    while (true)
    {
        
        ssize_t bytes_received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

        if (bytes_received == -1)
        {
            std::cout << "Failed to receive message from server\n";
        }
        else if (bytes_received == 0)
        {
            std::cout << "Server disconnected\n";
            break;
        }
        else
        {
            buffer[bytes_received] = '\0';
            std::cout << "Message: " << buffer << "\n" << std::endl;
        }
    }
}

int main()
{
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd == -1)
    {
        std::cout << "Failed to create socket\n";
        return 1;
    }

    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(8080);

    if (inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr) <= 0)
    {
        std::cout << "Invalid address\n";
        close(client_fd);
        return 1;
    }

    if (connect(client_fd, (struct sockaddr*)&server_address, sizeof(server_address)) == -1)
    {
        std::cout << "Failed to connect to server\n";
        close(client_fd);
        return 1;
    }

    std::cout << "Connected to server successfully!\n";

    std::thread send_thread(SendMessage, client_fd);
    std::thread receive_thread(ReceiveMessages, client_fd);

    send_thread.join();
    receive_thread.join();

    
    return 0;
}