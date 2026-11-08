#pragma once

#include "Command.h"
#include "Text.h"
#include "UniqueFd.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

// Multi-client chat server.
//
// Threads:
//  - The thread that calls Run() owns an event loop (poll) over the listening socket, the operator console (stdin)
//    and a wake-up pipe. It accepts connections, runs console commands and joins client threads that finished.
//  - Every connected client gets its own thread, which reads that client's frames and handles them.
//
// Shared state:
//  - clients_, banned_addresses_ and the guarded fields of every ClientSession are protected by clients_mutex_.
//  - Bytes go to a client's socket only while that session's send_mutex is held, so frames written by different
//    threads never interleave. clients_mutex_ is never held while sending, so one slow client cannot stall the rest.
//  - Sessions are owned through shared_ptr. A socket is closed only when its last owner lets go, so no thread can
//    send to a descriptor number that was closed and then reused by a newer connection.
class ChatServer
{
public:
    struct Options
    {
        std::uint16_t port = 8080;  // 0 picks a free port (the tests use this)
        std::size_t max_clients = 64;
        bool console_enabled = true;
    };

    static constexpr char kStopByte = 's';

    explicit ChatServer(Options options);
    ~ChatServer();

    ChatServer(const ChatServer&) = delete;
    ChatServer& operator=(const ChatServer&) = delete;

    // Opens the listening socket and the wake-up pipe.
    bool Start(std::string& error);

    // Serves clients until a stop is requested, then disconnects everyone and joins every client thread.
    void Run();

    // Makes Run() return. Safe to call from any thread.
    void RequestStop();

    // Write end of the wake-up pipe. A signal handler may write kStopByte to it, since write() is async-signal-safe.
    int StopFd() const;

    // The port actually bound, which differs from Options::port when that was 0.
    std::uint16_t Port() const;

private:
    struct ClientSession
    {
        std::uint32_t id = 0;
        std::uint32_t address = 0;  // IPv4, host byte order
        UniqueFd socket;
        std::mutex send_mutex;

        // Guarded by ChatServer::clients_mutex_.
        bool joined = false;
        bool is_admin = false;
        std::string name;
        std::string leave_notice;
        std::unordered_set<std::uint32_t> muted_ids;
    };

    using SessionPtr = std::shared_ptr<ClientSession>;

    static constexpr std::uint32_t kConsoleId = 0;
    static constexpr char kReapByte = 'r';

    // Event loop thread.
    bool HandleWakeUp();
    void AcceptClient();
    bool ReadConsole();
    void ReapFinishedWorkers();
    void DisconnectEveryone();

    // Client threads.
    void ServeClient(SessionPtr session);
    bool Handshake(ClientSession& session);
    void ReceiveLoop(ClientSession& session);
    void RemoveSession(ClientSession& session);

    // Any thread.
    void WakeEventLoop(char byte);
    void HandleLine(std::uint32_t sender_id, std::string_view line);
    void HandleCommand(std::uint32_t sender_id, const command::Command& parsed);
    void Broadcast(std::uint32_t sender_id, std::string_view message);
    void BroadcastNotice(std::string_view notice, std::uint32_t excluded_id);
    void Reply(std::uint32_t recipient_id, std::string_view message);
    void SendTo(ClientSession& session, std::string_view message);
    bool HasPermission(std::uint32_t sender_id, command::Permission permission) const;

    void CommandHelp(std::uint32_t sender_id);
    void CommandList(std::uint32_t sender_id);
    void CommandMsg(std::uint32_t sender_id, std::uint32_t target_id, const std::string& message);
    void CommandMute(std::uint32_t sender_id, std::uint32_t target_id, bool mute);
    void CommandRemove(std::uint32_t sender_id, std::uint32_t target_id, bool ban);
    void CommandAdmin(std::uint32_t sender_id, std::uint32_t target_id);

    // The caller must hold clients_mutex_.
    SessionPtr FindJoinedLocked(std::uint32_t id) const;
    std::string DisplayNameLocked(std::uint32_t id) const;

    const Options options_;
    UniqueFd listener_;
    UniqueFd wake_read_;
    UniqueFd wake_write_;
    text::LineBuffer console_input_;

    mutable std::mutex clients_mutex_;
    std::map<std::uint32_t, SessionPtr> clients_;
    std::unordered_set<std::uint32_t> banned_addresses_;

    // Event loop thread only.
    std::uint32_t next_client_id_ = 1;
    std::map<std::uint32_t, std::thread> workers_;

    std::mutex finished_mutex_;
    std::vector<std::uint32_t> finished_ids_;
};
