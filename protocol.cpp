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
