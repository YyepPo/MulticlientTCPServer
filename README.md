# MulticlientTCPServer

[![CI](https://github.com/YyepPo/MulticlientTCPServer/actions/workflows/ci.yml/badge.svg)](https://github.com/YyepPo/MulticlientTCPServer/actions/workflows/ci.yml)

A multi-client TCP chat server and command-line client in C++17, written directly on the Linux/POSIX socket API with no networking libraries. Messages travel in a small length-prefixed framing protocol, every client is served on its own thread, and the server shuts down cleanly on Ctrl+C.

## Features

- **Length-prefixed framing.** Every message is a 4-byte big-endian length followed by the payload, so messages survive TCP splitting and merging them. Oversized or empty frames are rejected and the sender is disconnected.
- **Thread per client.** The accept loop starts a joinable `std::thread` for each connection. Finished threads are joined as they end, and all of them are joined at shutdown.
- **Safe shared state.** The client registry is guarded by one mutex. Each socket has its own send lock, so frames from different threads never interleave, and the registry lock is never held while sending.
- **Safe socket lifetime.** Sessions are reference-counted. A socket is closed only after its last user releases it, so kicking a client can never close a descriptor that another thread is still using or that the OS has already handed to a new connection.
- **Graceful shutdown.** SIGINT and SIGTERM write one byte to a self-pipe that wakes the event loop. The server tells every client, shuts their sockets down and joins every thread.
- **Robust I/O.** Partial `send`/`recv` loops with `EINTR` retry, `MSG_NOSIGNAL` plus an ignored `SIGPIPE`, and a client limit. Each send to a client has an overall 5-second deadline from the moment it starts, and the name handshake an overall 30-second deadline, both enforced with `poll` and non-blocking I/O, so a peer cannot stretch them by trickling one byte at a time.
- **Input hygiene.** Control characters are replaced before text is relayed, so one client cannot send terminal escape sequences to others. Names are trimmed and limited to 32 bytes, and anything cut to fit a frame is cut between UTF-8 characters, never inside one.
- **RAII everywhere.** `UniqueFd` owns each descriptor (sockets and pipe ends). It is move-only and closes the descriptor exactly once.
- **Tests.** Unit tests cover framing, command parsing and text handling. End-to-end tests run a real server on loopback. CI builds with warnings as errors and runs everything under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer.

## Protocol

Transport is TCP; the default port is 8080. Both directions use the same frame format:

| Field | Size | Encoding |
| --- | --- | --- |
| `length` | 4 bytes | Unsigned integer, big-endian (network byte order). Valid range 1 to 4096. |
| `payload` | `length` bytes | UTF-8 text. |

A frame with a length of 0 or more than 4096 is a protocol error: the receiver drops the connection. The server prefixes relayed chat with the sender's name, so a line close to the limit is shortened (at a character boundary) to fit one frame.

Session flow:

1. The client connects. The server sends `Welcome! Enter your display name:`.
2. The client's first frame is its display name. The server replies with the client's id and tells everyone else that the client joined.
3. After that, every client frame is either a chat message (broadcast to everyone else) or a command starting with `/`. Every server frame is text for the client to display.
4. Either side may close the connection at any time. The server says goodbye before it shuts down.

## Commands

| Command | Who can use it | What it does |
| --- | --- | --- |
| `/help` | everyone | Lists the commands you can use |
| `/list` | everyone | Lists connected clients and their ids |
| `/msg <id> <text>` | everyone | Sends a private message |
| `/mute <id>` | everyone | Stops chat and private messages from that client reaching you |
| `/unmute <id>` | everyone | Undoes a mute |
| `/kick <id>` | admin, console | Disconnects a client |
| `/ban <id>` | admin, console | Disconnects a client and refuses its IP address until the server restarts |
| `/admin <id>` | console | Grants admin rights to a client |

Anything that does not start with `/` is sent to everyone else. On the server console, plain text is sent to every client as `[server]`. The client also understands `/quit`.

## Building

Linux only: the code uses `accept4`, `pipe2` and `MSG_NOSIGNAL`. Requires CMake 3.20+ and a C++17 compiler.

```bash
cmake -S . -B build
cmake --build build
```

## Running

```bash
./build/chat_server          # listens on port 8080; ./build/chat_server 9000 picks another port
./build/chat_client          # connects to 127.0.0.1:8080; ./build/chat_client <host> <port>
```

Start one client per terminal, type a name when asked, then chat or use commands. Stop the server with Ctrl+C.

## Testing

```bash
ctest --test-dir build --output-on-failure
```

Under ThreadSanitizer:

```bash
cmake -S . -B build-tsan -DCHAT_SANITIZE_THREAD=ON -DCHAT_WARNINGS_AS_ERRORS=ON
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

`-DCHAT_SANITIZE_ADDRESS=ON` builds with AddressSanitizer and UndefinedBehaviorSanitizer instead. On recent kernels the sanitizers may fail at startup unless address-space randomization is reduced first with `sudo sysctl vm.mmap_rnd_bits=28`; CI does this before every run.

## Design notes

- **Why framing.** TCP delivers a byte stream, not messages. Treating each `recv()` as one message works on an idle loopback and breaks under load, when one read returns half a message or two messages at once. The length prefix makes message boundaries explicit, and the 4096-byte limit bounds how much memory a peer can make the server allocate.
- **Why reference-counted sessions.** Closing a socket from a thread that does not own it is a classic bug: the descriptor number can be reused by the next `accept()`, and the old owner then reads from, or closes, the wrong connection. Here, other threads only call `shutdown()`, which wakes the owner without releasing the descriptor; the descriptor is closed when the last `shared_ptr` goes away.
- **Why a self-pipe.** A signal handler may only call async-signal-safe functions. It writes one byte to a pipe, and the event loop, which polls that pipe next to the listening socket and stdin, does the actual shutdown on a normal thread.
- **Why deadlines instead of socket timeouts.** A client that stops reading fills its socket buffer, and then `send()` blocks. `SO_SNDTIMEO` and `SO_RCVTIMEO` restart on every partial transfer, so a peer that moves one byte just before each timeout can hold a thread for hours. An absolute deadline, checked with `poll` before each non-blocking `send`/`recv`, cannot be stretched. The first missed deadline shuts the slow client down, after which every send to it fails at once, so the threads that broadcast to it are held up for seconds, not hours.

### Limitations

- IPv4 only.
- Bans are per IP address and last until the server restarts. Clients behind the same address (or every client on localhost) share a ban.
- No authentication or encryption: this is a plain-text protocol meant for learning and local networks.
- Thread per client suits tens of clients. Thousands would call for an `epoll` event loop with non-blocking sockets.

## Project layout

| Path | Contents |
| --- | --- |
| `src/UniqueFd.*` | Move-only RAII owner of a file descriptor |
| `src/Net.*` | Error-checked socket helpers: full sends and reads, timeouts, listen, connect |
| `src/Protocol.*` | Frame encoding and decoding |
| `src/Command.*` | Command table and parser |
| `src/Text.*` | Trimming, sanitizing, name rules, line buffering for stdin |
| `src/Console.*` | Thread-safe line output |
| `src/ChatServer.*` | Event loop, client threads, broadcasting and command handling |
| `src/ServerMain.cpp` | Server entry point and signal handling |
| `src/ClientMain.cpp` | Command-line client: a receive thread plus an input loop |
| `tests/Tests.cpp` | Unit and end-to-end tests |
| `.github/workflows/ci.yml` | Build and test on Ubuntu with sanitizers |
