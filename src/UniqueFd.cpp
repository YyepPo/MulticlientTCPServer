#include "UniqueFd.h"

#include <unistd.h>

#include <utility>

UniqueFd::UniqueFd(int fd) noexcept
    : fd_(fd)
{
}

UniqueFd::~UniqueFd()
{
    Reset();
}

UniqueFd::UniqueFd(UniqueFd&& other) noexcept
    : fd_(std::exchange(other.fd_, -1))
{
}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept
{
    if (this != &other)
    {
        Reset(std::exchange(other.fd_, -1));
    }

    return *this;
}

int UniqueFd::Get() const noexcept
{
    return fd_;
}

bool UniqueFd::IsValid() const noexcept
{
    return fd_ >= 0;
}

void UniqueFd::Reset(int fd) noexcept
{
    if (fd_ >= 0)
    {
        // close() is not retried on EINTR: on Linux the descriptor has already been released at that point.
        ::close(fd_);
    }

    fd_ = fd;
}
