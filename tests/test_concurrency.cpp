// Stage 4: many threads using one Store at the same time.

#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "kvstore/store.h"
#include "temp_log.h"

using kvstore::Store;

namespace {

constexpr int kThreads = 8;
constexpr int kKeysPerThread = 500;

std::string key_for(int thread, int i) {
    return "t" + std::to_string(thread) + "-" + std::to_string(i);
}

// Each thread writes its own keys while also reading other threads' keys.
void write_concurrently(Store& store) {
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&store, t] {
            for (int i = 0; i < kKeysPerThread; ++i) {
                store.set(key_for(t, i), std::to_string(i));
                (void)store.get(key_for((t + 1) % kThreads, i));
                (void)store.size();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
}

void check_all_keys(const Store& store) {
    REQUIRE(store.size() == kThreads * kKeysPerThread);
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kKeysPerThread; ++i) {
            REQUIRE(store.get(key_for(t, i)) == std::to_string(i));
        }
    }
}

}  // namespace

TEST_CASE("concurrent writes to memory aren't lost") {
    Store store;
    write_concurrently(store);
    check_all_keys(store);
}

TEST_CASE("concurrent writes produce a log that replays correctly") {
    TempLog log;
    {
        Store store(log.path(), {.sync = kvstore::SyncMode::None});
        write_concurrently(store);
    }
    // If two writes had ever been interleaved in the file, replay would find
    // a damaged entry here.
    Store reopened(log.path());
    check_all_keys(reopened);
}

TEST_CASE("concurrent overwrites of one key leave the log matching memory") {
    TempLog log;
    std::string final_value;
    {
        Store store(log.path(), {.sync = kvstore::SyncMode::None});
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&store, t] {
                for (int i = 0; i < 200; ++i) {
                    store.set("shared", std::to_string(t) + ":" + std::to_string(i));
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        final_value = *store.get("shared");
    }
    // The last write applied in memory must also be the last one in the log.
    Store reopened(log.path());
    CHECK(reopened.get("shared") == final_value);
}

TEST_CASE("compaction while other threads write doesn't lose data") {
    TempLog log;
    {
        Store store(log.path(), {.sync = kvstore::SyncMode::None});
        std::thread compactor([&store] {
            for (int i = 0; i < 20; ++i) {
                store.compact();
            }
        });
        write_concurrently(store);
        compactor.join();
    }
    Store reopened(log.path());
    check_all_keys(reopened);
}
