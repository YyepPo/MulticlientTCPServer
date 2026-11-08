#pragma once

#include "Net.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Wire format: every message travels as one frame.
//
//   [ length : 4 bytes, unsigned, big-endian ][ payload : length bytes of UTF-8 text ]
//
// TCP is a byte stream: one recv() can return part of a frame or several frames at once. The length prefix lets
// the receiver rebuild exactly one message at a time. Valid lengths are 1 to kMaxPayloadSize; anything else is a
// protocol error and the connection is dropped, so a peer cannot make the other side allocate unbounded memory.
namespace protocol
{

inline constexpr std::size_t kHeaderSize = 4;
inline constexpr std::size_t kMaxPayloadSize = 4096;

enum class ReceiveStatus
{
    Ok,
    Closed,         // the peer closed the connection cleanly between two frames
    ProtocolError,  // the length header was 0 or larger than kMaxPayloadSize
    IoError         // the read failed, timed out, or the peer closed in the middle of a frame
};

bool IsValidPayloadSize(std::size_t size);

// Returns header + payload. Precondition: IsValidPayloadSize(payload.size()).
std::string EncodeFrame(std::string_view payload);

// Returns the payload length announced by a 4-byte header.
std::uint32_t DecodeLength(const unsigned char* header);

// Sends one frame. Returns false for an invalid payload size, a failed write or a passed deadline.
// Callers that share a socket between threads must serialize their calls (see ChatServer::SendTo).
bool SendFrame(int fd, std::string_view payload, net::Deadline deadline = std::nullopt);

// Receives one frame. A passed deadline is reported as IoError.
ReceiveStatus ReceiveFrame(int fd, std::string& payload, net::Deadline deadline = std::nullopt);

}
