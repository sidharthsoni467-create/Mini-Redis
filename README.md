# MiniRedis

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)
![g++](https://img.shields.io/badge/build-g%2B%2B-brightgreen)
![No dependencies](https://img.shields.io/badge/dependencies-none-lightgrey)
![License: MIT](https://img.shields.io/badge/license-MIT-blue)

**An in-memory key-value store written from scratch in C++17 — TCP server, multi-client, O(1) LRU eviction, TTL expiry, and crash recovery from a write-ahead log. Standard library only, no dependencies.**

---

## What is this project?

Most programs keep their data in a database on disk. That is safe, but it is slow — reading from a disk is thousands of times slower than reading from memory. So busy systems put a **cache** in front of the database: a small, extremely fast store that keeps the most-used data in RAM. [Redis](https://redis.io) is the best-known example, and it is one of the most widely deployed pieces of infrastructure in the world.

**MiniRedis is a working reimplementation of the core ideas behind Redis, built from nothing.** You start a server; it listens on a TCP port. You connect with a terminal client and type `SET name Rahul` and it remembers. You type `GET name` and it answers instantly, from memory. Several people can connect at once and they all see the same data. Keys can be given an expiry, after which they vanish on their own. When the store hits its memory budget, it automatically throws out whichever key has gone longest without being used. And every write is recorded to a log file first — so if you kill the server outright with `kill -9`, restarting it brings the data back.

None of that is magic, and the point of this project is that I can explain every layer of it: how a TCP server accepts and serves many clients at once, how a hash map gives O(1) lookup, how pairing that hash map with a doubly linked list gives O(1) least-recently-used eviction, how a write-ahead log turns a volatile in-memory structure into something that survives a crash, and why time-to-live values **must** be written to disk as absolute timestamps rather than durations. This is a learning project, not production software, and its limitations are documented as carefully as its features.

---

## Architecture

```
                      ┌───────────────────────────────────────────────────┐
   ┌──────────┐       │              miniredis-server                     │
   │ client A │──TCP──┤                                                   │
   └──────────┘       │   ┌───────────┐                                   │
                      │   │  accept   │  spawns one thread per connection │
   ┌──────────┐       │   │   loop    │                                   │
   │ client B │──TCP──┤   └─────┬─────┘                                   │
   └──────────┘       │         │                                         │
                      │   ┌─────▼──────────────┐                          │
   ┌──────────┐       │   │  client thread     │  LineBuffer: reassembles │
   │  telnet  │──TCP──┤   │  (one per client)  │  the TCP byte stream     │
   └──────────┘       │   └─────┬──────────────┘  into whole lines        │
                      │         │                                         │
                      │   ┌─────▼──────────────┐                          │
                      │   │  protocol.cpp      │  line -> Command struct  │
                      │   │  parse + format    │  reply -> line           │
                      │   └─────┬──────────────┘                          │
                      │         │                                         │
                      │  ═══════▼═══════════════════════════════  mutex   │
                      │   ┌────────────────────────────────────┐  ┅┅┅┅┅   │
                      │   │            Store                   │          │
                      │   │                                    │          │
                      │   │  unordered_map<string, list-iter>  │          │
                      │   │              │                     │          │
                      │   │              ▼                     │          │
                      │   │  list<Entry>   MRU ⇄ … ⇄ LRU       │          │
                      │   │                       └─ evict here│          │
                      │   │                                    │          │
                      │   │  mem_used_ / mem_limit_            │          │
                      │   └───────┬────────────────────────────┘          │
                      │           │  (WAL write happens INSIDE the lock)  │
                      │   ┌───────▼────────────┐                          │
                      │   │        Wal         │  append + flush BEFORE   │
                      │   │  append / flush    │  the client sees "OK"    │
                      │   └───────┬────────────┘                          │
                      │  ═════════▼═════════════════════════════          │
                      │           │                                       │
                      │   ┌───────────────┐   sweeps expired keys         │
                      │   │ reaper thread │   once per second             │
                      │   └───────────────┘                               │
                      └───────────┬───────────────────────────────────────┘
                                  │
                          ┌───────▼────────┐
                          │ miniredis.wal  │  append-only, human-readable
                          │   (on disk)    │  replayed on startup
                          └────────────────┘
```

**Layer by layer:**

- **Network layer (`server.cpp`).** Creates a listening TCP socket with `SO_REUSEADDR`, accepts connections in a loop, and hands each one to its own `std::thread`. Installs signal handlers so Ctrl-C shuts the server down cleanly.
- **Framing layer (`protocol.cpp`, `LineBuffer`).** TCP is a byte stream with no message boundaries. Each connection owns a buffer that accumulates raw bytes and yields complete newline-terminated lines. A single `recv()` might contain half a command or three commands; this layer makes that invisible to everything above it.
- **Protocol layer (`protocol.cpp`).** Turns a line into a `Command` struct (verb, key, value, seconds) or an error. Turns the store's answer back into a reply line.
- **Storage layer (`store.h` / `store.cpp`).** The hash map, the LRU list, the TTL deadlines, the memory budget, and the single global mutex that protects all of it. This layer also owns the reaper thread.
- **Persistence layer (`wal.h` / `wal.cpp`).** Formats and appends records, flushes them, and replays the whole file at startup to rebuild the store.

**Data flow for one write:** bytes arrive → `LineBuffer` yields a line → `parse_command` yields a `Command` → `dispatch` calls `Store::set` → `Store` takes the mutex → appends to the WAL → mutates the map and list → evicts if over budget → flushes the WAL → releases the mutex → `dispatch` builds `OK` → `send_all` writes it back.

---

## Key concepts explained

### 1. How multiple clients are handled

The server calls `accept()` in a loop. Each accepted connection gets its own detached `std::thread` running `handle_client`, which loops on `recv()` until the client disconnects. All of those threads share **one** `Store` object.

Sharing mutable state between threads without protection is a data race, so `Store` owns a single `std::mutex`. Every public method takes it on entry and releases it on return. The mutex covers the hash map, the LRU list, the memory counter, **and the write-ahead log**.

Why one global mutex rather than something finer-grained? Because the alternatives cost more than they are worth here. A `std::shared_mutex` sounds ideal until you notice that `GET` is not a read: it splices the entry to the front of the LRU list, which mutates the list. A shared lock on `GET` would be an outright race. Sharding the store across several locks would break the LRU, because recency is inherently global — you would need per-shard LRU lists, which changes what eviction means.

The honest cost: **every command serialises, including the disk flush.** That is the primary throughput bottleneck of this design, and it is documented in [Limitations](#limitations).

### 2. How the WAL works — and why absolute timestamps matter

A write-ahead log is the oldest trick in database durability: **before you change memory, write down what you are about to do.** If the process dies, the log still describes everything that was acknowledged, and replaying it rebuilds the state.

MiniRedis's log is plain text, one record per line, so you can `cat` it:

```
SET 0 user:1 Rahul
SET 0 user:2 Ankit
EXPIRE 1780000123456 user:1
DEL user:2
EVICT session:88
```

Two rules make it work:

**Rule 1 — log and flush before replying.** `Store::set` appends the record and flushes it to the OS before it returns, and the server cannot build a reply until it returns. So it is impossible for a client to receive `OK` for a write that is not already in the log. If the order were reversed, a crash in that gap would lose an acknowledged write — the single failure a WAL exists to prevent.

**Rule 2 — expiry deadlines are stored as ABSOLUTE timestamps.** This is the subtlest part of the whole project.

Suppose the log recorded a duration: `EXPIRE user:1 10s`. Now the server crashes and you restart it three days later. Replay reads that record and grants `user:1` a fresh ten-second lease starting *now* — a key that should have died three days ago is alive again. Worse, it would happen every restart, forever. The log would have stopped being a *record of what happened* and become a *script that re-enacts it at the wrong time*.

So MiniRedis records the absolute instant: `EXPIRE 1780000123456 user:1`, where the number is milliseconds since the Unix epoch. Replay compares that against the current time and simply does not resurrect anything already past its deadline. The log now means the same thing whether it is replayed eleven seconds or eleven days later. Real Redis stores expiries as absolute Unix timestamps for precisely this reason.

This is also why the code uses `std::chrono::system_clock` and not `steady_clock`. `steady_clock` is monotonic and immune to clock adjustments, which makes it the right choice for measuring an elapsed interval — but its epoch is unspecified by the standard and is usually the machine's boot time. A `steady_clock` value written to a file is a tick count from an origin that no longer exists after a restart. `system_clock` is pinned to Unix time, so the same absolute instant is the same number across processes and reboots.

(You can see both clocks used correctly in this codebase: `system_clock` for persisted deadlines in `store.cpp`, `steady_clock` for the benchmark interval in `client.cpp`.)

### 3. How O(1) LRU works — doubly linked list + hash map

When the store hits its memory budget it must throw something out, and the policy is **least recently used**: evict whichever key has gone longest without being touched. The naive implementation stores a "last used" timestamp on each entry and scans everything to find the minimum — O(n) per eviction. The standard solution is O(1), and it works like this.

Keep **two** structures over the same entries:

```
map:   "user:1" ──┐   "user:2" ──┐   "cart:9" ──┐
                  │              │              │
                  ▼              ▼              ▼
list:  [ user:2 ] ⇄ [ cart:9 ] ⇄ [ user:1 ]
         MRU/front                  LRU/back
```

- A **hash map** from key → *an iterator pointing at that key's node in the list*. This gives O(1) lookup, as any hash map does.
- A **doubly linked list** in recency order: front = most recently used, back = least recently used.

**On every access**, the accessed node is moved to the front. Because the list is doubly linked and we already hold an iterator straight to the node, that move is not a search — it is four pointer writes:

```
    unlink:            node->prev->next = node->next
                       node->next->prev = node->prev

    relink at front:   node->next = head
                       head->prev = node
```

Four writes, no traversal, no copying, regardless of whether the list holds ten entries or ten million. That is the O(1).

**On eviction**, take the node at the *back*. By construction that is the least recently used entry. Read its key, erase that key from the map, unlink the node. Also O(1) — and this is why each list node stores a copy of its own key: without it, finding the map entry for the evicted node would need an O(n) scan and the whole design would collapse.

In this codebase the list is a `std::list<Entry>` and the move is `lru_.splice(lru_.begin(), lru_, it)`. The standard guarantees `splice` is constant-time for a single element **and that it invalidates no iterators** — iterators to a moved element stay valid and simply now refer into the destination list. That second guarantee is what the entire design rests on: if `splice` invalidated iterators, every iterator cached in the hash map would dangle after the first `GET`.

Using `std::list` rather than a hand-rolled intrusive list is a deliberate trade: the mechanism is identical (the standard library does the same four pointer writes), and hand-rolling it adds ~80 lines of pointer surgery whose only value is proving it can be done, at the cost of real unlink bugs.

**One ordering rule matters:** an expired key must never be spliced to the front. `Store::get` checks the deadline *before* touching the LRU, so a dead key is erased rather than promoted. Promoting a corpse to most-recently-used would corrupt the eviction order. For the same reason `KEYS` and `DBSIZE` do not touch the LRU at all — a full scan that touched every key would reset all recency at once and destroy the eviction signal entirely.

### 4. How lazy and active TTL expiry work together — and why both

There are two ways to remove an expired key, and MiniRedis does both.

**Lazy expiry.** Every command that looks a key up (`GET`, `DEL`, `EXPIRE`, `TTL`) compares its deadline against the current time before doing anything else. If the deadline has passed, the key is erased on the spot and the command behaves as though it were never there.

**Active expiry.** A background thread wakes once per second, takes the store mutex, walks the list, and erases everything that has expired.

Neither is sufficient alone:

- **Lazy alone leaks.** A key that is written, given a TTL, and then never read again is never examined again — so it is never removed, and its memory is held forever. In a memory-bounded store this is worse than a plain leak: the dead key occupies a slot in the recency list and can cause a **live** key to be evicted in its place.
- **Active alone is not correct.** The sweep runs on a one-second period, so a read arriving between a key's deadline and the next sweep would return data that should already be gone. The lazy check closes that window to zero — a read can never observe an expired key, no matter when the last sweep ran.

Together: **lazy guarantees correctness on the read path; active bounds memory on the never-read path.** Real Redis pairs them for exactly these reasons.

---

## Tech stack

| Component | Technology | Why |
|---|---|---|
| Language | C++17 | Manual control of memory and data-structure layout; `std::list` splice guarantees; structured bindings and `constexpr` statics keep it readable |
| Networking | POSIX BSD sockets (Winsock2 behind a shim) | Direct `socket`/`bind`/`listen`/`accept`; no framework hiding the mechanism |
| Concurrency | `std::thread` + `std::mutex` + `std::condition_variable` | One thread per client, one global lock, a CV so the reaper wakes instantly on shutdown |
| Hash table | `std::unordered_map` | O(1) average lookup — the core of any key-value store |
| Recency order | `std::list` (doubly linked) | O(1) `splice` with guaranteed iterator stability, which is what makes O(1) LRU possible |
| Time | `std::chrono::system_clock` | Absolute Unix-epoch timestamps, the only kind meaningful across a restart |
| Persistence | `std::ofstream` in append mode | Append-only WAL, flushed before every acknowledgement |
| Build | a single `g++` command per binary | No CMake, no Make, no Docker — the build is legible in one line |
| Dependencies | **none** | Standard library only |

---

## Libraries used

Every standard header actually included, what it provides, and where.

| Header | What it provides | Used in |
|---|---|---|
| `<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`, `<netdb.h>`, `<unistd.h>` | POSIX sockets: `socket`, `bind`, `listen`, `accept`, `connect`, `recv`, `send`, `shutdown`, `close`, `getaddrinfo`, `htons`/`htonl` | `net_compat.h` (POSIX branch) |
| `<winsock2.h>`, `<ws2tcpip.h>` | The Winsock2 equivalents plus `WSAStartup`/`closesocket` | `net_compat.h` (Windows branch) |
| `<csignal>` | `signal`, `SIG_IGN`, `sig_atomic_t` — ignoring `SIGPIPE`, catching `SIGINT`/`SIGTERM` | `net_compat.h`, `server.cpp` |
| `<string>` | `std::string` for keys, values, buffers, replies; `std::stoll`/`std::stoi` for parsing | every file |
| `<vector>` | `std::vector<std::string>` for `KEYS` results and replay lines | `store.*`, `wal.cpp`, `server.cpp` |
| `<unordered_map>` | The hash table: key → LRU list iterator | `store.h` |
| `<list>` | The doubly linked recency list and its O(1) `splice` | `store.h`, `store.cpp` |
| `<mutex>` | `std::mutex`, `std::lock_guard`, `std::unique_lock` — the single global lock | `store.h`, `store.cpp` |
| `<condition_variable>` | Lets the reaper sleep for a second but wake instantly on shutdown | `store.h`, `store.cpp` |
| `<atomic>` | `std::atomic<bool> running_`, written by shutdown and read by the reaper | `store.h`, `store.cpp` |
| `<thread>` | `std::thread` for the per-client threads and the reaper | `store.h`, `store.cpp`, `server.cpp` |
| `<chrono>` | `system_clock` for persisted deadlines; `steady_clock` for the benchmark interval | `store.cpp`, `client.cpp` |
| `<fstream>` | `std::ofstream` (append + flush) and `std::ifstream` (replay) | `wal.h`, `wal.cpp` |
| `<iostream>` | Console output, diagnostics, the client REPL | `server.cpp`, `client.cpp`, `wal.cpp` |
| `<algorithm>` | `std::sort` for deterministic `KEYS` output | `store.cpp` |
| `<cctype>` | `std::toupper` for case-insensitive command verbs | `protocol.cpp`, `client.cpp` |
| `<cstdlib>` | `std::exit`, `std::_Exit` | `server.cpp`, `client.cpp` |
| `<cstddef>` | `size_t` | `protocol.h`, `store.h`, `net_compat.h` |

---

## Supported commands

Seven commands. The **semantics** deliberately match real Redis (including the `-1` / `-2` conventions) even though the wire protocol does not.

| Command | Syntax | What it does | Returns |
|---|---|---|---|
| `SET` | `SET <key> <value...>` | Stores a value. The value is the entire rest of the line, so it may contain spaces. Overwrites any existing value and **clears any existing TTL** (as Redis does). | `OK` |
| `GET` | `GET <key>` | Fetches a value. Counts as an LRU access. | the value, or `(nil)` if missing or expired |
| `DEL` | `DEL <key>` | Removes a key. | `1` if removed, `0` if it did not exist |
| `EXPIRE` | `EXPIRE <key> <seconds>` | Sets a time-to-live. A non-positive value **deletes** the key (as Redis does). | `1` if the timeout was set, `0` if the key does not exist |
| `TTL` | `TTL <key>` | Remaining lifetime, rounded to the nearest second. Does **not** count as an LRU access. | seconds remaining · **`-1`** if the key exists but has no expiry · **`-2`** if the key does not exist |
| `KEYS` | `KEYS <pattern>` | Lists matching keys, sorted. `*` matches everything, `prefix*` matches by prefix, anything else is an exact match. O(n). Does **not** touch the LRU. | one line per key, or `(empty)` |
| `DBSIZE` | `DBSIZE` | Number of live keys. Reaps expired keys first, so the count is exact. | an integer |

Errors are returned as `(error) <message>`, e.g. `(error) unknown command 'INCR'`, `(error) wrong number of arguments for 'GET'`, `(error) value is not an integer or out of range`.

`QUIT` and `EXIT` are handled entirely inside `miniredis-cli` and never reach the server — they are not an eighth command.

**Deliberate divergences from Redis**, for the avoidance of doubt: `KEYS` supports only `*` and `prefix*` rather than full globs, and returns sorted output rather than hash order; `DBSIZE` is O(n) here because it reaps before counting, whereas Redis's is O(1) and can transiently over-count; `SET` requires a non-empty value.

---

## Project structure

```
miniredis/
├── net_compat.h     The only file that knows which OS this is. Sockets shim.
├── protocol.h       Command struct, reply grammar, LineBuffer, send_all, recv_line.
├── protocol.cpp     Line parsing and the shared wire helpers. Linked into BOTH binaries.
├── store.h          Entry struct, Store class. Carries the LRU and clock comment blocks.
├── store.cpp        Hash map + LRU + TTL + memory budget + mutex + reaper thread.
├── wal.h            Wal class and the flush-vs-fsync explanation.
├── wal.cpp          Record formatting, torn-line detection, replay.
├── server.cpp       main(), sockets, signals, accept loop, client threads, dispatch.
├── client.cpp       main(), REPL, reply rendering, optional --bench.
├── build.sh         The exact two g++ commands (Linux/macOS).
├── build.bat        The same for MinGW-w64 on Windows.
├── .gitignore       Binaries, objects, *.wal.
├── LICENSE          MIT.
└── README.md        This file.
```

---

## How to build and run

### 9a. GitHub Codespaces (recommended)

**Why this path:** you get a real Linux kernel with real POSIX sockets, working `kill -9`, and multiple terminals, with nothing to install locally.

**Step 1 — create the codespace.** On the repository page click **Code** → **Codespaces** → **Create codespace on main**. It takes about a minute.

> **Create it from a repository in your PERSONAL account.** Free Codespaces usage is included in personal accounts only — organization- and enterprise-owned repos get no free quota.

**Step 2 — verify the toolchain.** In the terminal at the bottom of the editor:

```bash
g++ --version
```

Expected output (version may differ; **anything from g++ 7 upward has full C++17 support**):

```
g++ (Ubuntu 13.3.0-6ubuntu2~24.04) 13.3.0
Copyright (C) 2023 Free Software Foundation, Inc.
This is free software; see the source for copying conditions.
```

No `.devcontainer` is needed — the default Codespaces "universal" image already ships a C++17-capable g++. See the [optional devcontainer note](#optional-pinning-the-toolchain-with-a-devcontainer) below for why one is deliberately *not* included.

**Step 3 — build.**

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pthread -o miniredis-server server.cpp store.cpp wal.cpp protocol.cpp
g++ -std=c++17 -O2 -Wall -Wextra -pthread -o miniredis-cli    client.cpp protocol.cpp
```

Or just `chmod +x build.sh && ./build.sh`. Expected output:

```
built: miniredis-server  miniredis-cli
```

(Warnings are enabled with `-Wall -Wextra`; a clean build prints nothing else.)

**Step 4 — run the server (terminal 1).**

```bash
./miniredis-server
```

```
[wal] no existing log at miniredis.wal -- starting empty
[wal] replayed 0 record(s), skipped 0
miniredis-server listening on 0.0.0.0:6399  wal=miniredis.wal  memory-limit=67108864 bytes
press Ctrl-C to stop
```

**Step 5 — open a second terminal.** In the terminal panel, click the **`+`** icon (or press **Ctrl+Shift+`**). **Both terminals are shells inside the same container**, sharing the same filesystem, the same process table, and the same `127.0.0.1`.

**Step 6 — run the client (terminal 2).**

```bash
./miniredis-cli
```

```
connected to 127.0.0.1:6399
commands: SET GET DEL EXPIRE TTL KEYS DBSIZE   (QUIT to exit)
miniredis>
```

> ### Important: the demo runs entirely inside the codespace
>
> The server and the client are **both inside the container**, and the client connects to `127.0.0.1`. That is by design and it is all you need.
>
> **Do not try to connect from your Windows machine to the forwarded `*.app.github.dev` URL.** Codespaces' default port forwarding is an **HTTPS reverse proxy**. Pointing a raw-TCP client at that URL does not open a TCP connection to this server — it starts an HTTPS handshake with GitHub's proxy. Every documented use of that URL is browser/HTTP framed. This protocol is raw TCP, so it will not work.
>
> *Footnote:* `gh codespace ports forward 6399:6399 -c <name>` is a **different** mechanism — the GitHub CLI opens a genuine local TCP listener on your machine and tunnels it to the container, so raw TCP over it is plausible. It needs `gh` installed and authenticated on Windows. It is **not** required for anything in this README and is untested for this project — use the two-terminal setup above.

**Step 7 — mind your free quota.** Personal accounts get **120 core-hours** and **15 GB-month** of storage. The default 2-core machine burns **2 core-hours per wall-clock hour**, so 120 core-hours is about **60 real hours per month**. A codespace auto-stops after **30 minutes** of inactivity by default, but a long-running server process or terminal output can keep it looking active — **do not rely on the timeout.**

Stop it explicitly when you finish:

```bash
gh codespace stop            # from inside the codespace, or from your machine
```

or in the browser: **github.com/codespaces** → the **`…`** menu next to the codespace → **Stop codespace**.

**Stopped vs deleted, and your WAL:** *stopping* preserves the container disk, so `miniredis.wal` and any uncommitted edits are still there when you restart. *Deleting* destroys that disk and `miniredis.wal` goes with it — which is fine (it is a runtime artifact and is gitignored), but commit your source first, and expect a fresh codespace to start with an empty database. A stopped codespace consumes **storage** quota but no compute.

#### Optional: pinning the toolchain with a devcontainer

This repository deliberately has **no** `.devcontainer/` directory. Reason: GitHub's billing rules say a **custom base image counts against your 15 GB storage quota, while containers based on the default image do not**. Since the default image already has everything this project needs, adding one costs quota and buys nothing.

If you want a pinned toolchain anyway, create `.devcontainer/devcontainer.json`:

```json
{
  "name": "MiniRedis (C++17)",
  "image": "mcr.microsoft.com/devcontainers/cpp:1-ubuntu-24.04",
  "postCreateCommand": "g++ --version"
}
```

...and accept the storage cost.

### 9b. WSL2 (Ubuntu on Windows)

**Install** (PowerShell as Administrator, then reboot):

```powershell
wsl --install -d Ubuntu
```

**Install the toolchain** (inside the Ubuntu shell):

```bash
sudo apt update && sudo apt install -y build-essential
g++ --version
```

**Build and run** — identical to Codespaces:

```bash
git clone https://github.com/<you>/miniredis.git
cd miniredis
./build.sh
./miniredis-server
```

Open a second Ubuntu terminal (Windows Terminal → new tab → Ubuntu) and run `./miniredis-cli`. Everything in the demo below works unchanged, `kill -9` included.

**Note:** clone into the WSL filesystem (`~/miniredis`), not into `/mnt/c/...`. Cross-filesystem I/O through the Windows drive mount is dramatically slower and will distort the benchmark.

### 9c. Native Windows (MinGW-w64)

Supported via the compatibility shim in `net_compat.h`. Install MSYS2 from <https://www.msys2.org>, then in the **MSYS2 MinGW 64-bit** shell:

```bash
pacman -S mingw-w64-x86_64-gcc
```

Build (note `-lws2_32` for Winsock2, and no `-pthread`):

```bat
build.bat
```

which runs:

```bat
g++ -std=c++17 -O2 -Wall -Wextra -o miniredis-server.exe server.cpp store.cpp wal.cpp protocol.cpp -lws2_32
g++ -std=c++17 -O2 -Wall -Wextra -o miniredis-cli.exe    client.cpp protocol.cpp -lws2_32
```

```
built: miniredis-server.exe  miniredis-cli.exe
```

**Differences to expect on Windows:**
- `std::thread` requires a **POSIX-threads** build of MinGW-w64 (MSYS2's `mingw-w64-x86_64-gcc` is one).
- There is no `SIGPIPE`, so the `signal(SIGPIPE, SIG_IGN)` call compiles to nothing.
- `SO_REUSEADDR` has weaker semantics on Windows — it permits binding a port that is *actively* in use, not merely one in `TIME_WAIT`. Fine for a dev tool; worth knowing.
- There is no `kill -9`. The closest equivalent is `taskkill /F /PID <pid>`, which also terminates without cleanup, so the crash-recovery demo still works.

---

## The demo

### Crash recovery — the headline demo

**Terminal 1:**

```bash
$ ./miniredis-server
[wal] no existing log at miniredis.wal -- starting empty
[wal] replayed 0 record(s), skipped 0
miniredis-server listening on 0.0.0.0:6399  wal=miniredis.wal  memory-limit=67108864 bytes
press Ctrl-C to stop
```

**Terminal 2:**

```
$ ./miniredis-cli
connected to 127.0.0.1:6399
commands: SET GET DEL EXPIRE TTL KEYS DBSIZE   (QUIT to exit)

miniredis> SET user:1 Rahul
OK
miniredis> GET user:1
Rahul
miniredis> TTL user:1
-1                       ← key exists, no expiry set
miniredis> EXPIRE user:1 10
1
miniredis> TTL user:1
10

   ... wait 10 seconds ...

miniredis> GET user:1
(nil)
miniredis> TTL user:1
-2                       ← key does not exist

miniredis> SET user:2 Ankit
OK
miniredis> SET greeting hello there, world
OK                       ← values may contain spaces
miniredis> GET greeting
hello there, world
miniredis> DBSIZE
2
```

**Terminal 3 (or terminal 2 after `QUIT`) — inspect the log:**

```bash
$ cat miniredis.wal
SET 0 user:1 Rahul
EXPIRE 1780000123456 user:1
SET 0 user:2 Ankit
SET 0 greeting hello there, world
```

Note `EXPIRE 1780000123456 user:1` — an **absolute** millisecond timestamp, not `10`. Note also that `user:1` expiring is **not** logged: the deadline above already accounts for it, and replay refuses to resurrect anything past its deadline.

**Now kill the server outright:**

```bash
$ pgrep -f miniredis-server
41823
$ kill -9 41823
```

Terminal 1 shows `Killed` — no shutdown message, no handlers, no destructors. Just death.

**Restart:**

```bash
$ ./miniredis-server
[wal] replayed 4 record(s), skipped 0
miniredis-server listening on 0.0.0.0:6399  wal=miniredis.wal  memory-limit=67108864 bytes
press Ctrl-C to stop
```

**Reconnect and check:**

```
$ ./miniredis-cli
miniredis> GET user:2
Ankit                    ← recovered from the WAL
miniredis> GET greeting
hello there, world       ← recovered, spaces intact
miniredis> GET user:1
(nil)                    ← correctly NOT resurrected: its absolute deadline had passed
miniredis> DBSIZE
2
```

That last line is the point of the absolute-timestamp rule. `user:1` was in the log with an `EXPIRE` record, and replay still refused to bring it back, because the deadline it recorded was a moment in the past — not a duration to restart.

### LRU eviction demo

Start a server with a tiny budget so eviction happens after a handful of keys:

```bash
$ ./miniredis-server --port 6399 --memory-limit 400 --wal lru-demo.wal
```

400 bytes ÷ (~96 bytes overhead + key + value) ≈ 3–4 entries.

```
miniredis> SET a 111
OK
miniredis> SET b 222
OK
miniredis> SET c 333
OK
miniredis> GET a
111                      ← 'a' is now the MOST recently used; 'b' is now the LEAST
miniredis> KEYS *
1) a
2) b
3) c
miniredis> SET d 444
OK                       ← over budget: the least recently used key is evicted
miniredis> KEYS *
1) a
2) c
3) d
miniredis> GET b
(nil)                    ← 'b' was evicted, exactly as LRU predicts
```

`b` went, not `a`, even though `a` was inserted first — because the `GET a` promoted `a` to the front of the recency list. That single line is the LRU working.

Eviction is recorded in the log:

```bash
$ cat lru-demo.wal
SET 0 a 111
SET 0 b 222
SET 0 c 333
SET 0 d 444
EVICT b
```

The `EVICT b` record is what makes recovery faithful. Without it, replay would re-apply `SET 0 b 222` and resurrect a key that the running server had already thrown away.

### Multi-client demo

Three terminals. Server in the first, two clients in the others.

**Client A:**
```
miniredis> SET shared "from client A"
OK
```

**Client B (a different terminal, a different TCP connection, a different server thread):**
```
miniredis> GET shared
"from client A"
miniredis> SET shared "now from client B"
OK
```

**Back in client A:**
```
miniredis> GET shared
"now from client B"
miniredis> DBSIZE
1
```

Both clients see one shared `Store`, serialised by one mutex. You can also connect with plain `telnet` to see the raw wire format, tags and all:

```bash
$ telnet 127.0.0.1 6399
SET x hello
OK
GET x
VALUE hello
GET nope
NIL
DBSIZE
INT 1
```

That is what `miniredis-cli` is stripping the tags off. Note that `telnet` sends CRLF and the parser handles it — the trailing `\r` is stripped during framing.

### Throughput

```bash
$ ./miniredis-cli --bench 20000
20000 SET ops in 1.83 s = 10928 ops/sec (single client, flush-on-every-write)
```

One client, one connection, synchronous request/response, and a WAL flush on every single write. This number is dominated by round-trip latency and by the flush inside the store lock — which is precisely the bottleneck described in [Limitations](#limitations). *(Your number will differ; run it and put your own in.)*

---

## How it works — step-by-step flow

Internal walkthroughs, layer by layer. This is the section to read before an interview.

### A `SET user:1 Rahul`

1. **Kernel → `recv()`.** The client thread's `recv()` returns some bytes. There is no guarantee it is exactly one command: it might be `SET user:1 Ra`, or `SET user:1 Rahul\nGET user:1\n`.
2. **Framing.** The bytes are appended to this connection's `LineBuffer`. The drain loop looks for `\n`. If none is present, the loop ends and we go back to `recv()`; the partial command stays buffered. If one is present, the line is popped, a trailing `\r` is stripped, and the remainder stays buffered for the next iteration.
3. **Parse.** `parse_command("SET user:1 Rahul")` trims whitespace, uppercases the first token to `SET`, takes `user:1` as the key, and takes **everything after the next space** as the value — which is why values may contain spaces. Result: a `Command{type=SET, key="user:1", value="Rahul", ok=true}`.
4. **Dispatch.** `dispatch()` switches on the type and calls `store.set("user:1", "Rahul")`.
5. **Lock.** `Store::set` takes the single global `std::mutex`. From here until it returns, no other client thread and not the reaper can touch the map, the list, the memory counter, or the log.
6. **Log.** `wal_.append_set(key, value, 0)` writes `SET 0 user:1 Rahul\n` into the `ofstream` buffer. The `0` is the absolute expiry deadline; zero means none. **`SET` clears any existing TTL**, mirroring Redis.
7. **Apply.** `insert_or_update_locked` looks the key up. If it exists: subtract its old cost from `mem_used_`, overwrite the value and deadline, add the new cost, and `splice` the node to the front. If it does not: `push_front` a new `Entry`, store the resulting iterator in the map, and add its cost.
8. **Evict.** `evict_if_needed_locked(true)` loops while `mem_used_ > mem_limit_` and more than one entry remains. Each pass takes `lru_.back()` — the least recently used entry by construction — writes an `EVICT <key>` record, erases it from map and list, and subtracts its cost.
9. **Flush.** One `wal_.flush()` covers the `SET` record and every `EVICT` record. This issues `write(2)`, so the bytes are now in the OS page cache and survive this process dying.
10. **Unlock.** The `lock_guard` goes out of scope.
11. **Reply.** Only now does `dispatch()` return `"OK\n"`, and `send_all()` loops until the kernel has accepted every byte. **The client cannot see `OK` before step 9 happened** — that is the whole ordering rule, enforced structurally rather than by convention.

### An `EXPIRE user:1 10`

1. Framing and parsing as above; `stoll` parses `10` and verifies it consumed the *entire* token (so `10abc` is rejected rather than silently read as `10`).
2. `Store::expire` takes the lock and looks the key up. Missing → return `0`. Already past its deadline → erase it and return `0`.
3. If `seconds <= 0`, Redis semantics apply: the key is **deleted**, a `DEL` record is logged, and `1` is returned.
4. Otherwise the **absolute deadline is computed here, once**: `at = now_ms() + seconds * 1000`. The relative `10` the client sent never reaches the log.
5. `EXPIRE <at> user:1` is appended. **This record is essential.** Without it, a key that received its TTL after its `SET` would replay with the `SET` record's deadline of `0` and become immortal.
6. `it->expire_at_ms = at`, and the entry is spliced to the front — an explicit operation on a key counts as a use.
7. Flush, unlock, reply `INT 1` → the client prints `1`.

### A `GET` on an expired key

1. `Store::get` takes the lock and finds the key in the map. It is still physically present — the reaper may not have run since it expired, and it may never have run for this key.
2. **`expired_locked(it)` is checked first, before anything else.** `it->expire_at_ms != 0 && it->expire_at_ms <= now_ms()` → true.
3. `erase_locked(it)`: subtract its cost from `mem_used_`, erase from `map_`, erase from `lru_`. **No `splice` happens.** Touching the LRU here would promote a dead key to most-recently-used and cause a live key to be evicted in its place later.
4. **No WAL record is written.** The expiry is already implied by the absolute deadline in the log, and replay refuses to resurrect anything past its deadline. Logging it would be redundant bytes in a log that already grows forever.
5. Return `false` → `dispatch` returns `NIL` → the client prints `(nil)`.

The key was logically dead the instant its deadline passed. Whether the reaper had gotten to it yet is invisible to the client — that is exactly what lazy expiry buys.

### Startup replay

1. `main` parses arguments and constructs `Wal` and `Store`. **No socket exists yet** — nothing can connect until recovery finishes.
2. `wal.replay(store)`. First it checks whether the file's **last byte is `\n`**. Then it reads every line with `getline`.
3. **Torn final record.** If the file did not end in `\n`, the last line is a fragment from a `kill -9` that landed mid-write. `getline` returns it as though it were real, so it is detected by that last-byte check, logged to stderr, and **discarded**. This is not data loss: a record that was never fully written was never flushed, so its `OK` was never sent to any client. What *would* be a bug is refusing to start over a ragged tail — a recovery path defeated by an ordinary crash is not a recovery path.
4. **Record by record.** `SET` → `replay_set` with the absolute deadline **read from the log, not recomputed**. `EXPIRE` → `replay_expire` updates the deadline on an existing key. `DEL` and `EVICT` → `replay_del` removes it. A record that fails to parse is logged and skipped; **startup never aborts on a bad line.**
5. `wal.open_for_append()` — *now* the file is opened for writing, once, in append mode, and held for the process lifetime. Opening only after replay makes it structurally impossible to append to a file we are still reading.
6. `store.finish_replay()`:
   - **Sweep expired.** Every key whose absolute deadline is already in the past is dropped. **This is the payoff for absolute timestamps:** a log written three days ago replays to exactly the state it described, not to a state where every TTL restarts from now.
   - **Trim to the memory limit**, logging each eviction. Because the log is open by this point, memory and log agree from the very first byte of uptime.
7. `store.start_reaper()` launches the background sweeper.
8. Only now: `socket()`, `SO_REUSEADDR`, `bind()`, `listen()`, signal handlers, and the accept loop. **No client can observe a partially recovered store.**

---

## Limitations

Honest list. Each of these is a deliberate scope decision, not an oversight.

- **Single global mutex.** Every command — reads included — serialises on one lock, and the WAL flush happens *inside* that lock, so all writers queue behind disk I/O. This is the primary throughput ceiling. It is the correct trade for this size, because the alternative (WAL writes outside the lock) allows the log's order and memory's order to diverge, which would make the log lie about the state.
- **`GET` is not a reader.** `GET` splices the entry to the front of the LRU list, mutating it. That is why a `std::shared_mutex` would be wrong here, not merely suboptimal.
- **Thread-per-connection does not scale.** Each connection costs a thread stack plus a kernel task struct. Fine into the low thousands; falls apart well before 10,000 connections. The production answer is an `epoll`/`kqueue`/IOCP event loop. Real Redis goes further and executes commands on a *single* thread, which is why it has no locks at all.
- **No RESP, no `redis-cli` compatibility.** The wire protocol is a custom human-readable line protocol. `redis-cli` cannot talk to this server, and it cannot talk to a real Redis.
- **No WAL compaction. The log grows forever.** Every `SET`, `DEL`, `EXPIRE` and `EVICT` is appended and nothing is ever rewritten. A key set a million times occupies a million log lines, and startup replay time grows linearly with total historical writes, not with live data size. Real Redis solves this with AOF rewriting and RDB snapshots.
- **Memory accounting is approximate.** `key.size() + value.size() + 96` is a *model*, not a measurement. It ignores string SSO, `unordered_map` bucket arrays, allocator block headers, and heap fragmentation. It is a budget that behaves sensibly and monotonically, not a reflection of real RSS.
- **Each key is stored twice** — once as the map key, once inside the list node — because eviction starts from the list and must reach the map. This is what buys O(1) eviction, but it is real memory spent.
- **Durability is flush, not fsync.** Writes reach the OS page cache before the client is acknowledged, so they survive process death (`kill -9`, segfault, SIGKILL). They do **not** survive machine power loss, which needs `fsync`. `fsync` is also unreachable from a `std::ofstream` in standard C++ without dropping to POSIX file descriptors, which would defeat the single-compatibility-header design.
- **Replay reproduces the key set exactly, but not the LRU ordering.** Reads are not logged (that would double the log volume for a store built for fast reads), so recency cannot be reconstructed. After a restart the recency order is WAL order — insertion order — so the first post-restart eviction may choose a different victim than the pre-crash server would have. Eviction is a memory-pressure heuristic, not a correctness property, so this is an acceptable trade; it is listed here because it is real.
- **Replay is not memory-bounded while it runs.** The full live set of the log is built in memory and only then trimmed to the limit, so replaying an enormous log can transiently exceed `--memory-limit`.
- **A WAL write failure leaves memory ahead of the log.** The client gets `ERR wal write failed`, but the in-memory mutation has already happened. Doing this properly needs an undo path; for a project this size, saying so is the right answer.
- **`KEYS` is O(n)** and holds the global lock for the whole scan. On a large store that is a latency spike for every other client. Real Redis has the identical problem and recommends `SCAN` instead.
- **`DBSIZE` is O(n)** because it reaps expired keys before counting so the answer is exact. Redis's is O(1) and can transiently over-count.
- **Strings only.** No lists, hashes, sets, or sorted sets.
- **No authentication, no TLS.** Anyone who can reach the port has full access. Do not run this on a public network.
- **No replication, no clustering, no persistence snapshots, no pub/sub, no transactions.**
- **Wall-clock dependence.** Expiry deadlines are `system_clock` values, so an NTP step or a manual clock change moves every TTL. Real Redis has exactly the same exposure and documents the same caveat.
- **`SET` requires a non-empty value**, so empty-string values are not representable. A trailing space in a value is also trimmed by the parser.
- **Line protocol limits.** Keys cannot contain spaces; values cannot contain newlines; a single line is capped at 64 KiB.

---

## Interview notes

Twelve questions this project invites, with tight answers.

**1. Why absolute timestamps in the WAL rather than durations?**
Because a duration in a log is a *recipe*, not a *record*. Replaying `EXPIRE k 10s` from a three-day-old log grants `k` a fresh ten-second lease starting now — a key that should have died three days ago comes back, and it would happen on every restart forever. An absolute deadline means the same instant regardless of when it is replayed, so replay simply drops anything already past due. Real Redis stores expiries as absolute Unix timestamps for the same reason.

**2. Why `system_clock` and not `steady_clock`?**
`steady_clock` is monotonic and immune to clock adjustments, which makes it right for measuring an elapsed interval — but the standard leaves its epoch unspecified, and in practice it is usually the machine's boot time. A `steady_clock` value written to a file is a tick count from an origin that no longer exists after a restart. `system_clock` is pinned to Unix time, so the same instant is the same number across processes and reboots. Both clocks are in this codebase: `system_clock` for persisted deadlines, `steady_clock` for the benchmark interval.

**3. Why is your LRU O(1)?**
Two structures over the same entries: a hash map from key to an *iterator into a doubly linked list*, and the list itself in recency order. A touch is not a search — I already hold an iterator to the node, so promoting it to the front is four pointer writes (unlink from two neighbours, relink at the head), independent of list length. Eviction takes the node at the back, which is by construction the least recently used. This works only because `std::list::splice` is specified to be constant-time and to invalidate no iterators; if it invalidated them, every iterator cached in the map would dangle after the first `GET`.

**4. Why both lazy and active expiry? Isn't one enough?**
Neither is sufficient alone. Lazy alone leaks: a key that is written, given a TTL, and never read again is never examined again, so its memory is held forever — and in a memory-bounded store that dead key can cause a *live* key to be evicted in its place. Active alone is not correct: the sweep is periodic, so a read landing between a deadline and the next sweep would return data that should already be gone. Lazy gives correctness on the read path; active bounds memory on the never-read path.

**5. What happens if the process dies mid-WAL-write?**
The last line in the file is a fragment with no terminating newline. On startup I check whether the file's last byte is `\n`; if not, the final line is discarded and replay continues normally. This loses nothing, because a record that was never fully written was never flushed, so its `OK` was never sent to a client. The failure mode I care about more is a recovery path that *refuses to start* over a ragged tail — a recovery path defeated by an ordinary crash is not a recovery path.

**6. Why one global mutex? Isn't that a bottleneck?**
Yes, and it is the documented ceiling. But the finer-grained alternatives cost more than they buy here. `shared_mutex` is not merely suboptimal, it is *wrong*: `GET` splices the entry to the front of the LRU list, so a read mutates the data structure and a shared lock would be a race. Sharding by key hash breaks the LRU, because recency is inherently global — you would need per-shard LRU lists, which changes what eviction means. One lock, correct, documented.

**7. Is the WAL write inside or outside the lock, and why does it matter?**
Inside. If it were outside, two concurrent `SET`s on the same key could commit to memory in one order and to the log in the opposite order, and replay would rebuild a *different* final value than the running server had — the log would be a lie. Holding one lock across both the append and the memory mutation makes log order and apply order the same total order by construction. The price is that flush latency sits in the critical section, so all writers serialise on disk. That is the right thing to trade for a log that cannot disagree with memory.

**8. Do you log evictions? Why?**
Yes, as an explicit `EVICT` record. The tempting answer is no — eviction is derived, so replay could just evict again. But it cannot: eviction order depends on recency, recency depends on `GET`s, and `GET`s are not logged (logging reads would double the log volume for a store built for fast reads). Without explicit records, replay would evict by insertion order instead, resurrecting evicted keys and dropping surviving ones. One extra line per eviction buys "recovered state equals pre-crash state". What I still cannot reproduce is the LRU *ordering* after replay — that is a documented limitation.

**9. What breaks at 10,000 connections?**
Thread-per-connection does. Each connection costs a thread stack plus a kernel task struct, and the scheduler starts thrashing long before 10k. Then the single mutex becomes the wall — every command serialises, including the disk flush. The fix is an event loop over `epoll`, where one thread multiplexes thousands of sockets without blocking. That is the change I would make first, and it would also remove the need for most of the locking.

**10. How does real Redis differ?**
Three big ways. **Concurrency:** Redis executes commands on a *single* thread and gets its concurrency from `epoll` multiplexing — no locks, no cache-line contention, no races, because only one thing ever touches the data. Redis 6 added I/O threads for socket reads and writes, but command execution stayed single-threaded. **Persistence:** Redis has AOF *with rewriting* plus RDB snapshots; mine has an append-only log that grows forever. **Everything else:** RESP wire protocol, many data types, replication, clustering, `SCAN`, multiple eviction policies. I matched Redis on *semantics* — the `-1`/`-2` `TTL` returns, `SET` clearing the TTL, non-positive `EXPIRE` deleting the key — and deliberately diverged on the wire protocol.

**11. `flush()` versus `fsync()` — what does your durability actually guarantee?**
`flush()` pushes the stream buffer out via `write(2)`, so the data is in the OS page cache. That survives *this process* dying — `kill -9`, segfault, SIGKILL — which is exactly the demo. It does **not** survive the machine losing power; that needs `fsync`, which blocks until the storage device acknowledges. I chose flush-only for two reasons: it matches the threat model precisely, and `fsync` is unreachable from a `std::ofstream` in standard C++ — getting it would mean POSIX `open`/`write`/`fsync` and platform `#ifdef`s spreading through the persistence layer.

**12. Your protocol is newline-delimited but TCP is a byte stream. How do you frame messages?**
Each connection owns a `LineBuffer`. `recv()` appends whatever it returns, then a drain loop extracts every *complete* newline-terminated line and leaves any partial tail buffered for the next `recv()`. Nowhere does the code assume one `recv()` equals one command — a single `recv()` can legitimately deliver half a command, or three commands, or one byte. That assumption works perfectly in local testing and fails the first time a packet splits. There is also a 64 KiB cap so a client streaming a line with no newline cannot drive the server out of memory.

---

## License

MIT — see [LICENSE](LICENSE).
````

---

# 6. RUNBOOK — do these in order

You run these. **The coding agent must not execute any of them.**

### Part A — create the repository

1. Go to <https://github.com/new>.
2. Owner: **your personal account** (not an organization — the free Codespaces quota does not apply to org-owned repos).
3. Repository name: `miniredis`. Description: *A Redis-like in-memory key-value store in C++17: TCP server, O(1) LRU, TTL expiry, and crash recovery from a write-ahead log.*
4. **Public** (this is a portfolio piece).
5. Do **not** add a README, `.gitignore`, or license — all three are in the file set.
6. Click **Create repository**.

### Part B — get the files in

Hand this markdown file to your coding agent and have it create all 13 files at the repository root. Then, from a terminal on your Windows machine with Git installed:

```bash
cd path\to\miniredis
git init
git add .
git commit -m "MiniRedis: C++17 key-value store with LRU, TTL, and WAL crash recovery"
git branch -M main
git remote add origin https://github.com/<your-username>/miniredis.git
git push -u origin main
```

If you would rather not install Git locally: create the codespace first (Part C), have the agent write the files, then commit from inside the codespace with the same commands.

### Part C — open a codespace

7. On the repository page: **Code** → **Codespaces** tab → **Create codespace on main**. Wait ~1 minute.
8. Confirm the toolchain:
   ```bash
   g++ --version
   ```
   Anything from g++ 7 upward is fine. Note the version — the README tells readers to check it too.

### Part D — build

9. Make the script executable and run it:
   ```bash
   chmod +x build.sh
   ./build.sh
   ```
   Expected: `built: miniredis-server  miniredis-cli`

   Or paste the two commands directly:
   ```bash
   g++ -std=c++17 -O2 -Wall -Wextra -pthread -o miniredis-server server.cpp store.cpp wal.cpp protocol.cpp
   g++ -std=c++17 -O2 -Wall -Wextra -pthread -o miniredis-cli    client.cpp protocol.cpp
   ```

10. If a warning appears, fix it before going further — `-Wall -Wextra` is on deliberately and a clean build should be silent.

### Part E — run the demo

11. **Terminal 1** — start the server:
    ```bash
    ./miniredis-server
    ```

12. **Open terminal 2**: click the **`+`** in the terminal panel, or press **Ctrl+Shift+`**. Both terminals are shells in the same container.

13. **Terminal 2** — start the client:
    ```bash
    ./miniredis-cli
    ```

14. Type the demo script:
    ```
    SET user:1 Rahul
    GET user:1
    TTL user:1
    EXPIRE user:1 10
    TTL user:1
    ```
    Wait ten seconds, then:
    ```
    GET user:1
    TTL user:1
    SET user:2 Ankit
    SET greeting hello there, world
    GET greeting
    DBSIZE
    QUIT
    ```

15. **Verify the WAL** (terminal 2, now back at a shell):
    ```bash
    cat miniredis.wal
    ```
    Confirm the `EXPIRE` line carries a **13-digit absolute millisecond timestamp**, not `10`. That single line is the headline talking point — point at it in the demo.

16. **Crash it:**
    ```bash
    pgrep -f miniredis-server
    kill -9 <the pid it printed>
    ```
    Terminal 1 prints `Killed` and nothing else.

17. **Restart and verify recovery** (terminal 1):
    ```bash
    ./miniredis-server
    ```
    Look for `[wal] replayed N record(s), skipped 0`. Then in terminal 2:
    ```bash
    ./miniredis-cli
    ```
    ```
    GET user:2      → Ankit                ← recovered
    GET greeting    → hello there, world   ← recovered, spaces intact
    GET user:1      → (nil)                ← correctly NOT resurrected
    ```

### Part F — the other two demos

18. **LRU.** Stop the server (Ctrl-C in terminal 1), then:
    ```bash
    ./miniredis-server --memory-limit 400 --wal lru-demo.wal
    ```
    In terminal 2, run the LRU sequence from the README §10 and confirm `b` is evicted and not `a`. Then `cat lru-demo.wal` and point at the `EVICT b` record.

19. **Multi-client.** Open a third terminal, run a second `./miniredis-cli`, and confirm both clients see the same data.

20. **Throughput.** With the server running:
    ```bash
    ./miniredis-cli --bench 20000
    ```
    Put your actual number into the README's Throughput section.

### Part G — commit and stop the codespace

21. Commit anything you changed:
    ```bash
    git add -A
    git commit -m "verified demo; recorded benchmark number"
    git push
    ```

22. **Stop the codespace so you stop burning quota.** Either:
    ```bash
    gh codespace stop
    ```
    or in the browser: **github.com/codespaces** → the **`…`** menu next to the codespace → **Stop codespace**.

    **Do not just close the browser tab.** The default idle timeout is 30 minutes, but a running server process or terminal output can keep the codespace looking active, and you are billed for the entire time it is active whether or not you are using it. On a 2-core machine you have roughly 60 real hours per month.

    **Stopped ≠ deleted.** A stopped codespace keeps its disk (including `miniredis.wal`) and consumes storage quota but no compute. Deleting it destroys the disk — commit first. If you want the codespace gone, delete it from the same menu, but only after pushing.

### Part H — polish for the recruiter

23. On the repository page click the gear next to **About** and add topics: `cpp`, `cpp17`, `redis`, `key-value-store`, `lru-cache`, `write-ahead-log`, `tcp-server`, `systems-programming`.
24. Set the About description to the one-liner from Part A step 3.
25. Optional but effective: record a 60-second terminal GIF of the `kill -9` → restart → `GET user:2` sequence and embed it under the README's **The demo** heading. That single loop is the most persuasive thing in the repository.
