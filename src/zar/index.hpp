// Index entries (FORMAT.md §6).
#pragma once

#include "zar/bytes.hpp"
#include "zar/format.hpp"

#include <optional>
#include <string>
#include <vector>

namespace zar {

struct Timestamp {
    std::int64_t seconds = 0;
    std::optional<std::uint32_t> nanoseconds;

    bool operator==(const Timestamp&) const = default;
};

struct Entry {
    EntryType type = EntryType::File;
    std::uint8_t flags = 0;
    std::string path;    // UTF-8 unless flags has entry_flags::kPathRaw

    // Content (FILE and HARDLINK)
    std::optional<std::uint64_t> size;
    std::optional<std::uint64_t> chunk_size;
    std::vector<std::int64_t> chunks;
    std::optional<std::int64_t> dict;
    std::optional<std::uint32_t> content_crc;

    std::optional<std::string> link_target;    // SYMLINK and HARDLINK
    std::optional<std::int64_t> subindex;      // DIR

    // Metadata
    std::optional<std::uint16_t> mode;
    std::optional<std::uint64_t> uid;
    std::optional<std::uint64_t> gid;
    std::optional<std::string> uname;
    std::optional<std::string> gname;
    std::optional<Timestamp> mtime;
    std::optional<Timestamp> atime;
    std::optional<Timestamp> ctime;
    std::optional<Timestamp> btime;

    bool operator==(const Entry&) const = default;
};

// Appends `entry` to an index payload.
void encode_entry(Bytes& out, const Entry& entry);

// Decodes an index payload. Entries of unknown types and fields with
// unknown tags are skipped (§6.1); anything malformed throws FormatError.
std::vector<Entry> decode_index(ByteView payload);

} // namespace zar
