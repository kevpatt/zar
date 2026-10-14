// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "zar/archive_reader.hpp"

#include "zar/crc32c.hpp"
#include "zar/posix.hpp"

#include <algorithm>
#include <fcntl.h>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

namespace zar {

// The archive's bytes: a file read at random, or a stream (a pipe) whose
// head -- everything before the base -- is held in memory and whose data
// blocks are read once, in order.
struct ArchiveReader::File {
    int fd = -1;
    bool owns_fd = true;
    std::uint64_t size = 0;
    std::string name;

    bool in_memory = false;
    Bytes memory;                     // in_memory: the whole archive

    bool stream = false;
    Bytes head;                       // stream: bytes [0, base)
    std::uint64_t stream_position = 0;    // stream: bytes consumed so far

    std::mutex dictionaries_mutex;
    std::map<std::int64_t, ZSTD_DDict*> dictionaries;

    ~File()
    {
        for (const auto& [offset, dict] : dictionaries)
            ZSTD_freeDDict(dict);
        if (fd >= 0 && owns_fd)
            ::close(fd);
    }

    // How many bytes read_at() can reach: for a stream, only its head.
    std::uint64_t reachable() const noexcept { return stream ? head.size() : size; }

    void read_at(std::uint64_t position, std::span<std::uint8_t> out) const
    {
        if (in_memory || stream) {
            const auto& bytes = in_memory ? memory : head;
            if (position > bytes.size() || out.size() > bytes.size() - position) {
                if (stream)
                    throw FormatError("data needed out of order; it can't be read from a pipe");
                throw TruncatedError();
            }
            std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(position), out.size(), out.begin());
            return;
        }
        if (position > size || out.size() > size - position ||
            pread_full(fd, out, position, name) != out.size())
            throw TruncatedError();
    }

    // Stream: the next `out.size()` bytes.
    void read_next(std::span<std::uint8_t> out)
    {
        if (read_full(fd, out, name) != out.size())
            throw TruncatedError();
        stream_position += out.size();
    }

    // Stream: appends the next `count` bytes to `out`. Memory grows only as
    // the data arrives, so a header claiming a huge size costs nothing
    // unless the stream really is that long.
    void read_next(Bytes& out, std::uint64_t count)
    {
        constexpr std::uint64_t kStep = 1 << 20;
        for (std::uint64_t done = 0; done < count;) {
            const auto n = static_cast<std::size_t>(std::min(kStep, count - done));
            out.resize(out.size() + n);
            read_next(std::span(out).last(n));
            done += n;
        }
    }

    void skip_to(std::uint64_t position)
    {
        if (position < stream_position)
            throw FormatError("blocks are out of order; this archive can't be read from a pipe");
        std::uint8_t discard[64 * 1024];
        while (stream_position < position) {
            const auto n = std::min<std::uint64_t>(sizeof discard, position - stream_position);
            read_next({discard, static_cast<std::size_t>(n)});
        }
    }
};

namespace {

// This implementation's limits; the format itself allows more.
constexpr std::uint64_t kMaxChunkSize = std::uint64_t{1} << 32;
constexpr std::uint64_t kMaxIndexSize = std::uint64_t{1} << 32;
constexpr std::size_t kMaxIndexDepth = 64;

std::string at(std::int64_t offset)
{
    return "block at offset " + std::to_string(offset);
}

// The declared content size of a payload that must be exactly one zstd frame.
std::uint64_t frame_content_size(ByteView payload, std::int64_t offset)
{
    if (ZSTD_findFrameCompressedSize(payload.data(), payload.size()) != payload.size())
        throw FormatError(at(offset) + " is not exactly one zstd frame");
    const auto size = ZSTD_getFrameContentSize(payload.data(), payload.size());
    if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR)
        throw FormatError(at(offset) + " has no zstd content size");
    // Each zstd block yields at most 128 KiB and takes at least 4 bytes. A
    // frame claiming more than that is lying, and asking for memory it
    // could never fill.
    constexpr std::uint64_t kMaxRatio = (128 << 10) / 4;
    if (size / kMaxRatio > payload.size())
        throw FormatError(at(offset) + " claims more data than it could hold");
    return size;
}

Bytes decompress_frame(ByteView payload, std::uint64_t size, const ZSTD_DDict* dict,
                       std::int64_t offset)
{
    // One context per thread, reused across frames.
    thread_local const std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> dctx(ZSTD_createDCtx(),
                                                                                 ZSTD_freeDCtx);
    if (!dctx)
        throw std::bad_alloc();

    Bytes out(static_cast<std::size_t>(size));
    const auto n = dict ? ZSTD_decompress_usingDDict(dctx.get(), out.data(), out.size(),
                                                     payload.data(), payload.size(), dict)
                        : ZSTD_decompressDCtx(dctx.get(), out.data(), out.size(),
                                              payload.data(), payload.size());
    if (ZSTD_isError(n))
        throw FormatError(at(offset) + ": " + ZSTD_getErrorName(n));
    if (n != size)
        throw FormatError(at(offset) + " decompressed to the wrong length");
    return out;
}

// The file position of `offset` from `base`, or a FormatError if it's -1 or
// lies outside the range of positions.
std::uint64_t position_of(std::uint64_t base, std::int64_t offset)
{
    const auto magnitude = offset < 0 ? 0 - static_cast<std::uint64_t>(offset) : static_cast<std::uint64_t>(offset);
    if (offset == kNoOffset || (offset < 0 && magnitude > base) ||
        (offset >= 0 && magnitude > std::numeric_limits<std::uint64_t>::max() - base - kHeaderSize))
        throw FormatError("invalid block offset " + std::to_string(offset));
    return offset < 0 ? base - magnitude : base + magnitude;
}

// Keeps the last entry for each path (§6.6), in walk order.
std::vector<Entry> last_wins(std::vector<Entry> entries)
{
    std::map<std::string_view, std::size_t> last;
    for (std::size_t i = 0; i < entries.size(); ++i)
        last[entries[i].path] = i;

    std::vector<bool> keep(entries.size());
    for (const auto& [path, i] : last)
        keep[i] = true;

    std::vector<Entry> out;
    out.reserve(last.size());
    for (std::size_t i = 0; i < entries.size(); ++i)
        if (keep[i])
            out.push_back(std::move(entries[i]));
    return out;
}

} // namespace

// Decompresses a solid archive's frame (§5.5) into memory or, past
// kInMemoryLimit, into an anonymous temporary file. Without a size limit,
// that file may not take more than the free space where it lives.
class ArchiveReader::Inflater {
public:
    Inflater(std::string name, std::optional<std::uint64_t> limit) : name_(std::move(name)), limit_(limit)
    {
        if (!dstream_)
            throw std::bad_alloc();
    }

    // Decompresses a whole ARCHIVE_ZSTD payload, read a piece at a time by
    // `read(position, piece)`, and checks it's complete and intact.
    template <typename Read>
    std::unique_ptr<File> inflate(const BlockHeader& block, Read read)
    {
        Bytes piece(1 << 20);
        for (std::uint64_t done = 0; done < block.payload_size;) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(piece.size(), block.payload_size - done));
            read(done, std::span(piece).first(n));
            feed(std::span(piece).first(n));
            done += n;
        }
        return finish(block.payload_crc);
    }

private:
    static constexpr std::size_t kInMemoryLimit = 64 << 20;

    void feed(ByteView compressed)
    {
        crc_ = crc32c(crc_, compressed);
        ZSTD_inBuffer in{compressed.data(), compressed.size(), 0};
        for (;;) {
            if (done_) {
                if (in.pos < in.size)
                    throw FormatError(name_ + ": data follows the end of the compressed archive");
                return;
            }
            ZSTD_outBuffer out{buffer_.data(), buffer_.size(), 0};
            const auto result = ZSTD_decompressStream(dstream_.get(), &out, &in);
            if (ZSTD_isError(result))
                throw FormatError(name_ + ": " + ZSTD_getErrorName(result));
            store({buffer_.data(), out.pos});
            done_ = result == 0;    // the frame is complete and fully flushed
            // A full output buffer may mean zstd holds more, even with no input left.
            if (in.pos == in.size && out.pos < out.size)
                return;
        }
    }

    std::unique_ptr<File> finish(std::uint32_t expected_crc)
    {
        if (!crc_matches(expected_crc, crc_))
            throw FormatError(name_ + ": the compressed archive fails its checksum");
        if (!done_)
            throw TruncatedError();

        auto file = std::make_unique<File>();
        file->name = name_;
        if (!spill_) {
            file->in_memory = true;
            file->size = memory_.size();
            file->memory = std::move(memory_);
        } else {
            file->fd = spill_.release();
            file->size = spilled_;
        }
        return file;
    }

    void store(ByteView data)
    {
        if (data.empty())
            return;
        total_ += data.size();
        if (limit_ && total_ > *limit_)
            throw std::runtime_error(name_ + ": the solid archive expands to more than the size limit (" +
                                     std::to_string(*limit_) + " bytes)");
        if (!spill_ && memory_.size() + data.size() <= kInMemoryLimit) {
            memory_.insert(memory_.end(), data.begin(), data.end());
            return;
        }
        if (!spill_) {
            const auto dir = temp_directory();
            spill_ = anonymous_temp_file(dir, ".unzar-");
            if (!limit_)
                spill_limit_ = free_space(dir);
            write(memory_);
            memory_ = Bytes();
        }
        write(data);
    }

    void write(ByteView data)
    {
        spilled_ += data.size();
        if (spill_limit_ && spilled_ > *spill_limit_)
            throw std::runtime_error(name_ + ": the solid archive expands to more than the free space in " +
                                     temp_directory().string() + " (set a limit with --max-size)");
        write_all(spill_.get(), data, "temporary file");
    }

    std::string name_;
    std::optional<std::uint64_t> limit_;
    std::uint64_t total_ = 0;
    std::unique_ptr<ZSTD_DStream, decltype(&ZSTD_freeDStream)> dstream_{ZSTD_createDStream(), ZSTD_freeDStream};
    Bytes buffer_ = Bytes(ZSTD_DStreamOutSize());
    bool done_ = false;
    std::uint32_t crc_ = 0;
    Bytes memory_;
    UniqueFd spill_;
    std::uint64_t spilled_ = 0;
    std::optional<std::uint64_t> spill_limit_;
};

ArchiveReader::ArchiveReader(std::unique_ptr<File> file, ArchiveHeader header, std::uint64_t base)
    : file_(std::move(file)), header_(header), base_(base)
{
}

ArchiveReader::ArchiveReader(ArchiveReader&&) noexcept = default;

ArchiveReader::~ArchiveReader() = default;

ArchiveReader ArchiveReader::open(const std::filesystem::path& path,
                                  std::function<void(std::string_view)> on_warning,
                                  std::optional<std::uint64_t> size_limit)
{
    auto file = std::make_unique<File>();
    file->name = path.string();
    file->fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (file->fd < 0)
        throw std::system_error(errno, std::generic_category(), file->name);
    struct stat st {};
    if (::fstat(file->fd, &st) != 0)
        throw std::system_error(errno, std::generic_category(), file->name);
    if (!S_ISREG(st.st_mode))
        throw std::runtime_error(file->name + ": not a regular file");
    file->size = static_cast<std::uint64_t>(st.st_size);
    return from_file(std::move(file), on_warning, true, size_limit);
}

ArchiveReader ArchiveReader::from_file(std::unique_ptr<File> file,
                                       std::function<void(std::string_view)> on_warning,
                                       bool allow_solid, std::optional<std::uint64_t> size_limit)
{
    const auto warn = [&](const std::string& message) {
        if (on_warning)
            on_warning(message);
    };

    // Where is the zar header? Normally at 0; otherwise the trailer says (§5.1).
    std::optional<std::uint64_t> start;
    if (file->size >= kHeaderSize) {
        HeaderBytes first;
        file->read_at(0, first);
        if (std::ranges::equal(ByteView(first).first(kMagic.size()), kMagic))
            start = 0;
    }
    std::optional<std::uint64_t> trailer_at;
    if (file->size >= kHeaderSize + kTrailerSize) {
        std::uint8_t tail[kTrailerSize];
        file->read_at(file->size - kTrailerSize, tail);
        const auto candidate = file->size - kTrailerSize;
        const auto distance = load_le<std::uint64_t>(&tail[kHeaderSize + 8]);
        // The zar header must fit between the start of the file and the trailer.
        if (load_le<std::uint16_t>(tail) == static_cast<std::uint16_t>(BlockType::Trailer) &&
            distance >= kHeaderSize && distance <= candidate) {
            if (!start)
                start = candidate - distance;
            if (*start == candidate - distance)
                trailer_at = candidate;
        }
    }
    if (!start)
        throw FormatError(file->name + ": not a zar archive");

    HeaderBytes header_bytes;
    file->read_at(*start, header_bytes);
    const auto header = ArchiveHeader::decode(header_bytes);
    ArchiveReader reader(std::move(file), header, *start + header.base_offset);

    // Candidate root indexes: the header's, then the trailer's (§5.2).
    std::vector<std::int64_t> roots;
    if (header.index_offset != kNoOffset)
        roots.push_back(header.index_offset);
    if (trailer_at) {
        try {
            const auto offset = static_cast<std::int64_t>(*trailer_at) - static_cast<std::int64_t>(reader.base_);
            const auto block = reader.block_header(offset);
            if (block.type != BlockType::Trailer || block.payload_size != kTrailerSize - kHeaderSize)
                throw FormatError("malformed trailer");
            const auto payload = reader.payload(block);
            const auto index = static_cast<std::int64_t>(load_le<std::uint64_t>(payload.data()));
            if (roots.empty() || roots.front() != index)
                roots.push_back(index);
        } catch (const FormatError& e) {
            warn(std::string("ignoring trailer: ") + e.what());
        }
    }
    if (roots.empty())
        throw FormatError(reader.file_->name + ": archive has no index");

    for (std::size_t i = 0; i < roots.size(); ++i) {
        try {
            if (const auto root = reader.block_header(roots[i]); root.type == BlockType::ArchiveZstd) {
                if (!allow_solid)
                    throw FormatError("a solid archive can't contain another");
                return from_file(reader.unwrap(root, size_limit), on_warning, false, size_limit);
            }
            std::vector<Entry> entries;
            std::set<std::int64_t> seen;
            reader.load_index(roots[i], nullptr, 0, seen, entries);
            reader.entries_ = last_wins(std::move(entries));
            break;
        } catch (const FormatError& e) {
            if (i + 1 == roots.size())
                throw;
            warn(std::string("index is damaged (") + e.what() + "); using the backup copy");
        }
    }
    return reader;
}

BlockHeader ArchiveReader::block_header(std::int64_t offset) const
{
    const auto position = position_of(base_, offset);
    HeaderBytes bytes;
    file_->read_at(position, bytes);
    const auto block = BlockHeader::decode(bytes, offset);
    // read_at() just reached past the header, so this can't underflow.
    if (block.payload_size > file_->reachable() - (position + kHeaderSize))
        throw FormatError(at(offset) + " extends past the end of the archive");
    return block;
}

Bytes ArchiveReader::payload(const BlockHeader& block) const
{
    Bytes bytes(static_cast<std::size_t>(block.payload_size));
    read_at(position_of(base_, block.offset) + kHeaderSize, bytes);
    if (!crc_matches(block.payload_crc, crc32c(bytes)))
        throw FormatError(at(block.offset) + " fails its checksum");
    return bytes;
}

void ArchiveReader::read_at(std::uint64_t position, std::span<std::uint8_t> out) const
{
    file_->read_at(position, out);
}

Bytes ArchiveReader::index_payload(std::int64_t offset) const
{
    const auto block = block_header(offset);
    if (block.type != BlockType::IndexRaw && block.type != BlockType::IndexZstd)
        throw FormatError(at(offset) + " is not an index");
    auto bytes = payload(block);
    if (block.type == BlockType::IndexRaw)
        return bytes;

    const auto size = frame_content_size(bytes, offset);
    if (size > kMaxIndexSize)
        throw FormatError(at(offset) + ": index is too large");
    return decompress_frame(bytes, size, nullptr, offset);
}

// Indexes must form a tree (§6.6): every index block is loaded at most once,
// which rules out both cycles and shared sub-indexes.
void ArchiveReader::load_index(std::int64_t offset, const Entry* parent, std::size_t depth,
                               std::set<std::int64_t>& seen, std::vector<Entry>& out) const
{
    if (!seen.insert(offset).second)
        throw FormatError(at(offset) + " is used as an index more than once");
    if (depth >= kMaxIndexDepth)
        throw FormatError("sub-indexes nest too deeply");

    for (auto& entry : decode_index(index_payload(offset))) {
        if (parent) {
            entry.path = parent->path + '/' + entry.path;
            entry.flags |= parent->flags & entry_flags::kPathRaw;
        }
        const auto subindex = entry.type == EntryType::Dir ? entry.subindex : std::nullopt;
        out.push_back(std::move(entry));
        if (subindex) {
            const Entry dir = out.back();    // `out` may reallocate
            load_index(*subindex, &dir, depth + 1, seen, out);
        }
    }
}

const ZSTD_DDict* ArchiveReader::dictionary(std::int64_t offset) const
{
    std::lock_guard lock(file_->dictionaries_mutex);
    auto& dictionaries = file_->dictionaries;
    if (const auto it = dictionaries.find(offset); it != dictionaries.end())
        return it->second;

    const auto block = block_header(offset);
    if (block.type != BlockType::ZstdDict)
        throw FormatError(at(offset) + " is not a dictionary");
    const auto bytes = payload(block);
    ZSTD_DDict* dict = ZSTD_createDDict(bytes.data(), bytes.size());
    if (!dict)
        throw FormatError(at(offset) + " is not a valid zstd dictionary");
    dictionaries.emplace(offset, dict);
    return dict;
}

std::vector<std::uint64_t> ArchiveReader::chunk_lengths(const Entry& entry)
{
    const auto size = entry.size.value_or(0);
    if (entry.chunks.empty()) {
        if (size != 0)
            throw FormatError("has a size but no data");
        return {};
    }
    if (!entry.size)
        throw FormatError("has data but no size");
    if (entry.chunks.size() > 1 && !entry.chunk_size)
        throw FormatError("has several chunks but no chunk size");

    const auto chunk = entry.chunk_size.value_or(size);
    if (chunk == 0)
        throw FormatError("has a chunk size of zero");
    if (chunk > kMaxChunkSize)
        throw FormatError("uses chunks larger than this unzar supports");
    const auto count = size / chunk + (size % chunk != 0);
    if (count != entry.chunks.size())
        throw FormatError("has " + std::to_string(entry.chunks.size()) + " chunks; its size implies " +
                          std::to_string(count));

    std::vector<std::uint64_t> lengths(entry.chunks.size(), chunk);
    lengths.back() = size - chunk * (count - 1);
    return lengths;
}

Bytes ArchiveReader::chunk(const Entry& entry, std::size_t index, std::uint64_t expected_length,
                           bool decompress) const
{
    const auto block = block_header(entry.chunks.at(index));
    return decode_chunk(entry, block, payload(block), expected_length, decompress);
}

Bytes ArchiveReader::decode_chunk(const Entry& entry, const BlockHeader& block, ByteView bytes,
                                  std::uint64_t expected_length, bool decompress) const
{
    const auto offset = block.offset;
    if (block.type != BlockType::DataRaw && block.type != BlockType::DataZstd)
        throw FormatError(at(offset) + " is not a data block");

    if (block.type == BlockType::DataRaw) {
        if (bytes.size() != expected_length)
            throw FormatError(at(offset) + " has the wrong length");
        return decompress ? Bytes(bytes.begin(), bytes.end()) : Bytes{};
    }

    const ZSTD_DDict* dict = entry.dict ? dictionary(*entry.dict) : nullptr;
    if (frame_content_size(bytes, offset) != expected_length)
        throw FormatError(at(offset) + " has the wrong decompressed length");
    return decompress ? decompress_frame(bytes, expected_length, dict, offset) : Bytes{};
}

std::pair<BlockHeader, Bytes> ArchiveReader::next_block(std::int64_t offset)
{
    if (!file_->stream)
        throw std::logic_error("next_block() is for archives read from a stream");
    file_->skip_to(position_of(base_, offset));
    HeaderBytes header_bytes;
    file_->read_next(header_bytes);
    const auto block = BlockHeader::decode(header_bytes, offset);
    if (block.payload_size > kMaxChunkSize)    // the writer stores a chunk raw rather than expand it
        throw FormatError(at(offset) + " is larger than any chunk");

    Bytes bytes;
    file_->read_next(bytes, block.payload_size);
    if (!crc_matches(block.payload_crc, crc32c(bytes)))
        throw FormatError(at(offset) + " fails its checksum");
    return {block, std::move(bytes)};
}

ArchiveReader ArchiveReader::open_stream(int fd, std::string name,
                                         std::function<void(std::string_view)> on_warning,
                                         std::optional<std::uint64_t> size_limit)
{
    (void)on_warning;    // a stream has no trailer, so no backup index to fall back on

    auto file = std::make_unique<File>();
    file->fd = fd;
    file->owns_fd = false;
    file->name = std::move(name);
    file->stream = true;

    HeaderBytes header_bytes;
    file->read_next(header_bytes);
    const auto header = ArchiveHeader::decode(header_bytes);

    // A solid archive is a single block, read whole (§5.5).
    if (header.index_offset >= 0) {
        file->skip_to(header.base_offset + static_cast<std::uint64_t>(header.index_offset));
        HeaderBytes block_bytes;
        file->read_next(block_bytes);
        const auto block = BlockHeader::decode(block_bytes, header.index_offset);
        if (block.type == BlockType::ArchiveZstd) {
            Inflater inflater(file->name, size_limit);
            auto inner = inflater.inflate(block, [&](std::uint64_t, std::span<std::uint8_t> piece) {
                file->read_next(piece);
            });
            return from_file(std::move(inner), on_warning, false, size_limit);
        }
    }

    // Otherwise, only an archive whose root index lies before its data can be
    // streamed (§5.4). The head -- everything before the base -- is kept.
    if (header.index_offset == kNoOffset || header.index_offset >= 0 || header.base_offset == 0)
        throw FormatError(file->name + ": the index isn't at the head of this archive, so it "
                                       "can't be read from a pipe (create it with zar --head-index, "
                                       "or save it to a file first)");
    file->head.assign(header_bytes.begin(), header_bytes.end());
    file->read_next(file->head, header.base_offset - kHeaderSize);    // base_offset >= kHeaderSize (§2.2)

    ArchiveReader reader(std::move(file), header, header.base_offset);
    std::vector<Entry> entries;
    std::set<std::int64_t> seen;
    reader.load_index(header.index_offset, nullptr, 0, seen, entries);
    reader.entries_ = last_wins(std::move(entries));
    return reader;
}

bool ArchiveReader::streaming() const noexcept
{
    return file_->stream;
}

std::unique_ptr<ArchiveReader::File> ArchiveReader::unwrap(const BlockHeader& block,
                                                           std::optional<std::uint64_t> size_limit) const
{
    const auto start = position_of(base_, block.offset) + kHeaderSize;
    Inflater inflater(file_->name, size_limit);
    return inflater.inflate(block, [&](std::uint64_t done, std::span<std::uint8_t> piece) {
        read_at(start + done, piece);
    });
}

} // namespace zar
