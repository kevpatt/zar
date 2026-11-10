// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "zar/hash.hpp"

#include <blake3.h>

namespace zar {

Digest blake3(ByteView data)
{
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data.data(), data.size());
    Digest digest;
    blake3_hasher_finalize(&hasher, digest.data(), digest.size());
    return digest;
}

Digest block_hash(BlockType type, ByteView payload)
{
    std::uint8_t type_bytes[2];
    store_le(type_bytes, static_cast<std::uint16_t>(type));
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, type_bytes, sizeof type_bytes);
    blake3_hasher_update(&hasher, payload.data(), payload.size());
    Digest digest;
    blake3_hasher_finalize(&hasher, digest.data(), digest.size());
    return digest;
}

std::string to_hex(ByteView bytes)
{
    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto b : bytes) {
        out += kHex[b >> 4];
        out += kHex[b & 0xf];
    }
    return out;
}

} // namespace zar
