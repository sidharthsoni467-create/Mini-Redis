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
