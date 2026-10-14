// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// Content hashes for finding chunks that are already in the archive.
#pragma once

#include "zar/bytes.hpp"

#include <cstddef>
#include <cstdint>

namespace zar {

// XXH3-128 of a chunk's uncompressed data. Equal hashes only nominate a
// block for reuse; the bytes are compared before it is reused.
struct ChunkHash {
    std::uint64_t low = 0;
    std::uint64_t high = 0;

    bool operator==(const ChunkHash&) const = default;

    struct Hasher {
        std::size_t operator()(const ChunkHash& h) const noexcept { return static_cast<std::size_t>(h.low); }
    };
};

ChunkHash hash_chunk(ByteView data) noexcept;

// For tests: replaces the hash, to force collisions.
inline ChunkHash (*hash_chunk_override)(ByteView) noexcept = nullptr;

} // namespace zar
