// Stage 3: kill the process for real, partway through a stream of writes, and
// check that nothing written before the crash is lost or damaged.

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

#include "doctest.h"
#include "kvstore/store.h"
#include "temp_log.h"

using kvstore::Store;

namespace {

std::string value_for(int i) {
    return "value-" + std::to_string(i) + std::string(static_cast<std::size_t>(i % 50), 'x');
}

// Starts a child process that writes key-<n>, key-<n+1>, ... as fast as it
// can, then kills it with SIGKILL, which can't be caught or cleaned up after.
// `run_for` is measured from when the child starts writing, since loading a
// large log can take a while, especially in this sanitizer-checked build.
void write_until_killed(const std::filesystem::path& path, int start,
                        std::chrono::milliseconds run_for) {
    int ready[2];
    REQUIRE(::pipe(ready) == 0);

    pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::close(ready[0]);
        Store store(path, {.sync = kvstore::SyncMode::None});
        for (int i = start;; ++i) {
            store.set("key-" + std::to_string(i), value_for(i));
            if (i == start) {
                // Tell the parent the first write is done.
                (void)::write(ready[1], "x", 1);
                ::close(ready[1]);
            }
        }
    }

    ::close(ready[1]);
    char byte;
    bool started = ::read(ready[0], &byte, 1) == 1;
    ::close(ready[0]);
    if (started) {
        std::this_thread::sleep_for(run_for);
    }
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    REQUIRE(WIFSIGNALED(status));
}

}  // namespace

TEST_CASE("survives being killed mid-write, repeatedly") {
    TempLog log;
    std::size_t previous = 0;

    for (int round = 0; round < 5; ++round) {
        write_until_killed(log.path(), static_cast<int>(previous),
                           std::chrono::milliseconds(150 + 37 * round));

        Store store(log.path());
        std::size_t count = store.size();
        INFO("round " << round << ": " << count << " keys");

        // Each round must have written more keys, and every key written
        // before the crash must be present with exactly the right value.
        CHECK(count > previous);
        for (std::size_t i = 0; i < count; ++i) {
            auto value = store.get("key-" + std::to_string(i));
            REQUIRE(value.has_value());
            REQUIRE(*value == value_for(static_cast<int>(i)));
        }
        previous = count;
    }
}
