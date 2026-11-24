#include "kvstore/handler.h"

#include <chrono>
#include <exception>
#include <optional>

namespace kvstore {

namespace {

const char* kHelp =
    "Commands:\n"
    "  SET <key> <value>   Store a value (the value may contain spaces)\n"
    "  SET <key> <value> EX <seconds>\n"
    "                      Store a value that expires after that many seconds\n"
    "  GET <key>           Print a value, or (nil) if the key doesn't exist\n"
    "  DEL <key>           Delete a key\n"
    "  TTL <key>           Seconds until a key expires (-1: never, -2: no such key)\n"
    "  COMPACT             Shrink the log to just the current data\n"
    "  HELP                Show this message\n"
    "  EXIT                Quit";

}  // namespace

std::string handle_command(Store& store, const Command& cmd) {
    try {
        switch (cmd.type) {
            case CommandType::Set: {
                std::optional<std::chrono::milliseconds> ttl;
                if (cmd.expire_seconds) {
                    ttl = std::chrono::seconds(*cmd.expire_seconds);
                }
                store.set(cmd.key, cmd.value, ttl);
                return "OK";
            }
            case CommandType::Get: {
                auto value = store.get(cmd.key);
                return value ? *value : "(nil)";
            }
            case CommandType::Delete:
                return store.remove(cmd.key) ? "1" : "0";
            case CommandType::Ttl: {
                std::int64_t ms = store.ttl_ms(cmd.key);
                // Round up, so a key with 0.3 seconds left reports 1, not 0.
                return std::to_string(ms < 0 ? ms : (ms + 999) / 1000);
            }
            case CommandType::Compact: {
                auto before = store.log_size();
                store.compact();
                return "OK (log went from " + std::to_string(before) + " to " +
                       std::to_string(store.log_size()) + " bytes)";
            }
            case CommandType::Help:
                return kHelp;
            case CommandType::Exit:
                return "";
            case CommandType::Invalid:
                return "ERROR: " + cmd.error;
        }
    } catch (const std::exception& e) {
        // A failed write leaves memory unchanged, so the store is still usable.
        return std::string("ERROR: ") + e.what();
    }
    return "ERROR: unhandled command";
}

}  // namespace kvstore
