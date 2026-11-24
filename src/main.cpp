// Interactive command line for the key-value store.
//
// Usage: ./bin/kv [log file]    (default: data.log)

#include <exception>
#include <iostream>
#include <memory>
#include <string>

#include "kvstore/command.h"
#include "kvstore/handler.h"
#include "kvstore/store.h"

int main(int argc, char* argv[]) {
    std::string log_path = argc > 1 ? argv[1] : "data.log";
    std::unique_ptr<kvstore::Store> store;
    try {
        store = std::make_unique<kvstore::Store>(log_path);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
    std::cout << "key-value-store. Loaded " << store->size() << " keys from " << log_path
              << ". Type HELP for commands.\n";

    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        kvstore::Command cmd = kvstore::parse_command(line);
        if (cmd.type == kvstore::CommandType::Exit) break;
        if (cmd.type == kvstore::CommandType::Invalid && line.empty()) continue;
        std::cout << kvstore::handle_command(*store, cmd) << "\n";
    }
    return 0;
}
