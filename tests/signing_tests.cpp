// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// Hashed and signed archives: what the writer stores, what the reader
// checks, and what tampering it catches.
#include "check.hpp"
#include "util.hpp"

#include "zar/archive_reader.hpp"
#include "zar/archive_writer.hpp"
#include "zar/crc32c.hpp"
#include "zar/creator.hpp"
#include "zar/editor.hpp"
#include "zar/extractor.hpp"
#include "zar/fields.hpp"
#include "zar/posix.hpp"
#include "zar/thread_pool.hpp"

#include <fcntl.h>
#include <functional>
#include <unistd.h>

using namespace zar;
using namespace zar::test;

namespace {

SecretKey test_key(std::uint8_t n)
{
    std::array<std::uint8_t, 32> seed{};
    seed.fill(n);
    return SecretKey::from_seed(seed);
}

// A small tree: a multi-chunk file, a small one, a duplicate, a hard link,
// and text similar enough to train a dictionary on.
fs::path make_tree(const fs::path& dir)
{
    const auto src = dir / "src";
    write_file(src / "big.bin", random_bytes(200'000, 1));
    write_file(src / "copy.bin", random_bytes(200'000, 1));
    for (int i = 0; i < 40; ++i)
        write_file(src / ("text/f" + std::to_string(i) + ".txt"),
                   text("struct record_" + std::to_string(i) + " { int id; char name[32]; double value; };\n"));
    fs::create_hard_link(src / "big.bin", src / "hard.bin");
    return src;
}

fs::path make_archive(const fs::path& archive, const fs::path& src, const CreateOptions& options)
{
    auto out = Output::create_file(archive, true);
    auto opts = options;
    opts.chunk_size = 64 << 10;
    create_archive(out, std::vector{src / "."}, opts);
    out.commit();
    return archive;
}

CreateOptions signed_by(std::initializer_list<std::uint8_t> keys)
{
    CreateOptions options;
    for (const auto k : keys)
        options.sign.push_back(test_key(k));
    options.sequence = 5;
    options.sign_time = 1'800'000'000;
    options.sign_name = "test";
    return options;
}

// Tests (or scans) everything; returns the warnings.
std::vector<std::string> check(ArchiveReader& reader, ExtractMode mode = ExtractMode::Test)
{
    std::vector<std::string> warnings;
    ExtractOptions options;
    options.mode = mode;
    options.on_warning = [&](std::string_view m) { warnings.emplace_back(m); };
    const auto groups = group_by_directory(reader.entries());
    Extractor extractor(reader, options);
    extractor.prepare(groups);
    for (const auto& group : groups) {
        extractor.enter(group);
        for (const auto* member : group.members)
            extractor.extract(*member);
    }
    extractor.finish();
    return warnings;
}

std::vector<std::string> check(const fs::path& archive, ExtractMode mode = ExtractMode::Test)
{
    auto reader = ArchiveReader::open(archive);
    return check(reader, mode);
}

bool mentions(const std::vector<std::string>& warnings, std::string_view text)
{
    return std::ranges::any_of(warnings, [&](const std::string& w) { return w.find(text) != std::string::npos; });
}

const Entry& find(const ArchiveReader& reader, std::string_view path)
{
    for (const auto& e : reader.entries())
        if (e.path == path)
            return e;
    throw std::runtime_error("no entry " + std::string(path));
}

// Rewrites the block at `offset` in an archive that starts its file:
// `change` may alter its header and payload (keeping the payload's size).
// The CRCs are then recomputed, so only a hash can tell.
void tamper(const fs::path& archive, std::int64_t offset, const std::function<void(BlockHeader&, Bytes&)>& change)
{
    const auto base = ArchiveReader::open(archive).header().base_offset;
    const auto position = static_cast<std::uint64_t>(static_cast<std::int64_t>(base) + offset);
    const UniqueFd fd(::open(archive.c_str(), O_RDWR));
    HeaderBytes header_bytes;
    pread_full(fd.get(), header_bytes, position, "archive");
    auto block = BlockHeader::decode(header_bytes, offset);
    Bytes payload(static_cast<std::size_t>(block.payload_size));
    pread_full(fd.get(), payload, position + kHeaderSize, "archive");
    change(block, payload);
    block.payload_crc = crc32c(payload);
    pwrite_all(fd.get(), block.encode(), position, "archive");
    pwrite_all(fd.get(), payload, position + kHeaderSize, "archive");
}

// The offset of the root: the header's, or the trailer's.
std::int64_t root_offset(const fs::path& archive)
{
    const auto header = ArchiveReader::open(archive).header();
    return header.index_offset;
}

} // namespace

TEST(hashed_archives_hash_every_block)
{
    TempDir tmp;
    const auto src = make_tree(tmp.path());
    CreateOptions options;
    options.hash = true;
    options.dict_size = 4096;
    const auto reader = ArchiveReader::open(make_archive(tmp.path() / "a.zar", src, options));
    CHECK(!reader.signature());

    bool saw_dict = false;
    for (const auto& e : reader.entries()) {
        if (e.chunks.empty())
            continue;
        CHECK(e.chunk_hashes.size() == e.chunks.size());
        for (std::size_t i = 0; i < e.chunks.size(); ++i) {
            const auto block = reader.block_header(e.chunks[i]);
            CHECK(e.chunk_hashes[i] == block_hash(block.type, reader.payload(block)));
        }
        if (e.dict) {
            saw_dict = true;
            const auto dict = reader.block_header(*e.dict);
            CHECK(e.dict_hash == block_hash(BlockType::ZstdDict, reader.payload(dict)));
        }
    }
    CHECK(saw_dict);
    CHECK(find(reader, "hard.bin").chunk_hashes == find(reader, "big.bin").chunk_hashes);
    CHECK(find(reader, "copy.bin").chunk_hashes == find(reader, "big.bin").chunk_hashes);    // deduplicated
    CHECK(check(tmp.path() / "a.zar").empty());

    // Unless asked, nothing is hashed.
    const auto plain = ArchiveReader::open(make_archive(tmp.path() / "b.zar", src, {}));
    CHECK(find(plain, "big.bin").chunk_hashes.empty());
}

TEST(hashes_catch_tampering_before_decompression)
{
    TempDir tmp;
    const auto src = make_tree(tmp.path());
    CreateOptions options;
    options.hash = true;
    const auto archive = tmp.path() / "a.zar";

    // A changed byte, with the block's CRC fixed up.
    make_archive(archive, src, options);
    const auto chunk = find(ArchiveReader::open(archive), "text/f3.txt").chunks.front();
    tamper(archive, chunk, [](BlockHeader&, Bytes& payload) { payload.back() ^= 1; });
    for (const auto mode : {ExtractMode::Test, ExtractMode::Scan}) {
        const auto warnings = check(archive, mode);
        CHECK(warnings.size() == 1 && mentions(warnings, "text/f3.txt") && mentions(warnings, "fails its hash"));
    }

    // The same bytes, presented as another type of block.
    make_archive(archive, src, options);
    const auto big = find(ArchiveReader::open(archive), "text/f7.txt").chunks.front();
    tamper(archive, big, [](BlockHeader& block, Bytes&) {
        block.type = block.type == BlockType::DataRaw ? BlockType::DataZstd : BlockType::DataRaw;
    });
    CHECK(mentions(check(archive, ExtractMode::Scan), "fails its hash"));
}

TEST(signed_archives_name_their_root_and_signers)
{
    TempDir tmp;
    const auto src = make_tree(tmp.path());
    for (const bool head_index : {false, true}) {
        auto options = signed_by({1, 2});
        options.head_index = head_index;
        options.dict_size = 4096;
        const auto archive = make_archive(tmp.path() / "s.zar", src, options);
        const auto reader = ArchiveReader::open(archive);
        CHECK(reader.signature().has_value());
        const auto& statement = reader.signature()->statement;
        CHECK(statement.sequence == 5 && statement.time == 1'800'000'000 && statement.name == "test");
        CHECK(reader.signature()->signers.size() == 2);

        const PublicKey key1[] = {test_key(1).public_key()};
        const PublicKey key2[] = {test_key(2).public_key()};
        const PublicKey stranger[] = {test_key(3).public_key()};
        CHECK(reader.signed_by(key1) == key1[0]);
        CHECK(reader.signed_by(key2) == key2[0]);
        CHECK(!reader.signed_by(stranger));
        CHECK(root_offset(archive) < 0 == head_index);
        CHECK(check(archive).empty());

        if (head_index) {    // and from a pipe
            const auto bytes = read_file(archive);
            int fds[2];
            CHECK(::pipe(fds) == 0);
            JoiningThread feeder([&, fd = fds[1]] {
                write_all(fd, bytes, "pipe");
                ::close(fd);
            });
            auto stream = ArchiveReader::open_stream(fds[0], "pipe");
            CHECK(stream.signed_by(key1).has_value());
            CHECK(check(stream).empty());
            feeder.join();
            ::close(fds[0]);
        }
    }
}

TEST(signatures_catch_a_changed_statement_or_root)
{
    TempDir tmp;
    const auto src = make_tree(tmp.path());
    const auto archive = tmp.path() / "s.zar";
    const PublicKey key[] = {test_key(1).public_key()};

    // A changed statement: the structure is fine, but no signature matches.
    make_archive(archive, src, signed_by({1}));
    // Version byte, ROOT (10 bytes), ROOT_HASH (35), then SEQUENCE's tag and
    // length: its value starts at byte 48.
    tamper(archive, root_offset(archive), [](BlockHeader&, Bytes& payload) { payload[48] ^= 1; });
    CHECK(ArchiveReader::open(archive).signature()->statement.sequence == 4);
    CHECK(!ArchiveReader::open(archive).signed_by(key));

    // A changed root index: caught before it is decompressed or trusted.
    make_archive(archive, src, signed_by({1}));
    const auto root = ArchiveReader::open(archive).signature()->statement.root;
    tamper(archive, root, [](BlockHeader&, Bytes& payload) { payload[payload.size() / 2] ^= 1; });
    CHECK_THROWS_WITH(ArchiveReader::open(archive), "fails its hash");

    // A changed data block, in a signed archive.
    make_archive(archive, src, signed_by({1}));
    const auto chunk = find(ArchiveReader::open(archive), "big.bin").chunks[1];
    tamper(archive, chunk, [](BlockHeader&, Bytes& payload) { payload[0] ^= 0x80; });
    auto reader = ArchiveReader::open(archive);
    CHECK(reader.signed_by(key).has_value());    // the signature itself is intact
    const auto warnings = check(reader);
    CHECK(mentions(warnings, "big.bin") && mentions(warnings, "fails its hash"));
}

TEST(signed_archives_must_hash_everything)
{
    TempDir tmp;
    const auto archive = tmp.path() / "x.zar";
    // Writes a signed archive whose one file has these index fields.
    const auto write = [&](const std::function<void(FieldWriter&, std::int64_t chunk, const Digest&)>& fields) {
        auto out = Output::create_file(archive, true);
        ArchiveWriter writer(out, kAlignLog2Default);
        const auto data = text("some data");
        const auto chunk = writer.write_block(BlockType::DataRaw, data);
        const auto hash = block_hash(BlockType::DataRaw, data);
        Bytes body;
        FieldWriter field(body);
        field.bytes(Tag::Path, std::string_view("f"));
        field.unsigned_number(Tag::Size, data.size());
        field.offsets(Tag::Chunks, std::vector{chunk});
        fields(field, chunk, hash);
        Bytes index{static_cast<std::uint8_t>(EntryType::File), 0};
        append_varint(index, body.size());
        index.insert(index.end(), body.begin(), body.end());
        const SecretKey keys[] = {test_key(1)};
        writer.finish(BlockType::IndexRaw, index, [&](std::int64_t at) {
            SignedRoot statement;
            statement.root = at;
            statement.root_hash = block_hash(BlockType::IndexRaw, index);
            return encode_signature(statement, keys);
        });
        out.commit();
    };
    const auto hashes = [](std::uint8_t algorithm, const Digest& d) {
        Bytes value{algorithm};
        value.insert(value.end(), d.begin(), d.end());
        return value;
    };

    write([&](FieldWriter& f, std::int64_t, const Digest& h) { f.bytes(Tag::ChunkHashes, ByteView(hashes(1, h))); });
    CHECK(ArchiveReader::open(archive).entries().size() == 1);

    write([](FieldWriter&, std::int64_t, const Digest&) {});
    CHECK_THROWS_WITH(ArchiveReader::open(archive), "its chunks have no hashes");

    write([&](FieldWriter& f, std::int64_t, const Digest& h) { f.bytes(Tag::ChunkHashes, ByteView(hashes(77, h))); });
    CHECK_THROWS_WITH(ArchiveReader::open(archive), "hash algorithm");

    write([&](FieldWriter& f, std::int64_t, const Digest& h) {
        f.bytes(Tag::ChunkHashes, ByteView(hashes(1, h)));
        f.fixed(Tag::Dict, std::uint64_t{0});
    });
    CHECK_THROWS_WITH(ArchiveReader::open(archive), "its dictionary has no hash");

    write([&](FieldWriter& f, std::int64_t, const Digest&) {
        Digest wrong{};
        wrong[0] = 1;
        f.bytes(Tag::ChunkHashes, ByteView(hashes(1, wrong)));
    });
    CHECK(mentions(check(archive), "fails its hash"));
}

TEST(sub_index_hashes_are_checked)
{
    TempDir tmp;
    const auto archive = tmp.path() / "sub.zar";
    const auto write = [&](bool right) {
        auto out = Output::create_file(archive, true);
        ArchiveWriter writer(out, kAlignLog2Default);
        Entry inner;
        inner.path = "inner";
        inner.type = EntryType::Dir;
        Bytes sub;
        encode_entry(sub, inner);
        const auto sub_at = writer.write_block(BlockType::IndexRaw, sub);
        Entry dir;
        dir.path = "d";
        dir.type = EntryType::Dir;
        dir.subindex = sub_at;
        dir.subindex_hash = block_hash(BlockType::IndexRaw, sub);
        if (!right)
            (*dir.subindex_hash)[31] ^= 1;
        Bytes root;
        encode_entry(root, dir);
        writer.finish(BlockType::IndexRaw, root);
        out.commit();
    };
    write(true);
    CHECK(ArchiveReader::open(archive).entries().size() == 2);
    write(false);
    CHECK_THROWS_WITH(ArchiveReader::open(archive), "fails its hash");
}

TEST(rebuilding_checks_keeps_and_adds_hashes)
{
    TempDir tmp;
    const auto src = make_tree(tmp.path());
    write_file(tmp.path() / "add/new.txt", text("new"));
    EditLists lists;
    lists.add = {"new.txt"};
    lists.remove = {"copy.bin"};
    const auto rebuild = [&](const fs::path& from, const fs::path& to, const CreateOptions& options) {
        const auto source = ArchiveReader::open(from);
        const auto plan = plan_edit(source.entries(), lists);
        auto out = Output::create_file(to, true);
        auto opts = options;
        opts.chunk_size = 64 << 10;
        const auto stats = rebuild_archive(out, source, plan.keep, plan.additions, tmp.path() / "add", opts);
        out.commit();
        return stats;
    };
    const PublicKey key[] = {test_key(1).public_key()};

    // A signed source: hashes kept and added, the signature dropped...
    const auto signed_archive = make_archive(tmp.path() / "s.zar", src, signed_by({1}));
    rebuild(signed_archive, tmp.path() / "r.zar", {});
    auto rebuilt = ArchiveReader::open(tmp.path() / "r.zar");
    CHECK(!rebuilt.signature());
    CHECK(find(rebuilt, "big.bin").chunk_hashes == find(ArchiveReader::open(signed_archive), "big.bin").chunk_hashes);
    CHECK(!find(rebuilt, "new.txt").chunk_hashes.empty());
    CHECK(check(rebuilt).empty());

    // ...unless the result is signed again.
    rebuild(signed_archive, tmp.path() / "r.zar", signed_by({1}));
    CHECK(ArchiveReader::open(tmp.path() / "r.zar").signed_by(key).has_value());

    // An unhashed source can be signed, without making it again.
    const auto plain = make_archive(tmp.path() / "p.zar", src, {});
    const auto stats = rebuild(plain, tmp.path() / "r.zar", signed_by({1}));
    CHECK(stats.signed_by == 1);
    auto resigned = ArchiveReader::open(tmp.path() / "r.zar");
    CHECK(resigned.signed_by(key).has_value() && check(resigned).empty());

    // A kept block that fails its hash stops the rebuild.
    const auto chunk = find(ArchiveReader::open(signed_archive), "big.bin").chunks[2];
    tamper(signed_archive, chunk, [](BlockHeader&, Bytes& payload) { payload[1] ^= 1; });
    CHECK_THROWS_WITH(rebuild(signed_archive, tmp.path() / "r.zar", {}), "fails its hash");
}

TEST(solid_archives_cannot_be_signed)
{
    TempDir tmp;
    const auto src = make_tree(tmp.path());
    auto options = signed_by({1});
    options.solid_limit = 1 << 20;
    auto out = Output::create_file(tmp.path() / "s.zar", true);
    CHECK_THROWS_WITH(create_archive(out, std::vector{src / "."}, options), "solid");
}
