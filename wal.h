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
