// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// Lays blocks out in an archive: alignment, the zar header, the index
// copies, and the trailer.
#pragma once

#include "zar/format.hpp"
#include "zar/output.hpp"

#include <optional>

namespace zar {

class ArchiveWriter {
public:
    // Normally the zar header is written immediately and blocks follow it.
    // With `head_index`, blocks are staged in a spool file instead, so that
    // finish() can put the index in front of them (§5.4).
    ArchiveWriter(Output& out, std::uint8_t align_log2, bool head_index = false);

    // Writes one block, padding first so its payload is aligned. Returns the
    // block's offset (relative to the base, §2.2).
    std::int64_t write_block(BlockType type, ByteView payload);

    // Writes the root index and the trailer. The zar header points to the
    // index too: to a copy at the front with head_index, otherwise (if the
    // output is seekable) to the same block the trailer references.
    void finish(BlockType index_type, ByteView index);

private:
    Output& sink() noexcept { return spool_ ? *spool_ : out_; }
    std::uint64_t alignment() const noexcept { return std::uint64_t{1} << header_.align_log2; }
    std::int64_t place(Output& sink, std::uint64_t relative, std::int64_t offset_shift,
                       BlockType type, ByteView payload);

    Output& out_;
    std::optional<Output> spool_;
    ArchiveHeader header_;
    std::uint64_t start_;     // position of the zar header in out_
    std::uint64_t origin_;    // position of the base in sink()
};

// Writes a solid archive (§5.5): a zar header, one ARCHIVE_ZSTD block whose
// payload is `frame` (a compressed archive, with the given CRC), and a
// trailer. The header points at the block, so it can be read from a pipe.
void write_solid_archive(Output& out, Output& frame, std::uint32_t frame_crc);

} // namespace zar
