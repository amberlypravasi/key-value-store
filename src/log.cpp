#include "kvstore/log.h"

#include <charconv>
#include <cstdio>
#include <string_view>

#include "kvstore/crc32.h"

namespace kvstore {

namespace {

std::uint32_t entry_checksum(std::string_view fields, std::string_view key,
                             std::string_view value) {
    return crc32(value, crc32(key, crc32(fields)));
}

std::string encode(const std::string& fields, const std::string& key,
                   const std::string& value) {
    char checksum[9];
    std::snprintf(checksum, sizeof checksum, "%08x", entry_checksum(fields, key, value));
    return fields + " " + checksum + "\n" + key + value + "\n";
}

// Reads a header one field at a time, without copying it. Each read fails
// if the next field isn't exactly what's expected.
class HeaderReader {
public:
    explicit HeaderReader(std::string_view text) : text_(text) {}

    bool op(char& out) {
        if (pos_ >= text_.size()) return false;
        out = text_[pos_++];
        return true;
    }

    bool number(std::uint64_t& out, int base = 10) {
        if (pos_ >= text_.size() || text_[pos_] != ' ') return false;
        const char* begin = text_.data() + pos_ + 1;
        const char* end = text_.data() + text_.size();
        auto [next, error] = std::from_chars(begin, end, out, base);
        if (error != std::errc() || next == begin) return false;
        pos_ = static_cast<std::size_t>(next - text_.data());
        return true;
    }

    bool at_end() const { return pos_ == text_.size(); }
    std::size_t pos() const { return pos_; }

private:
    std::string_view text_;
    std::size_t pos_ = 0;
};

}  // namespace

std::string encode_set(const std::string& key, const std::string& value,
                       std::optional<std::int64_t> expires_at_ms) {
    std::string lengths = std::to_string(key.size()) + " " + std::to_string(value.size());
    if (expires_at_ms) {
        return encode("T " + lengths + " " + std::to_string(*expires_at_ms), key, value);
    }
    return encode("S " + lengths, key, value);
}

std::string encode_delete(const std::string& key) {
    return encode("D " + std::to_string(key.size()), key, "");
}

DecodedLog decode_log(const std::string& bytes) {
    DecodedLog result;
    std::string_view all(bytes);
    std::size_t pos = 0;

    while (pos < all.size()) {
        // A header with no newline was cut off mid-write.
        std::size_t header_end = all.find('\n', pos);
        if (header_end == std::string_view::npos) break;

        std::string_view header = all.substr(pos, header_end - pos);
        HeaderReader reader(header);
        char op = 0;
        std::uint64_t key_len = 0;
        std::uint64_t value_len = 0;
        std::uint64_t expires_at = 0;
        std::uint64_t checksum = 0;
        bool parsed = reader.op(op) && (op == 'S' || op == 'T' || op == 'D') &&
                      reader.number(key_len) && (op == 'D' || reader.number(value_len)) &&
                      (op != 'T' || reader.number(expires_at));
        // The checksum covers the header text up to here.
        std::size_t fields_len = reader.pos();
        parsed = parsed && reader.number(checksum, 16) && reader.at_end() &&
                 checksum <= 0xFFFFFFFFu;
        if (!parsed) {
            // Writes only ever cut off the end of an entry, so a complete
            // header that can't be read means the file was damaged.
            result.corrupt = true;
            break;
        }

        // The body is the key, the value, and a closing newline. If it runs
        // past the end of the input, it was cut off mid-write.
        std::size_t body_start = header_end + 1;
        std::size_t remaining = all.size() - body_start;
        if (key_len > remaining || value_len > remaining - key_len ||
            key_len + value_len == remaining) {
            break;
        }
        std::size_t body_end = body_start + key_len + value_len;
        std::size_t entry_end = body_end + 1;

        std::string_view key = all.substr(body_start, key_len);
        std::string_view value = all.substr(body_start + key_len, value_len);
        if (all[body_end] != '\n' ||
            entry_checksum(header.substr(0, fields_len), key, value) != checksum) {
            // A damaged final entry is treated like one cut off by a crash.
            // A damaged entry with more data after it is real corruption.
            result.corrupt = entry_end < all.size();
            break;
        }

        LogEntry entry{op == 'D' ? LogEntry::Op::Delete : LogEntry::Op::Set,
                       std::string(key), std::string(value), std::nullopt};
        if (op == 'T') {
            entry.expires_at_ms = static_cast<std::int64_t>(expires_at);
        }
        result.entries.push_back(std::move(entry));
        pos = entry_end;
        result.valid_bytes = pos;
    }
    return result;
}

}  // namespace kvstore
