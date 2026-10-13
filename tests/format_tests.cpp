#include "check.hpp"

#include "zar/bytes.hpp"
#include "zar/crc32c.hpp"
#include "zar/format.hpp"

#include <algorithm>
#include <string_view>

using namespace zar;

namespace {

ByteView bytes_of(std::string_view s)
{
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

} // namespace

TEST(crc32c_known_vectors)
{
    CHECK(crc32c(ByteView{}) == 0);
    CHECK(crc32c(bytes_of("123456789")) == 0xe3069283);    // standard check value

    Bytes zeros(32, 0x00), ones(32, 0xff);
    CHECK(crc32c(zeros) == 0x8a9136aa);                     // RFC 3720 B.4
    CHECK(crc32c(ones) == 0x62a8ab43);
}

TEST(crc32c_is_incremental)
{
    const auto text = bytes_of("The quick brown fox jumps over the lazy dog, repeatedly.");
    for (std::size_t split = 0; split <= text.size(); ++split)
        CHECK(crc32c(crc32c(text.first(split)), text.subspan(split)) == crc32c(text));
}

TEST(crc32c_combine_matches_concatenation)
{
    const auto text = bytes_of("Chunks may be checksummed separately and in any order.");
    for (std::size_t split = 0; split <= text.size(); ++split) {
        const auto a = text.first(split), b = text.subspan(split);
        CHECK(crc32c_combine(crc32c(a), crc32c(b), b.size()) == crc32c(text));
    }

    // Across a long second part, too.
    Bytes big(3'000'017);
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<std::uint8_t>(i * 131 + (i >> 9));
    const ByteView all(big);
    CHECK(crc32c_combine(crc32c(all.first(5)), crc32c(all.subspan(5)), all.size() - 5) == crc32c(all));
}

TEST(varint_round_trip)
{
    for (std::uint64_t v : {0ull, 1ull, 127ull, 128ull, 300ull, 16383ull, 16384ull,
                            0xffffffffull, ~0ull}) {
        Bytes buf;
        append_varint(buf, v);
        ByteView in(buf);
        CHECK(read_varint(in) == v);
        CHECK(in.empty());
    }
}

TEST(varint_rejects_malformed)
{
    const auto parse = [](Bytes buf) {
        ByteView in(buf);
        return read_varint(in);
    };
    CHECK_THROWS(parse({}));
    CHECK_THROWS(parse({0x80}));                     // truncated
    CHECK_THROWS(parse({0x80, 0x00}));               // non-minimal
    CHECK_THROWS(parse(Bytes(10, 0xff)));            // too long
    CHECK_THROWS(parse({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02}));
}

TEST(archive_header_defaults)
{
    const auto back = ArchiveHeader::decode(ArchiveHeader{}.encode());
    CHECK(back.version == kFormatVersion);
    CHECK(back.align_log2 == 3);    // 8 bytes
    CHECK(back.index_offset == kNoOffset);
    CHECK(back.base_offset == 0);
}

TEST(archive_header_round_trip)
{
    ArchiveHeader h;
    h.index_offset = -4096;    // a front index, before the base
    h.align_log2 = 7;
    h.base_offset = 128 * 1000;

    const auto bytes = h.encode();
    CHECK(std::ranges::equal(ByteView(bytes).first(8), kMagic));

    const auto back = ArchiveHeader::decode(bytes);
    CHECK(back.index_offset == -4096);
    CHECK(back.align_log2 == 7);
    CHECK(back.base_offset == 128 * 1000);
}

TEST(archive_header_rejects_damage)
{
    const auto good = ArchiveHeader{}.encode();

    auto bad_magic = good;
    bad_magic[0] = 'Z';
    CHECK_THROWS(ArchiveHeader::decode(bad_magic));

    for (std::size_t i = 8; i < kHeaderSize; ++i) {
        auto flipped = good;
        flipped[i] ^= 0x10;
        CHECK_THROWS(ArchiveHeader::decode(flipped));
    }

    ArchiveHeader future;
    future.version.major = kFormatVersion.major + 1;
    CHECK_THROWS(ArchiveHeader::decode(future.encode()));

    ArchiveHeader bad_align;
    bad_align.align_log2 = kAlignLog2Max + 1;
    CHECK_THROWS(ArchiveHeader::decode(bad_align.encode()));

    ArchiveHeader unaligned_base;
    unaligned_base.align_log2 = 6;
    unaligned_base.base_offset = 32;
    CHECK_THROWS(ArchiveHeader::decode(unaligned_base.encode()));
}

TEST(archive_header_accepts_tight_packing)
{
    for (std::uint8_t log2 = 0; log2 < 3; ++log2) {
        ArchiveHeader h;
        h.align_log2 = log2;
        h.base_offset = 3u << log2;
        CHECK(ArchiveHeader::decode(h.encode()).align_log2 == log2);
    }
}

TEST(archive_header_accepts_newer_minor)
{
    ArchiveHeader h;
    h.version.minor = kFormatVersion.minor + 1;
    CHECK(ArchiveHeader::decode(h.encode()).version.minor == h.version.minor);
}

TEST(block_header_round_trip)
{
    BlockHeader h;
    h.type = BlockType::DataZstd;
    h.payload_size = 1 << 20;
    h.offset = -4096;
    h.payload_crc = 0xdeadbeef;

    const auto back = BlockHeader::decode(h.encode(), -4096);
    CHECK(back.type == BlockType::DataZstd);
    CHECK(back.payload_size == h.payload_size);
    CHECK(back.offset == -4096);
    CHECK(back.payload_crc == 0xdeadbeef);
}

TEST(block_header_rejects_damage)
{
    BlockHeader h;
    h.type = BlockType::IndexRaw;
    h.offset = 32;
    const auto good = h.encode();

    CHECK_THROWS(BlockHeader::decode(good, 64));    // lives somewhere else
    for (std::size_t i = 0; i < kHeaderSize; ++i) {
        auto flipped = good;
        flipped[i] ^= 0x01;
        CHECK_THROWS(BlockHeader::decode(flipped, 32));
    }
}

TEST(block_header_accepts_unknown_type)
{
    BlockHeader h;
    h.type = static_cast<BlockType>(0x8001);
    CHECK(BlockHeader::decode(h.encode(), 0).type == h.type);
}
