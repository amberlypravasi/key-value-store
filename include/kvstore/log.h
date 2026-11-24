#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kvstore {

// The write-ahead log is a file of entries, appended in order. Each entry is
// a header line followed by the raw key and value bytes:
//
//   S <key length> <value length> <checksum>\n<key><value>\n           (set)
//   T <key length> <value length> <expires at> <checksum>\n<key><value>\n
//                                                                 (set with expiry)
//   D <key length> <checksum>\n<key>\n                                 (delete)
//
// Writing lengths up front means keys and values can contain any bytes,
// including spaces and newlines. "Expires at" is a Unix time in milliseconds,
// so expiry still works after a restart. The checksum is a CRC-32, in hex, of
// the header text before it plus the key and value, so replay can detect
// damaged entries.

struct LogEntry {
    enum class Op { Set, Delete };

    Op op;
    std::string key;
    std::string value;                         // Empty for deletes.
    std::optional<std::int64_t> expires_at_ms;  // Only for sets with an expiry.
};

struct DecodedLog {
    std::vector<LogEntry> entries;
    // How many bytes at the start of the log were complete, valid entries.
    std::size_t valid_bytes = 0;
    // True if a damaged entry is followed by more data. A crash can only cut
    // off the *last* entry, so this means the file itself was damaged, and the
    // data after the damaged entry can't be trusted or safely thrown away.
    bool corrupt = false;
};

std::string encode_set(const std::string& key, const std::string& value,
                       std::optional<std::int64_t> expires_at_ms = std::nullopt);
std::string encode_delete(const std::string& key);

// Decodes entries until the end of the input, or until it reaches an entry
// that is incomplete, malformed, or fails its checksum.
DecodedLog decode_log(const std::string& bytes);

}  // namespace kvstore
