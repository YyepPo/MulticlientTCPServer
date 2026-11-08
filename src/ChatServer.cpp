#include "ChatServer.h"

#include "Console.h"
#include "Net.h"
#include "Protocol.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <exception>
#include <system_error>
#include <utility>

namespace
{

constexpr int kListenBacklog = 16;
constexpr std::size_t kUsageWidth = 18;

// Overall deadlines (not per-syscall timeouts), so a peer cannot stretch them by trickling one byte at a time.
// A client that stops reading must not block the threads that send to it for ever.
constexpr std::chrono::seconds kSendTimeout{5};

// A connection that never sends its name must not hold a thread and a client slot for ever.
constexpr std::chrono::seconds kHandshakeTimeout{30};

// Refusals are sent from the event loop thread, which must never wait long.
constexpr std::chrono::seconds kRefusalTimeout{1};

std::string ErrnoText(const char* what)
{
    return std::string(what) + ": " + std::system_category().message(errno);
}

}

ChatServer::ChatServer(Options options)
    : options_(options)
    , console_input_(protocol::kMaxPayloadSize)
{
}

ChatServer::~ChatServer()
{
    DisconnectEveryone();
}

bool ChatServer::Start(std::string& error)
{
    int pipe_fds[2] = {-1, -1};

    if (::pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC) != 0)
    {
        error = ErrnoText("pipe2");
        return false;
    }

    wake_read_.Reset(pipe_fds[0]);
    wake_write_.Reset(pipe_fds[1]);

    listener_ = net::CreateListeningSocket(options_.port, kListenBacklog, error);
    return listener_.IsValid();
}

void ChatServer::Run()
{
    console::PrintLine("Listening on port " + std::to_string(Port()) + ". Type /help for console commands, Ctrl+C to stop.");

    pollfd poll_fds[3] = {};
    poll_fds[0].fd = wake_read_.Get();
    poll_fds[0].events = POLLIN;
    poll_fds[1].fd = listener_.Get();
    poll_fds[1].events = POLLIN;
    poll_fds[2].fd = STDIN_FILENO;
    poll_fds[2].events = POLLIN;

    nfds_t poll_count = options_.console_enabled ? 3 : 2;

    while (true)
    {
        const int ready = ::poll(poll_fds, poll_count, -1);

        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            console::PrintError(ErrnoText("poll"));
            break;
        }

        if ((poll_fds[0].revents & POLLIN) != 0 && !HandleWakeUp())
        {
            break;
        }

        if ((poll_fds[1].revents & POLLIN) != 0)
        {
            AcceptClient();
        }

        if (poll_count == 3 && poll_fds[2].revents != 0 && !ReadConsole())
        {
            console::PrintLine("Console input closed. Stop the server with Ctrl+C or SIGTERM.");
            poll_count = 2;
        }
    }

    console::PrintLine("Shutting down...");
    DisconnectEveryone();
    console::PrintLine("Server stopped.");
}

void ChatServer::RequestStop()
{
    WakeEventLoop(kStopByte);
}

int ChatServer::StopFd() const
{
    return wake_write_.Get();
}

std::uint16_t ChatServer::Port() const
{
    sockaddr_in address{};
    socklen_t length = sizeof(address);

    if (::getsockname(listener_.Get(), reinterpret_cast<sockaddr*>(&address), &length) != 0)
    {
        return 0;
    }

    return ntohs(address.sin_port);
}

bool ChatServer::HandleWakeUp()
{
    char bytes[64];
    bool stop = false;

    while (true)
    {
        const ssize_t count = ::read(wake_read_.Get(), bytes, sizeof(bytes));

        if (count > 0)
        {
            for (ssize_t index = 0; index < count; ++index)
            {
                stop = stop || bytes[index] == kStopByte;
            }

            continue;
        }

        if (count < 0 && errno == EINTR)
        {
            continue;
        }

        // EAGAIN: the pipe is drained.
        break;
    }

    ReapFinishedWorkers();
    return !stop;
}

void ChatServer::AcceptClient()
{
    sockaddr_in peer{};
    socklen_t peer_length = sizeof(peer);
    UniqueFd client(::accept4(listener_.Get(), reinterpret_cast<sockaddr*>(&peer), &peer_length, SOCK_CLOEXEC));

    if (!client.IsValid())
    {
        // EAGAIN or ECONNABORTED: the connection went away between poll() and accept4(); nothing to do.
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNABORTED && errno != EINTR)
        {
            console::PrintError(ErrnoText("accept4"));
        }

        return;
    }

    const std::uint32_t address = ntohl(peer.sin_addr.s_addr);
    const std::string peer_text = net::FormatIpv4(address) + ":" + std::to_string(ntohs(peer.sin_port));

    std::string refusal;

    {
        const std::lock_guard<std::mutex> lock(clients_mutex_);

        if (banned_addresses_.count(address) != 0)
        {
            refusal = "You are banned from this server.";
        }
        else if (clients_.size() >= options_.max_clients)
        {
            refusal = "The server is full. Try again later.";
        }
    }

    if (!refusal.empty())
    {
        // The send buffer of a new connection is empty, so this returns at once; the deadline bounds it anyway.
        protocol::SendFrame(client.Get(), refusal, std::chrono::steady_clock::now() + kRefusalTimeout);
        console::PrintLine("Refused " + peer_text + ": " + refusal);
        return;
    }

    auto session = std::make_shared<ClientSession>();
    session->id = next_client_id_++;
    session->address = address;
    session->socket = std::move(client);

    {
        const std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.emplace(session->id, session);
    }

    // Make room in the map before starting the thread: if the map allocation failed after the thread had started,
    // a joinable std::thread would be destroyed and the process would terminate.
    const auto slot = workers_.try_emplace(session->id).first;

    try
    {
        slot->second = std::thread(&ChatServer::ServeClient, this, session);
    }
    catch (const std::system_error& exception)
    {
        workers_.erase(slot);
        console::PrintError(std::string("Could not start a client thread: ") + exception.what());

        const std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.erase(session->id);
        return;
    }

    console::PrintLine("Client " + std::to_string(session->id) + " connected from " + peer_text);
}

bool ChatServer::ReadConsole()
{
    char buffer[1024];
    const ssize_t count = ::read(STDIN_FILENO, buffer, sizeof(buffer));

    if (count < 0)
    {
        return errno == EINTR || errno == EAGAIN;
    }

    if (count == 0)
    {
        return false;
    }

    console_input_.Append(buffer, static_cast<std::size_t>(count));

    std::string line;

    while (console_input_.NextLine(line))
    {
        HandleLine(kConsoleId, line);
    }

    return true;
}

void ChatServer::ReapFinishedWorkers()
{
    std::vector<std::uint32_t> finished;

    {
        const std::lock_guard<std::mutex> lock(finished_mutex_);
        finished.swap(finished_ids_);
    }

    for (const std::uint32_t id : finished)
    {
        const auto found = workers_.find(id);

        if (found == workers_.end())
        {
            continue;
        }

        found->second.join();
        workers_.erase(found);
    }
}

void ChatServer::DisconnectEveryone()
{
    std::vector<SessionPtr> sessions;

    {
        const std::lock_guard<std::mutex> lock(clients_mutex_);

        for (const auto& entry : clients_)
        {
            sessions.push_back(entry.second);
        }
    }

    // Every send is bounded by kSendTimeout, so waiting for a send lock here is bounded too.
    for (const SessionPtr& session : sessions)
    {
        SendTo(*session, "The server is shutting down.");
        net::ShutdownSocket(session->socket.Get());
    }

    for (auto& entry : workers_)
    {
        entry.second.join();
    }

    workers_.clear();

    const std::lock_guard<std::mutex> lock(finished_mutex_);
    finished_ids_.clear();
}

void ChatServer::ServeClient(SessionPtr session)
{
    try
    {
        if (Handshake(*session))
        {
            ReceiveLoop(*session);
        }
    }
    catch (const std::exception& exception)
    {
        console::PrintError("Client " + std::to_string(session->id) + ": " + exception.what());
    }

    RemoveSession(*session);

    {
        const std::lock_guard<std::mutex> lock(finished_mutex_);
        finished_ids_.push_back(session->id);
    }

    WakeEventLoop(kReapByte);
}

bool ChatServer::Handshake(ClientSession& session)
{
    SendTo(session, "Welcome! Enter your display name:");

    std::string requested_name;
    const auto deadline = std::chrono::steady_clock::now() + kHandshakeTimeout;
    const protocol::ReceiveStatus status = protocol::ReceiveFrame(session.socket.Get(), requested_name, deadline);

    if (status != protocol::ReceiveStatus::Ok)
    {
        return false;
    }

    const std::string name = text::NormalizeName(requested_name);
    const std::string id_text = std::to_string(session.id);

    {
        const std::lock_guard<std::mutex> lock(clients_mutex_);
        session.name = name;
        session.joined = true;
    }

    SendTo(session, "Hi " + name + ", your id is " + id_text + ". Type /help for commands.");
    BroadcastNotice("* " + name + " (" + id_text + ") joined the chat.", session.id);
    console::PrintLine("Client " + id_text + " joined as '" + name + "'");

    return true;
}

void ChatServer::ReceiveLoop(ClientSession& session)
{
    std::string payload;

    while (true)
    {
        const protocol::ReceiveStatus status = protocol::ReceiveFrame(session.socket.Get(), payload);

        if (status == protocol::ReceiveStatus::ProtocolError)
        {
            SendTo(session, "Protocol error: frame length must be 1-" + std::to_string(protocol::kMaxPayloadSize) + " bytes. Disconnecting.");
            return;
        }

        if (status != protocol::ReceiveStatus::Ok)
        {
            return;
        }

        HandleLine(session.id, payload);
    }
}

void ChatServer::RemoveSession(ClientSession& session)
{
    const std::string id_text = std::to_string(session.id);
    bool was_joined = false;
    std::string notice;

    {
        const std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.erase(session.id);
        was_joined = session.joined;

        const std::string ending = session.leave_notice.empty() ? std::string(" left the chat.") : session.leave_notice;
        notice = "* " + session.name + " (" + id_text + ")" + ending;
    }

    // The peer must see the connection end even when this thread leaves because of a protocol error.
    net::ShutdownSocket(session.socket.Get());

    if (was_joined)
    {
        BroadcastNotice(notice, session.id);
    }

    console::PrintLine("Client " + id_text + " disconnected");
}

void ChatServer::WakeEventLoop(char byte)
{
    // The pipe is non-blocking. If it is full, a wake-up is already pending, so a failed write is harmless.
    const ssize_t written = ::write(wake_write_.Get(), &byte, 1);
    static_cast<void>(written);
}

void ChatServer::HandleLine(std::uint32_t sender_id, std::string_view line)
{
    const std::string clean = text::SanitizeText(text::Trim(line));

    if (clean.empty())
    {
        return;
    }

    if (clean.front() != '/')
    {
        Broadcast(sender_id, clean);
        return;
    }

    const command::ParseResult result = command::ParseCommand(clean);

    if (!result.command)
    {
        Reply(sender_id, result.error);
        return;
    }

    HandleCommand(sender_id, *result.command);
}

void ChatServer::HandleCommand(std::uint32_t sender_id, const command::Command& parsed)
{
    const command::Spec& spec = command::SpecFor(parsed.type);

    if (!HasPermission(sender_id, spec.permission))
    {
        Reply(sender_id, "You are not allowed to use " + std::string(spec.name) + ".");
        return;
    }

    switch (parsed.type)
    {
        case command::Type::Help:
            CommandHelp(sender_id);
            break;

        case command::Type::List:
            CommandList(sender_id);
            break;

        case command::Type::Msg:
            CommandMsg(sender_id, parsed.target_id, parsed.text);
            break;

        case command::Type::Mute:
            CommandMute(sender_id, parsed.target_id, true);
            break;

        case command::Type::Unmute:
            CommandMute(sender_id, parsed.target_id, false);
            break;

        case command::Type::Kick:
            CommandRemove(sender_id, parsed.target_id, false);
            break;

        case command::Type::Ban:
            CommandRemove(sender_id, parsed.target_id, true);
            break;

        case command::Type::Admin:
            CommandAdmin(sender_id, parsed.target_id);
            break;
    }
}

void ChatServer::Broadcast(std::uint32_t sender_id, std::string_view message)
{
    std::vector<SessionPtr> recipients;
    std::string line;

    {
        const std::lock_guard<std::mutex> lock(clients_mutex_);
        line = DisplayNameLocked(sender_id) + ": " + std::string(message);

        for (const auto& [id, session] : clients_)
        {
            if (id == sender_id || !session->joined || session->muted_ids.count(sender_id) != 0)
            {
                continue;
            }

            recipients.push_back(session);
        }
    }

    for (const SessionPtr& recipient : recipients)
    {
        SendTo(*recipient, line);
    }

    if (sender_id != kConsoleId)
    {
        console::PrintLine(line);
    }
}

void ChatServer::BroadcastNotice(std::string_view notice, std::uint32_t excluded_id)
{
    std::vector<SessionPtr> recipients;

    {
        const std::lock_guard<std::mutex> lock(clients_mutex_);

        for (const auto& [id, session] : clients_)
        {
            if (id != excluded_id && session->joined)
            {
                recipients.push_back(session);
            }
        }
    }

    for (const SessionPtr& recipient : recipients)
    {
        SendTo(*recipient, notice);
    }
}

void ChatServer::Reply(std::uint32_t recipient_id, std::string_view message)
{
    if (recipient_id == kConsoleId)
    {
        console::PrintLine(message);
        return;
    }

    SessionPtr recipient;

    {
        const std::lock_guard<std::mutex> lock(clients_mutex_);
        const auto found = clients_.find(recipient_id);

        if (found != clients_.end())
        {
            recipient = found->second;
        }
    }

    if (recipient)
    {
        SendTo(*recipient, message);
    }
}

void ChatServer::SendTo(ClientSession& session, std::string_view message)
{
    // A chat line is "name (id): " plus up to a full frame of text, so it can be longer than one frame.
    const std::string_view payload = text::TruncateUtf8(message, protocol::kMaxPayloadSize);

    if (payload.empty())
    {
        return;
    }

    const std::lock_guard<std::mutex> lock(session.send_mutex);

    // The deadline starts once the lock is held, so time spent waiting for another sender does not count.
    const auto deadline = std::chrono::steady_clock::now() + kSendTimeout;

    if (!protocol::SendFrame(session.socket.Get(), payload, deadline))
    {
        // The peer is gone or stalled past the send timeout. Waking its receive thread lets that thread clean up.
        net::ShutdownSocket(session.socket.Get());
    }
}

bool ChatServer::HasPermission(std::uint32_t sender_id, command::Permission permission) const
{
    if (sender_id == kConsoleId || permission == command::Permission::Everyone)
    {
        return true;
    }

    if (permission == command::Permission::ConsoleOnly)
    {
        return false;
    }

    const std::lock_guard<std::mutex> lock(clients_mutex_);
    const auto found = clients_.find(sender_id);

    return found != clients_.end() && found->second->is_admin;
}

void ChatServer::CommandHelp(std::uint32_t sender_id)
{
    std::string help = "Commands:";

    for (const command::Spec& spec : command::AllSpecs())
    {
        if (!HasPermission(sender_id, spec.permission))
        {
            continue;
        }

        std::string usage(spec.usage);

        if (usage.size() < kUsageWidth)
        {
            usage.append(kUsageWidth - usage.size(), ' ');
        }

        help += "\n  " + usage + std::string(spec.description);
    }

    if (sender_id == kConsoleId)
    {
        help += "\nAnything else you type is sent to every client as [server].";
    }
    else
    {
        help += "\nAnything else you type is sent to everyone. Type /quit to leave.";
    }

    Reply(sender_id, help);
}

void ChatServer::CommandList(std::uint32_t sender_id)
{
    std::vector<std::string> rows;

    {
        const std::lock_guard<std::mutex> lock(clients_mutex_);

        for (const auto& [id, session] : clients_)
        {
            if (!session->joined)
            {
                continue;
            }

            std::string row = "  " + std::to_string(id) + "  " + session->name;

            if (session->is_admin)
            {
                row += " (admin)";
            }

            if (id == sender_id)
            {
                row += " (you)";
            }

            rows.push_back(std::move(row));
        }
    }

    if (rows.empty())
    {
        Reply(sender_id, "No clients are connected.");
        return;
    }

    std::string list = "Connected clients (" + std::to_string(rows.size()) + "):";

    for (const std::string& row : rows)
    {
        list += "\n" + row;
    }

    Reply(sender_id, list);
}

void ChatServer::CommandMsg(std::uint32_t sender_id, std::uint32_t target_id, const std::string& message)
{
    if (target_id == sender_id)
    {
        Reply(sender_id, "You cannot message yourself.");
        return;
    }

    SessionPtr target;
    std::string sender_name;
    std::string target_name;
    bool muted = false;

    {
        const std::lock_guard<std::mutex> lock(clients_mutex_);
        target = FindJoinedLocked(target_id);

        if (target)
        {
            sender_name = DisplayNameLocked(sender_id);
            target_name = DisplayNameLocked(target_id);
            muted = target->muted_ids.count(sender_id) != 0;
        }
    }

    if (!target)
    {
        Reply(sender_id, "No client with id " + std::to_string(target_id) + ". Type /list to see who is online.");
        return;
    }

    if (!muted)
    {
        SendTo(*target, "(private) " + sender_name + ": " + message);
    }

    // The sender is not told about a mute, so muting someone stays private.
    Reply(sender_id, "(private to " + target_name + ") " + message);
}

void ChatServer::CommandMute(std::uint32_t sender_id, std::uint32_t target_id, bool mute)
{
    if (sender_id == kConsoleId)
    {
        Reply(sender_id, "The console does not receive chat, so there is nothing to mute.");
        return;
    }

    if (target_id == sender_id)
    {
        Reply(sender_id, "You cannot mute yourself.");
        return;
    }

    std::string reply;

    {
        const std::lock_guard<std::mutex> lock(clients_mutex_);
        const auto sender = clients_.find(sender_id);

        if (sender == clients_.end())
        {
            return;
        }

        std::unordered_set<std::uint32_t>& muted_ids = sender->second->muted_ids;

        if (!mute)
        {
            reply = muted_ids.erase(target_id) != 0
                ? "Unmuted client " + std::to_string(target_id) + "."
                : "Client " + std::to_string(target_id) + " was not muted.";
        }
        else if (!FindJoinedLocked(target_id))
        {
            reply = "No client with id " + std::to_string(target_id) + ".";
        }
        else if (muted_ids.insert(target_id).second)
        {
            reply = "Muted " + DisplayNameLocked(target_id) + ".";
        }
        else
        {
            reply = DisplayNameLocked(target_id) + " is already muted.";
        }
    }

    Reply(sender_id, reply);
}

void ChatServer::CommandRemove(std::uint32_t sender_id, std::uint32_t target_id, bool ban)
{
    if (target_id == sender_id)
    {
        Reply(sender_id, "You cannot remove yourself.");
        return;
    }

    SessionPtr target;
    std::string target_name;
    std::string actor_name;

    {
        const std::lock_guard<std::mutex> lock(clients_mutex_);
        target = FindJoinedLocked(target_id);

        if (target)
        {
            target_name = DisplayNameLocked(target_id);
            actor_name = DisplayNameLocked(sender_id);
            target->leave_notice = std::string(ban ? " was banned by " : " was kicked by ") + actor_name + ".";

            if (ban)
            {
                banned_addresses_.insert(target->address);
            }
        }
    }

    if (!target)
    {
        Reply(sender_id, "No client with id " + std::to_string(target_id) + ".");
        return;
    }

    SendTo(*target, std::string(ban ? "You have been banned by " : "You have been kicked by ") + actor_name + ".");

    // The target's own thread sees the shutdown, removes the session and announces why it left.
    net::ShutdownSocket(target->socket.Get());

    std::string summary = target_name;

    if (ban)
    {
        summary += " banned. Connections from " + net::FormatIpv4(target->address) + " are refused until restart.";
    }
    else
    {
        summary += " kicked.";
    }

    if (sender_id != kConsoleId)
    {
        Reply(sender_id, summary);
    }

    console::PrintLine(actor_name + ": " + summary);
}

void ChatServer::CommandAdmin(std::uint32_t sender_id, std::uint32_t target_id)
{
    SessionPtr target;
    std::string target_name;
    bool already_admin = false;

    {
        const std::lock_guard<std::mutex> lock(clients_mutex_);
        target = FindJoinedLocked(target_id);

        if (target)
        {
            target_name = DisplayNameLocked(target_id);
            already_admin = target->is_admin;
            target->is_admin = true;
        }
    }

    if (!target)
    {
        Reply(sender_id, "No client with id " + std::to_string(target_id) + ".");
        return;
    }

    if (already_admin)
    {
        Reply(sender_id, target_name + " is already an admin.");
        return;
    }

    SendTo(*target, "You are now an admin. Type /help to see the extra commands.");
    Reply(sender_id, target_name + " is now an admin.");
}

ChatServer::SessionPtr ChatServer::FindJoinedLocked(std::uint32_t id) const
{
    const auto found = clients_.find(id);

    if (found == clients_.end() || !found->second->joined)
    {
        return nullptr;
    }

    return found->second;
}

std::string ChatServer::DisplayNameLocked(std::uint32_t id) const
{
    if (id == kConsoleId)
    {
        return "[server]";
    }

    const SessionPtr session = FindJoinedLocked(id);

    if (!session)
    {
        return "client " + std::to_string(id);
    }

    return session->name + " (" + std::to_string(id) + ")";
}
