#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace kvstore {

enum class CommandType { Set, Get, Delete, Ttl, Compact, Help, Exit, Invalid };

struct Command {
    CommandType type = CommandType::Invalid;
    std::string key;
    std::string value;
    std::optional<std::int64_t> expire_seconds;  // From "SET key value EX <seconds>".
    std::string error;  // Set when type is Invalid.
};

// Parses one line of user input, such as "SET name Alisha" or "get name".
// Command names are case-insensitive. A SET value is the rest of the line,
// so it may contain spaces. A value ending in "EX <seconds>" sets an expiry
// instead, so a value can't itself end with those words.
Command parse_command(const std::string& line);

}  // namespace kvstore
