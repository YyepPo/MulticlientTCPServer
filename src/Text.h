#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace text
{

inline constexpr std::size_t kMaxNameLength = 32;
inline constexpr std::string_view kDefaultName = "Anonymous";

// Removes leading and trailing spaces, tabs, carriage returns and newlines.
std::string_view Trim(std::string_view value);

// Replaces ASCII control characters (including the ESC that starts terminal escape sequences) with '?', so one
// client cannot inject control sequences into other clients' terminals. Tabs become spaces; UTF-8 passes through.
std::string SanitizeText(std::string_view value);

// Returns the longest prefix of value that is at most max_size bytes and does not end inside a UTF-8 character.
std::string_view TruncateUtf8(std::string_view value, std::size_t max_size);

// Trims, sanitizes and limits a display name to kMaxNameLength bytes without splitting a UTF-8 character.
// Returns kDefaultName when nothing is left.
std::string NormalizeName(std::string_view value);

// Collects bytes read from a stream and hands them out one line at a time ('\n' separated, '\r' stripped).
class LineBuffer
{
public:
    explicit LineBuffer(std::size_t max_line_length);

    void Append(const char* data, std::size_t size);

    // Returns true and fills line when a line is available. Text longer than max_line_length without a newline is
    // returned in pieces of at most max_line_length bytes, cut between UTF-8 characters, so a missing newline cannot
    // grow the buffer without limit.
    bool NextLine(std::string& line);

private:
    std::string buffer_;
    std::size_t max_line_length_;
};

}
