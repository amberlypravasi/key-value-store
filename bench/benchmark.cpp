// Benchmarks for the key-value store.
//
// Usage: ./bin/kv-bench [--quick]
//
// Reports throughput (operations per second) and latency percentiles. p50 is
// the typical operation; p99 is the slowest 1 in 100, which is often what
// users notice.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "kvstore/server.h"
#include "kvstore/store.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using kvstore::Store;
using kvstore::StoreOptions;
using kvstore::SyncMode;

namespace {

double micros(Clock::duration d) {
    return std::chrono::duration<double, std::micro>(d).count();
}

struct Stats {
    double ops_per_sec;
    double p50_us;
    double p99_us;
};

// Turns per-operation latencies and total elapsed time into a Stats.
Stats summarize(std::vector<double> latencies_us, Clock::duration elapsed) {
    std::sort(latencies_us.begin(), latencies_us.end());
    auto percentile = [&](double p) {
        return latencies_us[static_cast<std::size_t>(p * (latencies_us.size() - 1))];
    };
    double seconds = std::chrono::duration<double>(elapsed).count();
    return {latencies_us.size() / seconds, percentile(0.50), percentile(0.99)};
}

void print_row(const std::string& name, const Stats& s) {
    std::printf("  %-34s %12.0f ops/s   p50 %8.1f us   p99 %8.1f us\n", name.c_str(),
                s.ops_per_sec, s.p50_us, s.p99_us);
}

fs::path temp_log(const std::string& name) {
    fs::path path = fs::temp_directory_path() / ("kv_bench_" + name + ".log");
    fs::remove(path);
    fs::remove(path.string() + ".compact");
    return path;
}

std::string value_for(int i) {
    return "value-" + std::to_string(i) + std::string(20, 'x');
}

// --- Writes -----------------------------------------------------------------

Stats bench_writes(SyncMode mode, int count) {
    fs::path path = temp_log("writes");
    StoreOptions options;
    options.sync = mode;
    Stats stats;
    {
        Store store(path, options);
        std::vector<double> latencies;
        latencies.reserve(count);
        auto start = Clock::now();
        for (int i = 0; i < count; ++i) {
            auto t0 = Clock::now();
            store.set("key-" + std::to_string(i), value_for(i));
            latencies.push_back(micros(Clock::now() - t0));
        }
        stats = summarize(std::move(latencies), Clock::now() - start);
    }
    fs::remove(path);
    return stats;
}

// --- Reads ------------------------------------------------------------------

Stats bench_reads(Store& store, int keys, int threads, int reads_per_thread) {
    std::vector<std::vector<double>> latencies(threads);
    std::vector<std::thread> workers;
    auto start = Clock::now();
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            latencies[t].reserve(reads_per_thread);
            for (int i = 0; i < reads_per_thread; ++i) {
                std::string key = "key-" + std::to_string((static_cast<long long>(i) * 7919 + t) % keys);
                auto t0 = Clock::now();
                auto value = store.get(key);
                latencies[t].push_back(micros(Clock::now() - t0));
                if (!value) std::abort();
            }
        });
    }
    for (auto& worker : workers) worker.join();
    auto elapsed = Clock::now() - start;

    std::vector<double> all;
    for (auto& l : latencies) all.insert(all.end(), l.begin(), l.end());
    return summarize(std::move(all), elapsed);
}

// --- Startup ----------------------------------------------------------------

void bench_replay(int entries, int distinct_keys) {
    fs::path path = temp_log("replay");
    {
        Store store(path, {.sync = SyncMode::None});
        for (int i = 0; i < entries; ++i) {
            store.set("key-" + std::to_string(i % distinct_keys), value_for(i));
        }
    }
    std::printf("  log: %d entries, %d keys, %.1f MB\n", entries, distinct_keys,
                fs::file_size(path) / 1e6);
    for (int run = 1; run <= 3; ++run) {
        auto start = Clock::now();
        Store store(path, {.sync = SyncMode::None});
        double ms = micros(Clock::now() - start) / 1000;
        std::printf("  run %d: %.0f ms to load %zu keys\n", run, ms, store.size());
    }
    fs::remove(path);
}

// --- Network ----------------------------------------------------------------

Stats bench_network(int clients, int requests_per_client) {
    Store store;
    kvstore::Server server(store, 0);
    std::thread server_thread([&] { server.run(); });

    std::vector<std::vector<double>> latencies(clients);
    std::vector<std::thread> workers;
    auto start = Clock::now();
    for (int c = 0; c < clients; ++c) {
        workers.emplace_back([&, c] {
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(server.port());
            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) std::abort();

            char reply[256];
            latencies[c].reserve(requests_per_client);
            for (int i = 0; i < requests_per_client; ++i) {
                std::string line = "SET c" + std::to_string(c) + "-" + std::to_string(i) + " " +
                                   value_for(i) + "\n";
                auto t0 = Clock::now();
                ::send(fd, line.data(), line.size(), 0);
                // Each reply is "OK\n", so wait until the newline arrives.
                std::size_t got = 0;
                while (got == 0 || reply[got - 1] != '\n') {
                    ssize_t n = ::recv(fd, reply + got, sizeof reply - got, 0);
                    if (n <= 0) std::abort();
                    got += static_cast<std::size_t>(n);
                }
                latencies[c].push_back(micros(Clock::now() - t0));
            }
            ::close(fd);
        });
    }
    for (auto& worker : workers) worker.join();
    auto elapsed = Clock::now() - start;

    server.stop();
    server_thread.join();

    std::vector<double> all;
    for (auto& l : latencies) all.insert(all.end(), l.begin(), l.end());
    return summarize(std::move(all), elapsed);
}

}  // namespace

int main(int argc, char* argv[]) {
    bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0;
    int scale = quick ? 10 : 1;

    std::printf("Writes, 1 thread, saving to a log:\n");
    print_row("fsync every write", bench_writes(SyncMode::EveryWrite, 20'000 / scale));
    print_row("batched fsync (every 100 ms)", bench_writes(SyncMode::Batched, 200'000 / scale));
    print_row("no fsync", bench_writes(SyncMode::None, 200'000 / scale));

    std::printf("\nReads from 100,000 keys in memory:\n");
    {
        Store store;
        for (int i = 0; i < 100'000; ++i) store.set("key-" + std::to_string(i), value_for(i));
        int reads = 1'000'000 / scale;
        print_row("1 thread", bench_reads(store, 100'000, 1, reads));
        print_row("8 threads", bench_reads(store, 100'000, 8, reads / 8 * 2));
    }

    std::printf("\nStartup (replaying the log):\n");
    bench_replay(1'000'000 / scale, 100'000 / scale);

    std::printf("\nOver TCP, SET round trips, in-memory store:\n");
    print_row("1 client", bench_network(1, 20'000 / scale));
    print_row("16 clients", bench_network(16, 5'000 / scale));
    return 0;
}
