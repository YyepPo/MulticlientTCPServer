#include "Protocol.h"

#include <cassert>

namespace protocol
{

bool IsValidPayloadSize(std::size_t size)
{
    return size > 0 && size <= kMaxPayloadSize;
}

std::string EncodeFrame(std::string_view payload)
{
    assert(IsValidPayloadSize(payload.size()));

    const auto length = static_cast<std::uint32_t>(payload.size());

    std::string frame;
    frame.reserve(kHeaderSize + payload.size());
    frame.push_back(static_cast<char>((length >> 24) & 0xFF));
    frame.push_back(static_cast<char>((length >> 16) & 0xFF));
    frame.push_back(static_cast<char>((length >> 8) & 0xFF));
    frame.push_back(static_cast<char>(length & 0xFF));
    frame.append(payload.data(), payload.size());

    return frame;
}

std::uint32_t DecodeLength(const unsigned char* header)
{
    return (static_cast<std::uint32_t>(header[0]) << 24)
        | (static_cast<std::uint32_t>(header[1]) << 16)
        | (static_cast<std::uint32_t>(header[2]) << 8)
        | static_cast<std::uint32_t>(header[3]);
}

bool SendFrame(int fd, std::string_view payload, net::Deadline deadline)
{
    if (!IsValidPayloadSize(payload.size()))
    {
        return false;
    }

    // Header and payload go out in one buffer, so a frame is written by a single SendAll call.
    const std::string frame = EncodeFrame(payload);
    return net::SendAll(fd, frame.data(), frame.size(), deadline);
}

ReceiveStatus ReceiveFrame(int fd, std::string& payload, net::Deadline deadline)
{
    unsigned char header[kHeaderSize];
    const net::IoStatus header_status = net::ReceiveExact(fd, reinterpret_cast<char*>(header), kHeaderSize, deadline);

    if (header_status == net::IoStatus::Closed)
    {
        return ReceiveStatus::Closed;
    }

    if (header_status == net::IoStatus::Error)
    {
        return ReceiveStatus::IoError;
    }

    const std::uint32_t length = DecodeLength(header);

    if (!IsValidPayloadSize(length))
    {
        return ReceiveStatus::ProtocolError;
    }

    payload.resize(length);
    const net::IoStatus payload_status = net::ReceiveExact(fd, payload.data(), payload.size(), deadline);

    if (payload_status != net::IoStatus::Ok)
    {
        payload.clear();
        return ReceiveStatus::IoError;
    }

    return ReceiveStatus::Ok;
}

}
