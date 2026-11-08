#include "Console.h"

#include <atomic>
#include <iostream>
#include <mutex>

namespace console
{

namespace
{

std::mutex g_output_mutex;
std::atomic<bool> g_output_enabled{true};

}

void PrintLine(std::string_view line)
{
    if (!g_output_enabled.load())
    {
        return;
    }

    const std::lock_guard<std::mutex> lock(g_output_mutex);
    std::cout << line << std::endl;
}

void PrintError(std::string_view line)
{
    const std::lock_guard<std::mutex> lock(g_output_mutex);
    std::cerr << line << std::endl;
}

void SetOutputEnabled(bool enabled)
{
    g_output_enabled.store(enabled);
}

}
