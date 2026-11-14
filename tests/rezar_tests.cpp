// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// rezar: planning edits from lists, and rebuilding archives with them.
#include "check.hpp"
#include "util.hpp"

#include "zar/archive_reader.hpp"
#include "zar/creator.hpp"
#include "zar/editor.hpp"
#include "zar/extractor.hpp"
#include "zar/names.hpp"
#include "zar/posix.hpp"

#include <algorithm>
#include <atomic>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

using namespace zar;
using namespace zar::test;

namespace {

Entry entry(EntryType type, std::string path, std::optional<std::string> target = {})
{
    Entry e;
    e.type = type;
    e.path = std::move(path);
    e.link_target = std::move(target);
    return e;
}

// a/        b/x  b/y      c/d/e (c and c/d implied)   h -> b/x (hard link)
std::vector<Entry> sample_entries()
{
    return {
        entry(EntryType::Dir, "a"),
        entry(EntryType::Dir, "b"),
        entry(EntryType::File, "b/x"),
        entry(EntryType::File, "b/y"),
        entry(EntryType::File, "c/d/e"),
        entry(EntryType::Hardlink, "h", "b/x"),
    };
}

std::vector<std::string> kept_paths(const EditPlan& plan)
{
    std::vector<std::string> paths;
    for (const auto& e : plan.keep)
        paths.push_back(e.path);
    return paths;
}

std::string problems(const EditLists& lists)
{
    try {
        plan_edit(sample_entries(), lists);
    } catch (const EditError& e) {
        return e.what();
    }
    return "";
}

fs::path make_archive(const fs::path& archive, const fs::path& source, CreateOptions options = {})
{
    auto out = Output::create_file(archive, true);
    create_archive(out, std::vector{source / "."}, options);
    out.commit();
    return archive;
}

CreateStats rebuild(const fs::path& archive, const fs::path& output, const EditLists& lists,
                    const fs::path& dir, CreateOptions options = {})
{
    const auto source = ArchiveReader::open(archive);
    const auto plan = plan_edit(source.entries(), lists);
    auto out = Output::create_file(output, true);
    const auto stats = rebuild_archive(out, source, plan.keep, plan.additions, dir, options);
    out.commit();
    return stats;
}

// Extracts (or tests) everything; returns the warnings.
std::vector<std::string> extract(const fs::path& archive, const fs::path& dest,
                                 ExtractMode mode = ExtractMode::Extract)
{
    std::vector<std::string> warnings;
    auto reader = ArchiveReader::open(archive);
    ExtractOptions options;
    options.directory = dest;
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

const Entry* find(const ArchiveReader& reader, std::string_view path)
{
    for (const auto& e : reader.entries())
        if (e.path == path)
            return &e;
    return nullptr;
}

// Each chunk's stored payload: the bytes as they sit in the archive.
std::vector<Bytes> payloads(const ArchiveReader& reader, const Entry& e)
{
    std::vector<Bytes> out;
    for (const auto offset : e.chunks)
        out.push_back(offset == kNoOffset ? Bytes{} : reader.payload(reader.block_header(offset)));
    return out;
}

} // namespace

TEST(edit_lists_are_split_on_lines_or_nuls)
{
    CHECK(split_list("a\nb/\n\nc d/**\n", false) == (std::vector<std::string>{"a", "b/", "c d/**"}));
    CHECK(split_list("a\nb", true) == (std::vector<std::string>{"a\nb"}));
    CHECK(split_list(std::string_view("x\0y\0", 4), true) == (std::vector<std::string>{"x", "y"}));
    CHECK(split_list("", false).empty());
}

TEST(edit_plans_delete_replace_and_add)
{
    EditLists lists;
    lists.remove = {"b/y", "a/"};
    lists.replace = {"c/d/e"};
    lists.add = {"b/new", "z/**", "c/d/f/"};
    const auto plan = plan_edit(sample_entries(), lists);
    CHECK(kept_paths(plan) == (std::vector<std::string>{"b", "b/x", "h"}));
    CHECK(plan.deleted == 2);
    CHECK(plan.removed.size() == 3);
    CHECK(plan.additions == (std::vector<Addition>{{Addition::Kind::File, "c/d/e"},
                                                   {Addition::Kind::File, "b/new"},
                                                   {Addition::Kind::Tree, "z"},
                                                   {Addition::Kind::Directory, "c/d/f"}}));
}

TEST(edit_plans_delete_trees_and_empty_directories)
{
    EditLists tree;
    tree.remove = {"b/**", "c/**"};
    CHECK(kept_paths(plan_edit(sample_entries(), tree)) == (std::vector<std::string>{"a", "h"}));

    // b/ alone is fine once everything in it is deleted too, in any order.
    EditLists alone;
    alone.remove = {"b/", "b/y", "b/x"};
    CHECK(plan_edit(sample_entries(), alone).deleted == 3);

    // An implied directory can be deleted with its contents.
    EditLists implied;
    implied.remove = {"c/d/"};
    CHECK(problems(implied).find("c/d/: not empty") != std::string::npos);
    implied.remove = {"c/**"};
    CHECK(problems(implied).empty());
}

TEST(edit_plans_add_after_deleting)
{
    EditLists lists;
    lists.remove = {"b/**"};
    lists.add = {"b/x"};    // gone once b/** is deleted
    CHECK(problems(lists).empty());

    EditLists either;
    either.remove = {"b/y"};
    either.add_replace = {"b/x", "b/q"};    // replaces one, adds the other
    const auto plan = plan_edit(sample_entries(), either);
    CHECK(plan.deleted == 1);
    CHECK(plan.removed.size() == 2);
    CHECK(plan.additions.size() == 2);
}

TEST(edit_plans_report_every_problem)
{
    EditLists lists;
    lists.add = {"b/x", "../up", "/abs", "b//x"};
    lists.replace = {"nope", "a", "b/y/"};
    lists.remove = {"a/x", "b/", "c/d/e", "c/d/e/"};
    const auto text = problems(lists);
    for (const auto* expected : {
             "b/x: already in the archive",
             "../up: not a valid archive path",
             "/abs: not a valid archive path",
             "b//x: not a valid archive path",
             "nope: not in the archive",
             "a: is a directory; list it as a/ (alone) or a/** (with its contents)",
             "b/y/: not a directory",
             "a/x: not in the archive",
             "b/: not empty",
             "c/d/e/: listed more than once",
         })
        if (text.find(expected) == std::string::npos) {
            CHECK(false);
            std::cerr << "  missing problem: " << expected << "\n  in: " << text << '\n';
        }

    EditLists twice;
    twice.remove = {"b/x"};
    twice.add = {"b/x"};    // one path, two lists
    CHECK(problems(twice).find("b/x: listed more than once") != std::string::npos);

    EditLists inside;
    inside.add = {"z/**", "z/q"};
    CHECK(problems(inside).find("z/q: already added with z/**") != std::string::npos);
}

TEST(hard_links_to_removed_files_keep_the_old_content)
{
    for (const bool replace : {false, true}) {
        EditLists lists;
        (replace ? lists.replace : lists.remove) = {"b/x"};
        const auto plan = plan_edit(sample_entries(), lists);
        const auto h = std::ranges::find(plan.keep, std::string("h"), &Entry::path);
        CHECK(h != plan.keep.end() && h->type == EntryType::File && !h->link_target);
    }
    EditLists other;
    other.remove = {"b/y"};
    const auto plan = plan_edit(sample_entries(), other);
    const auto h = std::ranges::find(plan.keep, std::string("h"), &Entry::path);
    CHECK(h != plan.keep.end() && h->type == EntryType::Hardlink);
}

TEST(rebuilding_copies_kept_blocks_as_they_are)
{
    TempDir tmp;
    const auto src = tmp.path() / "src";
    const auto big = random_bytes(300'000, 1);
    const auto text = [](char c) { return Bytes(50'000, static_cast<std::uint8_t>(c)); };
    write_file(src / "keep/big.bin", big);
    write_file(src / "keep/copy.bin", big);    // deduplicated: shares big.bin's blocks
    write_file(src / "keep/text.txt", text('t'));
    write_file(src / "old/gone.bin", random_bytes(400'000, 2));
    write_file(src / "swap.txt", text('a'));
    fs::create_symlink("keep/big.bin", src / "link");
    fs::create_hard_link(src / "swap.txt", src / "swap.hard");
    CreateOptions small_chunks;
    small_chunks.chunk_size = 64 << 10;
    const auto archive = make_archive(tmp.path() / "a.zar", src, small_chunks);

    const auto add = tmp.path() / "add";
    write_file(add / "swap.txt", text('b'));
    write_file(add / "new/deep/file.txt", text('n'));
    EditLists lists;
    lists.remove = {"old/**"};
    lists.replace = {"swap.txt"};
    lists.add = {"new/deep/file.txt"};    // new/ and new/deep/ are added too
    const auto output = tmp.path() / "b.zar";
    const auto stats = rebuild(archive, output, lists, add, small_chunks);
    CHECK(stats.copied_entries == 6);    // keep/ and its 3 files, link, swap.hard
    CHECK(stats.files == 2 && stats.directories == 2);
    CHECK(fs::file_size(output) < fs::file_size(archive) - 300'000);    // old/ reclaimed

    // Kept chunks are byte-for-byte the same blocks, still shared.
    const auto before = ArchiveReader::open(archive);
    const auto after = ArchiveReader::open(output);
    for (const auto* path : {"keep/big.bin", "keep/text.txt", "swap.hard"}) {
        const auto *old_entry = find(before, path), *new_entry = find(after, path);
        CHECK(old_entry && new_entry && payloads(before, *old_entry) == payloads(after, *new_entry));
    }
    CHECK(find(after, "keep/big.bin")->chunks == find(after, "keep/copy.bin")->chunks);
    CHECK(!find(after, "old") && !find(after, "old/gone.bin"));
    CHECK(find(after, "new") && find(after, "new")->type == EntryType::Dir);
    CHECK(find(after, "swap.hard")->type == EntryType::File);    // its target was replaced

    // Entries stay in path order.
    std::vector<std::string> paths;
    for (const auto& e : after.entries())
        paths.push_back(e.path);
    CHECK(std::ranges::is_sorted(paths, PathLess{}));

    CHECK(extract(output, tmp.path(), ExtractMode::Test).empty());
    const auto out = tmp.path() / "out";
    CHECK(extract(output, out).empty());
    CHECK(read_file(out / "keep/big.bin") == big);
    CHECK(read_file(out / "keep/copy.bin") == big);
    CHECK(read_file(out / "swap.txt") == text('b'));
    CHECK(read_file(out / "swap.hard") == text('a'));    // the old content
    CHECK(read_file(out / "new/deep/file.txt") == text('n'));
    CHECK(fs::read_symlink(out / "link") == "keep/big.bin");
    CHECK(!fs::exists(out / "old"));
}

TEST(rebuilding_keeps_holes_dictionaries_and_the_head_index)
{
    TempDir tmp;
    const auto src = tmp.path() / "src";
    for (int i = 0; i < 60; ++i)
        write_file(src / ("f" + std::to_string(i) + ".txt"),
                   text("struct record_" + std::to_string(i) + " { int id; char name[32]; double value; };\n"));
    {
        const UniqueFd fd(::open((src / "sparse").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
        CHECK(::ftruncate(fd.get(), 8 << 20) == 0);
    }
    CreateOptions options;
    options.dict_size = 4096;
    options.head_index = true;
    const auto archive = make_archive(tmp.path() / "a.zar", src, options);
    const auto before = ArchiveReader::open(archive);
    const auto dict_before = find(before, "f1.txt")->dict;
    CHECK(dict_before.has_value());

    write_file(tmp.path() / "add/g.txt", text("struct record_new { int id; char name[32]; double value; };\n"));
    EditLists lists;
    lists.remove = {"f0.txt"};
    lists.add = {"g.txt"};
    CreateOptions rebuilt;
    rebuilt.head_index = true;
    const auto output = tmp.path() / "b.zar";
    rebuild(archive, output, lists, tmp.path() / "add", rebuilt);

    const auto after = ArchiveReader::open(output);
    CHECK(after.header().index_offset < 0);    // a head index again
    const auto *kept = find(after, "f1.txt"), *added = find(after, "g.txt");
    CHECK(kept && added && kept->dict && kept->dict == added->dict);    // one dictionary, for both
    CHECK(find(before, "sparse")->chunks == find(after, "sparse")->chunks);    // all holes
    CHECK(extract(output, tmp.path() / "out").empty());
    CHECK(read_file(tmp.path() / "out/g.txt") == read_file(tmp.path() / "add/g.txt"));
    CHECK(fs::file_size(tmp.path() / "out/sparse") == 8 << 20);
}

TEST(deep_dedup_reuses_blocks_already_in_the_archive)
{
    TempDir tmp;
    const auto data = random_bytes(200'000, 3);
    write_file(tmp.path() / "src/old.bin", data);
    const auto archive = make_archive(tmp.path() / "a.zar", tmp.path() / "src");
    write_file(tmp.path() / "add/same.bin", data);
    EditLists lists;
    lists.add = {"same.bin"};

    for (const bool deep : {false, true}) {
        CreateOptions options;
        options.dedup_copied = deep;
        const auto output = tmp.path() / "b.zar";
        const auto stats = rebuild(archive, output, lists, tmp.path() / "add", options);
        const auto after = ArchiveReader::open(output);
        const bool shared = find(after, "same.bin")->chunks == find(after, "old.bin")->chunks;
        CHECK(shared == deep);
        CHECK((stats.deduplicated_bytes == data.size()) == deep);
        CHECK(extract(output, tmp.path(), ExtractMode::Test).empty());
    }
}

TEST(rebuilding_refuses_solid_archives)
{
    TempDir tmp;
    write_file(tmp.path() / "src/a.txt", text("small"));
    CreateOptions solid;
    solid.solid_limit = 1 << 20;
    solid.dict_over_solid = 0;
    const auto archive = make_archive(tmp.path() / "a.zar", tmp.path() / "src", solid);
    const auto source = ArchiveReader::open(archive);
    CHECK(source.solid());
    auto out = Output::create_file(tmp.path() / "b.zar", true);
    CHECK_THROWS_WITH(rebuild_archive(out, source, source.entries(), {}, tmp.path(), {}), "solid");
}

// --progress divides what the library reports by what measure_inputs() (or
// the index) says there is: each pass must add up to exactly that.
TEST(progress_adds_up_to_the_measured_total)
{
    TempDir tmp;
    const auto src = tmp.path() / "src";
    write_file(src / "big", random_bytes(300'000, 1));
    write_file(src / "small", text("small"));
    fs::create_directories(src / "empty");
    fs::create_hard_link(src / "big", src / "link");    // counted once
    {
        const UniqueFd fd(::open((src / "sparse").c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644));
        CHECK(::ftruncate(fd.get(), 3 << 20) == 0);    // a hole, which counts too
        CHECK(::pwrite(fd.get(), "x", 1, 1 << 20) == 1);
    }
    const std::uint64_t total = 300'000 + 5 + (3 << 20);
    CHECK(measure_inputs(std::vector{src / "."}, false) == total);
    CHECK(measure_inputs(std::vector{src / "small", src / "missing"}, false) == 5);

    // Every pass, trial ones included, reads it all. (Whether a dictionary
    // can be trained on so little depends on the zstd version, so these
    // passes don't depend on it.)
    enum Kind { Plain, Solid, Dict };
    for (const auto kind : {Plain, Solid, Dict}) {
        CreateOptions options;
        options.chunk_size = 64 << 10;
        if (kind == Solid) {
            options.solid_limit = 1 << 30;    // kept: it fits
            options.dict_over_solid = 0;
        }
        if (kind == Dict)
            options.dict_size = 4 << 10;    // a sampling pass, then the archive
        std::vector<std::pair<std::string, std::uint64_t>> passes;
        std::pair<std::uint64_t, std::uint64_t> written{};    // the last report
        options.on_stage = [&](std::string_view stage) { passes.emplace_back(stage, 0); written = {}; };
        options.on_progress = [&](std::uint64_t bytes) { passes.back().second += bytes; };
        options.on_written = [&](std::uint64_t data, std::uint64_t stored) {
            CHECK(data >= written.first && stored >= written.second);    // cumulative
            written = {data, stored};
        };
        make_archive(tmp.path() / "a.zar", src, options);
        const std::vector<std::string> expected = kind == Plain   ? std::vector<std::string>{""}
                                                  : kind == Solid ? std::vector<std::string>{"solid"}
                                                                  : std::vector<std::string>{"sampling", ""};
        CHECK(passes.size() == expected.size());
        for (std::size_t i = 0; i < passes.size() && i < expected.size(); ++i)
            CHECK(passes[i].first == expected[i] && passes[i].second == total);
        if (kind == Solid) {
            CHECK(written.first == 0);    // no ratio: it's compressed again as a whole
        } else {
            // Every byte read is written; the hole and the duplicate take nothing.
            CHECK(written.first == total);
            CHECK(written.second > 250'000 && written.second < 400'000);
        }
    }
    make_archive(tmp.path() / "a.zar", src, {});

    // Extracting: the files' sizes, with the hard link as a copy.
    std::atomic<std::uint64_t> extracted = 0;
    {
        auto reader = ArchiveReader::open(tmp.path() / "a.zar");
        ExtractOptions options;
        options.mode = ExtractMode::Test;
        options.on_progress = [&](std::uint64_t bytes) { extracted += bytes; };
        const auto groups = group_by_directory(reader.entries());
        Extractor extractor(reader, options);
        extractor.prepare(groups);
        for (const auto& group : groups) {
            extractor.enter(group);
            for (const auto* member : group.members)
                extractor.extract(*member);
        }
        extractor.finish();
    }
    CHECK(extracted == total + 300'000);

    // Rebuilding: the files kept (not their links), then those added.
    write_file(tmp.path() / "add/new", text("new file"));
    CreateOptions options;
    std::uint64_t rebuilt = 0;
    options.on_progress = [&](std::uint64_t bytes) { rebuilt += bytes; };
    std::uint64_t rebuilt_data = 0;
    options.on_written = [&](std::uint64_t data, std::uint64_t) { rebuilt_data = data; };
    EditLists lists;
    lists.add = {"new"};
    lists.remove = {"small"};
    rebuild(tmp.path() / "a.zar", tmp.path() / "b.zar", lists, tmp.path() / "add", options);
    CHECK(rebuilt == 300'000 + (3 << 20) + 8);
    CHECK(rebuilt_data == rebuilt);
}
