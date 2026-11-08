#include "Command.h"

#include "Text.h"

#include <charconv>
#include <system_error>
#include <utility>

namespace command
{

namespace
{

// Returns the next space-separated token and advances rest past it.
std::string_view NextToken(std::string_view& rest)
{
    rest = text::Trim(rest);

    const std::size_t end = rest.find(' ');
    const std::string_view token = rest.substr(0, end);
    rest = end == std::string_view::npos ? std::string_view() : rest.substr(end);

    return token;
}

ParseResult Fail(const Spec& spec, std::string_view reason)
{
    ParseResult result;
    result.error = std::string(reason) + ". Usage: " + std::string(spec.usage);
    return result;
}

ParseResult Succeed(Command parsed)
{
    ParseResult result;
    result.command = std::move(parsed);
    return result;
}

}

const std::vector<Spec>& AllSpecs()
{
    static const std::vector<Spec> specs =
    {
        {Type::Help, "/help", "/help", "List the commands you can use", Permission::Everyone},
        {Type::List, "/list", "/list", "List connected clients and their ids", Permission::Everyone},
        {Type::Msg, "/msg", "/msg <id> <text>", "Send a private message", Permission::Everyone},
        {Type::Mute, "/mute", "/mute <id>", "Stop receiving messages from a client", Permission::Everyone},
        {Type::Unmute, "/unmute", "/unmute <id>", "Receive messages from a client again", Permission::Everyone},
        {Type::Kick, "/kick", "/kick <id>", "Disconnect a client", Permission::Admin},
        {Type::Ban, "/ban", "/ban <id>", "Disconnect a client and refuse its IP address until restart", Permission::Admin},
        {Type::Admin, "/admin", "/admin <id>", "Grant admin rights to a client", Permission::ConsoleOnly},
    };

    return specs;
}

const Spec& SpecFor(Type type)
{
    for (const Spec& spec : AllSpecs())
    {
        if (spec.type == type)
        {
            return spec;
        }
    }

    return AllSpecs().front();
}

std::optional<std::uint32_t> ParseClientId(std::string_view token)
{
    std::uint32_t value = 0;
    const char* begin = token.data();
    const char* end = token.data() + token.size();
    const auto [parsed_end, error_code] = std::from_chars(begin, end, value);

    if (error_code != std::errc() || parsed_end != end || value == 0)
    {
        return std::nullopt;
    }

    return value;
}

ParseResult ParseCommand(std::string_view line)
{
    std::string_view rest = line;
    const std::string_view name = NextToken(rest);

    const Spec* spec = nullptr;

    for (const Spec& candidate : AllSpecs())
    {
        if (candidate.name == name)
        {
            spec = &candidate;
            break;
        }
    }

    if (spec == nullptr)
    {
        ParseResult result;
        result.error = "Unknown command '" + std::string(name) + "'. Type /help for the list.";
        return result;
    }

    Command parsed;
    parsed.type = spec->type;

    if (spec->type == Type::Help || spec->type == Type::List)
    {
        if (!text::Trim(rest).empty())
        {
            return Fail(*spec, "Unexpected arguments");
        }

        return Succeed(std::move(parsed));
    }

    const std::string_view id_token = NextToken(rest);

    if (id_token.empty())
    {
        return Fail(*spec, "Missing client id");
    }

    const std::optional<std::uint32_t> target_id = ParseClientId(id_token);

    if (!target_id)
    {
        return Fail(*spec, "Invalid client id '" + std::string(id_token) + "'");
    }

    parsed.target_id = *target_id;
    const std::string_view remainder = text::Trim(rest);

    if (spec->type == Type::Msg)
    {
        if (remainder.empty())
        {
            return Fail(*spec, "Missing message text");
        }

        parsed.text = std::string(remainder);
        return Succeed(std::move(parsed));
    }

    if (!remainder.empty())
    {
        return Fail(*spec, "Unexpected arguments");
    }

    return Succeed(std::move(parsed));
}

}
