// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
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

// The CRC of a ++ b, given crc32c(a), crc32c(b), and b's length, without
// touching the data. Lets chunks be checksummed independently, in any order.
std::uint32_t crc32c_combine(std::uint32_t crc_a, std::uint32_t crc_b, std::uint64_t length_b) noexcept;

} // namespace zar
