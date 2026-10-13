#include "zar/archive_reader.hpp"

#include "zar/crc32c.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <optional>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <zstd.h>

namespace zar {

struct ArchiveReader::File {
    int fd = -1;
    std::uint64_t size = 0;
    std::string name;

    std::mutex dictionaries_mutex;
    std::map<std::int64_t, ZSTD_DDict*> dictionaries;

    ~File()
    {
        for (const auto& [offset, dict] : dictionaries)
            ZSTD_freeDDict(dict);
        if (fd >= 0)
            ::close(fd);
    }

    void read_at(std::uint64_t position, std::span<std::uint8_t> out) const
    {
        if (position > size || out.size() > size - position)
            throw FormatError("archive is truncated");
        std::size_t done = 0;
        while (done < out.size()) {
            const auto n = ::pread(fd, out.data() + done, out.size() - done,
                                   static_cast<off_t>(position + done));
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                throw std::system_error(errno, std::generic_category(), name);
            if (n == 0)
                throw FormatError("archive is truncated");
            done += static_cast<std::size_t>(n);
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

ArchiveReader::ArchiveReader(std::unique_ptr<File> file, ArchiveHeader header, std::uint64_t base)
    : file_(std::move(file)), header_(header), base_(base)
{
}

ArchiveReader::ArchiveReader(ArchiveReader&&) noexcept = default;

ArchiveReader::~ArchiveReader() = default;

ArchiveReader ArchiveReader::open(const std::filesystem::path& path,
                                  std::function<void(std::string_view)> on_warning)
{
    const auto warn = [&](const std::string& message) {
        if (on_warning)
            on_warning(message);
    };

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
        if (load_le<std::uint16_t>(tail) == static_cast<std::uint16_t>(BlockType::Trailer) &&
            distance <= candidate - kHeaderSize) {
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
    const auto position = static_cast<std::int64_t>(base_) + offset;
    if (offset == kNoOffset || position < 0)
        throw FormatError("invalid block offset " + std::to_string(offset));

    HeaderBytes bytes;
    file_->read_at(static_cast<std::uint64_t>(position), bytes);
    const auto block = BlockHeader::decode(bytes, offset);
    const auto payload_at = static_cast<std::uint64_t>(position) + kHeaderSize;
    if (block.payload_size > file_->size - payload_at)
        throw FormatError(at(offset) + " extends past the end of the archive");
    return block;
}

Bytes ArchiveReader::payload(const BlockHeader& block) const
{
    Bytes bytes(static_cast<std::size_t>(block.payload_size));
    read_at(base_ + static_cast<std::uint64_t>(block.offset) + kHeaderSize, bytes);
    if (crc32c(bytes) != block.payload_crc)
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
    const auto offset = entry.chunks.at(index);
    const auto block = block_header(offset);
    if (block.type != BlockType::DataRaw && block.type != BlockType::DataZstd)
        throw FormatError(at(offset) + " is not a data block");
    auto bytes = payload(block);

    if (block.type == BlockType::DataRaw) {
        if (bytes.size() != expected_length)
            throw FormatError(at(offset) + " has the wrong length");
        return decompress ? bytes : Bytes{};
    }

    const ZSTD_DDict* dict = entry.dict ? dictionary(*entry.dict) : nullptr;
    if (frame_content_size(bytes, offset) != expected_length)
        throw FormatError(at(offset) + " has the wrong decompressed length");
    return decompress ? decompress_frame(bytes, expected_length, dict, offset) : Bytes{};
}

} // namespace zar
