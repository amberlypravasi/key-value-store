#include "kvstore/crc32.h"

#include <array>

namespace kvstore {

namespace {

// Precomputes the checksum of every possible byte, so crc32 can process one
// byte per table lookup instead of one bit at a time.
constexpr std::array<std::uint32_t, 256> make_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        table[i] = c;
    }
    return table;
}

constexpr auto kTable = make_table();

}  // namespace

std::uint32_t crc32(std::string_view data, std::uint32_t crc) {
    crc = ~crc;
    for (unsigned char byte : data) {
        crc = kTable[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

}  // namespace kvstore
