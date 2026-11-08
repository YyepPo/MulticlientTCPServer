cmake_minimum_required(VERSION 3.20)

project(MulticlientTCPServer VERSION 2.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This project uses Linux socket APIs (accept4, pipe2, MSG_NOSIGNAL) and builds on Linux only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(CHAT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(CHAT_SANITIZE_ADDRESS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(CHAT_SANITIZE_THREAD "Build with ThreadSanitizer" OFF)

if(CHAT_SANITIZE_ADDRESS AND CHAT_SANITIZE_THREAD)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be combined.")
endif()

if(CHAT_SANITIZE_ADDRESS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

if(CHAT_SANITIZE_THREAD)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

# Everything except the two main() files, so the tests can link the real server code.
add_library(chat_core STATIC
    src/ChatServer.cpp
    src/Command.cpp
    src/Console.cpp
    src/Net.cpp
    src/Protocol.cpp
    src/Text.cpp
    src/UniqueFd.cpp
)
target_include_directories(chat_core PUBLIC src)
target_link_libraries(chat_core PUBLIC Threads::Threads)
target_compile_options(chat_core PUBLIC -Wall -Wextra -Wpedantic -Wshadow)

if(CHAT_WARNINGS_AS_ERRORS)
    target_compile_options(chat_core PUBLIC -Werror)
endif()

add_executable(chat_server src/ServerMain.cpp)
target_link_libraries(chat_server PRIVATE chat_core)

add_executable(chat_client src/ClientMain.cpp)
target_link_libraries(chat_client PRIVATE chat_core)

include(CTest)

if(BUILD_TESTING)
    add_executable(chat_tests tests/Tests.cpp)
    target_link_libraries(chat_tests PRIVATE chat_core)
    add_test(NAME chat_tests COMMAND chat_tests)
endif()
