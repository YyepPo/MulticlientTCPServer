#include "Text.h"

namespace text
{

namespace
{

bool IsSpace(char character)
{
    return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

bool IsUtf8Continuation(char character)
{
    return (static_cast<unsigned char>(character) & 0xC0) == 0x80;
}

}

std::string_view Trim(std::string_view value)
{
    while (!value.empty() && IsSpace(value.front()))
    {
        value.remove_prefix(1);
    }

    while (!value.empty() && IsSpace(value.back()))
    {
        value.remove_suffix(1);
    }

    return value;
}

std::string SanitizeText(std::string_view value)
{
    std::string result;
    result.reserve(value.size());

    for (const char character : value)
    {
        const auto byte = static_cast<unsigned char>(character);

        if (character == '\t')
        {
            result.push_back(' ');
            continue;
        }

        if (byte < 0x20 || byte == 0x7F)
        {
            result.push_back('?');
            continue;
        }

        result.push_back(character);
    }

    return result;
}

std::string_view TruncateUtf8(std::string_view value, std::size_t max_size)
{
    if (value.size() <= max_size)
    {
        return value;
    }

    // Step back to the first byte of the character that straddles the limit, so it is dropped whole.
    std::size_t cut = max_size;

    while (cut > 0 && IsUtf8Continuation(value[cut]))
    {
        --cut;
    }

    return value.substr(0, cut);
}

std::string NormalizeName(std::string_view value)
{
    const std::string sanitized = SanitizeText(Trim(value));

    // Cutting can leave trailing spaces behind, so trim again afterwards.
    const std::string_view trimmed = Trim(TruncateUtf8(sanitized, kMaxNameLength));

    if (trimmed.empty())
    {
        return std::string(kDefaultName);
    }

    return std::string(trimmed);
}

LineBuffer::LineBuffer(std::size_t max_line_length)
    : max_line_length_(max_line_length)
{
}

void LineBuffer::Append(const char* data, std::size_t size)
{
    buffer_.append(data, size);
}

bool LineBuffer::NextLine(std::string& line)
{
    const std::size_t newline = buffer_.find('\n');

    if (newline != std::string::npos && newline <= max_line_length_)
    {
        line.assign(buffer_, 0, newline);
        buffer_.erase(0, newline + 1);

        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        return true;
    }

    if (buffer_.size() > max_line_length_)
    {
        std::size_t piece = TruncateUtf8(buffer_, max_line_length_).size();

        // Only invalid UTF-8 (a run of continuation bytes) can leave nothing; never hand out an empty piece.
        if (piece == 0)
        {
            piece = max_line_length_;
        }

        line.assign(buffer_, 0, piece);
        buffer_.erase(0, piece);
        return true;
    }

    return false;
}

}
