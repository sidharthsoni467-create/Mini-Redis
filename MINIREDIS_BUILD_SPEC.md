# MiniRedis — Research Findings, Decisions, and Full Build Spec

**Target reader:** an AI coding agent (Antigravity) that will materialise this repository verbatim.
**Author's environment:** Windows host, no WSL, no Linux, no macOS.
**Deliverable:** two C++17 binaries (`miniredis-server`, `miniredis-cli`), standard library only, plus a GitHub-ready README.

This document is a directive build spec. Sections 1–3 are research and decisions. Section 4 is the file structure. Section 5 prints every file in full. Section 6 is the ordered operator runbook.

**Agent instructions:** create every file in Section 5 exactly as printed, at the exact path given in its heading. Do not add files, do not add commands, do not add dependencies. Do not "improve" the scope. Everything in Section 5 is complete and compiles as printed.

---

# 1. RESEARCH FINDINGS

## 1.1 Platform / environment

### 1.1.1 GitHub Codespaces — free tier, verified

| Item | Verified value |
|---|---|
| Free compute, personal account | **120 core-hours / month** |
| Effective wall-clock time on the default 2-core machine | **~60 hours / month** (2 core-hours burned per wall-clock hour) |
| Free storage, personal account | **15 GB-month** |
| Applies to organization-owned repos? | **No** |
| Default idle timeout | **30 minutes**, configurable 5–240 min |
| Stopped codespace | Consumes **storage only**, not compute |

GitHub's own Codespaces page states the allocation directly: <cite index="4-1">users on the free plan get 120 core hours, or 60 hours of runtime on a 2-core codespace, plus 15 GB of storage each month</cite>. The core-hour arithmetic is confirmed independently: <cite index="3-1">for a personal GitHub Free account the included usage is 120 core-hours per month, and on a 2-core machine one hour of runtime consumes two core-hours, so that is roughly 60 real hours</cite>.

The organization exclusion is explicit in GitHub's billing docs: <cite index="5-1">GitHub plans for organizations and enterprises do not include a free quota for GitHub Codespaces; all GitHub personal accounts include a quota of free compute time and storage</cite>. The Enterprise Cloud docs repeat it and add the exhaustion behaviour: <cite index="6-1">free use of Codespaces is included in personal accounts only, not organization or enterprise accounts; compute and storage usage is deducted from the free quota until either is consumed, at which point Codespaces use is restricted unless a spending limit and payment method are configured</cite>.

**→ Action for this project: create the repo under your personal account, not an org.**

**Idle timeout.** GitHub's docs: <cite index="14-1">a codespace stops running after a period of inactivity, 30 minutes by default, changeable in personal settings; the new setting applies to codespaces created afterwards</cite>, and <cite index="13-1">the value must be between 5 and 240 minutes</cite>. Note the billing warning attached to that same page: <cite index="13-1">compute is billed for the whole time a codespace is active, whether or not you are using it</cite>, and <cite index="13-1">inactivity means the absence of activity indicative of a user's presence — typing or using the mouse resets the timer</cite>.

**This matters specifically for MiniRedis.** You will have a server process running in a terminal for the whole demo. Community guidance warns that <cite index="20-1">if something keeps the codespace active — terminal output, a running process, or server traffic — it can continue consuming compute time, so the best practice is to stop the codespace manually when you are done; stopped codespaces do not consume compute but still consume storage until deleted</cite>. Sources disagree slightly on whether a background process alone defeats the idle timer (GitHub's formal definition is user-presence-based; the community discussion says server traffic can keep it alive). **Pick the safe side: stop the codespace explicitly with `gh codespace stop` or the web UI when you finish. Do not rely on the timeout.**

**Stopped vs deleted, and your WAL file.** Stopping preserves the container disk — your `miniredis.wal` and any uncommitted source edits survive a stop and are there when you restart. **Deleting the codespace destroys that disk, and `miniredis.wal` goes with it.** This is fine (the WAL is a runtime artifact and is gitignored), but it means: commit your source before deleting, and expect a fresh codespace to start with no WAL, i.e. an empty database. Note also that the default retention period is configurable and <cite index="12-1">setting the retention period to 0 days results in immediate deletion of codespaces when they stop or time out</cite> — leave that at a non-zero value.

### 1.1.2 Codespaces port forwarding — the correctness point

GitHub's docs describe forwarding as: <cite index="39-1">port forwarding gives you access to TCP ports running within your codespace; if you are running a web application on a port you can forward it and access the application from the browser on your local machine, and when an application prints a localhost URL to the terminal the port is automatically forwarded and the URL becomes a clickable link</cite>.

Two distinct mechanisms hide behind the phrase "port forwarding", and the distinction is the whole point:

1. **The `*.app.github.dev` forwarded URL.** This is an HTTPS reverse proxy in front of your container port. It speaks HTTP. Pointing a raw-TCP client at it does not produce a TCP connection to your server — it produces an HTTPS handshake against GitHub's proxy. **`miniredis-cli` running on your Windows machine cannot use this URL.** Every GitHub-documented use of the forwarded URL is browser/HTTP framed.
2. **`gh codespace ports forward <remote>:<local>`.** This is different: the GitHub CLI opens a **real local TCP listener** on your Windows machine and tunnels it to the container. That it is genuinely raw TCP is visible in the CLI's own error text when the local port is busy — a reported failure reads `failed to listen to local port over tcp: listen tcp :3001: bind: address already in use` <cite index="38-1">(gh codespace ports forward 3001:3001 failing with "failed to listen to local port over tcp: listen tcp :3001: bind: address already in use")</cite>. The command's documented form is <cite index="43-1">`gh codespace ports forward <remote-port>:<local-port>... [flags]`</cite> and GitHub's own example is <cite index="42-1">`gh codespace ports forward 8000:8000 -c <codespace-name>`</cite>.

**Verdict:** the HTTP forwarded URL will not carry this protocol; the `gh` CLI tunnel plausibly will, because it is a real TCP listener. But it requires `gh` installed and authenticated on Windows, and it is not needed. **Run the whole demo inside the codespace, server and client both on `127.0.0.1`.** The README will say this explicitly and will mention the `gh` tunnel only as a footnote with the caveat that it is untested for this project.

### 1.1.3 Two terminals, `kill -9`, threads, file writes

All confirmed working, all standard container behaviour:

- **Two terminals in one codespace:** yes. VS Code's terminal panel supports arbitrarily many shells (`Ctrl+Shift+\`` or the `+` in the terminal panel) and they are all shells in the same container, sharing the same filesystem, the same `127.0.0.1`, and the same process table. This is the correct and intended way to run the server in one and the client in the other.
- **`kill -9`:** yes. A codespace is a Linux container with a normal PID namespace. `kill -9 <pid>` delivers SIGKILL and the process dies immediately without running handlers or destructors — exactly the crash you want to demo.
- **Background threads (`std::thread`, pthreads):** yes, normal.
- **File writes:** yes, normal ext4-backed writes to the workspace volume.

### 1.1.4 Do you need a `.devcontainer/devcontainer.json`?

**No — and there is a concrete billing reason not to add one.**

With no devcontainer config, Codespaces boots the **universal image**: <cite index="23-1">the large image published as `mcr.microsoft.com/devcontainers/universal:linux` includes runtime versions for popular languages including Python, Node, PHP, Java, Go, **C++**, Ruby and .NET, and it is the image Codespaces uses by default when no custom Dockerfile or image is specified</cite>. Current universal images are pinned to <cite index="21-1">`mcr.microsoft.com/devcontainers/universal:5`</cite>, and the v5.x line is <cite index="32-1">based on Ubuntu 24.04</cite> (Noble), whose default toolchain g++ is in the 13.x series. **Any g++ from 7 onward supports `-std=c++17` fully**, so this is comfortably satisfied — but the README instructs you to run `g++ --version` as step one and check, rather than trusting a number in a document.

The billing reason: GitHub's billing docs state that <cite index="5-1">with a custom base image the storage volume for your codespace includes the custom dev container in addition to all the files in the repository and the codespace, whereas containers based on the default image are not included in your storage volume, even if you add features via devcontainer.json</cite>. Adding a custom `image:` (e.g. `devcontainers/cpp`) therefore **starts charging your 15 GB quota for the image itself**. For a project whose only toolchain requirement is `g++`, that is a pure loss.

**Decision: no `.devcontainer/` directory in the repo.** The README includes the JSON as a copy-pasteable block for anyone who wants pinned reproducibility, clearly marked as optional and flagged with the storage cost.

## 1.2 Sockets and portability

### 1.2.1 POSIX vs Winsock2 — the differences that actually matter here

| Concern | POSIX | Winsock2 |
|---|---|---|
| Library init | none | `WSAStartup(MAKEWORD(2,2), &wsa)` required before any socket call |
| Handle type | `int` (an fd) | `SOCKET` (an opaque `UINT_PTR`) |
| Invalid handle | `-1` | `INVALID_SOCKET` (**not** `-1` — never compare `< 0`) |
| Close | `close(fd)` | `closesocket(s)` |
| Headers | `<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`, `<unistd.h>` | `<winsock2.h>`, `<ws2tcpip.h>`, link `-lws2_32` |
| `setsockopt` value arg | `const void*` | `const char*` |
| `recv`/`send` length | `size_t` / returns `ssize_t` | `int` / returns `int` |
| `SIGPIPE` on write-to-dead-peer | **process is killed by default** | no such signal |
| `SO_REUSEADDR` semantics | lets you rebind a port stuck in `TIME_WAIT` | **different and dangerous** — allows a second process to bind a port already actively bound |
| `SHUT_RDWR` | `SHUT_RDWR` | `SD_BOTH` |

The `SO_REUSEADDR` divergence is real and worth knowing: <cite index="96-1">when using SO_REUSEADDR on Windows the OS will simply allow multiple programs to bind to the same port, and an incoming connection is routed to only one of them; there is very little downside on Linux, so most server applications should use it there</cite>. For a dev-only tool this is acceptable, but it must be commented, not hidden.

### 1.2.2 The footguns, and how this project handles each

**`SO_REUSEADDR` and `TIME_WAIT`.** After a server closes, the kernel keeps the socket in `TIME_WAIT` and a fresh `bind()` on the same port fails with `EADDRINUSE`. As the classic reference puts it: <cite index="88-1">you can use setsockopt() to set SO_REUSEADDR, which explicitly allows a process to bind to a port that remains in TIME_WAIT (it still only allows a single process to be bound to that port)</cite>. Practically: <cite index="94-1">SO_REUSEADDR lets a new socket bind to a port previously held by a socket now in TIME_WAIT, bypassing a delay that typically lasts one to four minutes</cite>. Since the demo restarts the server repeatedly, **`SO_REUSEADDR` is mandatory here, set with `setsockopt` immediately after `socket()` and before `bind()`**.

**`SIGPIPE`.** Confirmed: on a stream socket whose peer has gone away, `send()` raises `SIGPIPE`, whose default disposition terminates the process. POSIX is explicit: <cite index="80-1">EPIPE — the socket is shut down for writing, or is connection-mode and no longer connected; in the latter case, for SOCK_STREAM or SOCK_SEQPACKET and if MSG_NOSIGNAL is not set, the SIGPIPE signal is generated to the calling thread</cite>. The Linux `send(2)` flag description: <cite index="78-1">MSG_NOSIGNAL (since Linux 2.2) — don't generate a SIGPIPE signal if the peer on a stream-oriented socket has closed the connection; the EPIPE error is still returned; this provides similar behavior to using sigaction(2) to ignore SIGPIPE, but whereas MSG_NOSIGNAL is a per-call feature, ignoring SIGPIPE sets a process attribute that affects all threads</cite>.

MSG_NOSIGNAL is not universally available (macOS historically lacked it and uses the `SO_NOSIGPIPE` socket option instead — <cite index="86-1">std.socket assumed MSG_NOSIGNAL existed on all platforms, which is not the case on OS X, causing send() to raise SIGPIPE when the peer had closed; the fix was to set SO_NOSIGPIPE on systems that support it</cite>). **This project does both, belt and braces: `signal(SIGPIPE, SIG_IGN)` once at startup (process-wide, one line, covers every thread), plus `MSG_NOSIGNAL` on every `send()` where the macro exists.** The compatibility header defines `MSG_NOSIGNAL` to `0` where it does not exist, so the call site never needs an `#ifdef`.

**Partial reads and partial writes.** `send()` may accept fewer bytes than offered; `recv()` may return fewer than the buffer holds. `send_all()` loops until every byte is accepted. On the read side there is no "read exactly N" requirement because framing is newline-based — the loop just keeps appending.

**Message framing.** TCP is a byte stream with no record boundaries. One `recv()` may deliver half a command, one and a half commands, or three commands. **Every connection owns a `LineBuffer`.** `recv()` appends raw bytes into it; a drain loop then extracts every *complete* `\n`-terminated line and leaves any partial tail in the buffer for the next `recv()`. `recv() == 1 command` is never assumed anywhere in this codebase.

**Trailing `\r`.** `telnet` and PuTTY send CRLF. `LineBuffer::next_line()` strips a trailing `\r` after splitting on `\n`, so the parser sees identical input from `miniredis-cli`, `telnet`, and `nc`.

**Clean shutdown.** `SIGINT`/`SIGTERM` handlers set a `volatile sig_atomic_t` flag and call `shutdown()` on the listening socket, which wakes the blocked `accept()`. The accept loop sees the flag and exits. The TTL reaper thread is woken immediately by a condition variable (not left sleeping for up to a second) and is **joined**. Client threads are **detached** and are not joined — see §2.6 for why that is safe here.

## 1.3 Storage: hash map + O(1) LRU

### 1.3.1 The `std::list` + `unordered_map` pattern, verified

Both properties the pattern depends on are guaranteed by the standard, per cppreference on `std::list::splice`:

- **Iterator stability:** <cite index="49-1">no elements are copied or moved, only the internal pointers of the list nodes are re-pointed; no iterators or references become invalidated, and iterators to moved elements remain valid</cite>.
- **Complexity:** for the single-element overload `splice(pos, other, it)`, <cite index="57-1">complexity is constant</cite>.

This is not incidental — it was fought for. LWG issue 250 records that the original wording invalidated iterators and that <cite index="51-1">this is unnecessary and defeats an important feature of splice; the resolution states that iterators referring to the moved elements continue to refer to their elements, but now behave as iterators into *this</cite>.

**This guarantee is the entire reason the pattern works.** The hash map stores a `std::list<Entry>::iterator`. Every `GET` splices that node to the front of the list. If splice invalidated iterators, every stored iterator in the map would rot on the first touch and the design would collapse. This gets a comment block in `store.h` and its own README section.

### 1.3.2 `std::list` vs a hand-rolled intrusive doubly linked list

| | `std::list` + `unordered_map` | Hand-rolled intrusive list |
|---|---|---|
| Lines of code | ~15 for the whole LRU | ~80, plus edge cases |
| Bug surface | essentially zero | head/tail/single-element/self-splice unlink bugs |
| Mechanism visibility | hidden behind `splice` | fully explicit |
| Memory | one node per entry, key stored twice | same, minus the map node overhead |
| Interview risk | "I used std::list" is a non-answer | must still explain it, but you wrote it |

**Chosen: `std::list` + `unordered_map`.** Reason: the interview question is *"how does your LRU achieve O(1)?"*, and the answer is a mechanism, not a container name. The mechanism is identical either way — a doubly linked node is unlinked from its neighbours and relinked at the head by rewriting four pointers, which is why it is constant-time regardless of list length. Hand-rolling that would add ~80 lines of pointer surgery whose only benefit is proving you can write it, at the cost of real unlink bugs. Writing the mechanism out in a comment and in the README costs nothing and demonstrates the same understanding.

**Non-negotiable consequence:** the README's LRU section must describe the four-pointer relink explicitly. "I used `std::list::splice`" is not an acceptable answer, and the spec below writes the full explanation for you.

### 1.3.3 The stored value

```cpp
struct Entry {
    std::string key;           // duplicated from the map key — see note
    std::string value;
    long long   expire_at_ms;  // ABSOLUTE ms since Unix epoch; 0 == no expiry
};
```

The key is stored **twice** — once as the `unordered_map` key, once inside the list node. This is deliberate and necessary: eviction starts from the *back of the list* and must remove the corresponding *map* entry, so the list node has to know its own key. Without the duplicate, eviction would be an O(n) map scan and the whole design would lose its O(1) claim. The cost is one extra `std::string` per entry; that is the standard trade and it is documented in Limitations.

### 1.3.4 Memory accounting

Real heap usage is not portably measurable: `std::string` has SSO, `std::unordered_map` allocates buckets and nodes opaquely, and the allocator adds per-block headers. The standard approximation, which this project uses, is:

```
entry_cost = key.size() + value.size() + ENTRY_OVERHEAD    // ENTRY_OVERHEAD = 96 bytes
```

`ENTRY_OVERHEAD` is a single fixed constant standing in for the list node's two pointers, the map node's pointer and cached hash, two `std::string` control blocks, and allocator headers. `mem_used_` is maintained **incrementally** — adjusted on every insert, update and erase — so the memory check is O(1) and never rescans the store.

This must be described in the README's Limitations section as **approximate**. It is a budget, not a measurement. The honest framing for an interview: "I am enforcing a *modelled* budget, not real RSS; the model is a linear function of key and value length plus a constant, and the constant is a guess calibrated to typical 64-bit libstdc++ node sizes."

### 1.3.5 Expiry / LRU interaction — the ordering rule

**A `GET` on an expired-but-not-yet-reaped key must not resurrect it and must not count as a use.** The order inside `get()` is fixed:

1. hash lookup — miss ⇒ return miss
2. **expiry check first** — if expired: erase the entry (memory accounting, map, list) and return miss. **No `splice`. The key is dead; touching the LRU for a dead key would promote a corpse to most-recently-used and would corrupt the eviction order.**
3. only on a live hit: `splice` to front, then return the value

Which commands touch the LRU:

| Command | Touches LRU? | Reason |
|---|---|---|
| `SET` | **yes** | a write is an access |
| `GET` (hit) | **yes** | a read is an access |
| `GET` (miss or expired) | **no** | nothing live was accessed |
| `EXPIRE` (success) | **yes** | an explicit operation on that key |
| `TTL` | **no** | introspection, not use — mirrors Redis, which reads TTL with a no-touch flag |
| `KEYS` | **no** | **critical:** a full scan that touched every key would reset all recency and destroy the LRU signal entirely |
| `DBSIZE` | **no** | same reason |

## 1.4 TTL semantics

### 1.4.1 Real Redis return values — mirrored exactly

Redis's `TTL` documentation: <cite index="60-1">in Redis 2.6 or older the command returned -1 if the key did not exist or if the key existed but had no associated expire; starting with Redis 2.8 the command returns -2 if the key does not exist and -1 if the key exists but has no associated expire</cite>. The reply contract is stated as <cite index="60-1">integer reply: TTL in seconds; -1 if the key exists but has no associated expiration; -2 if the key does not exist</cite>.

`EXPIRE` returns <cite index="74-1">1 if the timeout was set, 0 if the timeout was not set — for example if the key doesn't exist</cite>.

MiniRedis therefore returns:

| Case | Return |
|---|---|
| `TTL` on a missing key | `-2` |
| `TTL` on a key that has expired but not yet been reaped | `-2` (and the key is reaped on the spot) |
| `TTL` on a live key with no expiry set | `-1` |
| `TTL` on a live key with an expiry | remaining whole seconds |
| `EXPIRE` on an existing key | `1` |
| `EXPIRE` on a missing/expired key | `0` |
| `DEL` on an existing key | `1` |
| `DEL` on a missing key | `0` |

**Two further Redis semantics deliberately mirrored:**

1. **`SET` clears any existing TTL.** Redis: <cite index="75-1">the timeout of a key is cleared when the value at the key is replaced or deleted; commands that clear the timeout include SET</cite>. `SET k v` on a key that had 10 seconds left leaves it persistent.
2. **`EXPIRE` with a non-positive timeout deletes the key.** Redis: <cite index="68-1">calling EXPIRE/PEXPIRE with a non-positive timeout results in the key being deleted rather than expired</cite>. `EXPIRE k 0` and `EXPIRE k -5` therefore delete and return `1`.

**Deliberate divergences, all documented in the README:**

- `DBSIZE` in MiniRedis is O(n) because it reaps expired keys before counting, so its answer is exact. Redis's `DBSIZE` is O(1) and can transiently over-count keys that are logically expired but not yet reaped. Accuracy chosen over asymptotics at this scale.
- `KEYS` supports `*` and `prefix*` only, not Redis's full glob.
- `KEYS` output is sorted, for a deterministic demo. Redis returns hash-table order.

### 1.4.2 Clock choice — `system_clock`, and the trap

cppreference on `steady_clock`: <cite index="100-1">the time points of this clock cannot decrease as physical time moves forward and the time between ticks is constant; this clock is not related to wall clock time (for example, it can be time since last reboot) and is most suitable for measuring intervals</cite>. In practice <cite index="98-1">the starting point of steady_clock is typically the boot time of the machine, while system_clock's is typically 1 January 1970, the UNIX epoch</cite>. The standard pins `system_clock` down: <cite index="105-1">objects of type system_clock represent wall clock time from the system-wide realtime clock, and sys_time measures time since 1970-01-01 00:00:00 UTC excluding leap seconds — Unix time</cite>. And the general rule: <cite index="99-1">steady_clock uses an unspecified monotonic epoch — its zero might be system boot time or something entirely different — and you cannot meaningfully compare time_points from different clocks; never assume epochs are related</cite>.

**The trap, stated plainly:** a `steady_clock` value written to the WAL is a number of ticks since an unspecified origin, very often the boot time of that particular boot. After a restart — and certainly after a reboot — that number is meaningless. A key with 10 seconds left could reload as expired decades ago or expiring next century. **All persisted timestamps in MiniRedis are `system_clock` milliseconds since the Unix epoch.**

Redis reached the identical conclusion: <cite index="70-1">keys expiring information is stored as absolute Unix timestamps (in milliseconds in Redis 2.6 or greater), which means the time is flowing even when the Redis instance is not active; for expires to work well the computer time must be taken stable</cite>. That last clause is the honest cost of the choice and is documented: MiniRedis inherits Redis's exposure to wall-clock adjustments (NTP steps, manual clock changes).

**The one place `steady_clock` is correct** is the benchmark in `miniredis-cli`, which measures an elapsed interval and must not be perturbed by an NTP correction mid-run. Having both clocks in the codebase, each used for the thing it is for, is a strong interview artefact — point at it.

### 1.4.3 Lazy + active expiry, and why both

- **Lazy:** every `GET`, `DEL`, `EXPIRE` and `TTL` checks `expire_at_ms` against `now_ms()` before doing anything else, and erases on the spot if the key is dead. Cost: O(1), paid only on access.
- **Active:** a background reaper thread wakes every second, takes the store mutex, walks the list, and erases everything expired.

**Why both is not redundancy:**

- **Lazy alone leaks.** A key that is written, given a TTL, and then never read again is never examined again. Its memory is held forever. In an LRU-bounded store that is worse than a leak — a dead key sits in the recency list and can cause a *live* key to be evicted instead.
- **Active alone is not sufficient for correctness.** The sweep runs on a one-second period. A read arriving between a key's expiry instant and the next sweep would return a value that should already be gone. The lazy check closes that window to zero: a read never observes an expired key, no matter when the sweep last ran.

Together: **lazy gives correctness on the read path, active bounds memory on the never-read path.** Real Redis uses exactly this pairing, for exactly these reasons.

## 1.5 WAL and crash recovery

### 1.5.1 Flush vs fsync — what each actually guarantees

There are three places data can be after you "write" it:

1. **The user-space stream buffer** — inside `std::ofstream`. Lost if the *process* dies.
2. **The OS page cache** — kernel memory. Survives process death; lost if the *machine* dies. `ofstream::flush()` gets you here (it issues `write(2)`).
3. **The physical device** — survives everything. Only `fsync`/`fdatasync` gets you here.

`fsync(2)`: <cite index="110-1">fsync() transfers ("flushes") all modified in-core data of the file referred to by the file descriptor to the disk device so that all changed information can be retrieved even if the system crashes or is rebooted; this includes writing through or flushing a disk cache if present, and the call blocks until the device reports the transfer has completed</cite>. The distinction from `fflush` is exactly as expected: <cite index="113-1">fflush flushes data from the upper (library) buffer to the kernel buffer and returns; it is not as secure as fsync, and you need to call fsync afterwards to actually write the data to disk</cite>. Note also that even `fsync` is not the end of the story on consumer hardware — <cite index="116-1">while fsync() flushes all data from the host to the drive, the drive itself may not physically write the data to the platters for some time, and if the drive loses power the application may find only some or none of its data was written</cite>.

**Decision: `flush()` only, no `fsync`.** Two independent reasons:

1. **It exactly matches the threat model.** The demo is `kill -9`: the process dies, the kernel does not. Data in the page cache is fully intact and the restarted server reads it back. `fsync` would buy protection against machine power loss, which is not what is being demonstrated and not what a codespace container even meaningfully experiences.
2. **`fsync` is not reachable from standard C++17.** There is no portable way to obtain a file descriptor from a `std::ofstream`. Getting `fsync` would mean abandoning `<fstream>` for POSIX `open`/`write`/`fsync`, which drags platform `#ifdef`s into `wal.cpp` and destroys the "one small compatibility header" property this project is built around.

Cost of the choice, stated honestly in the README: **MiniRedis survives process crashes, not power loss.** The throughput cost of the alternative is not hypothetical — an `fsync` per write is a device round-trip, typically dropping a store like this from tens of thousands of ops/sec to a few hundred on spinning media, which is precisely why real systems batch it (<cite index="109-1">PostgreSQL shares a single WAL fsync across multiple client transactions via group commit; SQLite exposes synchronous = FULL / NORMAL / OFF; the trade-off is the durability window</cite>).

### 1.5.2 WAL format

Append-only, one record per line, space-separated, human-readable, `cat`-able:

```
SET <expire_at_ms> <key> <value...>
DEL <key>
EXPIRE <expire_at_ms> <key>
EVICT <key>
```

- `<expire_at_ms>` is **absolute** `system_clock` milliseconds since the Unix epoch. `0` means "no expiry".
- `<key>` is a single token (the wire protocol forbids spaces in keys, so no length prefix is needed).
- `<value...>` is the rest of the line, so values may contain spaces. Values cannot contain `\n` — the wire protocol makes that impossible.

**The absolute-timestamp rule, and why it is a headline point.** If the WAL recorded `SET k v ttl=10s`, then replaying a log written three days ago would grant `k` a *fresh* ten-second lease starting now — a key that should have died three days ago comes back alive. The log would not be a record of what happened; it would be a script that re-enacts it at the wrong time. Recording the absolute instant makes replay time-independent: a key whose recorded deadline is in the past is simply not resurrected, no matter when replay happens. This is heavily commented in `wal.cpp` and `store.h`.

### 1.5.3 Ordering rule

**Append + flush to the WAL before `OK` goes back to the client.** If the reply went first, a crash in the gap would leave the client believing a write succeeded that no longer exists — a lost acknowledged write, the one failure mode a WAL exists to prevent. The reverse gap (flushed but crashed before replying) is benign: the client sees a connection error, retries, and the operation is idempotent.

In this codebase the enforcement is structural, not conventional: **only `Store` calls `Wal`, and it does so while holding the store mutex.** The server's `dispatch()` cannot construct a reply until the `Store` method has returned, and that method does not return until `flush()` has.

### 1.5.4 Replay

On startup, before the socket is even created:

1. Determine whether the file's last byte is `\n`.
2. Read every line.
3. **If the file does not end in `\n`, discard the final line** — that is a torn record from a `kill -9` mid-write. Log it to stderr and continue. **The server must never refuse to start because of a torn tail.**
4. Parse each record. A record that fails to parse is logged and skipped; the replay continues. Startup never aborts on a bad record.
5. After all records: sweep keys whose absolute deadline is already in the past — **they are not resurrected** — then enforce the memory limit.

**Must `DEL` and `EXPIRE` be logged? Yes, both.** Reasoning from first principles: the WAL's contract is that replaying it reproduces the state. If `DEL` were not logged, the `SET` that created the key would still be there and replay would resurrect a deliberately deleted key. If `EXPIRE` were not logged, a key given a TTL after its `SET` would replay with the `SET` record's `expire_at_ms` of `0` and would become immortal. A log that only records creations is not a log of state, it is a log of half the state.

**Must eviction be logged? Yes — and this is the question worth thinking hardest about.**

The argument for *not* logging it: eviction is derived, not commanded. Replay applies the same memory limit and could just evict again.

That argument fails. Replay cannot reproduce the eviction *decisions*, because eviction order is driven by recency, and recency is driven by `GET`s, and `GET`s are not in the log (logging reads would double the log volume for a store whose whole point is fast reads). So if evictions are unlogged, replay evicts by a *different* order — insertion order — and keys that were evicted before the crash can come back while keys that survived can vanish. "Recovered state equals pre-crash state" would be false, and that is a much worse thing to have to defend in an interview than one extra log line per eviction.

**So: evictions are logged as an explicit `EVICT <key>` record**, distinct from `DEL` purely so the log reads clearly when you `cat` it; the replayer treats both as a delete.

**The honest limitation that remains either way, stated in the README:** the *set of keys* is reproduced exactly, but the *LRU ordering* is not. After replay the recency order is WAL order (i.e. insertion order), not the true pre-crash recency order, because reads are not logged. The first eviction after a restart may therefore pick a different victim than the pre-crash server would have. Given that eviction is a memory-pressure heuristic and not a correctness property, this is the right trade.

**Expirations are deliberately *not* logged.** They are fully derivable: the absolute deadline is already in the log, and replay drops anything past its deadline. Logging them would be redundant bytes.

### 1.5.5 Open-once vs reopen-per-write

**Open once at startup, in append mode, hold the handle for the process lifetime.** Reopening per write would add an `open`+`close` syscall pair to every single `SET` — strictly more expensive with no benefit, since `std::ios::app` already guarantees every write goes to the current end of file. The handle is opened *after* replay finishes, which cleanly guarantees the replayer can never be reading a file the appender is writing.

## 1.6 Concurrency

**Model: one `std::thread` per connected client, detached.**

Practical ceiling, and the honest limitation to document: each thread costs a stack (8 MB of virtual address space by default on Linux, a few KB resident) plus a kernel task struct, and every context switch between them costs scheduler time. The model is fine to the low thousands of connections and falls apart well before 10,000 — memory pressure, scheduler thrash, and O(n) wakeup behaviour dominate. The production answer is an event loop over `epoll`/`kqueue`/IOCP, where one thread multiplexes thousands of sockets and never blocks.

**The contrast worth making in an interview:** real Redis executes commands on a *single* thread. It is not slow because of that — it is fast because of it. There is no lock contention, no cache-line ping-pong between cores, and no data races, because there is only ever one thing touching the data structures. It gets its concurrency from `epoll` multiplexing, not from threads. (Redis 6+ added I/O threads for socket read/write, but command execution stayed single-threaded.) MiniRedis makes the opposite trade — threads for concurrency, a mutex for safety — and that trade is exactly what the single global mutex is paying for.

**Locking: one `std::mutex` inside `Store`.**

Correct for this size. Alternatives — sharded locks keyed by hash, or a reader-writer lock — each buy throughput and each cost something this project should not spend. Sharding breaks the global LRU list (recency is inherently global; you would need per-shard LRU, which changes the eviction semantics). A `shared_mutex` sounds free but is not: `GET` **mutates** the LRU list via `splice`, so a read command is a writer as far as the data structure is concerned, and a shared lock would be actively wrong.

**What the lock covers — exactly:**

The mutex covers `map_`, `lru_`, `mem_used_`, **and the WAL append and flush**. The WAL write happens **inside** the store lock, not outside it.

This is the detail to get right. If the WAL write were outside the lock, two concurrent `SET`s on the same key could serialise their memory updates in one order and their log records in the opposite order. Replay would then produce a different final value than the running server had — the log would be a *lie*. Holding one lock across both the log append and the memory mutation makes the WAL order and the apply order the same total order by construction.

The price is stated plainly: **the flush latency is inside the critical section, so all writers serialise behind disk I/O.** That is the documented bottleneck and it is the correct thing to trade for a WAL that cannot lie.

Order within the critical section, per `SET`:

1. append the `SET` record (buffered)
2. apply to `map_` / `lru_` / `mem_used_`
3. run eviction; append an `EVICT` record for each victim
4. **one** `flush()` covering all of the above
5. release the lock
6. `dispatch()` builds the reply, the server sends it

One flush per command regardless of how many keys were evicted. A `flush()` failure is reported to the client as `ERR wal write failed`. Deliberate simplification, commented as such rather than hidden: in that failure path the in-memory state is one operation ahead of the log. Recovering properly would mean an undo path; for a portfolio project the correct move is to say so.

**Reaper thread locking and shutdown.** The reaper does *not* spin on the store mutex. It sleeps on a `std::condition_variable` with a one-second `wait_for` against its **own** mutex, and only then briefly acquires the store mutex to sweep. It cannot starve the accept loop (which never takes the store mutex at all) or the client threads. On shutdown, `running_` is cleared and the CV is notified, so the reaper wakes immediately instead of sleeping out its second; it is then **joined**.

**`std::atomic`.** `running_` is `std::atomic<bool>` — written by the shutdown path and read by the reaper thread, so a plain `bool` would be a data race. The signal-handler flag is `volatile std::sig_atomic_t`, which is the only type a signal handler may portably write.

## 1.7 Protocol design edge cases

| Question | Decision |
|---|---|
| Values with spaces? | **Yes.** For `SET`, the value is the **entire rest of the line** after `SET <key> `. Still one `getline`. Keys remain single tokens. |
| Empty value? | `SET k` and `SET k ` ⇒ `ERR wrong number of arguments for 'SET'`. Empty string values are not supported. |
| Trailing whitespace | Trailing spaces/tabs/`\r` are trimmed from the line, so a value cannot end in a space. Documented. |
| Unknown command | `ERR unknown command 'FOO'` |
| Wrong arity | `ERR wrong number of arguments for 'GET'` |
| Non-integer `EXPIRE` arg | `ERR value is not an integer or out of range` (Redis's own wording) |
| Empty line | Silently ignored, no reply. Keeps `telnet` usable. |
| Error format | `ERR <message>` on one line. The client renders it as `(error) <message>`. |
| `KEYS` pattern | Pattern ending in `*` ⇒ prefix match (`*` alone ⇒ everything). Pattern without `*` ⇒ exact match. No full glob. |
| `KEYS` reply framing | `KEYS <n>` header line, then exactly `<n>` lines of `KEY <key>`. Count-prefixed, so the client reads a fixed number of lines and needs no terminator sentinel and no ambiguity if a key were ever to look like a terminator. |
| Max line length | **65536 bytes.** A connection that accumulates more than that with no `\n` gets `ERR line too long, closing connection` and is closed. Prevents a single client from driving the server out of memory. |

**Reply grammar (this is the one design decision worth defending carefully):**

```
OK                      -> SET success
VALUE <string>          -> GET hit
NIL                     -> GET miss
INT <n>                 -> DEL, EXPIRE, TTL, DBSIZE
ERR <message>           -> any error
KEYS <n>                -> KEYS header, followed by exactly n lines of: KEY <key>
```

**Why tagged rather than bare values.** A bare-value reply (`GET k` ⇒ `Rahul`, miss ⇒ `nil`) reads beautifully in a demo transcript and is what you sketched — but it is ambiguous: a key whose value is literally the string `nil` is indistinguishable from a miss, and a value of `OK` is indistinguishable from a `SET` acknowledgement. A one-word tag removes the ambiguity for the cost of one token.

**This does not cost you the demo transcript,** because `miniredis-cli` strips the tag before printing. On screen you still see:

```
miniredis> GET user:1
Rahul
miniredis> GET nope
(nil)
```

while the wire carries `VALUE Rahul` and `NIL`. You get an unambiguous machine-parseable protocol *and* a clean demo, and `telnet` output stays human-readable. **This is not RESP** — no length prefixes, no type sigils, no binary framing, still one command per line, still one `getline`.

## 1.8 Reference material

Semantics were checked against the official Redis command documentation for all seven commands (`SET`, `GET`, `DEL`, `EXPIRE`, `TTL`, `KEYS`, `DBSIZE`). Every divergence is enumerated in §1.4.1 and repeated in the README. Structural conventions common to "build your own Redis" write-ups — separating parse / execute / persist layers, replaying a log at startup, lazy plus active expiry — were noted as conventions; **no code was copied from any source.**

---

# 2. DECISIONS

### 1. Environment — **GitHub Codespaces**

**120 core-hours/month** compute and **15 GB-month** storage on a personal account; the default 2-core machine burns 2 core-hours per wall-clock hour, so ≈**60 real hours/month**; **30-minute** default idle timeout; **free tier does not apply to org-owned repos**. You get a real Linux kernel with real POSIX sockets, working `kill -9`, and multiple terminals, with zero installation on your Windows machine — which matters because you do not have WSL and setting it up is a yak-shave that can fail on Windows Home builds, Hyper-V settings, or a corporate-managed device. Sixty hours a month is an enormous budget for a project like this. **Fallback:** WSL2 if you want a local, quota-free environment and are willing to spend an afternoon on setup (it is genuinely the better long-term answer, just not the better *first* answer). **Second fallback:** MSYS2/MinGW-w64 on native Windows — the code compiles there thanks to the shim, but `kill -9` has no exact equivalent (`taskkill /F` is close) and you lose POSIX signal semantics. **MSVC: avoided, as you asked.**

### 2. Sockets — **POSIX primary, plus a Winsock shim confined to `net_compat.h`**

The shim comes to about 85 lines, slightly over your 60-line guideline, and it is genuinely self-contained: it exports `socket_t`, `INVALID_SOCK`, `sockopt_arg_t`, `closesock()`, `net_init()`, `ignore_sigpipe()`, `sock_recv()`, `sock_send()`, and fallbacks for `MSG_NOSIGNAL` and `SHUT_RDWR`. **There is not a single `#ifdef` in any other file** — that was the acceptance criterion and it is met. If you want it strictly under 60 lines, delete the `_WIN32` branch and the project becomes POSIX-only with no other edits.

### 3. LRU — **`std::list<Entry>` + `unordered_map<string, list<Entry>::iterator>`**

cppreference confirms `splice` is constant-time for a single element and that no iterators are invalidated. Hand-rolling the intrusive list adds ~80 lines of pointer surgery and real unlink bugs to demonstrate knowledge that a comment demonstrates for free. **The README explains the four-pointer relink mechanism explicitly**, because "I used `std::list`" is not an interview answer.

### 4. Clock — **`std::chrono::system_clock`, absolute milliseconds since the Unix epoch**

`steady_clock`'s epoch is unspecified and typically boot time, so a `steady_clock` value written to a file is meaningless after a restart. `system_clock` is pinned to Unix time by the standard. Redis stores expiries as absolute Unix timestamps for the identical reason. **`steady_clock` is used in exactly one place — the client's `--bench` interval measurement — which is what it is actually for.**

### 5. WAL — **four record types, `flush()` only, no `fsync`**

Format: `SET <expire_at_ms> <key> <value...>` / `DEL <key>` / `EXPIRE <expire_at_ms> <key>` / `EVICT <key>`. Append-only, human-readable, `cat`-able. Flush-only because the demo threat model is `kill -9` (process dies, kernel survives, page cache intact), and because `fsync` is unreachable from a `std::ofstream` without dropping to POSIX file descriptors and contaminating the portability story. **README states plainly: survives process crashes, not power loss.**

### 6. WAL-write vs store-lock ordering — **WAL write is INSIDE the store mutex**

Only `Store` touches `Wal`, and only while holding `mu_`. If the log write were outside the lock, two concurrent writers could commit to memory and to the log in opposite orders and replay would yield a different final state than the live server had. Holding one lock across both makes log order and apply order the same total order by construction. Cost: flush latency is in the critical section, so writers serialise on disk. **Documented as the primary bottleneck.**

### 7. Protocol — **rest-of-line values, tagged replies, count-prefixed `KEYS`**

`SET`'s value is everything after `SET <key> ` (spaces allowed; still one `getline`). Replies are tagged: `OK` / `VALUE <s>` / `NIL` / `INT <n>` / `ERR <msg>` / `KEYS <n>` + n × `KEY <k>`. Tags remove the "value literally equals `nil`" ambiguity that bare values have; the client strips them so the demo transcript still reads `GET user:1` ⇒ `Rahul`. `KEYS` is count-prefixed so the client reads a known number of lines. Max line 64 KiB.

### 8. Are evictions logged? — **Yes, as `EVICT <key>`**

Replay cannot re-derive eviction decisions, because eviction order depends on recency and recency depends on `GET`s, which are not logged. Unlogged evictions would let replay resurrect evicted keys and drop surviving ones, breaking "recovered state == pre-crash state". **Remaining honest limitation, documented:** the *key set* is reproduced exactly; the *LRU ordering* after replay is WAL order, not true pre-crash recency, so the first post-restart eviction may pick a different victim.

### 9. Files beyond your proposed structure

Added: **`net_compat.h`** (the shim — you sanctioned this conditionally, and the condition held). **`build.sh`** and **`build.bat`** (one-line convenience scripts, not a build system, as you sanctioned). **`.gitignore`** and **`LICENSE`** as you listed.

**Not added: `.devcontainer/devcontainer.json`.** Research reason: the default universal image already ships a C++17-capable g++, and GitHub's billing docs state that a **custom base image counts against your 15 GB storage quota while containers based on the default image do not**. Adding one costs quota and buys nothing here. The JSON is included in the README as an optional copy-paste block with that trade-off flagged.

**Total: 13 files.**

## Optional, your call

**1. Throughput number via `miniredis-cli --bench N` — RECOMMENDED, and built.**

Justification for building it despite being unasked: it is ~20 lines in `client.cpp`, uses **only in-scope commands** (it issues `SET`s), adds no server code, no new command, and no new protocol. It gives the README the one quantifiable number it otherwise lacks — "≈X,000 ops/sec, single client, flush-on-every-write" — which turns "I built a key-value store" into "I built a key-value store and I know what it costs". It also puts `steady_clock` in the codebase next to `system_clock`, each used correctly, which is a free interview talking point about clock selection. **If you disagree, delete `run_bench()` and the `--bench` argument; nothing else references them.**

**2. A `--fsync` flag to demo the durability/throughput trade — RECOMMENDED AGAINST.**
It cannot be implemented without abandoning `<fstream>` for POSIX `open`/`write`/`fsync`, which puts `#ifdef _WIN32` into `wal.cpp` and breaks the "one compatibility header" property. The trade-off is better explained in one README paragraph than bought at that price.

**3. Unit tests — RECOMMENDED AGAINST**, you excluded test frameworks and the demo script in §10 of the README is the acceptance test.

**4. `INFO`/`STATS` command exposing `mem_used_` — RECOMMENDED AGAINST**, it is an eighth command and you drew the line at seven. The LRU demo shows memory pressure working without it.

## Possible extensions (mentioned once, not built)

WAL compaction/snapshotting; `epoll` event loop replacing thread-per-client; sharded locking; `INCR`/`SETNX`; RESP compatibility for `redis-cli`; hash/list value types; replication.

---

# 3. FILE STRUCTURE AND PER-FILE ROLES

```
miniredis/
├── net_compat.h      Platform shim. The ONLY file that knows the OS. Exports socket_t,
│                     INVALID_SOCK, closesock(), net_init(), ignore_sigpipe(),
│                     sock_recv/sock_send, and MSG_NOSIGNAL/SHUT_RDWR fallbacks.
│                     No other file contains an #ifdef.
├── protocol.h        Command struct, CmdType enum, parse_command(), MAX_LINE_BYTES,
│                     plus the wire helpers shared by BOTH binaries: send_all(),
│                     LineBuffer (the TCP framing buffer), recv_line().
├── protocol.cpp      Implements the above. Linked into server AND client.
├── store.h           Entry struct, Store class. Owns the hash map, the LRU list, the
│                     memory accounting, the single global mutex, and the reaper thread.
│                     Carries the long comment blocks on splice/iterator validity and
│                     on the clock choice.
├── store.cpp         Implements Store. This is where the WAL-inside-the-lock ordering
│                     rule lives.
├── wal.h             Wal class: replay(), open_for_append(), append_*(), flush(), close().
│                     Deliberately lock-free — it is only ever called under Store::mu_.
├── wal.cpp           Record formatting, torn-final-line detection, record parsing,
│                     replay driver.
├── server.cpp        main(): arg parsing, replay, listening socket (SO_REUSEADDR),
│                     signal handlers, accept loop, per-client thread, framing loop,
│                     dispatch(), reply formatting, shutdown.
├── client.cpp        main(): connect, REPL, tag-stripping reply renderer, --bench.
├── build.sh          The exact two g++ commands, Linux/macOS. Convenience only.
├── build.bat         The same for native Windows + MinGW-w64 (adds -lws2_32).
├── .gitignore        Binaries, object files, *.wal.
├── LICENSE           MIT.
└── README.md         The graded deliverable.
```

**Build commands (also printed in §6 and in the README):**

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pthread -o miniredis-server server.cpp store.cpp wal.cpp protocol.cpp
g++ -std=c++17 -O2 -Wall -Wextra -pthread -o miniredis-cli    client.cpp protocol.cpp
```

Native Windows (MinGW-w64): drop `-pthread`, append `-lws2_32`, name the outputs `.exe`.

**Default port: 6399.** Not 6379, so a locally installed real Redis never collides. Not 6380, which is the conventional Redis TLS / second-instance port. Above 1024, so no root is needed to bind. **Below 32768**, which on Linux is the bottom of the default ephemeral source-port range (`/proc/sys/net/ipv4/ip_local_port_range`, typically `32768 60999`) — a listener above that boundary can lose a race against the kernel handing the same number out as an outbound source port. Overridable with `--port`.

---

# 4. IMPLEMENTATION NOTES FOR THE CODING AGENT

- Every file below is complete. There are no `TODO`s, no ellipses, no omitted bodies.
- Create files at the exact paths in the headings, relative to the repository root.
- Do not reformat, do not reorder, do not "simplify" the comment blocks — those comments are the interview script.
- Do not add a build system, tests, CI, Docker, or a `.devcontainer` directory.
- After creating all files, stop. Do not compile or run anything.

---

# 5. FILES

## `net_compat.h`

```cpp
#pragma once
//
// net_compat.h — the ONLY file in this project that knows which operating
// system we are compiling for.
//
// The whole point of this header is containment. Everything below is behind a
// single #if defined(_WIN32) / #else pair, and it exports a small vocabulary
// (socket_t, INVALID_SOCK, closesock, net_init, ignore_sigpipe, sock_recv,
// sock_send) that the rest of the codebase uses unconditionally. There is not
// one #ifdef anywhere else in this project. If you ever want to drop Windows
// support entirely, delete the _WIN32 branch below and nothing else changes.
//
// Primary target is POSIX/Linux (GitHub Codespaces, WSL2). The Windows branch
// exists so the project still builds with MinGW-w64 g++ if you want it to.
//
#if defined(_WIN32)

  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>

  using socket_t      = SOCKET;
  using sockopt_arg_t = const char*;          // Winsock setsockopt wants char*
  static const socket_t INVALID_SOCK = INVALID_SOCKET;

  // NOTE: on Windows, INVALID_SOCKET is not -1 and SOCKET is unsigned, which is
  // exactly why no code in this project ever tests a socket with "< 0".

  inline int  closesock(socket_t s) { return ::closesocket(s); }
  inline bool net_init()            { WSADATA w; return ::WSAStartup(MAKEWORD(2, 2), &w) == 0; }
  inline void ignore_sigpipe()      { /* Windows has no SIGPIPE. Nothing to do. */ }

  #ifndef MSG_NOSIGNAL
  #define MSG_NOSIGNAL 0
  #endif
  #ifndef SHUT_RDWR
  #define SHUT_RDWR SD_BOTH
  #endif

#else

  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <csignal>

  using socket_t      = int;
  using sockopt_arg_t = const void*;
  static const socket_t INVALID_SOCK = -1;

  inline int  closesock(socket_t s) { return ::close(s); }
  inline bool net_init()            { return true; }

  // SIGPIPE: on Linux, send()ing to a socket whose peer has already gone away
  // raises SIGPIPE, and SIGPIPE's DEFAULT DISPOSITION IS TO KILL THE PROCESS.
  // A client that hits Ctrl-C mid-command would take the whole server down.
  // We ignore it process-wide here (one call, covers every thread) and rely on
  // send() returning -1/EPIPE instead. We ALSO pass MSG_NOSIGNAL on every send
  // below, which is the per-call version of the same protection -- belt and
  // braces, because macOS historically lacked MSG_NOSIGNAL entirely.
  inline void ignore_sigpipe()      { ::signal(SIGPIPE, SIG_IGN); }

  #ifndef MSG_NOSIGNAL
  #define MSG_NOSIGNAL 0    // e.g. macOS; the signal(SIG_IGN) above still covers us
  #endif

#endif

#include <cstddef>

// recv/send take size_t on POSIX and int on Winsock, and return ssize_t vs int.
// These two inlines absorb that difference so no call site needs a cast.
// Return value is widened to long long; callers treat n <= 0 as "stop".
inline long long sock_recv(socket_t s, char* buf, size_t len) {
    return static_cast<long long>(::recv(s, buf, static_cast<int>(len), 0));
}
inline long long sock_send(socket_t s, const char* buf, size_t len) {
    return static_cast<long long>(::send(s, buf, static_cast<int>(len), MSG_NOSIGNAL));
}
```

## `protocol.h`

```cpp
#pragma once
//
// protocol.h — the MiniRedis wire protocol.
//
// REQUESTS: one command per line, terminated by '\n', space-separated,
// human-readable. A trailing '\r' is tolerated so telnet/PuTTY (which send
// CRLF) work identically to miniredis-cli.
//
//   SET <key> <value...>     value is the ENTIRE REST OF THE LINE (spaces OK)
//   GET <key>
//   DEL <key>
//   EXPIRE <key> <seconds>
//   TTL <key>
//   KEYS <pattern>           "*" or "prefix*" (prefix match), else exact match
//   DBSIZE
//
// REPLIES: one line, except KEYS which is count-prefixed.
//
//   OK                       SET succeeded
//   VALUE <string>           GET hit
//   NIL                      GET miss
//   INT <n>                  DEL / EXPIRE / TTL / DBSIZE
//   ERR <message>            any error
//   KEYS <n>                 followed by exactly <n> lines of "KEY <key>"
//
// Why replies are TAGGED rather than bare values: a bare-value protocol cannot
// distinguish a miss from a key whose value is literally the string "nil", nor
// a SET acknowledgement from a value of "OK". One leading token removes the
// ambiguity. miniredis-cli strips the tag before printing, so the human-facing
// transcript still reads "GET user:1" -> "Rahul".
//
// This is deliberately NOT the Redis RESP protocol: no length prefixes, no type
// sigils, no binary framing. One line in, one line out, parseable with a single
// getline().
//
#include <cstddef>
#include <string>
#include <vector>

#include "net_compat.h"

// Hard cap on a single protocol line. A client that streams 64 KiB with no
// newline is either broken or hostile; either way we refuse to buffer it
// unboundedly and close the connection.
constexpr size_t MAX_LINE_BYTES = 64 * 1024;

enum class CmdType { SET, GET, DEL, EXPIRE, TTL, KEYS, DBSIZE, UNKNOWN };

struct Command {
    CmdType     type     = CmdType::UNKNOWN;
    std::string key;
    std::string value;          // SET: the value. KEYS: the pattern.
    long long   seconds  = 0;   // EXPIRE only
    bool        ok       = false;
    bool        is_empty = false;  // blank line: ignore it, do not reply
    std::string error;          // populated when ok == false
};

// Parses one already-de-framed line (no trailing '\n'). Never throws.
Command parse_command(const std::string& line);

// ---------------------------------------------------------------------------
// Wire helpers shared by BOTH binaries. protocol.cpp is linked into the server
// and the client precisely so this framing logic exists exactly once.
// ---------------------------------------------------------------------------

// send() is not obliged to accept the whole buffer in one call. Loop until it
// has. Returns false on a dead connection.
bool send_all(socket_t fd, const std::string& data);

// TCP is a BYTE STREAM. It has no message boundaries. A single recv() may hand
// you half a command, or two and a half commands, or one byte. LineBuffer is
// the per-connection accumulator that turns that stream back into lines: you
// append whatever recv() gave you, then drain every COMPLETE line, and any
// partial tail simply stays in the buffer until the next recv() completes it.
//
// Nothing in this project ever assumes one recv() == one command.
class LineBuffer {
public:
    void   append(const char* data, size_t n);
    // Pops one complete line if the buffer holds a '\n'. Strips the '\n' and a
    // trailing '\r' if present. Returns false if no complete line is buffered.
    bool   next_line(std::string& out);
    size_t size() const;
    void   clear();
private:
    std::string buf_;
};

// Blocking: keeps recv()ing until one complete line is available. Used by the
// client, which is synchronous request/response. Returns false on EOF, error,
// or if the peer exceeds MAX_LINE_BYTES without sending a newline.
bool recv_line(socket_t fd, LineBuffer& lb, std::string& out);
```

## `protocol.cpp`

```cpp
#include "protocol.h"

#include <cctype>

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

static std::string to_upper(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

// Drops leading spaces/tabs from a string, in place.
static void ltrim(std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i) s.erase(0, i);
}

Command parse_command(const std::string& raw) {
    Command c;

    std::string line = raw;

    // Trailing '\r' matters: telnet and PuTTY terminate lines with CRLF, and
    // LineBuffer only splits on '\n'. Strip trailing whitespace generally --
    // the documented consequence is that a SET value cannot end in a space.
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    ltrim(line);

    if (line.empty()) { c.is_empty = true; return c; }

    size_t sp = line.find(' ');
    const std::string verb = to_upper(sp == std::string::npos ? line : line.substr(0, sp));
    std::string rest = (sp == std::string::npos) ? std::string() : line.substr(sp + 1);
    ltrim(rest);

    // ---- SET: value is the ENTIRE REST OF THE LINE, so values may contain
    // spaces. Still a single getline on the wire.
    if (verb == "SET") {
        size_t k = rest.find(' ');
        if (k == std::string::npos) { c.error = "wrong number of arguments for 'SET'"; return c; }
        c.key = rest.substr(0, k);
        std::string v = rest.substr(k + 1);
        ltrim(v);
        if (c.key.empty() || v.empty()) { c.error = "wrong number of arguments for 'SET'"; return c; }
        c.type  = CmdType::SET;
        c.value = v;
        c.ok    = true;
        return c;
    }

    // ---- Single-key commands. Keys are single tokens: an embedded space is an
    // arity error, not a key with a space in it.
    if (verb == "GET" || verb == "DEL" || verb == "TTL") {
        if (rest.empty() || rest.find(' ') != std::string::npos) {
            c.error = "wrong number of arguments for '" + verb + "'";
            return c;
        }
        c.key  = rest;
        c.type = (verb == "GET") ? CmdType::GET
               : (verb == "DEL") ? CmdType::DEL
                                 : CmdType::TTL;
        c.ok = true;
        return c;
    }

    if (verb == "EXPIRE") {
        size_t k = rest.find(' ');
        if (k == std::string::npos) { c.error = "wrong number of arguments for 'EXPIRE'"; return c; }
        c.key = rest.substr(0, k);
        std::string secs = rest.substr(k + 1);
        ltrim(secs);
        if (c.key.empty() || secs.empty() || secs.find(' ') != std::string::npos) {
            c.error = "wrong number of arguments for 'EXPIRE'";
            return c;
        }
        size_t consumed = 0;
        try {
            c.seconds = std::stoll(secs, &consumed);
        } catch (...) {
            c.error = "value is not an integer or out of range";
            return c;
        }
        // stoll("10abc") happily returns 10 without throwing, so verify that it
        // consumed the WHOLE token.
        if (consumed != secs.size()) {
            c.error = "value is not an integer or out of range";
            return c;
        }
        c.type = CmdType::EXPIRE;
        c.ok   = true;
        return c;
    }

    if (verb == "KEYS") {
        if (rest.empty() || rest.find(' ') != std::string::npos) {
            c.error = "wrong number of arguments for 'KEYS'";
            return c;
        }
        c.type  = CmdType::KEYS;
        c.value = rest;   // the pattern
        c.ok    = true;
        return c;
    }

    if (verb == "DBSIZE") {
        if (!rest.empty()) { c.error = "wrong number of arguments for 'DBSIZE'"; return c; }
        c.type = CmdType::DBSIZE;
        c.ok   = true;
        return c;
    }

    c.error = "unknown command '" + verb + "'";
    return c;
}

// ---------------------------------------------------------------------------
// Wire helpers
// ---------------------------------------------------------------------------

bool send_all(socket_t fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        // PARTIAL WRITES ARE NORMAL. send() returns how many bytes the kernel
        // accepted, which may be fewer than we offered when the socket send
        // buffer is full. Loop until everything is handed over.
        //
        // On EINTR we would also get n < 0, but std::signal on glibc installs
        // handlers with SA_RESTART, so blocking socket calls are automatically
        // restarted; we therefore treat n <= 0 as a hard error.
        long long n = sock_send(fd, data.data() + sent, data.size() - sent);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

void LineBuffer::append(const char* data, size_t n) { buf_.append(data, n); }

bool LineBuffer::next_line(std::string& out) {
    size_t p = buf_.find('\n');
    if (p == std::string::npos) return false;   // only a partial line so far
    out.assign(buf_, 0, p);
    buf_.erase(0, p + 1);
    if (!out.empty() && out.back() == '\r') out.pop_back();   // telnet sends CRLF
    return true;
}

size_t LineBuffer::size() const { return buf_.size(); }
void   LineBuffer::clear()      { buf_.clear(); }

bool recv_line(socket_t fd, LineBuffer& lb, std::string& out) {
    // A previous recv() may already have delivered more than one line, so check
    // the buffer BEFORE going back to the socket.
    if (lb.next_line(out)) return true;

    char tmp[4096];
    for (;;) {
        long long n = sock_recv(fd, tmp, sizeof(tmp));
        if (n <= 0) return false;                    // 0 == peer closed, <0 == error
        lb.append(tmp, static_cast<size_t>(n));
        if (lb.next_line(out)) return true;
        if (lb.size() > MAX_LINE_BYTES) return false;  // unbounded line guard
    }
}
```

## `store.h`

```cpp
#pragma once
//
// store.h — the in-memory key-value store: hash map + O(1) LRU + TTL tracking +
// memory budget + the single global mutex + the background expiry reaper.
//
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class Wal;

// ---------------------------------------------------------------------------
// CLOCK CHOICE -- read this before touching any timestamp in this project.
//
// now_ms() returns ABSOLUTE milliseconds since the Unix epoch, from
// std::chrono::system_clock.
//
// It is NOT std::chrono::steady_clock, and that is deliberate. steady_clock's
// epoch is unspecified by the standard and in practice is usually the boot time
// of the machine. That makes it perfect for measuring an elapsed interval and
// completely useless for anything written to disk: a steady_clock value stored
// in the WAL is a tick count from an origin that no longer exists after the
// process (or the machine) restarts. Replaying it would produce a key that
// expired decades ago, or one that expires next century.
//
// system_clock is pinned by the standard to Unix time -- the same absolute
// instant means the same number in this process, in the next process, and after
// a reboot. That is the only property that makes a persisted deadline
// meaningful. Real Redis stores expiries as absolute Unix timestamps for
// exactly this reason.
//
// The cost we accept: system_clock is adjustable, so an NTP step or a manual
// clock change moves every deadline. Redis has the same exposure and documents
// the same caveat.
//
// The ONE place steady_clock is correct in this project is the client's --bench
// interval measurement, which must not be perturbed by a clock adjustment.
// ---------------------------------------------------------------------------
long long now_ms();

struct Entry {
    // The key is stored here IN ADDITION to being the unordered_map's key.
    // This duplication is load-bearing, not sloppiness: eviction starts at the
    // BACK of the LRU list and must then erase the corresponding map entry, so
    // the list node has to know its own key. Without the copy, eviction would
    // require an O(n) scan of the map and the whole O(1) claim would collapse.
    // Cost: one extra std::string per entry. Documented in README Limitations.
    std::string key;
    std::string value;

    // ABSOLUTE deadline: system_clock milliseconds since the Unix epoch.
    // 0 means "no expiry". Never a relative duration -- see the WAL comments.
    long long   expire_at_ms;
};

class Store {
public:
    Store(Wal& wal, size_t memory_limit_bytes);
    ~Store();

    Store(const Store&)            = delete;
    Store& operator=(const Store&) = delete;

    // ---- Command implementations. Each takes the lock internally; callers
    // must NOT hold it. Each mutating command writes and flushes the WAL
    // BEFORE returning, so the server cannot possibly reply OK to a write that
    // is not already in the log.
    bool                      set(const std::string& key, const std::string& value);
    bool                      get(const std::string& key, std::string& out);
    int                       del(const std::string& key);      // 1 / 0 / -1 == WAL failure
    int                       expire(const std::string& key, long long seconds); // 1 / 0 / -1
    long long                 ttl(const std::string& key);      // secs / -1 / -2
    std::vector<std::string>  keys(const std::string& pattern);
    size_t                    dbsize();

    // ---- Replay path. Called only during startup, before any socket exists.
    // These deliberately do NOT write to the WAL: the WAL handle is not even
    // open for append yet, which is what makes it structurally impossible to
    // append to a log we are in the middle of reading.
    void replay_set(const std::string& key, const std::string& value, long long expire_at_ms);
    void replay_del(const std::string& key);
    void replay_expire(const std::string& key, long long expire_at_ms);

    // Called AFTER the WAL is open for append: drops keys whose absolute
    // deadline has already passed, then trims to the memory limit. Because the
    // log is now open, the evictions this performs ARE logged, keeping the log
    // and memory consistent from the very first byte of uptime.
    void finish_replay();

    void   start_reaper();
    void   stop_reaper();       // idempotent
    size_t memory_used();

private:
    using ListIt = std::list<Entry>::iterator;

    // All *_locked helpers assume mu_ is already held by the caller.
    bool   expired_locked(const ListIt& it) const;
    void   touch_locked(const ListIt& it);
    void   erase_locked(ListIt it);
    void   insert_or_update_locked(const std::string& key,
                                   const std::string& value,
                                   long long expire_at_ms);
    void   evict_if_needed_locked(bool log_evictions);
    void   sweep_expired_locked();
    size_t entry_cost(const std::string& k, const std::string& v) const;
    void   reaper_loop();

    Wal&   wal_;
    size_t mem_limit_;
    size_t mem_used_ = 0;

    // -----------------------------------------------------------------------
    // THE O(1) LRU, and why it is O(1).
    //
    //   lru_   : a doubly linked list. FRONT == most recently used,
    //            BACK == least recently used (the next eviction victim).
    //   map_   : key -> ITERATOR INTO lru_.
    //
    // A "touch" (promote a key to most-recently-used) is:
    //     lru_.splice(lru_.begin(), lru_, it);
    //
    // Mechanically, splice does not copy, move, reallocate, or search. The list
    // node holding this entry is unlinked from between its two neighbours and
    // relinked at the head, by rewriting FOUR pointers:
    //     prev->next = next;      next->prev = prev;      (unlink)
    //     node->next = head;      head->prev = node;      (relink at front)
    // Four pointer writes, independent of how many entries the list holds.
    // That is the O(1). It has nothing to do with std::list being a standard
    // container -- a hand-rolled intrusive list does the identical four writes.
    //
    // THE GUARANTEE THE WHOLE DESIGN RESTS ON: the C++ standard promises that
    // splice invalidates NO iterators or references -- iterators to the moved
    // element remain valid and simply now refer into the destination list. That
    // is why the iterators cached in map_ survive every touch. If splice
    // invalidated iterators, every entry in map_ would dangle after the first
    // GET and this pattern would be unusable. (This guarantee is not accidental
    // history: LWG issue 250 exists specifically because the original wording
    // did invalidate them, and it was changed because doing so "defeats an
    // important feature of splice".)
    //
    // Eviction is therefore also O(1): take lru_.back(), which is by
    // construction the least recently used entry, read its .key, erase it from
    // map_, and pop it off the list.
    // -----------------------------------------------------------------------
    std::list<Entry>                           lru_;
    std::unordered_map<std::string, ListIt>    map_;

    // -----------------------------------------------------------------------
    // THE LOCK.
    //
    // One global mutex covering map_, lru_, mem_used_, AND the WAL append and
    // flush. The WAL write happens INSIDE this lock, not outside it.
    //
    // Why inside: if the log write were outside, two concurrent SETs on the
    // same key could commit to memory in one order and to the log in the
    // opposite order, and replay would then rebuild a DIFFERENT final value
    // than the running server actually had. The log would be a lie. Holding one
    // lock across both the append and the memory mutation makes log order and
    // apply order the same total order by construction.
    //
    // The price, stated honestly: flush latency sits inside the critical
    // section, so all writers serialise behind disk I/O. That is the primary
    // throughput bottleneck of this design and it is the correct thing to
    // trade for a WAL that cannot disagree with memory.
    //
    // Why not std::shared_mutex? Because GET is not a reader. GET splices the
    // entry to the front of lru_, which MUTATES the list. A shared lock on GET
    // would be an outright data race.
    // -----------------------------------------------------------------------
    std::mutex mu_;

    // The reaper sleeps on its OWN mutex + condvar, and only briefly acquires
    // mu_ to sweep. It therefore cannot starve client threads, and on shutdown
    // it wakes immediately instead of sleeping out its remaining second.
    std::thread             reaper_;
    std::mutex              reaper_mu_;
    std::condition_variable reaper_cv_;

    // Written by the shutdown path, read by the reaper thread: a plain bool
    // here would be a data race.
    std::atomic<bool>       running_{false};

    // Approximate per-entry bookkeeping overhead in bytes: the list node's two
    // pointers, the map node's pointer and cached hash, two std::string control
    // blocks, and allocator headers. A single calibrated constant, not a
    // measurement. See README Limitations -- this is a MODELLED budget, not
    // real RSS.
    static constexpr size_t ENTRY_OVERHEAD = 96;
};
```

## `store.cpp`

```cpp
#include "store.h"
#include "wal.h"

#include <algorithm>
#include <chrono>

long long now_ms() {
    // system_clock, NOT steady_clock. See the long comment in store.h.
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

Store::Store(Wal& wal, size_t memory_limit_bytes)
    : wal_(wal), mem_limit_(memory_limit_bytes) {}

Store::~Store() { stop_reaper(); }

size_t Store::entry_cost(const std::string& k, const std::string& v) const {
    return k.size() + v.size() + ENTRY_OVERHEAD;
}

bool Store::expired_locked(const ListIt& it) const {
    return it->expire_at_ms != 0 && it->expire_at_ms <= now_ms();
}

void Store::touch_locked(const ListIt& it) {
    // O(1) promote-to-front. Four pointer rewrites; no copy, no search, no
    // iterator invalidation. See the mechanism comment in store.h.
    lru_.splice(lru_.begin(), lru_, it);
}

void Store::erase_locked(ListIt it) {
    // Read it->key BEFORE erasing the list node -- afterwards the node, and the
    // string living inside it, are gone.
    mem_used_ -= entry_cost(it->key, it->value);
    map_.erase(it->key);
    lru_.erase(it);
}

void Store::insert_or_update_locked(const std::string& key,
                                    const std::string& value,
                                    long long expire_at_ms) {
    auto mit = map_.find(key);
    if (mit != map_.end()) {
        ListIt it = mit->second;
        mem_used_ -= entry_cost(it->key, it->value);
        it->value         = value;
        it->expire_at_ms  = expire_at_ms;
        mem_used_ += entry_cost(it->key, it->value);
        touch_locked(it);                 // a write is an access
    } else {
        lru_.push_front(Entry{key, value, expire_at_ms});
        map_[key] = lru_.begin();
        mem_used_ += entry_cost(key, value);
    }
}

void Store::evict_if_needed_locked(bool log_evictions) {
    // Evict from the BACK -- by construction the least recently used entry.
    //
    // The "size() > 1" guard means the entry we just inserted is never itself
    // evicted by the same operation that created it. A single entry larger than
    // the whole memory limit is therefore kept and the budget is knowingly
    // exceeded, rather than a SET succeeding and instantly vanishing. Deliberate
    // simplification; real Redis would return an OOM error instead.
    while (mem_used_ > mem_limit_ && lru_.size() > 1) {
        ListIt victim = std::prev(lru_.end());

        // Evictions ARE logged. Replay cannot re-derive them: eviction order
        // depends on recency, recency depends on GETs, and GETs are not in the
        // log. Without an explicit record, a restart would resurrect evicted
        // keys and drop surviving ones -- "recovered state == pre-crash state"
        // would simply be false. One extra log line is a cheap price for that
        // property. (What we still cannot reproduce is the LRU ORDERING after
        // replay; see README Limitations.)
        if (log_evictions) wal_.append_evict(victim->key);

        erase_locked(victim);
    }
}

void Store::sweep_expired_locked() {
    const long long now = now_ms();
    for (auto it = lru_.begin(); it != lru_.end(); ) {
        if (it->expire_at_ms != 0 && it->expire_at_ms <= now) {
            mem_used_ -= entry_cost(it->key, it->value);
            map_.erase(it->key);
            it = lru_.erase(it);          // erase returns the next iterator
        } else {
            ++it;
        }
    }
    // Expirations are deliberately NOT written to the WAL. They are fully
    // derivable: the absolute deadline is already recorded, and replay refuses
    // to resurrect anything past its deadline. Logging them would be redundant
    // bytes in a log that already grows forever.
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

bool Store::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(mu_);

    // SET clears any existing TTL, mirroring real Redis: replacing the value at
    // a key removes its timeout.
    const long long no_expiry = 0;

    // ORDER MATTERS, and all of it is inside the one lock:
    //   1. log the intent
    //   2. apply it to memory
    //   3. evict if we are over budget, logging each victim
    //   4. ONE flush covering all of the above
    // The caller cannot build a reply until this function returns, so the
    // record is durable in the OS before the client ever sees "OK".
    wal_.append_set(key, value, no_expiry);
    insert_or_update_locked(key, value, no_expiry);
    evict_if_needed_locked(true);
    return wal_.flush();
}

bool Store::get(const std::string& key, std::string& out) {
    std::lock_guard<std::mutex> lk(mu_);

    auto mit = map_.find(key);
    if (mit == map_.end()) return false;

    ListIt it = mit->second;

    // LAZY EXPIRY, and the ordering rule that makes it correct: check the
    // deadline BEFORE touching the LRU. An expired key must not be resurrected
    // and must NOT count as a use -- splicing a dead key to the front would
    // promote a corpse to most-recently-used and corrupt the eviction order.
    if (expired_locked(it)) { erase_locked(it); return false; }

    touch_locked(it);          // live hit: a read IS an access
    out = it->value;
    return true;
}

int Store::del(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);

    auto mit = map_.find(key);
    if (mit == map_.end()) return 0;

    ListIt it = mit->second;
    if (expired_locked(it)) {
        // Already logically gone. Reap it, but do NOT log a DEL -- the deadline
        // in the log already accounts for it.
        erase_locked(it);
        return 0;
    }

    // DEL must be logged. Without it, replay would faithfully re-apply the SET
    // that created this key and resurrect something deliberately deleted.
    wal_.append_del(key);
    erase_locked(it);
    return wal_.flush() ? 1 : -1;
}

int Store::expire(const std::string& key, long long seconds) {
    std::lock_guard<std::mutex> lk(mu_);

    auto mit = map_.find(key);
    if (mit == map_.end()) return 0;                       // Redis: 0

    ListIt it = mit->second;
    if (expired_locked(it)) { erase_locked(it); return 0; }

    // Redis semantics: EXPIRE with a non-positive timeout DELETES the key
    // rather than expiring it.
    if (seconds <= 0) {
        wal_.append_del(key);
        erase_locked(it);
        return wal_.flush() ? 1 : -1;
    }

    // ABSOLUTE deadline, computed once, here. The relative "seconds" the client
    // sent never reaches the log. See the WAL comments for why.
    const long long at = now_ms() + seconds * 1000;

    // EXPIRE must be logged. Without it, a key that got its TTL after its SET
    // would replay with the SET record's expire_at_ms of 0 and become immortal.
    wal_.append_expire(key, at);
    it->expire_at_ms = at;
    touch_locked(it);          // an explicit operation on this key is a use
    return wal_.flush() ? 1 : -1;
}

long long Store::ttl(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);

    auto mit = map_.find(key);
    if (mit == map_.end()) return -2;                      // Redis: missing key

    ListIt it = mit->second;
    if (expired_locked(it)) { erase_locked(it); return -2; }

    if (it->expire_at_ms == 0) return -1;                  // Redis: no expiry set

    // NOTE: TTL deliberately does NOT touch the LRU. It is introspection, not
    // use. Real Redis reads the TTL with an explicit no-touch flag for the same
    // reason.
    long long remaining = it->expire_at_ms - now_ms();
    if (remaining < 0) remaining = 0;
    return (remaining + 500) / 1000;   // round to nearest second, as Redis does
}

std::vector<std::string> Store::keys(const std::string& pattern) {
    std::lock_guard<std::mutex> lk(mu_);

    sweep_expired_locked();   // never report a logically dead key

    std::vector<std::string> out;
    const bool prefix_mode = !pattern.empty() && pattern.back() == '*';
    const std::string needle =
        prefix_mode ? pattern.substr(0, pattern.size() - 1) : pattern;

    // O(n) full scan, and it deliberately does NOT touch the LRU. Touching
    // every key here would reset all recency at once and destroy the eviction
    // signal entirely -- one KEYS call would make the LRU meaningless.
    for (const Entry& e : lru_) {
        if (prefix_mode) {
            if (e.key.size() >= needle.size() &&
                e.key.compare(0, needle.size(), needle) == 0) {
                out.push_back(e.key);
            }
        } else if (e.key == needle) {
            out.push_back(e.key);
        }
    }

    // Sorted for a deterministic demo. Real Redis returns hash-table order.
    std::sort(out.begin(), out.end());
    return out;
}

size_t Store::dbsize() {
    std::lock_guard<std::mutex> lk(mu_);
    // O(n) because we reap first, so the count is exact. Redis's DBSIZE is O(1)
    // and can transiently over-count keys that are logically expired but not
    // yet reaped. Accuracy chosen over asymptotics at this scale; documented.
    sweep_expired_locked();
    return map_.size();
}

size_t Store::memory_used() {
    std::lock_guard<std::mutex> lk(mu_);
    return mem_used_;
}

// ---------------------------------------------------------------------------
// Replay path -- startup only, single-threaded, WAL not yet open for append
// ---------------------------------------------------------------------------

void Store::replay_set(const std::string& key, const std::string& value,
                       long long expire_at_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    insert_or_update_locked(key, value, expire_at_ms);
    // No eviction here on purpose: trimming mid-replay would discard entries
    // that a LATER record might have deleted anyway, and it would evict by a
    // half-built recency order. finish_replay() does one trim at the end.
}

void Store::replay_del(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto mit = map_.find(key);
    if (mit != map_.end()) erase_locked(mit->second);
}

void Store::replay_expire(const std::string& key, long long expire_at_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    auto mit = map_.find(key);
    if (mit != map_.end()) mit->second->expire_at_ms = expire_at_ms;
}

void Store::finish_replay() {
    std::lock_guard<std::mutex> lk(mu_);

    // Anything whose ABSOLUTE deadline has already passed is dropped, not
    // resurrected. This is the payoff for storing absolute timestamps: a log
    // written three days ago replays to exactly the state it described, not to
    // a state where every TTL restarts from now.
    sweep_expired_locked();

    // The WAL is open for append by the time we get here, so these evictions
    // ARE logged -- memory and log agree from the first byte of uptime.
    evict_if_needed_locked(true);
    wal_.flush();
}

// ---------------------------------------------------------------------------
// Active expiry (the reaper)
// ---------------------------------------------------------------------------

void Store::start_reaper() {
    running_.store(true);
    reaper_ = std::thread(&Store::reaper_loop, this);
}

void Store::stop_reaper() {
    running_.store(false);
    reaper_cv_.notify_all();          // wake it NOW, don't wait out the second
    if (reaper_.joinable()) reaper_.join();
}

void Store::reaper_loop() {
    // ACTIVE EXPIRY. Why this exists alongside the lazy check in get()/ttl():
    // lazy expiry alone LEAKS. A key that is written, given a TTL, and then
    // never read again is never examined again, so its memory is held forever
    // -- and worse, in a memory-bounded store a dead key sits in the recency
    // list and can cause a LIVE key to be evicted in its place.
    //
    // Conversely, active expiry alone is not sufficient for CORRECTNESS: this
    // sweep runs on a one-second period, so a read arriving between a key's
    // deadline and the next sweep would return data that should already be
    // gone. The lazy check closes that window to zero.
    //
    // Lazy gives correctness on the read path; active bounds memory on the
    // never-read path. Real Redis pairs them for exactly these reasons.
    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lk(reaper_mu_);
            reaper_cv_.wait_for(lk, std::chrono::seconds(1),
                                [this] { return !running_.load(); });
        }
        if (!running_.load()) break;

        std::lock_guard<std::mutex> lk(mu_);
        sweep_expired_locked();
    }
}
```

## `wal.h`

```cpp
#pragma once
//
// wal.h — the write-ahead log.
//
// FORMAT (append-only, one record per line, space-separated, cat-able):
//
//   SET <expire_at_ms> <key> <value...>
//   DEL <key>
//   EXPIRE <expire_at_ms> <key>
//   EVICT <key>
//
// <expire_at_ms> is ABSOLUTE system_clock milliseconds since the Unix epoch,
// or 0 for "no expiry". <key> is a single token (the wire protocol forbids
// spaces in keys, so no length prefix is needed). <value...> is the rest of
// the line, so values may contain spaces; they can never contain '\n' because
// the wire protocol is newline-framed.
//
// THREADING: this class contains NO locking, on purpose. Every method is called
// only from Store, and only while Store::mu_ is held. Putting a second lock in
// here would be redundant and would invite lock-ordering bugs.
//
#include <fstream>
#include <string>

class Store;

class Wal {
public:
    explicit Wal(std::string path);
    ~Wal();

    Wal(const Wal&)            = delete;
    Wal& operator=(const Wal&) = delete;

    // PHASE 1: read the existing log and rebuild the store. Must be called
    // BEFORE open_for_append(). Never throws, never aborts startup: a torn
    // final record or a malformed line is logged to stderr and skipped.
    void replay(Store& store);

    // PHASE 2: open once, in append mode, and hold the handle for the process
    // lifetime. Reopening per write would add an open+close syscall pair to
    // every SET for no benefit -- std::ios::app already guarantees each write
    // lands at the current end of file. Opening only AFTER replay makes it
    // structurally impossible to append to a log we are still reading.
    bool open_for_append();

    void append_set(const std::string& key, const std::string& value, long long expire_at_ms);
    void append_del(const std::string& key);
    void append_expire(const std::string& key, long long expire_at_ms);
    void append_evict(const std::string& key);

    // Pushes the stream buffer out via write(2), i.e. into the OS page cache.
    //
    // WHAT THIS DOES AND DOES NOT GUARANTEE: after flush() the data survives
    // the death of THIS PROCESS (kill -9, segfault, SIGKILL) because the kernel
    // holds it. It does NOT survive the death of the MACHINE -- for that you
    // need fsync(), which pushes through to the physical device and blocks
    // until the device acknowledges.
    //
    // We deliberately do not fsync. Two reasons: (1) the demo threat model is
    // exactly kill -9, where a flush to the OS is precisely sufficient; (2)
    // there is no portable way to get a file descriptor out of a std::ofstream,
    // so fsync would mean abandoning <fstream> for POSIX open/write/fsync and
    // dragging platform #ifdefs into this file -- which would destroy the
    // "one small compatibility header" property this project is built around.
    //
    // Stated honestly in the README: survives process crashes, not power loss.
    bool flush();

    void close();

private:
    static bool apply_record(const std::string& record, Store& store);

    std::string   path_;
    std::ofstream out_;
};
```

## `wal.cpp`

```cpp
#include "wal.h"
#include "store.h"

#include <iostream>
#include <vector>

Wal::Wal(std::string path) : path_(std::move(path)) {}

Wal::~Wal() { close(); }

// Returns true if the file's last byte is '\n' (or the file is empty/absent).
// This is how a torn record is detected: a record killed mid-write has no
// terminating newline.
static bool file_ends_with_newline(const std::string& path, bool& exists) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { exists = false; return true; }
    exists = true;
    std::streamoff size = f.tellg();
    if (size <= 0) return true;
    f.seekg(size - 1);
    char c = 0;
    f.get(c);
    return c == '\n';
}

void Wal::replay(Store& store) {
    bool exists   = false;
    bool ends_nl  = file_ends_with_newline(path_, exists);

    if (!exists) {
        std::cerr << "[wal] no existing log at " << path_ << " -- starting empty\n";
        return;
    }

    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        std::cerr << "[wal] WARNING: cannot read " << path_ << " -- starting empty\n";
        return;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);

    // TORN FINAL RECORD.
    //
    // kill -9 can land halfway through a write, leaving a truncated last line.
    // std::getline happily returns that fragment as if it were a real line, so
    // we detect it by checking whether the file actually ended with a newline.
    //
    // The rule is: DISCARD it and keep going. A write-ahead log whose last
    // record is incomplete is a log whose last operation was never acknowledged
    // to the client -- so dropping it is not data loss, it is correctness. What
    // would be a bug is refusing to start, or crashing, because the tail is
    // ragged. A recovery path that can be defeated by an ordinary crash is not
    // a recovery path.
    if (!ends_nl && !lines.empty()) {
        std::cerr << "[wal] torn final record discarded: \"" << lines.back() << "\"\n";
        lines.pop_back();
    }

    size_t applied = 0, skipped = 0;
    for (const std::string& rec : lines) {
        if (rec.empty()) continue;
        if (apply_record(rec, store)) {
            ++applied;
        } else {
            ++skipped;
            std::cerr << "[wal] skipping malformed record: \"" << rec << "\"\n";
        }
    }

    std::cerr << "[wal] replayed " << applied << " record(s), skipped "
              << skipped << "\n";
}

bool Wal::apply_record(const std::string& rec, Store& store) {
    size_t p = rec.find(' ');
    if (p == std::string::npos) return false;

    const std::string op   = rec.substr(0, p);
    const std::string rest = rec.substr(p + 1);

    if (op == "SET") {
        size_t a = rest.find(' ');
        if (a == std::string::npos) return false;

        long long at = 0;
        size_t consumed = 0;
        const std::string ts = rest.substr(0, a);
        try { at = std::stoll(ts, &consumed); } catch (...) { return false; }
        if (consumed != ts.size()) return false;

        const std::string tail = rest.substr(a + 1);
        size_t b = tail.find(' ');
        if (b == std::string::npos) return false;          // need key AND value

        const std::string key   = tail.substr(0, b);
        const std::string value = tail.substr(b + 1);
        if (key.empty() || value.empty()) return false;

        // 'at' is an ABSOLUTE deadline read straight from the log. We do NOT
        // recompute it from "now". If the log says this key was due to die at
        // 14:03:11 UTC on Tuesday, that is when it dies, whether we are
        // replaying eleven seconds or eleven days later. Storing a relative
        // TTL here would mean a three-day-old log re-granted every key a fresh
        // lease starting at replay time -- the log would stop being a record of
        // what happened and become a script that re-enacts it at the wrong
        // moment. finish_replay() then drops anything already past its
        // deadline, so expired keys are never resurrected.
        store.replay_set(key, value, at);
        return true;
    }

    if (op == "DEL" || op == "EVICT") {
        // EVICT is a distinct record type purely so the log reads clearly when
        // you cat it; on replay it is simply a delete.
        if (rest.empty() || rest.find(' ') != std::string::npos) return false;
        store.replay_del(rest);
        return true;
    }

    if (op == "EXPIRE") {
        size_t a = rest.find(' ');
        if (a == std::string::npos) return false;

        long long at = 0;
        size_t consumed = 0;
        const std::string ts = rest.substr(0, a);
        try { at = std::stoll(ts, &consumed); } catch (...) { return false; }
        if (consumed != ts.size()) return false;

        const std::string key = rest.substr(a + 1);
        if (key.empty() || key.find(' ') != std::string::npos) return false;

        store.replay_expire(key, at);
        return true;
    }

    return false;   // unknown opcode
}

bool Wal::open_for_append() {
    out_.open(path_, std::ios::out | std::ios::app | std::ios::binary);
    if (!out_) {
        std::cerr << "[wal] FATAL: cannot open " << path_ << " for append\n";
        return false;
    }
    return true;
}

void Wal::append_set(const std::string& key, const std::string& value,
                     long long expire_at_ms) {
    out_ << "SET " << expire_at_ms << ' ' << key << ' ' << value << '\n';
}

void Wal::append_del(const std::string& key) {
    out_ << "DEL " << key << '\n';
}

void Wal::append_expire(const std::string& key, long long expire_at_ms) {
    out_ << "EXPIRE " << expire_at_ms << ' ' << key << '\n';
}

void Wal::append_evict(const std::string& key) {
    out_ << "EVICT " << key << '\n';
}

bool Wal::flush() {
    if (!out_.is_open()) return false;
    out_.flush();
    if (!out_) {
        std::cerr << "[wal] ERROR: write or flush failed\n";
        return false;
    }
    return true;
}

void Wal::close() {
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
}
```

## `server.cpp`

```cpp
//
// server.cpp — miniredis-server
//
// main(), argument parsing, WAL replay, the listening socket, signal handling,
// the accept loop, one thread per client, the TCP line-framing loop, command
// dispatch, and reply formatting.
//
#include "net_compat.h"
#include "protocol.h"
#include "store.h"
#include "wal.h"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Shutdown plumbing
// ---------------------------------------------------------------------------

static socket_t                     g_listen_fd = INVALID_SOCK;
static volatile std::sig_atomic_t   g_shutdown  = 0;

// A signal handler may only portably touch a volatile sig_atomic_t. It must not
// allocate, must not lock, must not use iostreams. Setting the flag alone is
// not enough, though: the main thread is blocked in accept() and would never
// look at the flag. shutdown() on the listening socket wakes that accept() with
// an error, at which point the loop checks the flag and exits cleanly.
//
// We shutdown() rather than close() here on purpose: close() would free the fd
// NUMBER while another thread might be about to receive it from a new socket(),
// a classic fd-reuse race. shutdown() leaves the number reserved; main() closes
// it afterwards, once nothing is blocked on it.
extern "C" void on_signal(int) {
    g_shutdown = 1;
    if (g_listen_fd != INVALID_SOCK) ::shutdown(g_listen_fd, SHUT_RDWR);
}

// ---------------------------------------------------------------------------
// Dispatch: Command -> reply string (see protocol.h for the reply grammar)
// ---------------------------------------------------------------------------

static std::string dispatch(Store& store, const Command& cmd) {
    if (!cmd.ok) return "ERR " + cmd.error + "\n";

    switch (cmd.type) {
    case CmdType::SET:
        // store.set() does not return until the record is flushed, so this
        // "OK" can never precede the log entry that justifies it.
        return store.set(cmd.key, cmd.value) ? "OK\n" : "ERR wal write failed\n";

    case CmdType::GET: {
        std::string v;
        return store.get(cmd.key, v) ? ("VALUE " + v + "\n") : "NIL\n";
    }

    case CmdType::DEL: {
        int r = store.del(cmd.key);
        if (r < 0) return "ERR wal write failed\n";
        return "INT " + std::to_string(r) + "\n";
    }

    case CmdType::EXPIRE: {
        int r = store.expire(cmd.key, cmd.seconds);
        if (r < 0) return "ERR wal write failed\n";
        return "INT " + std::to_string(r) + "\n";
    }

    case CmdType::TTL:
        // -2 == no such key, -1 == key exists with no expiry. Mirrors Redis.
        return "INT " + std::to_string(store.ttl(cmd.key)) + "\n";

    case CmdType::KEYS: {
        std::vector<std::string> ks = store.keys(cmd.value);
        std::string out = "KEYS " + std::to_string(ks.size()) + "\n";
        for (const std::string& k : ks) out += "KEY " + k + "\n";
        return out;
    }

    case CmdType::DBSIZE:
        return "INT " + std::to_string(store.dbsize()) + "\n";

    default:
        return "ERR unknown command\n";
    }
}

// ---------------------------------------------------------------------------
// Per-client thread
// ---------------------------------------------------------------------------

static void handle_client(socket_t fd, Store* store) {
    LineBuffer lb;
    char buf[4096];

    for (;;) {
        long long n = sock_recv(fd, buf, sizeof(buf));
        if (n <= 0) break;                       // 0 == peer closed, <0 == error

        // -------------------------------------------------------------------
        // LINE FRAMING OVER A BYTE STREAM.
        //
        // TCP does not preserve message boundaries. This single recv() may have
        // delivered half of one command, or three whole commands, or two
        // commands and the first four bytes of a third. So: append everything
        // we got to the per-connection buffer, then drain every COMPLETE line
        // out of it. Whatever partial tail remains simply stays buffered until
        // a later recv() finishes it.
        //
        // The bug this avoids is assuming one recv() == one command, which
        // works perfectly in testing (small commands, fast local loopback) and
        // then fails in production the first time a packet splits.
        // -------------------------------------------------------------------
        lb.append(buf, static_cast<size_t>(n));

        bool close_now = false;
        std::string line;
        while (lb.next_line(line)) {
            Command cmd = parse_command(line);
            if (cmd.is_empty) continue;          // blank line: no reply, stay open
            if (!send_all(fd, dispatch(*store, cmd))) { close_now = true; break; }
        }
        if (close_now) break;

        // Unbounded-line guard: if a peer has streamed more than MAX_LINE_BYTES
        // without a newline, it is broken or hostile. Refuse to keep buffering.
        if (lb.size() > MAX_LINE_BYTES) {
            send_all(fd, "ERR line too long, closing connection\n");
            break;
        }
    }

    closesock(fd);
}

// ---------------------------------------------------------------------------

static void print_usage() {
    std::cout <<
        "miniredis-server — a tiny Redis-like key-value store\n"
        "\n"
        "Usage: miniredis-server [options]\n"
        "  --port <n>            TCP port to listen on        (default 6399)\n"
        "  --memory-limit <n>    approximate byte budget      (default 67108864 = 64 MiB)\n"
        "  --wal <path>          write-ahead log file         (default miniredis.wal)\n"
        "  --help                show this message\n";
}

int main(int argc, char** argv) {
    int         port      = 6399;
    size_t      mem_limit = 64ull * 1024 * 1024;
    std::string wal_path  = "miniredis.wal";

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if      (a == "--port")         port      = std::stoi(next("--port"));
        else if (a == "--memory-limit") mem_limit = static_cast<size_t>(std::stoull(next("--memory-limit")));
        else if (a == "--wal")          wal_path  = next("--wal");
        else if (a == "--help")         { print_usage(); return 0; }
        else { std::cerr << "unknown option: " << a << "\n"; print_usage(); return 2; }
    }

    ignore_sigpipe();      // see net_compat.h -- without this a vanished client kills us
    if (!net_init()) { std::cerr << "network init failed\n"; return 1; }

    // ---- Startup order matters: replay the log, THEN open it for append, THEN
    // trim. Opening only after replay makes it impossible to append to a file
    // we are still reading; trimming only after opening means the evictions the
    // trim performs are themselves logged.
    Wal   wal(wal_path);
    Store store(wal, mem_limit);

    wal.replay(store);
    if (!wal.open_for_append()) return 1;
    store.finish_replay();
    store.start_reaper();

    // ---- Listening socket
    g_listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd == INVALID_SOCK) { std::cerr << "socket() failed\n"; return 1; }

    // SO_REUSEADDR, set BEFORE bind().
    //
    // When a server exits, the kernel keeps its socket in TIME_WAIT (typically
    // 1-4 minutes) to absorb stray in-flight packets from the old connection.
    // Without this option, restarting the server inside that window fails with
    // EADDRINUSE / "Address already in use". Since the demo restarts this
    // server constantly -- including via kill -9 -- that would be intolerable.
    //
    // Caveat worth knowing: on Windows SO_REUSEADDR has DIFFERENT and weaker
    // semantics (it lets a second process bind a port that is actively in use,
    // not merely one in TIME_WAIT). Acceptable for a dev tool; flagged here
    // rather than hidden.
    int yes = 1;
    ::setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<sockopt_arg_t>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(static_cast<unsigned short>(port));

    if (::bind(g_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "bind() failed on port " << port
                  << " -- is something already listening there?\n";
        return 1;
    }
    if (::listen(g_listen_fd, 64) != 0) { std::cerr << "listen() failed\n"; return 1; }

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    // Deliberately NOT printed as an http:// URL: Codespaces auto-forwards a
    // port when it sees a localhost URL in terminal output, and an HTTP
    // forward is useless (and confusing) for a raw TCP protocol.
    std::cout << "miniredis-server listening on 0.0.0.0:" << port
              << "  wal=" << wal_path
              << "  memory-limit=" << mem_limit << " bytes" << std::endl;
    std::cout << "press Ctrl-C to stop" << std::endl;

    while (!g_shutdown) {
        sockaddr_in cli{};
        socklen_t   cl = sizeof(cli);
        socket_t    fd = ::accept(g_listen_fd, reinterpret_cast<sockaddr*>(&cli), &cl);
        if (fd == INVALID_SOCK) {
            if (g_shutdown) break;      // the signal handler woke us: expected
            continue;                   // transient accept error: keep serving
        }
        // ONE THREAD PER CLIENT, detached. The honest ceiling: each thread costs
        // a stack plus a kernel task struct, so this model is fine into the low
        // thousands of connections and falls apart well before 10k. Real Redis
        // solves this with an epoll event loop and a SINGLE command-execution
        // thread -- no locks, no contention, no races, because there is only
        // ever one thing touching the data.
        std::thread(handle_client, fd, &store).detach();
    }

    std::cout << "\nshutting down..." << std::endl;

    closesock(g_listen_fd);
    g_listen_fd = INVALID_SOCK;
    store.stop_reaper();               // wakes the reaper immediately, then joins

    std::cout.flush();

    // We do NOT join the detached client threads, and we exit via _Exit rather
    // than returning from main. Returning would run the destructors of `store`
    // and `wal` while a client thread might still be inside them -- a genuine
    // use-after-free. _Exit terminates immediately and destroys nothing, which
    // is exactly what we want.
    //
    // This is safe for durability precisely because of the WAL ordering rule:
    // every acknowledged write was already flushed before its OK was sent. The
    // worst a mid-write exit can leave behind is a half-written final line --
    // which is the torn-record case the replayer already handles by design.
    std::_Exit(0);
}
```

## `client.cpp`

```cpp
//
// client.cpp — miniredis-cli
//
// A synchronous REPL: read a line from stdin, send it, read one reply, render
// it. Also carries an optional --bench mode.
//
#include "net_compat.h"
#include "protocol.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

static socket_t connect_to(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) {
        return INVALID_SOCK;
    }

    socket_t fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd != INVALID_SOCK &&
        ::connect(fd, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
        closesock(fd);
        fd = INVALID_SOCK;
    }
    ::freeaddrinfo(res);
    return fd;
}

// Reads one reply and prints it in human-readable form.
//
// This is where the wire's type tags get stripped. The wire carries
// "VALUE Rahul" so that a value of literally "nil" is unambiguous; the human
// sees "Rahul". Returns false if the connection died.
static bool print_reply(socket_t fd, LineBuffer& lb) {
    std::string line;
    if (!recv_line(fd, lb, line)) {
        std::cout << "(connection closed by server)\n";
        return false;
    }

    if (line == "OK")  { std::cout << "OK\n";    return true; }
    if (line == "NIL") { std::cout << "(nil)\n"; return true; }

    if (line.rfind("VALUE ", 0) == 0) { std::cout << line.substr(6) << "\n"; return true; }
    if (line.rfind("INT ",   0) == 0) { std::cout << line.substr(4) << "\n"; return true; }
    if (line.rfind("ERR ",   0) == 0) { std::cout << "(error) " << line.substr(4) << "\n"; return true; }

    if (line.rfind("KEYS ", 0) == 0) {
        // Count-prefixed, so we know exactly how many more lines to read. No
        // terminator sentinel, and therefore no way for a key to be mistaken
        // for one.
        long long n = 0;
        try { n = std::stoll(line.substr(5)); } catch (...) { n = 0; }
        if (n <= 0) { std::cout << "(empty)\n"; return true; }
        for (long long i = 0; i < n; ++i) {
            std::string k;
            if (!recv_line(fd, lb, k)) {
                std::cout << "(connection closed by server)\n";
                return false;
            }
            std::cout << (i + 1) << ") "
                      << (k.rfind("KEY ", 0) == 0 ? k.substr(4) : k) << "\n";
        }
        return true;
    }

    std::cout << line << "\n";   // unrecognised tag: show it raw
    return true;
}

// OPTIONAL EXTRA (not part of the seven commands): a throughput measurement.
// It issues only SETs, adds no server code and no new protocol, and gives the
// README one quantifiable number. Delete this function and the --bench argument
// if you do not want it; nothing else references either.
static int run_bench(socket_t fd, long long n) {
    LineBuffer lb;
    const std::string value = "benchmark-value-0123456789";

    // steady_clock HERE, and system_clock for expiry deadlines in the server.
    // Each is used for exactly what it is for: steady_clock is monotonic and
    // immune to NTP steps, which is what you want when measuring an elapsed
    // interval; it is useless for a persisted timestamp because its epoch is
    // unspecified. system_clock is the reverse. Same header, opposite jobs.
    const auto t0 = std::chrono::steady_clock::now();

    for (long long i = 0; i < n; ++i) {
        const std::string cmd = "SET bench:" + std::to_string(i) + " " + value + "\n";
        if (!send_all(fd, cmd)) { std::cerr << "send failed at op " << i << "\n"; return 1; }
        std::string reply;
        if (!recv_line(fd, lb, reply)) { std::cerr << "recv failed at op " << i << "\n"; return 1; }
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    std::cout << n << " SET ops in " << secs << " s = "
              << (secs > 0 ? static_cast<double>(n) / secs : 0.0)
              << " ops/sec (single client, flush-on-every-write)\n";
    return 0;
}

static void print_usage() {
    std::cout <<
        "miniredis-cli — terminal client for miniredis-server\n"
        "\n"
        "Usage: miniredis-cli [options]\n"
        "  --host <h>    server host   (default 127.0.0.1)\n"
        "  --port <n>    server port   (default 6399)\n"
        "  --bench <n>   send n SET commands, report ops/sec, exit\n"
        "  --help        show this message\n";
}

int main(int argc, char** argv) {
    std::string host  = "127.0.0.1";
    std::string port  = "6399";
    long long   bench = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::cerr << "missing value for " << what << "\n"; std::exit(2); }
            return argv[++i];
        };
        if      (a == "--host")  host  = next("--host");
        else if (a == "--port")  port  = next("--port");
        else if (a == "--bench") bench = std::stoll(next("--bench"));
        else if (a == "--help")  { print_usage(); return 0; }
        else { std::cerr << "unknown option: " << a << "\n"; print_usage(); return 2; }
    }

    if (!net_init())    { std::cerr << "network init failed\n"; return 1; }
    ignore_sigpipe();

    socket_t fd = connect_to(host, port);
    if (fd == INVALID_SOCK) {
        std::cerr << "could not connect to " << host << ":" << port
                  << " -- is miniredis-server running?\n";
        return 1;
    }

    if (bench > 0) {
        int rc = run_bench(fd, bench);
        closesock(fd);
        return rc;
    }

    std::cout << "connected to " << host << ":" << port << "\n"
              << "commands: SET GET DEL EXPIRE TTL KEYS DBSIZE   (QUIT to exit)\n";

    LineBuffer  lb;
    std::string line;

    for (;;) {
        std::cout << "miniredis> " << std::flush;
        if (!std::getline(std::cin, line)) break;              // EOF / Ctrl-D
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        // QUIT/EXIT are handled entirely client-side. They are NOT an eighth
        // command and never reach the server.
        std::string first = line.substr(0, line.find(' '));
        for (char& c : first) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (first == "QUIT" || first == "EXIT") break;

        if (!send_all(fd, line + "\n"))  { std::cout << "(connection closed by server)\n"; break; }
        if (!print_reply(fd, lb))        break;
    }

    closesock(fd);
    return 0;
}
```

## `build.sh`

```bash
#!/usr/bin/env bash
# Convenience script holding the exact g++ commands. This is NOT a build system
# and is not required -- you can paste either line by hand.
set -euo pipefail

g++ -std=c++17 -O2 -Wall -Wextra -pthread -o miniredis-server \
    server.cpp store.cpp wal.cpp protocol.cpp

g++ -std=c++17 -O2 -Wall -Wextra -pthread -o miniredis-cli \
    client.cpp protocol.cpp

echo "built: miniredis-server  miniredis-cli"
```

## `build.bat`

```bat
@echo off
REM Native Windows build with MinGW-w64 g++.
REM Differences from the Linux build: link -lws2_32 for Winsock2, and drop
REM -pthread (MinGW's std::thread needs a POSIX-threads build of the toolchain,
REM which MSYS2's mingw-w64-x86_64-gcc is).

g++ -std=c++17 -O2 -Wall -Wextra -o miniredis-server.exe server.cpp store.cpp wal.cpp protocol.cpp -lws2_32
if errorlevel 1 exit /b 1

g++ -std=c++17 -O2 -Wall -Wextra -o miniredis-cli.exe client.cpp protocol.cpp -lws2_32
if errorlevel 1 exit /b 1

echo built: miniredis-server.exe  miniredis-cli.exe
```

## `.gitignore`

```gitignore
# binaries
miniredis-server
miniredis-cli
*.exe

# objects
*.o
*.obj

# runtime state — the WAL is a runtime artifact, never commit it
*.wal
```

## `LICENSE`

```
MIT License

Copyright (c) 2026 Rahul

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## `README.md`

````markdown
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
