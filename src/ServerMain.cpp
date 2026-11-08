#include "ChatServer.h"
#include "Console.h"
#include "Net.h"

#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <string>

namespace
{

volatile std::sig_atomic_t g_stop_fd = -1;

// Runs on whichever thread receives SIGINT or SIGTERM. Only async-signal-safe calls are allowed here,
// so it just writes one byte to the server's wake-up pipe and lets the event loop do the rest.
void HandleStopSignal(int)
{
    const int saved_errno = errno;

    if (g_stop_fd >= 0)
    {
        const char byte = ChatServer::kStopByte;
        const ssize_t written = ::write(g_stop_fd, &byte, 1);
        static_cast<void>(written);
    }

    errno = saved_errno;
}

bool InstallSignalHandlers()
{
    struct sigaction stop_action{};
    stop_action.sa_handler = HandleStopSignal;
    sigemptyset(&stop_action.sa_mask);
    stop_action.sa_flags = SA_RESTART;

    // Writing to a socket whose peer has gone raises SIGPIPE, which would kill the process. Sends also use
    // MSG_NOSIGNAL; ignoring the signal covers every other write path.
    struct sigaction ignore_action{};
    ignore_action.sa_handler = SIG_IGN;
    sigemptyset(&ignore_action.sa_mask);

    return ::sigaction(SIGINT, &stop_action, nullptr) == 0
        && ::sigaction(SIGTERM, &stop_action, nullptr) == 0
        && ::sigaction(SIGPIPE, &ignore_action, nullptr) == 0;
}

}

int main(int argc, char* argv[])
{
    ChatServer::Options options;

    if (argc > 2 || (argc == 2 && !net::ParsePort(argv[1], options.port)))
    {
        console::PrintError("Usage: chat_server [port]   (default port 8080)");
        return 2;
    }

    ChatServer server(options);
    std::string error;

    if (!server.Start(error))
    {
        console::PrintError("Could not start the server: " + error);
        return 1;
    }

    g_stop_fd = server.StopFd();

    if (!InstallSignalHandlers())
    {
        console::PrintError("Could not install the signal handlers.");
        return 1;
    }

    server.Run();

    // The pipe closes with the server; make sure a late signal cannot write to a reused descriptor.
    g_stop_fd = -1;

    return 0;
}
