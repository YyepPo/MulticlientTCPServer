#include "Console.h"
#include "Net.h"
#include "Protocol.h"
#include "Text.h"
#include "UniqueFd.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

namespace
{

constexpr const char* kDefaultHost = "127.0.0.1";
constexpr std::uint16_t kDefaultPort = 8080;

// Prints every frame from the server until the connection ends, then wakes the input loop through wake_fd
// so the program exits even while it is waiting for the user to type.
void ReceiveLoop(int socket_fd, int wake_fd, const std::atomic<bool>& quitting)
{
    std::string payload;

    while (true)
    {
        const protocol::ReceiveStatus status = protocol::ReceiveFrame(socket_fd, payload);

        if (status == protocol::ReceiveStatus::Ok)
        {
            console::PrintLine(payload);
            continue;
        }

        if (quitting.load())
        {
            break;
        }

        if (status == protocol::ReceiveStatus::Closed)
        {
            console::PrintLine("Disconnected from the server.");
        }
        else if (status == protocol::ReceiveStatus::ProtocolError)
        {
            console::PrintError("The server sent an invalid frame. Disconnecting.");
        }
        else
        {
            console::PrintError("Connection lost.");
        }

        break;
    }

    const char byte = 'x';
    const ssize_t written = ::write(wake_fd, &byte, 1);
    static_cast<void>(written);
}

// Reads lines from stdin and sends each one as a frame. Returns when the user types /quit or closes stdin,
// or when the receive thread reports that the connection is gone.
void InputLoop(int socket_fd, int wake_fd, std::atomic<bool>& quitting)
{
    pollfd poll_fds[2] = {};
    poll_fds[0].fd = STDIN_FILENO;
    poll_fds[0].events = POLLIN;
    poll_fds[1].fd = wake_fd;
    poll_fds[1].events = POLLIN;

    text::LineBuffer input(protocol::kMaxPayloadSize);
    char buffer[1024];
    std::string line;

    while (true)
    {
        const int ready = ::poll(poll_fds, 2, -1);

        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return;
        }

        if (poll_fds[1].revents != 0)
        {
            return;
        }

        if (poll_fds[0].revents == 0)
        {
            continue;
        }

        const ssize_t count = ::read(STDIN_FILENO, buffer, sizeof(buffer));

        if (count < 0 && errno == EINTR)
        {
            continue;
        }

        if (count <= 0)
        {
            quitting.store(true);
            return;
        }

        input.Append(buffer, static_cast<std::size_t>(count));

        while (input.NextLine(line))
        {
            const std::string_view message = text::Trim(line);

            if (message.empty())
            {
                continue;
            }

            if (message == "/quit")
            {
                quitting.store(true);
                return;
            }

            if (!protocol::SendFrame(socket_fd, message))
            {
                console::PrintError("Could not send the message: the connection is gone.");
                return;
            }
        }
    }
}

}

int main(int argc, char* argv[])
{
    std::string host = kDefaultHost;
    std::uint16_t port = kDefaultPort;

    if (argc > 3 || (argc == 3 && !net::ParsePort(argv[2], port)))
    {
        console::PrintError("Usage: chat_client [host] [port]   (defaults: 127.0.0.1 8080)");
        return 2;
    }

    if (argc >= 2)
    {
        host = argv[1];
    }

    // Sends use MSG_NOSIGNAL; ignoring SIGPIPE as well keeps any other write path from killing the client.
    std::signal(SIGPIPE, SIG_IGN);

    std::string error;
    UniqueFd socket_fd = net::ConnectToServer(host, port, error);

    if (!socket_fd.IsValid())
    {
        console::PrintError("Could not connect to " + host + ":" + std::to_string(port) + ": " + error);
        return 1;
    }

    int pipe_fds[2] = {-1, -1};

    if (::pipe2(pipe_fds, O_CLOEXEC) != 0)
    {
        console::PrintError("Could not create the wake-up pipe.");
        return 1;
    }

    UniqueFd wake_read(pipe_fds[0]);
    UniqueFd wake_write(pipe_fds[1]);

    console::PrintLine("Connected to " + host + ":" + std::to_string(port) + ". Type /quit to leave.");

    std::atomic<bool> quitting{false};
    std::thread receiver(ReceiveLoop, socket_fd.Get(), wake_write.Get(), std::cref(quitting));

    InputLoop(socket_fd.Get(), wake_read.Get(), quitting);

    // Unblocks the receive thread, which then exits.
    quitting.store(true);
    net::ShutdownSocket(socket_fd.Get());
    receiver.join();

    return 0;
}
