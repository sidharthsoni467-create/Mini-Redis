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
