// On-disk structures of the zar format. See FORMAT.md for the specification;
// section numbers below refer to it.
#pragma once

#include "zar/bytes.hpp"
#include "zar/error.hpp"

#include <array>
#include <compare>
#include <cstdint>
#include <string>

namespace zar {

struct Version {
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    std::uint16_t patch = 0;

    auto operator<=>(const Version&) const = default;
    std::string to_string() const;
};

inline constexpr Version kFormatVersion{0, 1, 0};

inline constexpr std::size_t kHeaderSize = 32;        // zar and block headers
inline constexpr std::size_t kTrailerSize = kHeaderSize + 16;
inline constexpr std::array<std::uint8_t, 8> kMagic{0x89, 'Z', 'A', 'R', '\r', '\n', 0x1a, '\n'};

// Offsets are relative to the archive's base (§2.2); -1 means "none".
inline constexpr std::int64_t kNoOffset = -1;

inline constexpr std::uint8_t kAlignLog2Max = 20;     // §2.1
inline constexpr std::uint8_t kAlignLog2Default = 3;  // 8 bytes

// §4.2
enum class BlockType : std::uint16_t {
    IndexRaw = 0x0001,
    IndexZstd = 0x0002,
    DataRaw = 0x0003,
    DataZstd = 0x0004,
    ZstdDict = 0x0005,
    Trailer = 0x0006,
    ArchiveZstd = 0x0007,    // a whole archive, compressed as one frame (§5.5)
};

// §6.2
enum class EntryType : std::uint8_t {
    File = 1,
    Dir = 2,
    Symlink = 3,
    Hardlink = 4,
};

// §6.3
namespace entry_flags {
inline constexpr std::uint8_t kPathRaw = 1u << 0;
}

// §6.5
enum class Tag : std::uint64_t {
    Path = 0x01,
    Size = 0x02,
    ChunkSize = 0x03,
    Chunks = 0x04,
    Dict = 0x05,
    ContentCrc = 0x06,
    LinkTarget = 0x07,
    Subindex = 0x08,
    Mode = 0x10,
    Uid = 0x11,
    Gid = 0x12,
    Uname = 0x13,
    Gname = 0x14,
    Mtime = 0x20,
    Atime = 0x21,
    Ctime = 0x22,
    Btime = 0x23,
    MtimeNsec = 0x24,
    AtimeNsec = 0x25,
    CtimeNsec = 0x26,
    BtimeNsec = 0x27,
};

using HeaderBytes = std::array<std::uint8_t, kHeaderSize>;

// §3
struct ArchiveHeader {
    Version version = kFormatVersion;
    std::uint8_t flags = 0;
    std::uint8_t align_log2 = kAlignLog2Default;
    std::int64_t index_offset = kNoOffset;    // kNoOffset: look for a trailer (§5)
    std::uint32_t base_offset = 0;

    HeaderBytes encode() const;
    // Validates magic, CRC, flags, major version, and alignment.
    static ArchiveHeader decode(const HeaderBytes& bytes);
};

// §4.1
struct BlockHeader {
    BlockType type{};
    std::uint16_t flags = 0;
    std::uint64_t payload_size = 0;
    std::int64_t offset = 0;
    std::uint32_t payload_crc = 0;

    HeaderBytes encode() const;
    // Validates CRC, reserved fields, and that the block claims to live at
    // `expected_offset`. Unknown block types are accepted (§4.2).
    static BlockHeader decode(const HeaderBytes& bytes, std::int64_t expected_offset);
};

} // namespace zar
