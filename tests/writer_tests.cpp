// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// End-to-end: write archives with create_archive(), then decode them using
// only the format primitives and libzstd.
#include "check.hpp"
#include "util.hpp"

#include "zar/crc32c.hpp"
#include "zar/creator.hpp"
#include "zar/dictionary.hpp"
#include "zar/index.hpp"

#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <random>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

using namespace zar;
using namespace zar::test;
namespace fs = std::filesystem;

namespace {

// A minimal, independent reader over an in-memory archive.
struct Archive {
    Bytes bytes;
    ArchiveHeader header;
    std::map<std::string, Entry> entries;

    explicit Archive(Bytes data) : bytes(std::move(data))
    {
        header = ArchiveHeader::decode(header_at(0));

        // The trailer must agree with the header (§5).
        const auto trailer_at = static_cast<std::int64_t>(bytes.size() - kTrailerSize);
        const auto trailer = BlockHeader::decode(header_at(trailer_at), trailer_at);
        CHECK(trailer.type == BlockType::Trailer);
        CHECK(load_le<std::uint64_t>(&bytes[bytes.size() - 8]) == static_cast<std::uint64_t>(trailer_at));
        const auto trailer_index = static_cast<std::int64_t>(load_le<std::uint64_t>(&bytes[bytes.size() - 16]));
        CHECK(header.index_offset == trailer_index);

        for (auto& e : decode_index(block(header.index_offset)))
            entries.emplace(e.path, std::move(e));
    }

    HeaderBytes header_at(std::int64_t offset) const
    {
        HeaderBytes h;
        std::copy_n(bytes.begin() + offset, kHeaderSize, h.begin());
        return h;
    }

    BlockHeader block_header(std::int64_t offset) const
    {
        return BlockHeader::decode(header_at(offset), offset);
    }

    // The decompressed payload of the block at `offset`, after checking its CRC.
    Bytes block(std::int64_t offset) const
    {
        const auto h = block_header(offset);
        const ByteView payload(&bytes[static_cast<std::size_t>(offset) + kHeaderSize],
                               static_cast<std::size_t>(h.payload_size));
        if (crc32c(payload) != h.payload_crc)
            throw std::runtime_error("payload checksum mismatch");

        if (h.type == BlockType::IndexRaw || h.type == BlockType::DataRaw)
            return {payload.begin(), payload.end()};

        const auto size = ZSTD_getFrameContentSize(payload.data(), payload.size());
        if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR)
            throw std::runtime_error("zstd frame lacks a content size");
        Bytes out(static_cast<std::size_t>(size));
        if (ZSTD_decompress(out.data(), out.size(), payload.data(), payload.size()) != size)
            throw std::runtime_error("zstd decompression failed");
        return out;
    }

    Bytes content(const Entry& e) const
    {
        Bytes out;
        for (const auto offset : e.chunks) {
            const auto chunk = block(offset);
            out.insert(out.end(), chunk.begin(), chunk.end());
        }
        return out;
    }

    const Entry* find(const std::string& path) const
    {
        const auto it = entries.find(path);
        return it == entries.end() ? nullptr : &it->second;
    }
};

Archive build(const fs::path& out_path, std::vector<fs::path> inputs, CreateOptions options,
              CreateStats* stats = nullptr)
{
    auto out = Output::create_file(out_path, true);
    const auto result = create_archive(out, inputs, options);
    out.commit();
    if (stats)
        *stats = result;
    return Archive(read_file(out_path));
}

} // namespace

TEST(writer_round_trips_a_tree)
{
    TempDir tmp;
    const auto src = tmp.path() / "project";
    const auto big = random_bytes(10'000, 1);
    Bytes repetitive;
    for (int i = 0; i < 2000; ++i) {
        const auto line = text("line " + std::to_string(i % 7) + " of a compressible file\n");
        repetitive.insert(repetitive.end(), line.begin(), line.end());
    }

    write_file(src / "README", text("hello\n"));
    write_file(src / "empty", {});
    write_file(src / "src/big.bin", big);
    write_file(src / "src/notes.txt", repetitive);
    write_file(src / "photo.jpg", repetitive);    // compressible, but stored raw by extension
    fs::create_directories(src / "empty-dir");
    fs::create_symlink("src/notes.txt", src / "link");
    fs::create_hard_link(src / "README", src / "README.hard");

    CreateOptions options;
    options.chunk_size = 4096;
    options.threads = 3;
    CreateStats stats;
    const auto archive = build(tmp.path() / "out.zar", {src / "."}, options, &stats);

    CHECK(stats.warnings == 0);
    CHECK(stats.files == 5);
    CHECK(stats.hardlinks == 1);
    CHECK(stats.symlinks == 1);
    CHECK(archive.header.align_log2 == kAlignLog2Default);

    // "project/." stores the directory's contents, not the directory.
    CHECK(archive.find("project") == nullptr);
    CHECK(archive.entries.size() == 9);

    const auto* readme = archive.find("README");
    CHECK(readme && readme->type == EntryType::File);
    CHECK(readme && archive.content(*readme) == text("hello\n"));
    CHECK(readme && readme->content_crc == crc32c(text("hello\n")));
    struct stat st {};
    ::stat((src / "README").c_str(), &st);
    CHECK(readme && readme->mode == (st.st_mode & 07777));

    const auto* empty = archive.find("empty");
    CHECK(empty && !empty->size && empty->chunks.empty());

    const auto* bin = archive.find("src/big.bin");
    CHECK(bin && bin->size == big.size());
    CHECK(bin && bin->chunk_size == 4096);
    CHECK(bin && bin->chunks.size() == 3);
    CHECK(bin && archive.content(*bin) == big);
    for (const auto offset : bin ? bin->chunks : std::vector<std::int64_t>{})
        CHECK(archive.block_header(offset).type == BlockType::DataRaw);    // random: incompressible

    const auto* notes = archive.find("src/notes.txt");
    CHECK(notes && archive.content(*notes) == repetitive);
    CHECK(notes && archive.block_header(notes->chunks.front()).type == BlockType::DataZstd);

    const auto* photo = archive.find("photo.jpg");
    CHECK(photo && archive.content(*photo) == repetitive);
    CHECK(photo && archive.block_header(photo->chunks.front()).type == BlockType::DataRaw);

    const auto* dir = archive.find("empty-dir");
    CHECK(dir && dir->type == EntryType::Dir);
    CHECK(archive.find("src") && archive.find("src")->type == EntryType::Dir);

    const auto* link = archive.find("link");
    CHECK(link && link->type == EntryType::Symlink && link->link_target == "src/notes.txt");

    const auto* hard = archive.find("README.hard");
    CHECK(hard && hard->type == EntryType::Hardlink && hard->link_target == "README");
    CHECK(hard && readme && hard->chunks == readme->chunks);    // data stored once
}

TEST(writer_stores_multiple_inputs_by_name)
{
    TempDir tmp;
    write_file(tmp.path() / "a/one.txt", text("1"));
    write_file(tmp.path() / "b.txt", text("2"));

    const auto archive = build(tmp.path() / "out.zar", {tmp.path() / "a", tmp.path() / "b.txt"}, {});
    CHECK(archive.find("a") && archive.find("a")->type == EntryType::Dir);
    CHECK(archive.find("a/one.txt") && archive.content(*archive.find("a/one.txt")) == text("1"));
    CHECK(archive.find("b.txt") && archive.content(*archive.find("b.txt")) == text("2"));
}

TEST(writer_aligns_payloads)
{
    TempDir tmp;
    for (int i = 0; i < 20; ++i)
        write_file(tmp.path() / "in" / ("f" + std::to_string(i)), random_bytes(static_cast<std::size_t>(i * 37), static_cast<unsigned>(i)));

    for (const std::uint8_t log2 : {std::uint8_t{0}, std::uint8_t{3}, std::uint8_t{7}}) {
        CreateOptions options;
        options.align_log2 = log2;
        const auto archive = build(tmp.path() / "out.zar", {tmp.path() / "in" / "."}, options);
        CHECK(archive.header.align_log2 == log2);
        CHECK((archive.header.index_offset + 32) % (1 << log2) == 0);
        for (const auto& [path, entry] : archive.entries)
            for (const auto offset : entry.chunks)
                CHECK((offset + 32) % (1 << log2) == 0);
    }
}

TEST(writer_handles_non_utf8_names)
{
    TempDir tmp;
    const std::string latin1_name = "caf\xe9.txt";
    write_file(tmp.path() / "in" / "ok.txt", text("ok"));
    if (!std::ofstream(tmp.path() / "in" / latin1_name)) {
        std::cout << "  (filesystem rejects non-UTF-8 names; skipping)\n";
        return;
    }

    CreateStats stats;
    auto archive = build(tmp.path() / "out.zar", {tmp.path() / "in" / "."}, {}, &stats);
    CHECK(stats.warnings == 1);
    CHECK(archive.find(latin1_name) && archive.find(latin1_name)->flags == entry_flags::kPathRaw);

    CreateOptions latin1;
    latin1.names = NameMode::Latin1;
    archive = build(tmp.path() / "out.zar", {tmp.path() / "in" / "."}, latin1);
    CHECK(archive.find("caf\xc3\xa9.txt") && archive.find("caf\xc3\xa9.txt")->flags == 0);

    CreateOptions skip;
    skip.names = NameMode::Skip;
    archive = build(tmp.path() / "out.zar", {tmp.path() / "in" / "."}, skip);
    CHECK(archive.entries.size() == 1);
}

TEST(writer_streams_without_seeking)
{
    TempDir tmp;
    write_file(tmp.path() / "in/x", text("streamed"));
    const auto path = tmp.path() / "out.zar";

    // An O_APPEND descriptor can't be patched in place, like a pipe.
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    const int saved = ::dup(STDOUT_FILENO);
    ::dup2(fd, STDOUT_FILENO);
    ::close(fd);
    {
        auto out = Output::standard_output();
        CHECK(!out.seekable());
        create_archive(out, std::vector{tmp.path() / "in" / "."}, {});
        out.commit();
    }
    ::dup2(saved, STDOUT_FILENO);
    ::close(saved);

    const auto bytes = read_file(path);
    HeaderBytes h;
    std::copy_n(bytes.begin(), kHeaderSize, h.begin());
    CHECK(ArchiveHeader::decode(h).index_offset == kNoOffset);

    // The trailer alone locates the index.
    const auto index = static_cast<std::int64_t>(load_le<std::uint64_t>(&bytes[bytes.size() - 16]));
    auto patched = bytes;
    ArchiveHeader fixed = ArchiveHeader::decode(h);
    fixed.index_offset = index;
    const auto encoded = fixed.encode();
    std::copy(encoded.begin(), encoded.end(), patched.begin());
    const Archive archive(patched);
    CHECK(archive.find("x") && archive.content(*archive.find("x")) == text("streamed"));
}

TEST(inputs_follow_posix_path_rules)
{
    TempDir tmp;
    write_file(tmp.path() / "proj/src/main.cpp", text("int main() {}"));
    write_file(tmp.path() / "file.txt", text("plain"));
    fs::create_directory_symlink("proj", tmp.path() / "link");
    const auto out = tmp.path() / "out.zar";

    // A directory is stored under its own name.
    auto archive = build(out, {tmp.path() / "proj"}, {});
    CHECK(archive.find("proj") && archive.find("proj")->type == EntryType::Dir);
    CHECK(archive.find("proj/src/main.cpp") != nullptr);

    // A trailing slash changes nothing for a directory...
    archive = build(out, {tmp.path() / "proj/"}, {});
    CHECK(archive.find("proj/src/main.cpp") != nullptr);

    // ...but a file with one is an error, as in POSIX.
    CHECK_THROWS(build(out, {tmp.path() / "file.txt/"}, {}));
    archive = build(out, {tmp.path() / "file.txt"}, {});
    CHECK(archive.entries.size() == 1 && archive.find("file.txt"));

    // "/." stores the contents.
    archive = build(out, {tmp.path() / "proj/."}, {});
    CHECK(archive.find("proj") == nullptr);
    CHECK(archive.find("src/main.cpp") != nullptr);

    // A symlink is stored as a symlink, unless a slash says to follow it.
    archive = build(out, {tmp.path() / "link"}, {});
    CHECK(archive.entries.size() == 1 && archive.find("link")->type == EntryType::Symlink);
    archive = build(out, {tmp.path() / "link/"}, {});
    CHECK(archive.find("link") && archive.find("link")->type == EntryType::Dir);
    CHECK(archive.find("link/src/main.cpp") != nullptr);

    // Missing inputs and the root by name are errors.
    CHECK_THROWS(build(out, {tmp.path() / "missing"}, {}));
    CHECK_THROWS(build(out, {"/"}, {}));
}

TEST(several_inputs_must_not_collide)
{
    TempDir tmp;
    write_file(tmp.path() / "a/src/one.txt", text("1"));
    write_file(tmp.path() / "b/src/two.txt", text("2"));
    write_file(tmp.path() / "b/same.txt", text("b"));
    write_file(tmp.path() / "c/same.txt", text("c"));
    const auto out = tmp.path() / "out.zar";

    const auto archive = build(out, {tmp.path() / "a", tmp.path() / "b"}, {});
    CHECK(archive.find("a/src/one.txt") && archive.find("b/src/two.txt"));

    CHECK_THROWS(build(out, {tmp.path() / "a/src", tmp.path() / "b/src"}, {}));    // both "src"
    CHECK_THROWS(build(out, {tmp.path() / "b/.", tmp.path() / "c/."}, {}));        // both "same.txt"
}

TEST(default_archive_names)
{
    const auto name = [](const char* input) { return default_archive_path(parse_input(input)).native(); };
    CHECK(name("proj") == "proj.zar");
    CHECK(name("proj/") == "proj.zar");
    CHECK(name("proj/.") == "proj.zar");
    CHECK(name("a/b/proj//") == "proj.zar");    // always in the current directory
    CHECK(name("/a/b/proj/.") == "proj.zar");
    CHECK(name("a/b/notes.txt") == "notes.txt.zar");
    CHECK(name(".") == fs::current_path().filename().native() + ".zar");
    CHECK(name("./") == name("."));
    CHECK(name("..") == fs::current_path().parent_path().filename().native() + ".zar");
    CHECK(name("../.") == name(".."));
    CHECK_THROWS(name("/."));
}

TEST(dictionary_samples_stay_bounded_and_deterministic)
{
    const auto collect = [] {
        SampleCollector samples;
        for (unsigned i = 0; i < 1000; ++i)
            samples.add(random_bytes(200'000, i % 50));    // only the first 128K is kept
        return samples;
    };
    const auto a = collect();
    CHECK(a.total() <= (32 << 20) + (128 << 10));
    CHECK(a.total() > (8 << 20));
    CHECK(a.total() == collect().total());
    CHECK(a.count() == a.total() / (128 << 10));

    SampleCollector few;
    few.add(text("too little to train on"));
    CHECK(!few.train(kDefaultDictSize));
}
