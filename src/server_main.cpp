// Network server for the key-value store.
//
// Usage: ./bin/kv-server [port] [log file]    (defaults: 7379, data.log)
//
// Connect with any line-based client, for example: nc localhost 7379

#include <pthread.h>
#include <signal.h>

#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "kvstore/server.h"
#include "kvstore/store.h"

int main(int argc, char* argv[]) {
    std::string log_path = argc > 2 ? argv[2] : "data.log";

    // Block Ctrl+C and kill signals in every thread, then wait for them below,
    // so the server can shut down cleanly instead of stopping mid-command.
    //
    // A process can start with these signals ignored (shells do this for
    // background jobs), and an ignored signal never reaches sigwait. Restore
    // the default first so Ctrl+C always works.
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    std::unique_ptr<kvstore::Store> store;
    std::unique_ptr<kvstore::Server> server;
    try {
        int port = argc > 1 ? std::stoi(argv[1]) : 7379;
        store = std::make_unique<kvstore::Store>(log_path);
        server = std::make_unique<kvstore::Server>(*store, static_cast<std::uint16_t>(port));
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
    std::cout << "Loaded " << store->size() << " keys from " << log_path << ".\n"
              << "Listening on 127.0.0.1:" << server->port() << ". Press Ctrl+C to stop.\n";

    std::thread accept_thread([&] { server->run(); });

    int received;
    sigwait(&signals, &received);
    std::cout << "\nShutting down...\n";
    server->stop();
    accept_thread.join();
    return 0;
}
