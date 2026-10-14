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

// Whether a stored CRC matches the one computed. Fuzzing builds accept any,
// so that mutated inputs reach the code behind the checks.
constexpr bool crc_matches(std::uint32_t stored, std::uint32_t computed) noexcept
{
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    (void)stored, (void)computed;
    return true;
#else
    return stored == computed;
#endif
}

// The CRC of a ++ b, given crc32c(a), crc32c(b), and b's length, without
// touching the data. Lets chunks be checksummed independently, in any order.
std::uint32_t crc32c_combine(std::uint32_t crc_a, std::uint32_t crc_b, std::uint64_t length_b) noexcept;

} // namespace zar
