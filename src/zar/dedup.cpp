// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "zar/dedup.hpp"

// The bundled xxHash is compiled in; an installed one is linked instead.
#ifndef ZAR_SYSTEM_XXHASH
#define XXH_INLINE_ALL
#endif
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
