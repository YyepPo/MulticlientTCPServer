#pragma once

#include <string_view>

// Line output shared by every thread. A line is always written whole, never mixed with another thread's output.
namespace console
{

// Prints one line to stdout.
void PrintLine(std::string_view line);

// Prints one line to stderr. Always printed, even when normal output is disabled.
void PrintError(std::string_view line);

// Turns PrintLine output on or off. The tests switch it off to keep their own output readable.
void SetOutputEnabled(bool enabled);

}
