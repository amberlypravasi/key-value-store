#pragma once

#include <cstdint>
#include <string_view>

namespace kvstore {

// CRC-32 checksum (the same one zip and Ethernet use). If any bit of `data`
// changes, the checksum almost certainly changes too.
std::uint32_t crc32(std::string_view data, std::uint32_t crc = 0);

}  // namespace kvstore
