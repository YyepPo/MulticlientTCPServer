#pragma once

// Owns one POSIX file descriptor (a socket or a pipe end) and closes it exactly once.
// Copying is deleted because two owners would close the same descriptor; moving transfers ownership.
class UniqueFd
{
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept;
    ~UniqueFd();

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept;
    UniqueFd& operator=(UniqueFd&& other) noexcept;

    int Get() const noexcept;
    bool IsValid() const noexcept;

    // Closes the current descriptor, if any, and takes ownership of fd.
    void Reset(int fd = -1) noexcept;

private:
    int fd_ = -1;
};
