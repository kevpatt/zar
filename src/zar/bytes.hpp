// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// Little-endian integer encoding and LEB128 varints.
#pragma once

#include "zar/error.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace zar {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

template <std::unsigned_integral T>
constexpr void store_le(std::uint8_t* out, T value) noexcept
{
    for (std::size_t i = 0; i < sizeof(T); ++i)
        out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

template <std::unsigned_integral T>
constexpr T load_le(const std::uint8_t* in) noexcept
{
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        value |= static_cast<T>(static_cast<T>(in[i]) << (8 * i));
    return value;
}

template <std::unsigned_integral T>
void append_le(Bytes& out, T value)
{
    std::uint8_t buf[sizeof(T)];
    store_le(buf, value);
    out.insert(out.end(), buf, buf + sizeof(T));
}

inline void append_varint(Bytes& out, std::uint64_t value)
{
    while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

// Decodes a varint from the front of `in` and advances past it. Rejects
// truncated, overlong (> 10 bytes or > 64 bits), and non-minimal encodings.
inline std::uint64_t read_varint(ByteView& in)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < in.size() && i < 10; ++i) {
        const std::uint8_t byte = in[i];
        const std::uint64_t group = byte & 0x7f;
        if (i == 9 && group > 1)
            throw FormatError("varint overflows 64 bits");
        value |= group << (7 * i);
        if (!(byte & 0x80)) {
            if (byte == 0 && i > 0)
                throw FormatError("non-minimal varint");
            in = in.subspan(i + 1);
            return value;
        }
    }
    throw FormatError("truncated varint");
}

} // namespace zar
