#pragma once

#include "UniqueFd.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// Thin, error-checked helpers over the POSIX socket API.
namespace net
{

enum class IoStatus
{
    Ok,
    Closed,  // the peer closed the connection before any byte of this read arrived
    Error    // the call failed, the deadline passed, or the peer closed part-way through
};

// An absolute time limit for a whole operation. Unlike SO_RCVTIMEO/SO_SNDTIMEO, which restart on every partial
// read or write, a deadline cannot be stretched by a peer that trickles one byte at a time.
using Deadline = std::optional<std::chrono::steady_clock::time_point>;

// Writes the whole buffer, retrying on partial writes and EINTR. Never raises SIGPIPE.
// With a deadline, gives up and returns false once it passes.
bool SendAll(int fd, const char* data, std::size_t size, Deadline deadline = std::nullopt);

// Reads exactly size bytes, retrying on partial reads and EINTR.
// With a deadline, gives up and returns Error once it passes.
IoStatus ReceiveExact(int fd, char* buffer, std::size_t size, Deadline deadline = std::nullopt);

// Wakes every thread blocked on the socket and tells the peer that no more data follows.
// Safe to call from any thread and more than once; the descriptor stays open until its owner closes it.
void ShutdownSocket(int fd) noexcept;

// Sets SO_RCVTIMEO or SO_SNDTIMEO. A zero timeout means "block forever".
bool SetTimeout(int fd, int option, std::chrono::seconds timeout);

// Creates a non-blocking IPv4 listening socket bound to every interface. Port 0 picks a free port.
UniqueFd CreateListeningSocket(std::uint16_t port, int backlog, std::string& error);

// Resolves host (a name or a dotted IPv4 address) and connects a blocking TCP socket to it.
UniqueFd ConnectToServer(const std::string& host, std::uint16_t port, std::string& error);

// Formats an IPv4 address given in host byte order, for example "127.0.0.1".
std::string FormatIpv4(std::uint32_t address);

// Parses a TCP port number in the range 1-65535.
bool ParsePort(const std::string& text, std::uint16_t& port);

}
