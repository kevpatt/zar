#include "zar/archive_writer.hpp"

#include "zar/crc32c.hpp"

#include <algorithm>

namespace zar {
namespace {

constexpr std::uint64_t round_up(std::uint64_t value, std::uint64_t alignment) noexcept
{
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

ArchiveWriter::ArchiveWriter(Output& out, std::uint8_t align_log2)
    : out_(out), start_(out.position())
{
    header_.align_log2 = align_log2;
    out_.write(header_.encode());
}

std::uint64_t ArchiveWriter::relative_position() const noexcept
{
    return out_.position() - start_;
}

void ArchiveWriter::pad_to(std::uint64_t relative)
{
    out_.write_zeros(static_cast<std::size_t>(relative - relative_position()));
}

std::int64_t ArchiveWriter::write_block(BlockType type, ByteView payload)
{
    // Positions are relative to the zar header, and base_offset is 0, so a
    // position is also the block's offset (§2.2).
    const std::uint64_t alignment = std::uint64_t{1} << header_.align_log2;
    const auto payload_at = round_up(relative_position() + kHeaderSize, alignment);
    const auto header_at = payload_at - kHeaderSize;
    pad_to(header_at);

    BlockHeader block;
    block.type = type;
    block.payload_size = payload.size();
    block.offset = static_cast<std::int64_t>(header_at);
    block.payload_crc = crc32c(payload);
    out_.write(block.encode());
    out_.write(payload);
    return block.offset;
}

void ArchiveWriter::finish(std::int64_t root_index)
{
    const std::uint64_t alignment = std::uint64_t{1} << std::min<std::uint8_t>(header_.align_log2, 3);
    const auto trailer_at = round_up(relative_position(), alignment);
    pad_to(trailer_at);

    std::uint8_t payload[16];
    store_le(&payload[0], static_cast<std::uint64_t>(root_index));
    store_le(&payload[8], trailer_at);    // distance back to the zar header

    BlockHeader trailer;
    trailer.type = BlockType::Trailer;
    trailer.payload_size = sizeof payload;
    trailer.offset = static_cast<std::int64_t>(trailer_at);
    trailer.payload_crc = crc32c(payload);
    out_.write(trailer.encode());
    out_.write(payload);

    if (out_.seekable()) {
        header_.index_offset = root_index;
        out_.write_at(start_, header_.encode());
    }
}

} // namespace zar
