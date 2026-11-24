// Data written to the log survives reopening the store (stage 2), and damage
// to the log is handled safely (stage 3).

#include <filesystem>
#include <fstream>
#include <chrono>
#include <string>
#include <thread>

#include "doctest.h"
#include "kvstore/log.h"
#include "kvstore/store.h"
#include "temp_log.h"

namespace fs = std::filesystem;
using kvstore::Store;

TEST_CASE("starts empty and creates the log when none exists") {
    TempLog log;
    Store store(log.path());
    CHECK(store.size() == 0);
    CHECK(fs::exists(log.path()));
}

TEST_CASE("data survives reopening the store") {
    TempLog log;
    {
        Store store(log.path());
        store.set("name", "Alisha");
        store.set("city", "Denver");
    }
    Store reopened(log.path());
    CHECK(reopened.size() == 2);
    CHECK(reopened.get("name") == "Alisha");
    CHECK(reopened.get("city") == "Denver");
}

TEST_CASE("overwrites and deletes survive reopening") {
    TempLog log;
    {
        Store store(log.path());
        store.set("color", "blue");
        store.set("color", "green");
        store.set("temp", "x");
        store.remove("temp");
    }
    Store reopened(log.path());
    CHECK(reopened.get("color") == "green");
    CHECK_FALSE(reopened.get("temp").has_value());
    CHECK(reopened.size() == 1);
}

TEST_CASE("values with spaces and newlines survive reopening") {
    TempLog log;
    {
        Store store(log.path());
        store.set("poem", "roses are red\nviolets are blue");
    }
    Store reopened(log.path());
    CHECK(reopened.get("poem") == "roses are red\nviolets are blue");
}

TEST_CASE("deleting a missing key doesn't write to the log") {
    TempLog log;
    Store store(log.path());
    CHECK_FALSE(store.remove("missing"));
    CHECK(fs::file_size(log.path()) == 0);
}

TEST_CASE("recovers from an entry cut off by a crash, and keeps later writes") {
    TempLog log;
    {
        Store store(log.path());
        store.set("saved", "yes");
    }
    // Simulate a crash partway through writing the next entry.
    std::string entry = kvstore::encode_set("lost", "never");
    std::ofstream(log.path(), std::ios::app | std::ios::binary)
        << entry.substr(0, entry.size() - 4);

    {
        Store store(log.path());
        CHECK(store.get("saved") == "yes");
        CHECK(store.size() == 1);
        store.set("after", "crash");
    }
    Store reopened(log.path());
    CHECK(reopened.get("after") == "crash");
    CHECK(read_file(log.path()).find("lost") == std::string::npos);
}

TEST_CASE("works without fsync") {
    TempLog log;
    {
        Store store(log.path(), {.sync = kvstore::SyncMode::None});
        store.set("fast", "write");
    }
    Store reopened(log.path());
    CHECK(reopened.get("fast") == "write");
}

TEST_CASE("a damaged last entry is trimmed like a cut-off write") {
    TempLog log;
    {
        Store store(log.path());
        store.set("kept", "yes");
        store.set("damaged", "hello");
    }
    std::string bytes = read_file(log.path());
    bytes[bytes.rfind("hello")] ^= 0x01;
    write_file(log.path(), bytes);

    Store reopened(log.path());
    CHECK(reopened.get("kept") == "yes");
    CHECK_FALSE(reopened.get("damaged").has_value());
}

TEST_CASE("refuses to open a log damaged in the middle, and leaves it unchanged") {
    TempLog log;
    {
        Store store(log.path());
        store.set("a", "1");
        store.set("b", "hello");
        store.set("c", "3");
    }
    std::string bytes = read_file(log.path());
    bytes[bytes.find("hello")] ^= 0x01;
    write_file(log.path(), bytes);

    CHECK_THROWS_WITH_AS(Store(log.path()), doctest::Contains("corrupted"), std::runtime_error);
    CHECK(read_file(log.path()) == bytes);
}

TEST_CASE("compaction shrinks the log and keeps the data") {
    TempLog log;
    {
        Store store(log.path());
        for (int i = 0; i < 100; ++i) {
            store.set("counter", std::to_string(i));
        }
        store.set("temp", "x");
        store.remove("temp");
        store.set("name", "Alisha");

        auto before = fs::file_size(log.path());
        store.compact();
        CHECK(fs::file_size(log.path()) < before / 10);
        CHECK(store.get("counter") == "99");

        // Writes after compaction go to the new log.
        store.set("after", "compaction");
    }
    Store reopened(log.path());
    CHECK(reopened.size() == 3);
    CHECK(reopened.get("counter") == "99");
    CHECK(reopened.get("name") == "Alisha");
    CHECK(reopened.get("after") == "compaction");
    CHECK_FALSE(fs::exists(log.path().string() + ".compact"));
}

TEST_CASE("compacting an empty store leaves an empty log") {
    TempLog log;
    Store store(log.path());
    store.set("a", "1");
    store.remove("a");
    store.compact();
    CHECK(fs::file_size(log.path()) == 0);
}

TEST_CASE("a leftover file from a crash during compaction is ignored") {
    TempLog log;
    {
        Store store(log.path());
        store.set("real", "data");
    }
    // A crash before the rename leaves a partial compaction file behind.
    std::string leftover = log.path().string() + ".compact";
    write_file(leftover, "S 4 5 0000");

    Store reopened(log.path());
    CHECK(reopened.get("real") == "data");
    CHECK_FALSE(fs::exists(leftover));
}

TEST_CASE("batched syncing saves data, and syncs in the background") {
    TempLog log;
    {
        kvstore::StoreOptions options;
        options.sync = kvstore::SyncMode::Batched;
        options.sync_interval = std::chrono::milliseconds(5);
        Store store(log.path(), options);
        for (int i = 0; i < 100; ++i) {
            store.set("key" + std::to_string(i), std::to_string(i));
            if (i % 10 == 0) {
                // Give the background thread a chance to run mid-stream.
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }
    Store reopened(log.path());
    CHECK(reopened.size() == 100);
    CHECK(reopened.get("key99") == "99");
}
