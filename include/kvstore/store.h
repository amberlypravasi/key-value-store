#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace kvstore {

// When writes are synced to disk with fsync. A write that hasn't been synced
// survives the program crashing (the OS still has it), but not a power loss.
enum class SyncMode {
    EveryWrite,  // Each write waits for its own fsync. Safest and slowest.
    Batched,     // A background thread syncs every `sync_interval`. A power loss
                 // can lose up to that much recent data.
    None,        // Never sync; leave it to the OS. Fastest.
};

// Returns the current Unix time in milliseconds.
std::int64_t system_now_ms();

struct StoreOptions {
    SyncMode sync = SyncMode::EveryWrite;
    std::chrono::milliseconds sync_interval{100};  // Only used with Batched.
    // The clock used for expiring keys. Tests replace it with a fake one.
    std::function<std::int64_t()> now_ms = system_now_ms;
};

// A key-value store backed by a hash map, so set, get, and remove take O(1)
// time on average.
//
// With a log file, every change is appended to a write-ahead log before it is
// applied in memory, and the log is replayed on startup to rebuild the data.
//
// All methods are safe to call from many threads at once. Reads run in
// parallel; writes run one at a time.
class Store {
public:
    // Keeps data in memory only. Nothing is saved.
    Store() = default;
    explicit Store(StoreOptions options);

    // Saves changes to the log at `log_path`, creating it if needed, and loads
    // any data already in it.
    //
    // Throws if the log is corrupted, rather than starting with missing data.
    explicit Store(const std::filesystem::path& log_path, StoreOptions options = {});

    ~Store();

    // A Store owns an open file, so it can't be copied.
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // Stores `value` under `key`, replacing any existing value and expiry.
    // With `ttl`, the key expires and disappears after that much time.
    void set(const std::string& key, const std::string& value,
             std::optional<std::chrono::milliseconds> ttl = std::nullopt);

    // Returns the value for `key`, or std::nullopt if the key doesn't exist
    // or has expired.
    std::optional<std::string> get(const std::string& key) const;

    // Removes `key`. Returns true if it existed, false otherwise.
    bool remove(const std::string& key);

    // Returns how many keys are stored, not counting expired ones.
    std::size_t size() const;

    // Returns the milliseconds until `key` expires, -1 if it never expires,
    // or -2 if it doesn't exist. (Redis uses the same convention.)
    std::int64_t ttl_ms(const std::string& key) const;

    // Rewrites the log so it holds only the current value of each key,
    // dropping overwritten, deleted, and expired entries. Safe to crash at any
    // point: the old log stays in place until the new one is complete.
    void compact();

    // Returns the size of the log file in bytes, or 0 if there is no log.
    std::uintmax_t log_size() const;

private:
    struct Entry {
        std::string value;
        std::optional<std::int64_t> expires_at_ms;
    };

    // Loads the log into memory and returns how many bytes of it were valid.
    std::size_t replay();
    void append_to_log(const std::string& entry);
    std::filesystem::path compact_path() const;

    // Updates memory only. Callers hold the write lock.
    void apply_set(const std::string& key, std::string value,
                   std::optional<std::int64_t> expires_at_ms);
    void apply_remove(const std::string& key);
    // Removes keys whose expiry has passed. Callers hold the write lock.
    void purge_expired(std::int64_t now);
    bool is_expired(const Entry& entry, std::int64_t now) const;

    // For SyncMode::Batched: wakes every sync_interval and syncs if needed.
    void sync_loop();

    StoreOptions options_;

    // Readers take a shared lock; writers take an exclusive one. A writer holds
    // it while appending to the log *and* updating memory, so the order of
    // entries in the log always matches the order changes were applied.
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Entry> data_;
    // Keys with an expiry, ordered by when they expire, so expired keys can
    // be found without scanning every key.
    std::set<std::pair<std::int64_t, std::string>> expiries_;

    std::filesystem::path log_path_;
    int log_fd_ = -1;  // -1 when there is no log.

    // Background syncing, for SyncMode::Batched.
    std::thread sync_thread_;
    std::mutex sync_mutex_;
    std::condition_variable sync_wakeup_;
    bool stop_syncing_ = false;
    std::atomic<bool> unsynced_writes_{false};
    std::atomic<int> sync_error_{0};  // errno from a failed background sync.
};

}  // namespace kvstore
