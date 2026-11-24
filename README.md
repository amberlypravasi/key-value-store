# key-value-store

A key-value database built from scratch in C++20, with saving to disk, crash recovery,
and a network server.

```
> SET name Alisha
OK
> GET name
Alisha
> DEL name
1
> GET name
(nil)
> SET session abc123 EX 60
OK
> TTL session
60
> COMPACT
OK (log went from 88 to 18 bytes)
```

## Build and run

Requires a C++20 compiler (Apple clang or g++) and `make`.

```bash
make          # build ./bin/kv and ./bin/kv-server
make run      # start the interactive command line, saving to data.log
./bin/kv my.log   # or choose the log file
make test     # run the tests, with memory error checking
make tsan     # run the tests with the data race checker (ThreadSanitizer)
make bench    # run the benchmarks (add --quick to ./bin/kv-bench for a short run)
make clean    # delete build output
```

### Network server

```bash
make server                     # listen on 127.0.0.1:7379, saving to data.log
./bin/kv-server 8000 my.log     # or choose the port and log file
```

Connect from another terminal with `nc localhost 7379` and type the same commands. Many
clients can connect at once. Press Ctrl+C to stop the server cleanly.

The server listens only on `127.0.0.1` and has no authentication, so it's for use on your
own computer.

Tests use [doctest](https://github.com/doctest/doctest) v2.4.11, included as
`third_party/doctest.h` (MIT license).

## Project layout

| Path | What it is |
|---|---|
| `include/kvstore/store.h`, `src/store.cpp` | The store itself: a hash map, plus saving to and loading from the log |
| `include/kvstore/log.h`, `src/log.cpp` | Write-ahead log format: encoding and decoding entries |
| `include/kvstore/crc32.h`, `src/crc32.cpp` | CRC-32 checksums for detecting damaged entries |
| `include/kvstore/command.h`, `src/command.cpp` | Turns a line like `SET name Alisha` into a command |
| `include/kvstore/handler.h`, `src/handler.cpp` | Runs a command and builds the response; shared by the command line and server |
| `include/kvstore/server.h`, `src/server.cpp` | TCP server with one thread per client |
| `src/main.cpp` | Interactive command line |
| `src/server_main.cpp` | Starts the server, and shuts it down cleanly on Ctrl+C |
| `tests/` | Tests for each part, including `test_crash.cpp`, which kills a writing process for real |
| `bench/benchmark.cpp` | Benchmarks: write sync modes, reads, startup, and network |

## Roadmap

Each stage builds on the last. Write tests before or alongside each feature, and commit
often so the history shows how the project grew.

### Stage 1: In-memory store
- [x] Implement `set`, `get`, `remove`, and `size` with `std::unordered_map`
- [x] Make every test in `tests/test_store.cpp` pass

**Design question:** Why did you choose that container? What are the time complexities of
its operations, and when would `std::map` be a better choice?

### Stage 2: Save to disk with a write-ahead log
- [x] Before changing memory, append each `SET` and `DEL` to a log file
- [x] On startup, replay the log to rebuild the store
- [x] Store values that contain spaces or newlines by writing each string's length first
- [x] Add tests: write data, create a new `Store` from the same file, and check the data

Each log entry is a header line with the key and value lengths and a checksum, then the
data. The log stays easy to inspect with `cat`:

```
S 4 6 93e5c202
nameAlisha
D 4 0142347e
name
```

**Design question:** Why write to the log *before* updating memory? What does `fsync` do,
and what does it cost?

### Stage 3: Crash recovery
- [x] Handle a log whose last entry was only partly written, because the program crashed
      mid-write. Startup trims the partial entry so new writes aren't lost after it.
- [x] Add a test that writes a truncated log by hand and checks recovery
- [x] Add a CRC-32 checksum to each entry, so replay can detect entries that are complete
      but damaged
- [x] Test it by actually killing the process (`SIGKILL`) in the middle of a stream of
      writes, five times in a row, and checking every key afterward
- [x] Add compaction (`COMPACT`): rewrite the log so it holds only current values, then
      swap it in safely

**How damage is handled.** A crash can only cut off the *last* entry, so a bad final entry
is trimmed. A bad entry with more data after it means the file itself was damaged. Trimming
there would silently discard every later entry, so the store refuses to start and leaves
the file untouched instead.

**How compaction stays safe.** The current data is written to `data.log.compact` and
synced to disk. Then `rename()` replaces the log in a single step, and the folder is synced
so the rename itself is saved. A crash before the rename leaves the old log intact, and the
leftover file is deleted on the next startup. A crash after the rename leaves the complete
new log.

**Design question:** Why does the store refuse to start on a damaged log instead of
recovering what it can? When might a real database choose differently?

### Stage 4: Network server
- [x] Accept TCP connections, and have each client send the same commands as the command line
- [x] Handle many clients at once with threads
- [x] Protect the store with a lock, and test that concurrent writes don't lose data
- [x] Shut down cleanly: stop accepting, disconnect clients, and wait for their threads

**How locking works.** The store uses a `std::shared_mutex`, a reader-writer lock. Reads take
a shared lock, so they run in parallel. Writes take an exclusive lock and hold it while
appending to the log *and* updating memory. If two writes could interleave those steps, the
log could record them in a different order than memory applied them, and a restart would
bring back the wrong value. A test hammers one key from 8 threads to check for exactly that.

**How the server handles real network input.**
- A command can arrive split across several packets, or several commands in one packet.
  The server buffers input and only handles complete lines.
- A client that disconnects mid-response would normally kill the whole server with
  `SIGPIPE`. The server turns that into an ordinary error instead.
- A client that sends more than 1 MB without a newline is disconnected, so one client
  can't make the server run out of memory.

**Design question:** One thread per client is simple, but what happens with 10,000 clients?
How would a thread pool, or an event loop with `poll`/`epoll`, change the design?

### Stage 5: Measure and extend
- [x] Write a benchmark that reports operations per second and latency (p50 and p99)
- [x] Compare `fsync` on every write vs. batched vs. none, with a choice of `SyncMode`
- [x] Profile startup and make it about 3x faster
- [x] Add expiring keys: `SET key value EX 60`, plus `TTL key`

**Sync modes.** `StoreOptions::sync` picks the tradeoff between speed and safety:

| Mode | When writes reach disk | Can lose on power loss |
|---|---|---|
| `EveryWrite` (default) | Before each write returns | Nothing |
| `Batched` | A background thread syncs every 100 ms | Up to 100 ms of writes |
| `None` | Whenever the OS decides | Possibly several seconds of writes |

In every mode, a *program* crash loses nothing. The data is already in the OS by the time
`set` returns. Only a power loss or OS crash can lose unsynced writes.

The background thread syncs a duplicate of the log's file descriptor, so writers are only
blocked for the instant it takes to duplicate it, not for the whole `fsync`.

**Expiring keys.** The log stores each key's expiry as a wall-clock time
(`T <key length> <value length> <expires at> <checksum>`), so expiry still works after a
restart. Expired keys are hidden from reads right away, and removed from memory on the next
write. Keys with an expiry are also kept in a set sorted by expiry time, so finding expired
keys never requires scanning every key. Compaction drops them from the log. Logs written
before this stage still load, since the `S` and `D` entries didn't change.

One limitation: a `SET` value that ends in `EX <number>` is read as an expiry, so a value
can't itself end with those words.

**Design question:** The read benchmark below shows 8 threads are no faster than 1. Why
doesn't the reader-writer lock let reads scale, and how would splitting the store into
shards, each with its own lock, change that? What would sharding complicate?

## Results

Measured on a MacBook Pro (Apple silicon) with `make bench`, an optimized build. Numbers
vary by machine and from run to run.

### Writes: the cost of durability

| Sync mode | Writes/sec | p50 latency | p99 latency |
|---|---|---|---|
| `fsync` every write | ~10,000 | 114 µs | 178 µs |
| Batched (every 100 ms) | ~116,000 | 6.9 µs | 18 µs |
| No `fsync` | ~133,000 | 6.8 µs | 18 µs |

Batching syncs gets **about 11x the write throughput** of syncing every write, and is
within about 13% of never syncing. Note that `fsync` on macOS doesn't force the drive to
flush its own cache (`F_FULLFSYNC` does), so true every-write durability would cost more
than this table shows.

### Startup: the biggest bottleneck

Replaying a 1-million-entry, 58 MB log:

| | Time |
|---|---|
| Before | ~2,200 ms |
| After | ~750 ms (**about 3x faster**) |

Profiling showed two problems:

1. **Decoding entries took about 1,500 ms.** Each header was parsed by creating a
   `std::istringstream`, and the checksum was checked by rebuilding the header as a new
   string. The new parser reads numbers in place with `std::from_chars`, and checksums the
   header bytes already in memory, with no copies.
2. **Reading the file took about 560 ms.** It was read one character at a time through
   `std::istreambuf_iterator`. Reading it with `read()` into a buffer sized to the file
   takes about 75 ms.

### Reads

| Readers | Reads/sec | p50 latency | p99 latency |
|---|---|---|---|
| 1 thread | ~1,200,000 | 0.6 µs | 1.1 µs |
| 8 threads | ~1,130,000 | 1.1 µs | 55 µs |

More readers didn't help. Every read updates a reader count inside the shared lock, so all
8 threads compete over the same piece of memory. This is the next bottleneck to fix (see
the design question above).

### Over the network

`SET` requests sent over TCP to the server, each waiting for its `OK`:

| Clients | Requests/sec | p50 latency | p99 latency |
|---|---|---|---|
| 1 | ~22,000 | 38 µs | 80 µs |
| 16 | ~58,000 | 199 µs | 1,091 µs |

With one client, most of each request's time is the network round trip, not the store.
More clients keep the server busy in parallel, which raises throughput about 2.6x, at the
cost of each request waiting longer.

## What's next

- **Sharding:** split the store into shards, each with its own lock, so reads scale with
  threads
- **Event loop:** replace one thread per client with `poll`/`kqueue`, to support thousands
  of clients
- **Snapshots:** have compaction run in the background, so writes don't pause during it
- **Replication:** stream the log to a second server, so it can take over if the first fails
