#include "doctest.h"
#include "kvstore/command.h"

using kvstore::CommandType;
using kvstore::parse_command;

TEST_CASE("parses SET with a value containing spaces") {
    auto cmd = parse_command("SET greeting hello world");
    CHECK(cmd.type == CommandType::Set);
    CHECK(cmd.key == "greeting");
    CHECK(cmd.value == "hello world");
}

TEST_CASE("command names are case-insensitive") {
    CHECK(parse_command("get name").type == CommandType::Get);
    CHECK(parse_command("Del name").type == CommandType::Delete);
    CHECK(parse_command("exit").type == CommandType::Exit);
    CHECK(parse_command("compact").type == CommandType::Compact);
}

TEST_CASE("rejects malformed commands") {
    CHECK(parse_command("").type == CommandType::Invalid);
    CHECK(parse_command("GET").type == CommandType::Invalid);
    CHECK(parse_command("SET key").type == CommandType::Invalid);
    CHECK(parse_command("FLY key").type == CommandType::Invalid);
}

TEST_CASE("unknown commands are reported as unknown") {
    CHECK(parse_command("fly").error == "unknown command: FLY");
}

TEST_CASE("a SET value ending in EX <seconds> sets an expiry") {
    auto cmd = parse_command("SET session my token EX 60");
    CHECK(cmd.type == CommandType::Set);
    CHECK(cmd.value == "my token");
    CHECK(cmd.expire_seconds == 60);

    CHECK(parse_command("set k v ex 5").expire_seconds == 5);
    CHECK_FALSE(parse_command("SET k v").expire_seconds.has_value());
    CHECK(parse_command("SET k EXTRA 5").value == "EXTRA 5");  // Not the word EX.
}

TEST_CASE("rejects invalid expiry times") {
    CHECK(parse_command("SET k v EX 0").type == CommandType::Invalid);
    CHECK(parse_command("SET k v EX -5").type == CommandType::Invalid);
    CHECK(parse_command("SET k v EX soon").type == CommandType::Invalid);
    CHECK(parse_command("SET k v EX 999999999999").type == CommandType::Invalid);
    CHECK(parse_command("SET k EX 5").error == "SET needs a value");
}

TEST_CASE("parses TTL") {
    auto cmd = parse_command("ttl session");
    CHECK(cmd.type == CommandType::Ttl);
    CHECK(cmd.key == "session");
}
