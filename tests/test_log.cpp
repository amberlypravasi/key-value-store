#include "doctest.h"
#include "kvstore/crc32.h"
#include "kvstore/log.h"

using kvstore::decode_log;
using kvstore::encode_delete;
using kvstore::encode_set;
using kvstore::LogEntry;

TEST_CASE("crc32 matches the standard check value") {
    CHECK(kvstore::crc32("123456789") == 0xCBF43926u);
    CHECK(kvstore::crc32("") == 0u);
}

TEST_CASE("encodes entries with lengths and a checksum before the data") {
    std::string set = encode_set("name", "Alisha");
    CHECK(set.starts_with("S 4 6 "));
    CHECK(set.ends_with("\nnameAlisha\n"));
    CHECK(set.size() == std::string("S 4 6 12345678\nnameAlisha\n").size());

    std::string del = encode_delete("name");
    CHECK(del.starts_with("D 4 "));
    CHECK(del.ends_with("\nname\n"));
}

TEST_CASE("decodes what was encoded, including spaces and newlines") {
    std::string bytes = encode_set("a b", "line 1\nline 2") + encode_delete("a b") +
                        encode_set("", "");
    auto log = decode_log(bytes);

    REQUIRE(log.entries.size() == 3);
    CHECK(log.entries[0].op == LogEntry::Op::Set);
    CHECK(log.entries[0].key == "a b");
    CHECK(log.entries[0].value == "line 1\nline 2");
    CHECK(log.entries[1].op == LogEntry::Op::Delete);
    CHECK(log.entries[2].key == "");
    CHECK(log.valid_bytes == bytes.size());
    CHECK_FALSE(log.corrupt);
}

TEST_CASE("an entry cut off partway is not corruption") {
    std::string complete = encode_set("a", "1");
    std::string full = complete + encode_set("b", "2");

    // Try every possible cut point inside the second entry.
    for (std::size_t cut = complete.size(); cut < full.size(); ++cut) {
        auto log = decode_log(full.substr(0, cut));
        CHECK(log.entries.size() == 1);
        CHECK(log.valid_bytes == complete.size());
        CHECK_FALSE(log.corrupt);
    }
}

TEST_CASE("a damaged byte in the last entry is treated like a cut-off write") {
    std::string first = encode_set("a", "1");
    std::string bytes = first + encode_set("b", "hello");
    bytes[bytes.size() - 3] ^= 0x01;  // Flip one bit in "hello".

    auto log = decode_log(bytes);
    CHECK(log.entries.size() == 1);
    CHECK(log.valid_bytes == first.size());
    CHECK_FALSE(log.corrupt);
}

TEST_CASE("a damaged byte with entries after it is corruption") {
    std::string first = encode_set("a", "1");
    std::string bytes = first + encode_set("b", "hello") + encode_set("c", "3");
    bytes[bytes.find("hello")] ^= 0x01;

    auto log = decode_log(bytes);
    CHECK(log.entries.size() == 1);
    CHECK(log.valid_bytes == first.size());
    CHECK(log.corrupt);
}

TEST_CASE("no single-bit flip in an entry produces wrong data") {
    std::string entry = encode_set("key", "value");
    std::string next = encode_set("x", "y");
    for (std::size_t i = 0; i < entry.size(); ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            std::string damaged = entry;
            damaged[i] ^= static_cast<char>(1 << bit);
            auto log = decode_log(damaged + next);
            // Some flips are harmless, like 'a' -> 'A' in the hex checksum.
            // Those may decode, but only to the original key and value.
            if (!log.entries.empty()) {
                CHECK(log.entries[0].key == "key");
                CHECK(log.entries[0].value == "value");
            }
        }
    }
}

TEST_CASE("a malformed header is corruption") {
    CHECK(decode_log("X 1 00000000\na\n").corrupt);
    CHECK(decode_log("S abc\n").corrupt);
    CHECK(decode_log("S 1 1\nab\n").corrupt);  // Missing checksum.
}

TEST_CASE("encodes and decodes a set with an expiry") {
    std::string bytes = encode_set("k", "v", 1700000000123);
    CHECK(bytes.starts_with("T 1 1 1700000000123 "));

    auto log = decode_log(bytes);
    REQUIRE(log.entries.size() == 1);
    CHECK(log.entries[0].op == LogEntry::Op::Set);
    CHECK(log.entries[0].expires_at_ms == 1700000000123);
    CHECK_FALSE(decode_log(encode_set("k", "v")).entries[0].expires_at_ms.has_value());
}

TEST_CASE("headers must match the format exactly") {
    std::string good = encode_set("k", "v");
    CHECK(decode_log(good).entries.size() == 1);
    // Extra spaces, or extra fields, are rejected.
    CHECK(decode_log("S  1 1 00000000\nkv\n").corrupt);
    CHECK(decode_log(good.substr(0, good.find('\n')) + " 7\nkv\n").corrupt);
}
