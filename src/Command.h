#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Parsing of the slash commands typed by clients and by the server operator.
namespace command
{

enum class Type
{
    Help,
    List,
    Msg,
    Mute,
    Unmute,
    Kick,
    Ban,
    Admin
};

enum class Permission
{
    Everyone,
    Admin,       // the server console, or a client granted admin rights
    ConsoleOnly  // the server console only
};

struct Spec
{
    Type type;
    std::string_view name;
    std::string_view usage;
    std::string_view description;
    Permission permission;
};

struct Command
{
    Type type = Type::Help;
    std::uint32_t target_id = 0;
    std::string text;
};

struct ParseResult
{
    std::optional<Command> command;
    std::string error;  // explains the problem when command is empty
};

// Every command the server understands, in /help order.
const std::vector<Spec>& AllSpecs();

const Spec& SpecFor(Type type);

// Parses a line that starts with '/'. The command name must match a whole word ("/muted" is not "/mute").
ParseResult ParseCommand(std::string_view line);

// Client ids are positive 32-bit integers; 0 is reserved for the server console.
std::optional<std::uint32_t> ParseClientId(std::string_view token);

}
