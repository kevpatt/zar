// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "zar/dedup.hpp"

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace zar {

ChunkHash hash_chunk(ByteView data) noexcept
{
    if (hash_chunk_override)
        return hash_chunk_override(data);
    const auto h = XXH3_128bits(data.data(), data.size());
    return {h.low64, h.high64};
}

} // namespace zar
