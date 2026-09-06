# MulticlientTCPServer

A multi-client TCP chat server and command-line client written from scratch in C++ on top of the raw Linux/POSIX socket API — no networking libraries, no frameworks. Each connected client is handled on its own thread, and the server supports broadcast messaging plus a small `/command` protocol for whispers, private messages, muting, listing users, and banning.

Built as a hands-on exercise in socket programming, RAII resource ownership, and multithreaded server design.

## Features

- **Multi-client** — the accept loop spawns a detached `std::thread` per connection, so clients are served concurrently.
- **RAII socket ownership** — a `Socket` class owns the file descriptor and closes it in its destructor. Copying is deleted (two owners would double-close the same fd); move construction/assignment transfers ownership instead.
- **Name handshake** — on connect the server sends a welcome message and reads back the client's display name before registering them.
- **Broadcast chat** — any plain message is relayed to every other connected client.
- **Command protocol** — messages starting with `/` are parsed as commands instead of chat.
- **Per-client mute lists** — each client keeps an `unordered_set` of muted senders; the broadcast loop skips recipients who have muted the sender.
- **Permission check** — `/getall` and `/ban` require the server console or a client flagged as admin.
- **Server console** — a background thread reads stdin so the operator can run the same commands as sender id `0`.
- **Shared state under lock** — clients live in an `unordered_map<int, ClientData>` keyed by fd, guarded by a mutex.

## Commands

| Command | Who can use it | What it does |
| --- | --- | --- |
| `/getall` | server / admin | Prints name, fd, ban status and admin status for every connected client |
| `/whisper <id>` | anyone | Sends a private notification to one client |
| `/msg <id> <text>` | anyone | Sends a private message to one client |
| `/mute <id>` | anyone | Stops receiving broadcasts from that client |
| `/unmute <id>` | anyone | Undoes a mute |
| `/ban <id>` | server / admin | Marks the client banned, notifies them, half-closes with `shutdown(SHUT_WR)`, closes the fd and drops them from the registry |

Anything not starting with `/` is broadcast to all other clients.

## Building

Linux only — the code uses `<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>` and `<unistd.h>`. Requires C++17 (structured bindings) and pthreads.

```bash
g++ -std=c++17 -pthread Server.cpp -o server
g++ -std=c++17 -pthread Client.cpp -o client
```

`ClientData.cpp` is `#include`d directly by `Server.cpp`, so it does not need to be compiled separately.

## Running

Start the server (listens on port `8080`, `INADDR_ANY`, with `SO_REUSEADDR` set):

```bash
./server
```

Then start one client per terminal — the client connects to `127.0.0.1:8080` and runs a send thread and a receive thread so you can type and receive at the same time:

```bash
./client
```

Type your name when prompted, then chat or issue commands.

## Project layout

| File | Contents |
| --- | --- |
| `Server.cpp` | `Socket` RAII wrapper, `TCPServer` (create/bind/listen), the accept loop, per-client receive threads, and every command handler |
| `ClientData.cpp` | `ClientData` — per-client state: fd, name, ban flag, admin flag, and the mute set |
| `Client.cpp` | Standalone CLI client with separate send and receive threads |

## What I learned

- The full TCP server lifecycle: `socket()` → `bind()` → `listen()` → `accept()` → `recv()`/`send()`.
- Why a socket wrapper must delete its copy operations but keep move semantics — unique ownership of an OS handle.
- Distinguishing `recv()` returning `0` (peer closed, clean disconnect) from `-1` (error), and handling `EINTR`.
- Graceful disconnect with `shutdown(SHUT_WR)` before `close()`.
- Protecting shared containers with `std::mutex` / `std::lock_guard` when many threads touch them.
- `SO_REUSEADDR` to avoid "Address already in use" while iterating on the server.
