#include "Net.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <limits>
#include <memory>
#include <system_error>

namespace net
{

namespace
{

std::string ErrnoText(const char* what)
{
    return std::string(what) + ": " + std::system_category().message(errno);
}

// Waits until fd reports one of events (or an error/hang-up, which the next send/recv then reports).
// Returns false when the deadline passes first or poll() fails.
bool WaitUntil(int fd, short events, std::chrono::steady_clock::time_point deadline)
{
    while (true)
    {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());

        if (remaining.count() <= 0)
        {
            return false;
        }

        pollfd poll_fd{};
        poll_fd.fd = fd;
        poll_fd.events = events;

        const auto timeout = std::min<std::chrono::milliseconds::rep>(remaining.count(), std::numeric_limits<int>::max());
        const int ready = ::poll(&poll_fd, 1, static_cast<int>(timeout));

        if (ready > 0)
        {
            return true;
        }

        if (ready < 0 && errno != EINTR)
        {
            return false;
        }
    }
}

bool IsRetryable(const Deadline& deadline)
{
    // With a deadline the socket is used with MSG_DONTWAIT, so "would block" just means "poll again".
    return errno == EINTR || (deadline && (errno == EAGAIN || errno == EWOULDBLOCK));
}

}

bool SendAll(int fd, const char* data, std::size_t size, Deadline deadline)
{
    const int flags = deadline ? (MSG_NOSIGNAL | MSG_DONTWAIT) : MSG_NOSIGNAL;
    std::size_t sent_total = 0;

    while (sent_total < size)
    {
        if (deadline && !WaitUntil(fd, POLLOUT, *deadline))
        {
            return false;
        }

        const ssize_t sent = ::send(fd, data + sent_total, size - sent_total, flags);

        if (sent < 0)
        {
            if (IsRetryable(deadline))
            {
                continue;
            }

            return false;
        }

        sent_total += static_cast<std::size_t>(sent);
    }

    return true;
}

IoStatus ReceiveExact(int fd, char* buffer, std::size_t size, Deadline deadline)
{
    const int flags = deadline ? MSG_DONTWAIT : 0;
    std::size_t received_total = 0;

    while (received_total < size)
    {
        if (deadline && !WaitUntil(fd, POLLIN, *deadline))
        {
            return IoStatus::Error;
        }

        const ssize_t received = ::recv(fd, buffer + received_total, size - received_total, flags);

        if (received > 0)
        {
            received_total += static_cast<std::size_t>(received);
            continue;
        }

        if (received == 0)
        {
            return received_total == 0 ? IoStatus::Closed : IoStatus::Error;
        }

        if (IsRetryable(deadline))
        {
            continue;
        }

        return IoStatus::Error;
    }

    return IoStatus::Ok;
}

void ShutdownSocket(int fd) noexcept
{
    if (fd >= 0)
    {
        ::shutdown(fd, SHUT_RDWR);
    }
}

bool SetTimeout(int fd, int option, std::chrono::seconds timeout)
{
    timeval value{};
    value.tv_sec = static_cast<time_t>(timeout.count());

    return ::setsockopt(fd, SOL_SOCKET, option, &value, sizeof(value)) == 0;
}

UniqueFd CreateListeningSocket(std::uint16_t port, int backlog, std::string& error)
{
    UniqueFd socket_fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));

    if (!socket_fd.IsValid())
    {
        error = ErrnoText("socket");
        return {};
    }

    const int enable = 1;

    if (::setsockopt(socket_fd.Get(), SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) != 0)
    {
        error = ErrnoText("setsockopt(SO_REUSEADDR)");
        return {};
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (::bind(socket_fd.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        error = ErrnoText("bind");
        return {};
    }

    if (::listen(socket_fd.Get(), backlog) != 0)
    {
        error = ErrnoText("listen");
        return {};
    }

    return socket_fd;
}

UniqueFd ConnectToServer(const std::string& host, std::uint16_t port, std::string& error)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* results = nullptr;
    const std::string service = std::to_string(port);
    const int status = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &results);

    if (status != 0)
    {
        error = std::string("getaddrinfo: ") + ::gai_strerror(status);
        return {};
    }

    const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> results_guard(results, &::freeaddrinfo);
    error = "no address to connect to";

    for (const addrinfo* entry = results; entry != nullptr; entry = entry->ai_next)
    {
        UniqueFd socket_fd(::socket(entry->ai_family, entry->ai_socktype | SOCK_CLOEXEC, entry->ai_protocol));

        if (!socket_fd.IsValid())
        {
            error = ErrnoText("socket");
            continue;
        }

        if (::connect(socket_fd.Get(), entry->ai_addr, entry->ai_addrlen) == 0)
        {
            return socket_fd;
        }

        error = ErrnoText("connect");
    }

    return {};
}

std::string FormatIpv4(std::uint32_t address)
{
    return std::to_string((address >> 24) & 0xFF) + "." + std::to_string((address >> 16) & 0xFF) + "."
        + std::to_string((address >> 8) & 0xFF) + "." + std::to_string(address & 0xFF);
}

bool ParsePort(const std::string& text, std::uint16_t& port)
{
    unsigned int value = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto [parsed_end, error_code] = std::from_chars(begin, end, value);

    if (error_code != std::errc() || parsed_end != end || value == 0 || value > 65535)
    {
        return false;
    }

    port = static_cast<std::uint16_t>(value);
    return true;
}

}
