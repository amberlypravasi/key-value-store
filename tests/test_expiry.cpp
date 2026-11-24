// Stage 5: keys that expire. These use a fake clock, so they don't sleep.

#include <chrono>
#include <cstdint>
#include <string>

#include "doctest.h"
#include "kvstore/store.h"
#include "temp_log.h"

using namespace std::chrono_literals;
using kvstore::Store;
using kvstore::StoreOptions;

namespace {

// A clock the test moves forward by hand.
struct FakeClock {
    std::int64_t now = 1'000'000;

    StoreOptions options(kvstore::SyncMode sync = kvstore::SyncMode::None) {
        StoreOptions options;
        options.sync = sync;
        options.now_ms = [this] { return now; };
        return options;
    }
};

}  // namespace

TEST_CASE("a key disappears when its time is up") {
    FakeClock clock;
    Store store(clock.options());
    store.set("session", "abc", 10s);

    clock.now += 9'999;
    CHECK(store.get("session") == "abc");
    CHECK(store.size() == 1);

    clock.now += 1;
    CHECK_FALSE(store.get("session").has_value());
    CHECK(store.size() == 0);
}

TEST_CASE("ttl_ms reports time left, -1 for no expiry, and -2 for missing keys") {
    FakeClock clock;
    Store store(clock.options());
    store.set("temp", "x", 5s);
    store.set("forever", "y");

    clock.now += 2'000;
    CHECK(store.ttl_ms("temp") == 3'000);
    CHECK(store.ttl_ms("forever") == -1);
    CHECK(store.ttl_ms("missing") == -2);

    clock.now += 3'000;
    CHECK(store.ttl_ms("temp") == -2);
}

TEST_CASE("setting a key again replaces its expiry") {
    FakeClock clock;
    Store store(clock.options());

    SUBCASE("with no expiry, the key stays forever") {
        store.set("k", "v1", 1s);
        store.set("k", "v2");
        clock.now += 5'000;
        CHECK(store.get("k") == "v2");
        CHECK(store.ttl_ms("k") == -1);
    }
    SUBCASE("with a new expiry, the new one counts") {
        store.set("k", "v1", 1s);
        store.set("k", "v2", 10s);
        clock.now += 5'000;
        CHECK(store.get("k") == "v2");
        CHECK(store.ttl_ms("k") == 5'000);
    }
}

TEST_CASE("deleting an expired key reports that it didn't exist") {
    FakeClock clock;
    Store store(clock.options());
    store.set("k", "v", 1s);
    clock.now += 1'000;
    CHECK_FALSE(store.remove("k"));
}

TEST_CASE("expiry survives a restart") {
    FakeClock clock;
    TempLog log;
    {
        Store store(log.path(), clock.options());
        store.set("short", "a", 5s);
        store.set("long", "b", 60s);
        store.set("forever", "c");
    }
    clock.now += 10'000;  // Time passes while the store is closed.

    Store reopened(log.path(), clock.options());
    CHECK_FALSE(reopened.get("short").has_value());
    CHECK(reopened.get("long") == "b");
    CHECK(reopened.ttl_ms("long") == 50'000);
    CHECK(reopened.get("forever") == "c");
    CHECK(reopened.size() == 2);
}

TEST_CASE("compaction drops expired keys and keeps the expiry of the rest") {
    FakeClock clock;
    TempLog log;
    {
        Store store(log.path(), clock.options());
        store.set("gone", std::string(1000, 'x'), 1s);
        store.set("kept", "y", 60s);
        clock.now += 2'000;
        store.compact();
        CHECK(std::filesystem::file_size(log.path()) < 100);
    }
    Store reopened(log.path(), clock.options());
    CHECK(reopened.size() == 1);
    CHECK(reopened.ttl_ms("kept") == 58'000);
}
