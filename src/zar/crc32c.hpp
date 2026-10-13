// CRC-32C (Castagnoli), hardware-accelerated where available.
#pragma once

#include <cstdint>
#include <span>

namespace zar {

// Extends `crc` (the result of a previous call, or 0 to start) over `data`.
// crc32c(crc32c(0, a), b) == crc32c(0, a ++ b).
std::uint32_t crc32c(std::uint32_t crc, std::span<const std::uint8_t> data) noexcept;

inline std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept
{
    return crc32c(0, data);
}

} // namespace zar
