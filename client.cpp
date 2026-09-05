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
