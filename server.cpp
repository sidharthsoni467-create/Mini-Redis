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
