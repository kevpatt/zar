// Access to an archive on disk or in a stream. After opening, every const
// member is safe to call from several threads at once.
#pragma once

#include "zar/format.hpp"
#include "zar/index.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string_view>
#include <utility>

struct ZSTD_DDict_s;

namespace zar {

class ArchiveReader {
public:
    // Opens an archive that starts at the beginning of `path` or, failing
    // that, one located from a trailer at its end (§5.1). Problems that can
    // be worked around, such as a damaged index copy, go to `on_warning`.
    static ArchiveReader open(const std::filesystem::path& path,
                              std::function<void(std::string_view)> on_warning = {});

    // Reads an archive from a stream such as a pipe, which works when its
    // index is at the head (§5.4). The header and the index are read now;
    // data blocks must then be fetched in order with next_block(). The
    // caller keeps ownership of `fd`.
    static ArchiveReader open_stream(int fd, std::string name,
                                     std::function<void(std::string_view)> on_warning = {});

    bool streaming() const noexcept;

    ArchiveReader(ArchiveReader&&) noexcept;
    ~ArchiveReader();

    const ArchiveHeader& header() const noexcept { return header_; }

    // Every entry, with sub-indexes (§6.6) merged in and their paths made
    // archive-relative. Where several entries share a path, the last wins.
    const std::vector<Entry>& entries() const noexcept { return entries_; }

    // Reads and validates the block header at `offset`.
    BlockHeader block_header(std::int64_t offset) const;

    // Reads a block's payload and verifies its CRC.
    Bytes payload(const BlockHeader& block) const;

    // The expected length of each chunk of a FILE or HARDLINK entry, after
    // checking SIZE, CHUNK_SIZE, and CHUNKS against each other (§6.5).
    static std::vector<std::uint64_t> chunk_lengths(const Entry& entry);

    // Reads, verifies, and (if `decompress`) decompresses one chunk. Without
    // decompression it still checks the zstd frame's declared content size,
    // and returns an empty buffer.
    Bytes chunk(const Entry& entry, std::size_t index, std::uint64_t expected_length,
                bool decompress) const;

    // The same, for a block already read (e.g. by next_block()).
    Bytes decode_chunk(const Entry& entry, const BlockHeader& block, ByteView payload,
                       std::uint64_t expected_length, bool decompress) const;

    // Stream only: reads the block at `offset`, skipping anything before it.
    // Offsets must increase from call to call. Verifies the payload's CRC.
    std::pair<BlockHeader, Bytes> next_block(std::int64_t offset);

private:
    struct File;

    ArchiveReader(std::unique_ptr<File> file, ArchiveHeader header, std::uint64_t base);
    void read_at(std::uint64_t position, std::span<std::uint8_t> out) const;
    Bytes index_payload(std::int64_t offset) const;
    void load_index(std::int64_t offset, const Entry* parent, std::size_t depth,
                    std::set<std::int64_t>& seen, std::vector<Entry>& out) const;
    const ZSTD_DDict_s* dictionary(std::int64_t offset) const;

    std::unique_ptr<File> file_;    // also holds the dictionary cache
    ArchiveHeader header_;
    std::uint64_t base_;    // file position of offset 0
    std::vector<Entry> entries_;
};

} // namespace zar
