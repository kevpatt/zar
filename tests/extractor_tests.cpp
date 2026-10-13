// End-to-end: archives written by create_archive() (or crafted by hand),
// read back with ArchiveReader and Extractor.
#include "check.hpp"
#include "util.hpp"

#include "zar/archive_reader.hpp"
#include "zar/archive_writer.hpp"
#include "zar/creator.hpp"
#include "zar/extractor.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace zar;
using namespace zar::test;

namespace {

fs::path make_archive(const fs::path& archive, const fs::path& source)
{
    auto out = Output::create_file(archive, true);
    create_archive(out, std::vector{source / "."}, {});
    out.commit();
    return archive;
}

struct Item {
    Entry entry;
    Bytes data;
};

// Writes an archive with exactly these entries, valid or not.
fs::path craft_archive(const fs::path& archive, std::vector<Item> items)
{
    auto out = Output::create_file(archive, true);
    ArchiveWriter writer(out, kAlignLog2Default);
    Bytes index;
    for (auto& [entry, data] : items) {
        if (!data.empty()) {
            entry.size = data.size();
            entry.chunks = {writer.write_block(BlockType::DataRaw, data)};
        }
        encode_entry(index, entry);
    }
    writer.finish(BlockType::IndexRaw, index);
    out.commit();
    return archive;
}

Entry make(EntryType type, std::string path, std::optional<std::string> target = {})
{
    Entry e;
    e.type = type;
    e.path = std::move(path);
    e.link_target = std::move(target);
    return e;
}

struct Run {
    ExtractStats stats;
    std::vector<std::string> warnings;
};

Run run(const fs::path& archive, ExtractOptions options)
{
    Run result;
    options.on_warning = [&](std::string_view m) { result.warnings.emplace_back(m); };
    auto reader = ArchiveReader::open(archive, options.on_warning);
    const auto groups = group_by_directory(reader.entries());
    Extractor extractor(reader, options);
    extractor.prepare(groups);
    for (const auto& group : groups) {
        extractor.enter(group);
        for (const auto* member : group.members)
            extractor.extract(*member);
    }
    result.stats = extractor.finish();
    return result;
}

ExtractOptions into(const fs::path& dir, ExtractMode mode = ExtractMode::Extract)
{
    ExtractOptions options;
    options.directory = dir;
    options.mode = mode;
    return options;
}

struct stat stat_of(const fs::path& path)
{
    struct stat st {};
    ::lstat(path.c_str(), &st);
    return st;
}

} // namespace

TEST(extract_round_trips_contents_and_metadata)
{
    TempDir tmp;
    const auto src = tmp.path() / "src";
    const auto blob = random_bytes(300'000, 7);
    write_file(src / "a.txt", text("alpha\n"));
    write_file(src / "dir/blob.bin", blob);
    write_file(src / "locked/inside.txt", text("in a read-only directory"));
    fs::create_directories(src / "empty");
    fs::create_symlink("dir/blob.bin", src / "link");
    fs::create_hard_link(src / "a.txt", src / "a.hard");
    ::chmod((src / "a.txt").c_str(), 0640);
    ::chmod((src / "locked").c_str(), 0555);
    const timespec times[2] = {{0, UTIME_OMIT}, {1'600'000'000, 0}};
    ::utimensat(AT_FDCWD, (src / "dir/blob.bin").c_str(), times, 0);
    ::utimensat(AT_FDCWD, (src / "dir").c_str(), times, 0);

    const auto archive = make_archive(tmp.path() / "a.zar", src);
    const auto dest = tmp.path() / "out";
    const auto result = run(archive, into(dest));

    CHECK(result.warnings.empty());
    CHECK(result.stats.files == 4);    // including the hard link, extracted as a copy
    CHECK(read_file(dest / "a.txt") == text("alpha\n"));
    CHECK(read_file(dest / "a.hard") == text("alpha\n"));
    CHECK(read_file(dest / "dir/blob.bin") == blob);
    CHECK(read_file(dest / "locked/inside.txt") == text("in a read-only directory"));
    CHECK(fs::is_directory(dest / "empty"));
    CHECK(fs::read_symlink(dest / "link") == "dir/blob.bin");
    CHECK((stat_of(dest / "a.txt").st_mode & 07777) == 0640);
    CHECK((stat_of(dest / "locked").st_mode & 07777) == 0555);
    CHECK(stat_of(dest / "dir/blob.bin").st_mtime == 1'600'000'000);
    CHECK(stat_of(dest / "dir").st_mtime == 1'600'000'000);    // set after its contents

    ::chmod((src / "locked").c_str(), 0755);    // so TempDir can clean up
    ::chmod((dest / "locked").c_str(), 0755);
}

TEST(extract_requires_overwrite_for_existing_paths)
{
    TempDir tmp;
    write_file(tmp.path() / "src/f.txt", text("original"));
    const auto archive = make_archive(tmp.path() / "a.zar", tmp.path() / "src");
    const auto dest = tmp.path() / "out";
    run(archive, into(dest));

    write_file(dest / "f.txt", text("changed"));
    CHECK_THROWS(run(archive, into(dest)));
    CHECK(read_file(dest / "f.txt") == text("changed"));    // nothing was written

    auto options = into(dest);
    options.overwrite = true;
    CHECK(run(archive, options).warnings.empty());
    CHECK(read_file(dest / "f.txt") == text("original"));
}

TEST(extract_merges_or_replaces_directories)
{
    TempDir tmp;
    write_file(tmp.path() / "src/sub/f.txt", text("f"));
    const auto archive = make_archive(tmp.path() / "a.zar", tmp.path() / "src");
    const auto dest = tmp.path() / "out";
    write_file(dest / "sub/stale.txt", text("stale"));

    CHECK_THROWS(run(archive, into(dest)));    // the directory exists

    auto merge = into(dest);
    merge.overwrite = true;
    run(archive, merge);
    CHECK(fs::exists(dest / "sub/f.txt"));
    CHECK(fs::exists(dest / "sub/stale.txt"));

    auto replace = merge;
    replace.dirs = DirMode::Replace;
    run(archive, replace);
    CHECK(fs::exists(dest / "sub/f.txt"));
    CHECK(!fs::exists(dest / "sub/stale.txt"));
}

TEST(scan_and_test_detect_damage_without_writing)
{
    TempDir tmp;
    write_file(tmp.path() / "src/data.bin", random_bytes(50'000, 3));
    write_file(tmp.path() / "src/ok.txt", text("fine"));
    const auto archive = make_archive(tmp.path() / "a.zar", tmp.path() / "src");

    for (const auto mode : {ExtractMode::Scan, ExtractMode::Test})
        CHECK(run(archive, into(tmp.path() / "unused", mode)).warnings.empty());
    CHECK(!fs::exists(tmp.path() / "unused"));

    // Flip one byte in the middle of data.bin's stored payload.
    std::int64_t chunk = 0;
    std::uint64_t base = 0;
    {
        const auto reader = ArchiveReader::open(archive);
        for (const auto& e : reader.entries())
            if (e.path == "data.bin")
                chunk = e.chunks.front();
        base = reader.header().base_offset;
    }
    auto bytes = read_file(archive);
    bytes[static_cast<std::size_t>(base + static_cast<std::uint64_t>(chunk)) + kHeaderSize + 25'000] ^= 0x40;
    write_file(archive, bytes);

    for (const auto mode : {ExtractMode::Scan, ExtractMode::Test}) {
        const auto result = run(archive, into(tmp.path() / "unused", mode));
        CHECK(result.warnings.size() == 1);
        CHECK(!result.warnings.empty() && result.warnings[0].find("checksum") != std::string::npos);
    }

    const auto dest = tmp.path() / "out";
    const auto result = run(archive, into(dest));
    CHECK(result.warnings.size() == 1);
    CHECK(!fs::exists(dest / "data.bin"));    // no partial file left behind
    CHECK(read_file(dest / "ok.txt") == text("fine"));
}

TEST(parallel_extraction_is_exact_at_any_thread_count)
{
    TempDir tmp;
    const auto src = tmp.path() / "src";
    std::vector<std::pair<fs::path, Bytes>> files;
    for (unsigned i = 0; i < 40; ++i) {
        // Sizes straddle the chunk size: empty, partial, exact, and many chunks.
        const std::size_t size = (i % 5 == 0) ? 0 : (i % 5 == 1) ? 4096 : (i * 7919) % 60'000;
        auto data = (i % 2) ? random_bytes(size, i) : Bytes(size, static_cast<std::uint8_t>('a' + i % 26));
        files.emplace_back(fs::path("d" + std::to_string(i % 4)) / ("f" + std::to_string(i)), std::move(data));
        write_file(src / files.back().first, files.back().second);
    }

    const auto archive = tmp.path() / "a.zar";
    {
        auto out = Output::create_file(archive, true);
        CreateOptions options;
        options.chunk_size = 4096;
        create_archive(out, std::vector{src / "."}, options);
        out.commit();
    }

    for (const unsigned threads : {1u, 2u, 16u}) {
        const auto dest = tmp.path() / ("out" + std::to_string(threads));
        auto options = into(dest);
        options.threads = threads;
        const auto result = run(archive, options);
        CHECK(result.warnings.empty());
        CHECK(result.stats.files == files.size());
        for (const auto& [path, data] : files)
            CHECK(read_file(dest / path) == data);

        auto test = into(dest, ExtractMode::Test);
        test.threads = threads;
        CHECK(run(archive, test).warnings.empty());
    }
}

TEST(damage_in_one_chunk_fails_only_that_file)
{
    TempDir tmp;
    const auto src = tmp.path() / "src";
    write_file(src / "big.bin", random_bytes(100'000, 1));
    write_file(src / "other.bin", random_bytes(100'000, 2));
    const auto archive = tmp.path() / "a.zar";
    {
        auto out = Output::create_file(archive, true);
        CreateOptions options;
        options.chunk_size = 8192;
        create_archive(out, std::vector{src / "."}, options);
        out.commit();
    }

    std::int64_t middle = 0;
    {
        const auto reader = ArchiveReader::open(archive);
        for (const auto& e : reader.entries())
            if (e.path == "big.bin")
                middle = e.chunks[e.chunks.size() / 2];
    }
    auto bytes = read_file(archive);
    bytes[static_cast<std::size_t>(middle) + kHeaderSize + 100] ^= 0x01;
    write_file(archive, bytes);

    const auto dest = tmp.path() / "out";
    auto options = into(dest);
    options.threads = 8;
    const auto result = run(archive, options);
    CHECK(result.warnings.size() == 1);
    CHECK(!fs::exists(dest / "big.bin"));
    CHECK(read_file(dest / "other.bin") == random_bytes(100'000, 2));
}

TEST(archive_symlinks_cannot_redirect_extraction)
{
    TempDir tmp;
    const auto outside = tmp.path() / "outside";
    fs::create_directories(outside);

    // A symlink "a" pointing outside, then a file that would land inside it.
    const auto archive = craft_archive(tmp.path() / "evil.zar", {
        {make(EntryType::Symlink, "a", outside.string()), {}},
        {make(EntryType::File, "a/x"), text("payload")},
    });
    const auto dest = tmp.path() / "out";
    const auto result = run(archive, into(dest));

    CHECK(!fs::exists(outside / "x"));
    CHECK(read_file(dest / "a/x") == text("payload"));    // "a" became a real directory
    CHECK(!fs::is_symlink(dest / "a"));
    CHECK(result.warnings.size() == 1);    // the symlink couldn't be created
}

TEST(existing_symlinks_in_destination_are_not_followed)
{
    TempDir tmp;
    const auto outside = tmp.path() / "outside";
    write_file(outside / "keep.txt", text("keep"));
    const auto dest = tmp.path() / "out";
    fs::create_directories(dest);
    fs::create_directory_symlink(outside, dest / "a");

    const auto archive = craft_archive(tmp.path() / "a.zar", {
        {make(EntryType::Dir, "a"), {}},
        {make(EntryType::File, "a/x"), text("payload")},
    });

    auto merge = into(dest);
    merge.overwrite = true;
    const auto merged = run(archive, merge);
    CHECK(!fs::exists(outside / "x"));
    CHECK(!merged.warnings.empty());

    auto replace = merge;
    replace.dirs = DirMode::Replace;
    const auto replaced = run(archive, replace);
    CHECK(replaced.warnings.empty());
    CHECK(!fs::is_symlink(dest / "a"));
    CHECK(read_file(dest / "a/x") == text("payload"));
    CHECK(read_file(outside / "keep.txt") == text("keep"));    // the link's target is untouched
    CHECK(!fs::exists(outside / "x"));
}

TEST(unsafe_paths_are_rejected)
{
    TempDir tmp;
    for (const auto* path : {"../escape", "/etc/evil", "a/../../escape", "a//b"}) {
        const auto archive = craft_archive(tmp.path() / "evil.zar",
                                           {{make(EntryType::File, path), text("x")}});
        CHECK_THROWS(ArchiveReader::open(archive));
    }
    CHECK(!fs::exists(tmp.path().parent_path() / "escape"));
}

TEST(sub_indexes_load_and_must_form_a_tree)
{
    TempDir tmp;
    // The first block after the zar header always lands at offset 32.
    static constexpr std::int64_t kFirst = 32;

    const auto write = [&](const std::vector<Entry>& sub, const std::vector<Entry>& root) {
        const auto path = tmp.path() / "sub.zar";
        auto out = Output::create_file(path, true);
        ArchiveWriter writer(out, kAlignLog2Default);
        Bytes sub_index, root_index;
        for (const auto& e : sub)
            encode_entry(sub_index, e);
        for (const auto& e : root)
            encode_entry(root_index, e);
        CHECK(writer.write_block(BlockType::IndexRaw, sub_index) == kFirst);
        writer.finish(BlockType::IndexRaw, root_index);
        out.commit();
        return path;
    };
    const auto dir_with_subindex = [](std::string path) {
        auto e = make(EntryType::Dir, std::move(path));
        e.subindex = kFirst;
        return e;
    };

    // A valid sub-index: its paths are relative to the directory.
    const auto good = ArchiveReader::open(write({make(EntryType::Dir, "inner")}, {dir_with_subindex("d")}));
    CHECK(good.entries().size() == 2);
    CHECK(good.entries().size() == 2 && good.entries()[1].path == "d/inner");

    // A sub-index that refers to itself.
    CHECK_THROWS(ArchiveReader::open(write({dir_with_subindex("loop")}, {dir_with_subindex("d")})));

    // Two directories sharing one sub-index.
    CHECK_THROWS(ArchiveReader::open(write({}, {dir_with_subindex("d1"), dir_with_subindex("d2")})));
}

TEST(head_index_archives_extract_from_a_pipe)
{
    TempDir tmp;
    const auto src = tmp.path() / "src";
    std::vector<std::pair<std::string, Bytes>> files;
    for (unsigned i = 0; i < 25; ++i) {
        files.emplace_back("d" + std::to_string(i % 3) + "/f" + std::to_string(i),
                           random_bytes((i * 4999) % 30'000, i));
        write_file(src / files.back().first, files.back().second);
    }
    fs::create_symlink("d0/f0", src / "link");
    fs::create_hard_link(src / "d1/f1", src / "hard");

    for (const std::uint8_t align : {std::uint8_t{0}, std::uint8_t{3}, std::uint8_t{12}}) {
        const auto archive = tmp.path() / "head.zar";
        {
            auto out = Output::create_file(archive, true);
            CreateOptions options;
            options.head_index = true;
            options.chunk_size = 4096;
            options.align_log2 = align;
            create_archive(out, std::vector{src / "."}, options);
            out.commit();
        }
        const auto bytes = read_file(archive);

        // Random access still works, through the head copy of the index.
        CHECK(ArchiveReader::open(archive).header().index_offset < 0);

        // Feed the archive through a pipe from another thread.
        int fds[2];
        CHECK(::pipe(fds) == 0);
        std::jthread feeder([&, fd = fds[1]] {
            for (std::size_t done = 0; done < bytes.size();) {
                const auto n = ::write(fd, bytes.data() + done, std::min<std::size_t>(65'536, bytes.size() - done));
                if (n <= 0)
                    break;
                done += static_cast<std::size_t>(n);
            }
            ::close(fd);
        });

        auto reader = ArchiveReader::open_stream(fds[0], "pipe");
        CHECK(reader.streaming());
        const auto dest = tmp.path() / ("out" + std::to_string(align));
        auto options = into(dest);
        options.threads = 4;
        Run result;
        options.on_warning = [&](std::string_view m) { result.warnings.emplace_back(m); };
        const auto groups = group_by_directory(reader.entries());
        Extractor extractor(reader, options);
        extractor.prepare(groups);
        for (const auto& group : groups) {
            extractor.enter(group);
            for (const auto* member : group.members)
                extractor.extract(*member);
        }
        extractor.finish();
        feeder.join();
        ::close(fds[0]);

        CHECK(result.warnings.empty());
        for (const auto& [path, data] : files)
            CHECK(read_file(dest / path) == data);
        CHECK(read_file(dest / "hard") == files[1].second);
        CHECK(fs::read_symlink(dest / "link") == "d0/f0");
    }
}

TEST(streaming_needs_a_head_index)
{
    TempDir tmp;
    write_file(tmp.path() / "src/f", text("x"));
    const auto archive = make_archive(tmp.path() / "a.zar", tmp.path() / "src");
    const auto bytes = read_file(archive);

    int fds[2];
    CHECK(::pipe(fds) == 0);
    CHECK(::write(fds[1], bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
    ::close(fds[1]);
    CHECK_THROWS(ArchiveReader::open_stream(fds[0], "pipe"));
    ::close(fds[0]);
}

TEST(reader_finds_an_archive_after_other_data)
{
    TempDir tmp;
    write_file(tmp.path() / "src/f.txt", text("embedded"));
    const auto archive = make_archive(tmp.path() / "a.zar", tmp.path() / "src");

    auto bytes = random_bytes(1001, 9);    // e.g. a self-extractor stub
    const auto original = read_file(archive);
    bytes.insert(bytes.end(), original.begin(), original.end());
    write_file(archive, bytes);

    const auto dest = tmp.path() / "out";
    CHECK(run(archive, into(dest)).warnings.empty());
    CHECK(read_file(dest / "f.txt") == text("embedded"));
}

TEST(listing_groups_by_directory)
{
    std::vector<Entry> entries;
    for (const auto* path : {"z.txt", "a-b/f", "a/b/c/deep", "a/x", "a/b/y", "m"})
        entries.push_back(make(EntryType::File, path));
    entries.push_back(make(EntryType::Dir, "a"));

    const auto groups = group_by_directory(entries);
    std::vector<std::string> order;
    for (const auto& g : groups) {
        order.push_back(g.path + "/");
        for (const auto* m : g.members)
            order.push_back(m->path);
    }
    const std::vector<std::string> expected{
        "/", "m", "z.txt",          // the root
        "a/", "a/x",                 // a directory's subdirectories follow it...
        "a/b/", "a/b/y",
        "a/b/c/", "a/b/c/deep",
        "a-b/", "a-b/f",             // ...before its later siblings
    };
    CHECK(order == expected);
    CHECK(groups[1].entry != nullptr);    // "a" has its own entry
    CHECK(groups[2].entry == nullptr);    // "a/b" is only implied
}
