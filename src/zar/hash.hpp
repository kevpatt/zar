// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// Cryptographic hashes (FORMAT.md §6.7): BLAKE3-256, of whole blocks.
#pragma once

#include "zar/bytes.hpp"
#include "zar/format.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace zar {

// Hash algorithms, as stored before each digest (§6.7).
enum class HashAlgorithm : std::uint8_t {
    Blake3 = 1,    // BLAKE3-256
};

inline constexpr std::size_t kDigestSize = 32;
using Digest = std::array<std::uint8_t, kDigestSize>;

// The digest recorded for a hole, which has no block (§6.5).
inline constexpr Digest kHoleDigest{};

Digest blake3(ByteView data);

// A block's hash (§6.7): of its type and its stored payload, so it survives
// the block being copied elsewhere.
Digest block_hash(BlockType type, ByteView payload);

// Whether a stored digest matches the one computed. Fuzzing builds accept
// any, as with CRCs (crc_matches()).
inline bool digest_matches(const Digest& stored, const Digest& computed) noexcept
{
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    (void)stored, (void)computed;
    return true;
#else
    return stored == computed;
#endif
}

// Lowercase hexadecimal.
std::string to_hex(ByteView bytes);

} // namespace zar
