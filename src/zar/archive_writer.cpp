// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "zar/archive_writer.hpp"

#include "zar/crc32c.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace zar {
namespace {

constexpr std::uint64_t round_up(std::uint64_t value, std::uint64_t alignment) noexcept
{
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr std::uint64_t alignment_of(const ArchiveHeader& header) noexcept
{
    return std::uint64_t{1} << header.align_log2;
}

// Ends an archive with a trailer (§5.1). `base` and `header` are the output
// positions of the base and of the zar header.
void write_trailer(Output& out, std::uint64_t base, std::uint64_t header, std::int64_t root,
                   std::uint64_t alignment)
{
    const auto relative = out.position() - base;
    const auto trailer_at = round_up(relative, std::min<std::uint64_t>(alignment, 8));
    out.write_zeros(static_cast<std::size_t>(trailer_at - relative));

    std::uint8_t payload[16];
    store_le(&payload[0], static_cast<std::uint64_t>(root));
    store_le(&payload[8], base - header + trailer_at);    // distance back to the zar header

    BlockHeader trailer;
    trailer.type = BlockType::Trailer;
    trailer.payload_size = sizeof payload;
    trailer.offset = static_cast<std::int64_t>(trailer_at);
    trailer.payload_crc = crc32c(payload);
    out.write(trailer.encode());
    out.write(payload);
}

} // namespace

void write_solid_archive(Output& out, Output& frame, std::uint32_t frame_crc)
{
    const auto start = out.position();
    ArchiveHeader header;
    header.index_offset = kHeaderSize;    // the block right after the header
    out.write(header.encode());

    BlockHeader block;
    block.type = BlockType::ArchiveZstd;
    block.payload_size = frame.position();
    block.offset = kHeaderSize;
    block.payload_crc = frame_crc;
    out.write(block.encode());
    frame.copy_to(out);

    write_trailer(out, start, start, header.index_offset, alignment_of(header));
}

std::uint64_t solid_archive_size(std::uint64_t frame_size)
{
    // As in write_trailer(), with the default alignment.
    return round_up(2 * kHeaderSize + frame_size, 8) + kTrailerSize;
}

ArchiveWriter::ArchiveWriter(Output& out, std::uint8_t align_log2, bool head_index)
    : out_(out), start_(out.position()), origin_(out.position())
{
    header_.align_log2 = align_log2;
    if (head_index) {
        spool_.emplace(out.make_spool());
        origin_ = 0;
    } else {
        out_.write(header_.encode());
    }
}

// Writes a block into `sink` at the first suitably aligned position at or
// after the current one. `relative` is the sink's current position measured
// from a point that is aligned in the final archive; the block's offset is
// its position from that point plus `offset_shift`.
std::int64_t ArchiveWriter::place(Output& sink, std::uint64_t relative, std::int64_t offset_shift,
                                  BlockType type, ByteView payload)
{
    const auto payload_at = round_up(relative + kHeaderSize, alignment());
    const auto header_at = payload_at - kHeaderSize;
    sink.write_zeros(static_cast<std::size_t>(header_at - relative));

    BlockHeader block;
    block.type = type;
    block.payload_size = payload.size();
    block.offset = static_cast<std::int64_t>(header_at) + offset_shift;
    block.payload_crc = crc32c(payload);
    sink.write(block.encode());
    sink.write(payload);
    return block.offset;
}

std::int64_t ArchiveWriter::write_block(BlockType type, ByteView payload)
{
    // The base is aligned, so positions measured from it can be aligned directly.
    return place(sink(), sink().position() - origin_, 0, type, payload);
}

std::int64_t ArchiveWriter::write_dictionary(ByteView dict)
{
    if (!spool_)
        return write_block(BlockType::ZstdDict, dict);

    // It will end at the base, after padding that aligns its payload.
    dictionary_.assign(dict.begin(), dict.end());
    return -static_cast<std::int64_t>(round_up(dict.size(), alignment()) + kHeaderSize);
}

void ArchiveWriter::finish(BlockType index_type, ByteView index)
{
    if (!spool_) {
        const auto root = write_block(index_type, index);
        write_trailer(out_, origin_, start_, root, alignment());
        if (out_.seekable()) {
            header_.index_offset = root;
            out_.write_at(start_, header_.encode());
        }
        return;
    }

    // zar header | head index | padding | [dictionary] | base: spooled blocks |
    // tail index | trailer
    //
    // Everything before the base is known up front, so this works on a pipe.
    const auto head_payload_at = round_up(2 * kHeaderSize, alignment());
    const auto head_end = head_payload_at + index.size();
    const auto dict_span = dictionary_.empty() ? 0 : kHeaderSize + round_up(dictionary_.size(), alignment());
    const auto base = round_up(head_end + dict_span, alignment());
    if (base > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("the index is too large to put at the head of the archive");

    // The head index lies before the base, so its offset is negative.
    header_.base_offset = static_cast<std::uint32_t>(base);
    header_.index_offset =
        static_cast<std::int64_t>(head_payload_at - kHeaderSize) - static_cast<std::int64_t>(base);
    out_.write(header_.encode());
    place(out_, kHeaderSize, -static_cast<std::int64_t>(base), index_type, index);
    if (!dictionary_.empty()) {
        const auto dict_at = base - dict_span;
        out_.write_zeros(static_cast<std::size_t>(dict_at - head_end));
        place(out_, dict_at, -static_cast<std::int64_t>(base), BlockType::ZstdDict, dictionary_);
        out_.write_zeros(static_cast<std::size_t>(base - (dict_at + kHeaderSize + dictionary_.size())));
    } else {
        out_.write_zeros(static_cast<std::size_t>(base - head_end));
    }

    spool_->copy_to(out_);
    spool_.reset();
    origin_ = start_ + base;

    const auto tail = write_block(index_type, index);    // the second copy (§5.2)
    write_trailer(out_, origin_, start_, tail, alignment());
}

} // namespace zar
