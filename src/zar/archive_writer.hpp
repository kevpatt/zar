// Lays blocks out in an archive: alignment, the zar header, and the trailer.
#pragma once

#include "zar/format.hpp"
#include "zar/output.hpp"

namespace zar {

class ArchiveWriter {
public:
    // Writes a placeholder zar header at the output's current position.
    ArchiveWriter(Output& out, std::uint8_t align_log2);

    // Writes one block, padding first so its payload is aligned. Returns the
    // block's offset.
    std::int64_t write_block(BlockType type, ByteView payload);

    // Writes the trailer and, if the output is seekable, records the root
    // index in the zar header too (§5).
    void finish(std::int64_t root_index);

private:
    std::uint64_t relative_position() const noexcept;
    void pad_to(std::uint64_t relative);

    Output& out_;
    ArchiveHeader header_;
    std::uint64_t start_;
};

} // namespace zar
