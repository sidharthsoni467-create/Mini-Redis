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
