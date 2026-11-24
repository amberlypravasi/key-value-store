#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <unordered_set>

#include "kvstore/store.h"

namespace kvstore {

// A TCP server that speaks the same line-based commands as the command line.
// Each line a client sends gets a one-line response (HELP's is several lines).
// Each client is served by its own thread.
//
// It listens only on 127.0.0.1, so only programs on this computer can connect.
// There is no authentication, so it shouldn't be exposed to a network.
//
// Responses are one line each, so a value containing a newline would break
// the protocol. Values sent over the network can't contain newlines, since a
// newline ends the command.
class Server {
public:
    // Starts listening right away. Pass port 0 to let the OS pick a free port,
    // then read it with port().
    Server(Store& store, std::uint16_t port);

    // Calls stop(). If run() is going on another thread, wait for it to return
    // before destroying the server.
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    std::uint16_t port() const { return port_; }

    // Accepts clients until stop() is called.
    void run();

    // Stops accepting clients, disconnects the current ones, and waits for
    // their threads to finish. Safe to call from any thread, more than once.
    void stop();

private:
    void serve_client(int fd);

    Store& store_;
    int listen_fd_ = -1;
    int wake_pipe_[2] = {-1, -1};  // Written to by stop() to wake up run().
    std::uint16_t port_ = 0;

    std::mutex mutex_;
    std::condition_variable clients_done_;
    std::unordered_set<int> clients_;  // Sockets of connected clients.
    bool stopping_ = false;
};

}  // namespace kvstore
