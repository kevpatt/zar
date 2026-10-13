#include "zar/format.hpp"

#include "zar/crc32c.hpp"

#include <algorithm>

namespace zar {
namespace {

constexpr std::size_t kCrcOffset = kHeaderSize - 4;

void seal(HeaderBytes& bytes)
{
    store_le(&bytes[kCrcOffset], crc32c(ByteView(bytes).first(kCrcOffset)));
}

void check_seal(const HeaderBytes& bytes, const char* what)
{
    if (load_le<std::uint32_t>(&bytes[kCrcOffset]) != crc32c(ByteView(bytes).first(kCrcOffset)))
        throw FormatError(std::string(what) + " checksum mismatch");
}

} // namespace

std::string Version::to_string() const
{
    return std::to_string(major) + '.' + std::to_string(minor) + '.' + std::to_string(patch);
}

HeaderBytes ArchiveHeader::encode() const
{
    HeaderBytes bytes{};
    std::ranges::copy(kMagic, bytes.begin());
    store_le(&bytes[8], version.major);
    store_le(&bytes[10], version.minor);
    store_le(&bytes[12], version.patch);
    bytes[14] = flags;
    bytes[15] = align_log2;
    store_le(&bytes[16], static_cast<std::uint64_t>(index_offset));
    store_le(&bytes[24], base_offset);
    seal(bytes);
    return bytes;
}

ArchiveHeader ArchiveHeader::decode(const HeaderBytes& bytes)
{
    if (!std::ranges::equal(ByteView(bytes).first(kMagic.size()), kMagic))
        throw FormatError("not a zar archive");
    check_seal(bytes, "zar header");

    ArchiveHeader header;
    header.version = {load_le<std::uint16_t>(&bytes[8]),
                      load_le<std::uint16_t>(&bytes[10]),
                      load_le<std::uint16_t>(&bytes[12])};
    header.flags = bytes[14];
    header.align_log2 = bytes[15];
    header.index_offset = static_cast<std::int64_t>(load_le<std::uint64_t>(&bytes[16]));
    header.base_offset = load_le<std::uint32_t>(&bytes[24]);

    if (header.version.major != kFormatVersion.major)
        throw FormatError("unsupported format version " + header.version.to_string());
    if (header.flags != 0)
        throw FormatError("zar header has unknown flags set");
    if (header.align_log2 > kAlignLog2Max)
        throw FormatError("invalid alignment in zar header");
    if (header.base_offset % (1u << header.align_log2) != 0)
        throw FormatError("zar header base offset is not aligned");
    return header;
}

HeaderBytes BlockHeader::encode() const
{
    HeaderBytes bytes{};
    store_le(&bytes[0], static_cast<std::uint16_t>(type));
    store_le(&bytes[2], flags);
    store_le(&bytes[8], payload_size);
    store_le(&bytes[16], static_cast<std::uint64_t>(offset));
    store_le(&bytes[24], payload_crc);
    seal(bytes);
    return bytes;
}

BlockHeader BlockHeader::decode(const HeaderBytes& bytes, std::int64_t expected_offset)
{
    check_seal(bytes, "block header");

    BlockHeader header;
    header.type = static_cast<BlockType>(load_le<std::uint16_t>(&bytes[0]));
    header.flags = load_le<std::uint16_t>(&bytes[2]);
    header.payload_size = load_le<std::uint64_t>(&bytes[8]);
    header.offset = static_cast<std::int64_t>(load_le<std::uint64_t>(&bytes[16]));
    header.payload_crc = load_le<std::uint32_t>(&bytes[24]);

    if (header.offset != expected_offset)
        throw FormatError("block header at offset " + std::to_string(expected_offset) +
                          " claims offset " + std::to_string(header.offset));
    if (header.flags != 0 || load_le<std::uint32_t>(&bytes[4]) != 0)
        throw FormatError("block header has unknown flags or reserved bits set");
    return header;
}

} // namespace zar
