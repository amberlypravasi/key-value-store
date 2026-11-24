#include "kvstore/command.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>

namespace kvstore {

namespace {

// The longest expiry allowed: 10 years, in seconds.
constexpr std::int64_t kMaxExpireSeconds = 10LL * 365 * 24 * 60 * 60;

Command invalid(const std::string& error) {
    Command cmd;
    cmd.error = error;
    return cmd;
}

std::string to_upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return text;
}

std::string trim_right(const std::string& text) {
    std::size_t end = text.find_last_not_of(" \t");
    return end == std::string::npos ? "" : text.substr(0, end + 1);
}

// If a SET value ends with "EX <seconds>", moves the seconds into `cmd` and
// removes them from the value. Returns an error message if the seconds aren't
// valid, or an empty string otherwise.
std::string split_expiry(Command& cmd) {
    std::string value = trim_right(cmd.value);
    std::size_t number_start = value.find_last_of(" \t");
    if (number_start == std::string::npos) return "";

    std::string before = trim_right(value.substr(0, number_start));
    std::size_t ex_start = before.find_last_of(" \t");
    ex_start = ex_start == std::string::npos ? 0 : ex_start + 1;
    if (to_upper(before.substr(ex_start)) != "EX") return "";

    std::string number = value.substr(number_start + 1);
    std::int64_t seconds = 0;
    auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), seconds);
    if (error != std::errc() || end != number.data() + number.size() || seconds < 1 ||
        seconds > kMaxExpireSeconds) {
        return "EX needs a whole number of seconds from 1 to " +
               std::to_string(kMaxExpireSeconds);
    }
    cmd.value = trim_right(before.substr(0, ex_start));
    cmd.expire_seconds = seconds;
    return "";
}

}  // namespace

Command parse_command(const std::string& line) {
    std::istringstream in(line);
    std::string name;
    if (!(in >> name)) {
        return invalid("empty command");
    }
    name = to_upper(name);

    Command cmd;
    if (name == "HELP") {
        cmd.type = CommandType::Help;
        return cmd;
    }
    if (name == "COMPACT") {
        cmd.type = CommandType::Compact;
        return cmd;
    }
    if (name == "EXIT" || name == "QUIT") {
        cmd.type = CommandType::Exit;
        return cmd;
    }

    if (name == "GET") {
        cmd.type = CommandType::Get;
    } else if (name == "DEL" || name == "DELETE") {
        cmd.type = CommandType::Delete;
    } else if (name == "SET") {
        cmd.type = CommandType::Set;
    } else if (name == "TTL") {
        cmd.type = CommandType::Ttl;
    } else {
        return invalid("unknown command: " + name);
    }

    if (!(in >> cmd.key)) {
        return invalid(name + " needs a key");
    }
    if (cmd.type == CommandType::Set) {
        std::getline(in >> std::ws, cmd.value);
        if (std::string error = split_expiry(cmd); !error.empty()) {
            return invalid(error);
        }
        if (cmd.value.empty()) {
            return invalid("SET needs a value");
        }
    }
    return cmd;
}

}  // namespace kvstore
