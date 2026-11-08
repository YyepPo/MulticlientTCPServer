// Unit tests for framing, command parsing and text handling, plus end-to-end tests that run the real server on
// loopback. No test framework: each test is a function, and CHECK records failures without stopping the run.

#include "ChatServer.h"
#include "Command.h"
#include "Console.h"
#include "Net.h"
#include "Protocol.h"
#include "Text.h"
#include "UniqueFd.h"

#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{

std::atomic<int> g_failures{0};

void Check(bool condition, const char* expression, const char* file, int line)
{
    if (!condition)
    {
        std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, expression);
        ++g_failures;
    }
}

#define CHECK(condition) Check((condition), #condition, __FILE__, __LINE__)

constexpr int kStressClients = 4;
constexpr int kStressMessagesEach = 25;
constexpr std::chrono::seconds kClientReceiveTimeout{5};

// ---------------------------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------------------------

struct SocketPair
{
    UniqueFd first;
    UniqueFd second;
};

SocketPair MakeSocketPair()
{
    int fds[2] = {-1, -1};
    const int result = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    CHECK(result == 0);

    return SocketPair{UniqueFd(fds[0]), UniqueFd(fds[1])};
}

std::string NextFrame(int fd)
{
    std::string payload;

    if (protocol::ReceiveFrame(fd, payload) != protocol::ReceiveStatus::Ok)
    {
        return "<no frame>";
    }

    return payload;
}

// Reads frames until one contains needle. Fails when the connection ends or the receive timeout expires.
bool WaitForFrameContaining(int fd, std::string_view needle)
{
    std::string payload;

    while (protocol::ReceiveFrame(fd, payload) == protocol::ReceiveStatus::Ok)
    {
        if (payload.find(needle) != std::string::npos)
        {
            return true;
        }
    }

    return false;
}

// Connects to the server and completes the name handshake.
UniqueFd JoinChat(std::uint16_t port, const std::string& name)
{
    std::string error;
    UniqueFd client = net::ConnectToServer("127.0.0.1", port, error);
    CHECK(client.IsValid());

    if (!client.IsValid())
    {
        return client;
    }

    net::SetTimeout(client.Get(), SO_RCVTIMEO, kClientReceiveTimeout);

    CHECK(WaitForFrameContaining(client.Get(), "Enter your display name"));
    CHECK(protocol::SendFrame(client.Get(), name));
    CHECK(WaitForFrameContaining(client.Get(), "your id is"));

    return client;
}

// Runs a ChatServer on a free loopback port in a background thread for the lifetime of the object.
class TestServer
{
public:
    TestServer()
        : server_(MakeOptions())
    {
        std::string error;
        started_ = server_.Start(error);
        CHECK(started_);

        if (started_)
        {
            loop_ = std::thread([this]()
            {
                server_.Run();
            });
        }
    }

    ~TestServer()
    {
        Stop();
    }

    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;

    void Stop()
    {
        if (loop_.joinable())
        {
            server_.RequestStop();
            loop_.join();
        }
    }

    bool Started() const
    {
        return started_;
    }

    std::uint16_t Port() const
    {
        return server_.Port();
    }

private:
    static ChatServer::Options MakeOptions()
    {
        ChatServer::Options options;
        options.port = 0;
        options.console_enabled = false;
        return options;
    }

    ChatServer server_;
    bool started_ = false;
    std::thread loop_;
};

// ---------------------------------------------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------------------------------------------

void TestEncodeFrameHeaderIsBigEndian()
{
    const std::string frame = protocol::EncodeFrame(std::string(300, 'a'));

    CHECK(frame.size() == protocol::kHeaderSize + 300);
    CHECK(static_cast<unsigned char>(frame[0]) == 0x00);
    CHECK(static_cast<unsigned char>(frame[1]) == 0x00);
    CHECK(static_cast<unsigned char>(frame[2]) == 0x01);
    CHECK(static_cast<unsigned char>(frame[3]) == 0x2C);

    const auto* header = reinterpret_cast<const unsigned char*>(frame.data());
    CHECK(protocol::DecodeLength(header) == 300);
}

void TestFrameRoundTrip()
{
    SocketPair pair = MakeSocketPair();
    CHECK(protocol::SendFrame(pair.first.Get(), "hello world"));

    std::string payload;
    CHECK(protocol::ReceiveFrame(pair.second.Get(), payload) == protocol::ReceiveStatus::Ok);
    CHECK(payload == "hello world");
}

void TestTwoFramesInOneWrite()
{
    SocketPair pair = MakeSocketPair();
    const std::string bytes = protocol::EncodeFrame("first") + protocol::EncodeFrame("second");
    CHECK(net::SendAll(pair.first.Get(), bytes.data(), bytes.size()));

    std::string payload;
    CHECK(protocol::ReceiveFrame(pair.second.Get(), payload) == protocol::ReceiveStatus::Ok);
    CHECK(payload == "first");
    CHECK(protocol::ReceiveFrame(pair.second.Get(), payload) == protocol::ReceiveStatus::Ok);
    CHECK(payload == "second");
}

void TestFrameArrivingOneByteAtATime()
{
    SocketPair pair = MakeSocketPair();
    const std::string bytes = protocol::EncodeFrame("split across many reads");
    const int writer_fd = pair.first.Get();

    std::thread writer([&bytes, writer_fd]()
    {
        for (const char byte : bytes)
        {
            net::SendAll(writer_fd, &byte, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::string payload;
    const protocol::ReceiveStatus status = protocol::ReceiveFrame(pair.second.Get(), payload);
    writer.join();

    CHECK(status == protocol::ReceiveStatus::Ok);
    CHECK(payload == "split across many reads");
}

void TestOversizedLengthIsRejected()
{
    SocketPair pair = MakeSocketPair();
    const unsigned char header[4] = {0x00, 0x00, 0x10, 0x01};  // 4097 bytes
    CHECK(net::SendAll(pair.first.Get(), reinterpret_cast<const char*>(header), sizeof(header)));

    std::string payload;
    CHECK(protocol::ReceiveFrame(pair.second.Get(), payload) == protocol::ReceiveStatus::ProtocolError);
}

void TestZeroLengthIsRejected()
{
    SocketPair pair = MakeSocketPair();
    const unsigned char header[4] = {0x00, 0x00, 0x00, 0x00};
    CHECK(net::SendAll(pair.first.Get(), reinterpret_cast<const char*>(header), sizeof(header)));

    std::string payload;
    CHECK(protocol::ReceiveFrame(pair.second.Get(), payload) == protocol::ReceiveStatus::ProtocolError);
}

void TestCleanCloseBetweenFrames()
{
    SocketPair pair = MakeSocketPair();
    pair.first.Reset();

    std::string payload;
    CHECK(protocol::ReceiveFrame(pair.second.Get(), payload) == protocol::ReceiveStatus::Closed);
}

void TestCloseInsideFrameIsAnError()
{
    SocketPair pair = MakeSocketPair();
    const std::string frame = protocol::EncodeFrame("truncated payload");
    CHECK(net::SendAll(pair.first.Get(), frame.data(), frame.size() - 3));
    pair.first.Reset();

    std::string payload;
    CHECK(protocol::ReceiveFrame(pair.second.Get(), payload) == protocol::ReceiveStatus::IoError);
}

void TestSendFrameRejectsInvalidSizes()
{
    SocketPair pair = MakeSocketPair();

    CHECK(!protocol::SendFrame(pair.first.Get(), ""));
    CHECK(!protocol::SendFrame(pair.first.Get(), std::string(protocol::kMaxPayloadSize + 1, 'x')));
    CHECK(protocol::SendFrame(pair.first.Get(), std::string(protocol::kMaxPayloadSize, 'x')));
}

// A deadline bounds the whole read: with no data arriving, ReceiveFrame must give up shortly after it passes.
void TestReceiveDeadline()
{
    SocketPair pair = MakeSocketPair();
    const auto start = std::chrono::steady_clock::now();

    std::string payload;
    const protocol::ReceiveStatus status = protocol::ReceiveFrame(pair.second.Get(), payload, start + std::chrono::milliseconds(100));
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(status == protocol::ReceiveStatus::IoError);
    CHECK(elapsed >= std::chrono::milliseconds(100));
    CHECK(elapsed < std::chrono::seconds(2));
}

// The deadline still lets a complete frame through, even when it arrives in pieces.
void TestReceiveDeadlineAllowsPartialArrival()
{
    SocketPair pair = MakeSocketPair();
    const std::string frame = protocol::EncodeFrame("in two parts");
    const int writer_fd = pair.first.Get();

    std::thread writer([&frame, writer_fd]()
    {
        net::SendAll(writer_fd, frame.data(), 3);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        net::SendAll(writer_fd, frame.data() + 3, frame.size() - 3);
    });

    std::string payload;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const protocol::ReceiveStatus status = protocol::ReceiveFrame(pair.second.Get(), payload, deadline);
    writer.join();

    CHECK(status == protocol::ReceiveStatus::Ok);
    CHECK(payload == "in two parts");
}

// ---------------------------------------------------------------------------------------------------------------
// Command parsing
// ---------------------------------------------------------------------------------------------------------------

void TestParseMsg()
{
    const command::ParseResult result = command::ParseCommand("/msg 3 hello there");
    CHECK(result.command.has_value());

    if (!result.command)
    {
        return;
    }

    CHECK(result.command->type == command::Type::Msg);
    CHECK(result.command->target_id == 3);
    CHECK(result.command->text == "hello there");
}

void TestParseErrors()
{
    CHECK(!command::ParseCommand("/msg 3").command);
    CHECK(!command::ParseCommand("/msg abc hi").command);
    CHECK(!command::ParseCommand("/mute").command);
    CHECK(!command::ParseCommand("/mute 0").command);
    CHECK(!command::ParseCommand("/ban 5 extra").command);
    CHECK(!command::ParseCommand("/list now").command);
    CHECK(!command::ParseCommand("/nope").command);
    CHECK(!command::ParseCommand("/").command);
}

// Regression: the old parser skipped only 5 characters of "/unmute", so unmuting never worked.
void TestUnmuteParses()
{
    const command::ParseResult result = command::ParseCommand("/unmute 7");

    CHECK(result.command.has_value());
    CHECK(result.command && result.command->type == command::Type::Unmute);
    CHECK(result.command && result.command->target_id == 7);
}

// Regression: the old parser matched prefixes, so "/banana 3" ran /ban.
void TestCommandNamesMatchWholeWords()
{
    CHECK(!command::ParseCommand("/muted 3").command);
    CHECK(!command::ParseCommand("/banana 3").command);
}

void TestParseClientId()
{
    CHECK(command::ParseClientId("42") == std::optional<std::uint32_t>(42));
    CHECK(command::ParseClientId("4294967295") == std::optional<std::uint32_t>(4294967295u));
    CHECK(!command::ParseClientId(""));
    CHECK(!command::ParseClientId("0"));
    CHECK(!command::ParseClientId("-1"));
    CHECK(!command::ParseClientId("12a"));
    CHECK(!command::ParseClientId("4294967296"));
}

// ---------------------------------------------------------------------------------------------------------------
// Text handling
// ---------------------------------------------------------------------------------------------------------------

void TestSanitizeText()
{
    CHECK(text::SanitizeText("plain text") == "plain text");
    CHECK(text::SanitizeText("a\tb") == "a b");
    CHECK(text::SanitizeText("\x1b[31mred") == "?[31mred");
    CHECK(text::SanitizeText("line\nbreak") == "line?break");
    CHECK(text::SanitizeText("caf\xc3\xa9") == "caf\xc3\xa9");
}

void TestTruncateUtf8()
{
    CHECK(text::TruncateUtf8("short", 10) == "short");
    CHECK(text::TruncateUtf8("abcdef", 4) == "abcd");

    // "ab" + a 2-byte character: a 3-byte limit would split the character, so it is dropped whole.
    CHECK(text::TruncateUtf8("ab\xc3\xa9", 3) == "ab");
    CHECK(text::TruncateUtf8("ab\xc3\xa9", 4) == "ab\xc3\xa9");
}

void TestNormalizeName()
{
    CHECK(text::NormalizeName("  Enes  ") == "Enes");
    CHECK(text::NormalizeName("   ") == "Anonymous");
    CHECK(text::NormalizeName(std::string(40, 'x')) == std::string(32, 'x'));

    // 31 ASCII bytes plus a 2-byte character: the cut must drop the character whole, not split it.
    CHECK(text::NormalizeName(std::string(31, 'x') + "\xc3\xa9") == std::string(31, 'x'));
}

void TestLineBuffer()
{
    text::LineBuffer buffer(8);
    std::string line;

    buffer.Append("ab", 2);
    CHECK(!buffer.NextLine(line));

    buffer.Append("c\r\nde\n", 6);
    CHECK(buffer.NextLine(line) && line == "abc");
    CHECK(buffer.NextLine(line) && line == "de");
    CHECK(!buffer.NextLine(line));

    // No newline and longer than the limit: handed out in limit-sized pieces.
    buffer.Append("0123456789", 10);
    CHECK(buffer.NextLine(line) && line == "01234567");
    CHECK(!buffer.NextLine(line));
}

void TestLineBufferCutsBetweenCharacters()
{
    text::LineBuffer buffer(4);
    std::string line;

    // "abc" + a 2-byte character + "x": a 4-byte piece would split the character, so the piece stops before it.
    buffer.Append("abc\xc3\xa9x", 6);
    CHECK(buffer.NextLine(line) && line == "abc");
    CHECK(!buffer.NextLine(line));
}

// ---------------------------------------------------------------------------------------------------------------
// End to end: a real server on loopback
// ---------------------------------------------------------------------------------------------------------------

void TestChatSession()
{
    TestServer server;

    if (!server.Started())
    {
        return;
    }

    UniqueFd alice = JoinChat(server.Port(), "Alice");
    UniqueFd bob = JoinChat(server.Port(), "Bob");

    CHECK(WaitForFrameContaining(alice.Get(), "* Bob (2) joined the chat."));

    CHECK(protocol::SendFrame(alice.Get(), "hello everyone"));
    CHECK(NextFrame(bob.Get()) == "Alice (1): hello everyone");

    CHECK(protocol::SendFrame(bob.Get(), "/msg 1 just for you"));
    CHECK(NextFrame(alice.Get()) == "(private) Bob (2): just for you");
    CHECK(NextFrame(bob.Get()) == "(private to Alice (1)) just for you");

    // Alice mutes Bob. Bob's /list reply proves the server finished handling his "spam" before Alice asks for her
    // list, so if the mute did not work, "spam" would reach Alice before her own /list reply.
    CHECK(protocol::SendFrame(alice.Get(), "/mute 2"));
    CHECK(NextFrame(alice.Get()) == "Muted Bob (2).");
    CHECK(protocol::SendFrame(bob.Get(), "spam"));
    CHECK(protocol::SendFrame(bob.Get(), "/list"));
    CHECK(WaitForFrameContaining(bob.Get(), "Connected clients"));
    CHECK(protocol::SendFrame(alice.Get(), "/list"));
    CHECK(NextFrame(alice.Get()).rfind("Connected clients (2):", 0) == 0);

    CHECK(protocol::SendFrame(alice.Get(), "/unmute 2"));
    CHECK(NextFrame(alice.Get()) == "Unmuted client 2.");

    CHECK(protocol::SendFrame(alice.Get(), "/kick 2"));
    CHECK(NextFrame(alice.Get()) == "You are not allowed to use /kick.");

    server.Stop();

    CHECK(WaitForFrameContaining(alice.Get(), "The server is shutting down."));

    std::string payload;
    CHECK(protocol::ReceiveFrame(alice.Get(), payload) == protocol::ReceiveStatus::Closed);
}

void TestInvalidFrameDisconnectsTheClient()
{
    TestServer server;

    if (!server.Started())
    {
        return;
    }

    UniqueFd carol = JoinChat(server.Port(), "Carol");
    const unsigned char bad_header[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(net::SendAll(carol.Get(), reinterpret_cast<const char*>(bad_header), sizeof(bad_header)));

    CHECK(WaitForFrameContaining(carol.Get(), "Protocol error"));

    std::string payload;
    CHECK(protocol::ReceiveFrame(carol.Get(), payload) == protocol::ReceiveStatus::Closed);
}

// Several clients broadcast at the same time. Run under ThreadSanitizer, this exercises the shared registry,
// the per-socket send locks and session lifetimes concurrently.
void TestConcurrentBroadcasts()
{
    TestServer server;

    if (!server.Started())
    {
        return;
    }

    std::vector<UniqueFd> clients;

    for (int index = 0; index < kStressClients; ++index)
    {
        clients.push_back(JoinChat(server.Port(), "user" + std::to_string(index)));
    }

    const int expected = (kStressClients - 1) * kStressMessagesEach;
    std::vector<int> received(kStressClients, 0);
    std::vector<std::thread> senders;

    for (int index = 0; index < kStressClients; ++index)
    {
        const int fd = clients[static_cast<std::size_t>(index)].Get();
        int& count = received[static_cast<std::size_t>(index)];

        senders.emplace_back([fd, expected, &count]()
        {
            for (int message = 0; message < kStressMessagesEach; ++message)
            {
                CHECK(protocol::SendFrame(fd, "msg " + std::to_string(message)));
            }

            std::string payload;

            while (count < expected && protocol::ReceiveFrame(fd, payload) == protocol::ReceiveStatus::Ok)
            {
                if (payload.find("): msg ") != std::string::npos)
                {
                    ++count;
                }
            }
        });
    }

    for (std::thread& sender : senders)
    {
        sender.join();
    }

    for (const int count : received)
    {
        CHECK(count == expected);
    }
}

}

int main()
{
    console::SetOutputEnabled(false);

    const std::vector<std::pair<const char*, void (*)()>> tests =
    {
        {"EncodeFrameHeaderIsBigEndian", TestEncodeFrameHeaderIsBigEndian},
        {"FrameRoundTrip", TestFrameRoundTrip},
        {"TwoFramesInOneWrite", TestTwoFramesInOneWrite},
        {"FrameArrivingOneByteAtATime", TestFrameArrivingOneByteAtATime},
        {"OversizedLengthIsRejected", TestOversizedLengthIsRejected},
        {"ZeroLengthIsRejected", TestZeroLengthIsRejected},
        {"CleanCloseBetweenFrames", TestCleanCloseBetweenFrames},
        {"CloseInsideFrameIsAnError", TestCloseInsideFrameIsAnError},
        {"SendFrameRejectsInvalidSizes", TestSendFrameRejectsInvalidSizes},
        {"ReceiveDeadline", TestReceiveDeadline},
        {"ReceiveDeadlineAllowsPartialArrival", TestReceiveDeadlineAllowsPartialArrival},
        {"ParseMsg", TestParseMsg},
        {"ParseErrors", TestParseErrors},
        {"UnmuteParses", TestUnmuteParses},
        {"CommandNamesMatchWholeWords", TestCommandNamesMatchWholeWords},
        {"ParseClientId", TestParseClientId},
        {"SanitizeText", TestSanitizeText},
        {"TruncateUtf8", TestTruncateUtf8},
        {"NormalizeName", TestNormalizeName},
        {"LineBuffer", TestLineBuffer},
        {"LineBufferCutsBetweenCharacters", TestLineBufferCutsBetweenCharacters},
        {"ChatSession", TestChatSession},
        {"InvalidFrameDisconnectsTheClient", TestInvalidFrameDisconnectsTheClient},
        {"ConcurrentBroadcasts", TestConcurrentBroadcasts},
    };

    for (const auto& [name, test] : tests)
    {
        const int failures_before = g_failures.load();
        test();

        const bool passed = g_failures.load() == failures_before;
        std::printf("%s  %s\n", passed ? "PASS" : "FAIL", name);
    }

    if (g_failures.load() != 0)
    {
        std::printf("%d check(s) failed\n", g_failures.load());
        return 1;
    }

    std::printf("All tests passed\n");
    return 0;
}
