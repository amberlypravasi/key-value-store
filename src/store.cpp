#include "kvstore/store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <stdexcept>
#include <system_error>

#include "kvstore/log.h"

namespace kvstore {

namespace {

[[noreturn]] void throw_errno(const std::string& what, int error = errno) {
    throw std::system_error(error, std::generic_category(), what);
}

// write() may write only part of the buffer, so loop until it's all out.
void write_all(int fd, const std::string& bytes) {
    const char* data = bytes.data();
    std::size_t left = bytes.size();
    while (left > 0) {
        ssize_t written = ::write(fd, data, left);
        if (written < 0) {
            if (errno == EINTR) continue;
            throw_errno("could not write to log");
        }
        data += written;
        left -= static_cast<std::size_t>(written);
    }
}

// On macOS, fsync hands data to the drive but the drive may still cache it.
// fcntl(F_FULLFSYNC) forces it all the way to storage, at a cost.
void sync_fd(int fd, const std::string& what) {
    if (::fsync(fd) != 0) {
        throw_errno("could not sync " + what);
    }
}

// A rename is only durable once the folder that holds the file is synced.
void sync_directory(const std::filesystem::path& file) {
    std::filesystem::path dir = file.parent_path();
    int fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        throw_errno("could not open folder " + dir.string());
    }
    int result = ::fsync(fd);
    ::close(fd);
    if (result != 0) {
        throw_errno("could not sync folder " + dir.string());
    }
}

// Reads a whole file with one read() call per chunk. Much faster than reading
// through an istream one character at a time. Returns false if it doesn't exist.
bool read_file(const std::filesystem::path& path, std::string& out) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return false;
        throw_errno("could not open log " + path.string());
    }
    struct stat info;
    if (::fstat(fd, &info) != 0) {
        int error = errno;
        ::close(fd);
        throw_errno("could not read log " + path.string(), error);
    }
    out.resize(static_cast<std::size_t>(info.st_size));
    std::size_t done = 0;
    while (done < out.size()) {
        ssize_t n = ::read(fd, out.data() + done, out.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            int error = errno;
            ::close(fd);
            throw_errno("could not read log " + path.string(), error);
        }
        done += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return true;
}

}  // namespace

std::int64_t system_now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

Store::Store(StoreOptions options) : options_(std::move(options)) {}

Store::Store(const std::filesystem::path& log_path, StoreOptions options)
    : options_(std::move(options)), log_path_(log_path) {
    // A leftover compaction file means a crash happened before it replaced the
    // log. The log is still complete, so the leftover can be deleted.
    std::filesystem::remove(compact_path());

    std::size_t valid_bytes = replay();
    log_fd_ = ::open(log_path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (log_fd_ < 0) {
        throw_errno("could not open log " + log_path_.string());
    }

    // Cut off any partial entry at the end. Otherwise new entries would be
    // appended after it, and replay would stop before reaching them.
    if (std::filesystem::file_size(log_path_) > valid_bytes &&
        ::ftruncate(log_fd_, static_cast<off_t>(valid_bytes)) != 0) {
        int error = errno;
        ::close(log_fd_);
        throw_errno("could not trim log " + log_path_.string(), error);
    }

    if (options_.sync == SyncMode::Batched) {
        sync_thread_ = std::thread(&Store::sync_loop, this);
    }
}

Store::~Store() {
    if (sync_thread_.joinable()) {
        {
            std::lock_guard lock(sync_mutex_);
            stop_syncing_ = true;
        }
        sync_wakeup_.notify_one();
        sync_thread_.join();
    }
    if (log_fd_ >= 0) {
        // On a clean shutdown, save any writes the background sync hasn't yet.
        if (unsynced_writes_) {
            ::fsync(log_fd_);
        }
        ::close(log_fd_);
    }
}

void Store::set(const std::string& key, const std::string& value,
                std::optional<std::chrono::milliseconds> ttl) {
    std::unique_lock lock(mutex_);
    std::int64_t now = options_.now_ms();
    purge_expired(now);

    std::optional<std::int64_t> expires_at;
    if (ttl) {
        expires_at = now + ttl->count();
    }
    append_to_log(encode_set(key, value, expires_at));
    apply_set(key, value, expires_at);
}

std::optional<std::string> Store::get(const std::string& key) const {
    std::shared_lock lock(mutex_);
    auto it = data_.find(key);
    if (it == data_.end() || is_expired(it->second, options_.now_ms())) {
        return std::nullopt;
    }
    return it->second.value;
}

bool Store::remove(const std::string& key) {
    std::unique_lock lock(mutex_);
    purge_expired(options_.now_ms());
    if (!data_.contains(key)) {
        return false;
    }
    append_to_log(encode_delete(key));
    apply_remove(key);
    return true;
}

std::size_t Store::size() const {
    std::shared_lock lock(mutex_);
    // Expired keys stay in memory until the next write purges them, so
    // subtract any that have expired since. expiries_ is sorted by time, so
    // this only looks at those keys.
    std::int64_t now = options_.now_ms();
    std::size_t expired = 0;
    for (auto it = expiries_.begin(); it != expiries_.end() && it->first <= now; ++it) {
        ++expired;
    }
    return data_.size() - expired;
}

std::int64_t Store::ttl_ms(const std::string& key) const {
    std::shared_lock lock(mutex_);
    std::int64_t now = options_.now_ms();
    auto it = data_.find(key);
    if (it == data_.end() || is_expired(it->second, now)) {
        return -2;
    }
    if (!it->second.expires_at_ms) {
        return -1;
    }
    return *it->second.expires_at_ms - now;
}

void Store::compact() {
    std::unique_lock lock(mutex_);
    if (log_fd_ < 0) {
        return;
    }
    purge_expired(options_.now_ms());

    // 1. Write the current data to a separate file and sync it to disk.
    //    If anything fails here, the original log is untouched.
    std::filesystem::path temp = compact_path();
    int temp_fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    if (temp_fd < 0) {
        throw_errno("could not create " + temp.string());
    }
    try {
        std::string bytes;
        for (const auto& [key, entry] : data_) {
            bytes += encode_set(key, entry.value, entry.expires_at_ms);
        }
        write_all(temp_fd, bytes);
        sync_fd(temp_fd, temp.string());
    } catch (...) {
        ::close(temp_fd);
        std::filesystem::remove(temp);
        throw;
    }

    // 2. Swap it in. rename() replaces the log in one step, so after a crash
    //    the log is either entirely old or entirely new, never half of each.
    if (std::rename(temp.c_str(), log_path_.c_str()) != 0) {
        int error = errno;
        ::close(temp_fd);
        std::filesystem::remove(temp);
        throw_errno("could not replace log with " + temp.string(), error);
    }

    // 3. The open file now *is* the log, so keep appending to it. Switch
    //    before syncing the folder, so that if the sync fails, later writes
    //    still go to the new log and not to the old, replaced one.
    ::close(log_fd_);
    log_fd_ = temp_fd;
    unsynced_writes_ = false;
    sync_directory(log_path_);
}

std::uintmax_t Store::log_size() const {
    std::shared_lock lock(mutex_);
    struct stat info;
    if (log_fd_ < 0 || ::fstat(log_fd_, &info) != 0) {
        return 0;
    }
    return static_cast<std::uintmax_t>(info.st_size);
}

std::size_t Store::replay() {
    std::string bytes;
    if (!read_file(log_path_, bytes)) {
        return 0;  // No log yet, so start empty.
    }

    DecodedLog log = decode_log(bytes);
    if (log.corrupt) {
        // Trimming here would silently throw away every entry after the
        // damage. Stop instead, and leave the file as it is for inspection.
        throw std::runtime_error("log " + log_path_.string() + " is corrupted at byte " +
                                 std::to_string(log.valid_bytes) +
                                 "; not starting, so no data is lost");
    }

    for (LogEntry& entry : log.entries) {
        if (entry.op == LogEntry::Op::Set) {
            apply_set(entry.key, std::move(entry.value), entry.expires_at_ms);
        } else {
            apply_remove(entry.key);
        }
    }
    // Drop keys that expired while the store was closed.
    purge_expired(options_.now_ms());
    return log.valid_bytes;
}

void Store::append_to_log(const std::string& entry) {
    if (log_fd_ < 0) {
        return;
    }
    // Report a failed background sync on the next write, since the
    // background thread has no caller to report it to.
    if (int error = sync_error_.exchange(0)) {
        throw_errno("an earlier background sync of " + log_path_.string() + " failed", error);
    }
    write_all(log_fd_, entry);
    if (options_.sync == SyncMode::EveryWrite) {
        sync_fd(log_fd_, log_path_.string());
    } else if (options_.sync == SyncMode::Batched) {
        unsynced_writes_ = true;
    }
}

std::filesystem::path Store::compact_path() const {
    return log_path_.string() + ".compact";
}

void Store::apply_set(const std::string& key, std::string value,
                      std::optional<std::int64_t> expires_at_ms) {
    Entry& entry = data_[key];
    if (entry.expires_at_ms) {
        expiries_.erase({*entry.expires_at_ms, key});
    }
    entry.value = std::move(value);
    entry.expires_at_ms = expires_at_ms;
    if (expires_at_ms) {
        expiries_.insert({*expires_at_ms, key});
    }
}

void Store::apply_remove(const std::string& key) {
    auto it = data_.find(key);
    if (it == data_.end()) {
        return;
    }
    if (it->second.expires_at_ms) {
        expiries_.erase({*it->second.expires_at_ms, key});
    }
    data_.erase(it);
}

void Store::purge_expired(std::int64_t now) {
    // No delete is logged: the log already records each key's expiry, so
    // replay drops expired keys on its own.
    while (!expiries_.empty() && expiries_.begin()->first <= now) {
        data_.erase(expiries_.begin()->second);
        expiries_.erase(expiries_.begin());
    }
}

bool Store::is_expired(const Entry& entry, std::int64_t now) const {
    return entry.expires_at_ms && *entry.expires_at_ms <= now;
}

void Store::sync_loop() {
    std::unique_lock lock(sync_mutex_);
    while (!stop_syncing_) {
        sync_wakeup_.wait_for(lock, options_.sync_interval, [this] { return stop_syncing_; });
        if (!unsynced_writes_.exchange(false)) {
            continue;
        }
        // Sync a duplicate of the log's file descriptor, so writers only wait
        // for the moment it takes to duplicate it, not for the whole fsync.
        int fd;
        {
            std::shared_lock data_lock(mutex_);
            fd = ::dup(log_fd_);
        }
        if (fd < 0 || ::fsync(fd) != 0) {
            sync_error_ = errno;
        }
        if (fd >= 0) {
            ::close(fd);
        }
    }
}

}  // namespace kvstore
