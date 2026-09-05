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
