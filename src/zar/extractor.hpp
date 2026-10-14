// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// Extracting, testing, and scanning archive entries.
#pragma once

#include "zar/archive_reader.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace zar {

enum class ExtractMode {
    Extract,    // write files to disk
    Test,       // decompress and verify everything, write nothing
    Scan,       // verify structure and checksums without decompressing or writing
};

// What to do with directories that already exist at the destination.
enum class DirMode {
    Merge,      // keep them and extract into them
    Replace,    // delete each existing directory being extracted whole first
};

struct ExtractOptions {
    ExtractMode mode = ExtractMode::Extract;
    std::filesystem::path directory = ".";
    bool overwrite = false;    // required if anything to be extracted already exists
    DirMode dirs = DirMode::Merge;
    unsigned threads = 0;      // 0: one per CPU core
    // Recreate hard links with link(2) where the target is extracted too and
    // the filesystem allows it, rather than extracting independent copies.
    bool hard_links = false;
    // The most file data to decompress (Extract and Test), checked up front.
    // Without it, extraction is limited to the destination's free space.
    std::optional<std::uint64_t> size_limit;
    std::function<void(std::string_view)> on_warning;    // may be called from any thread
};

struct ExtractStats {
    std::uint64_t files = 0;
    std::uint64_t directories = 0;
    std::uint64_t symlinks = 0;
    std::uint64_t hardlinks = 0;    // created with link(2); copies count as files
    std::uint64_t bytes = 0;
    std::uint64_t warnings = 0;
};

// A directory and the non-directory entries directly inside it. The root
// group has an empty path. `entry` is null for directories that have no
// entry of their own (the root, or a path like "a" implied by "a/b").
struct DirectoryGroup {
    std::string path;
    const Entry* entry = nullptr;
    std::vector<const Entry*> members;
};

// Groups entries by directory: parents before children, everything sorted.
std::vector<DirectoryGroup> group_by_directory(std::span<const Entry> entries);

// The permission bits to give a file whose archived mode is `mode`.
// Set-user-ID and set-group-ID bits are kept only if its owner was restored
// from the archive (which needs root): otherwise they'd apply to whoever
// is extracting.
mode_t permitted_mode(std::uint16_t mode, bool owner_restored);

// The last component of an entry's path.
std::string_view base_name(std::string_view path);

class Extractor {
public:
    // A streaming reader is read forward as finish() extracts the data.
    Extractor(ArchiveReader& reader, const ExtractOptions& options);
    ~Extractor();

    // Checks before anything is written. Throws if the files' total size
    // exceeds options.size_limit (or the free space), or if any path to be
    // extracted already exists and options.overwrite isn't set. With
    // DirMode::Replace, deletes existing directories that are extracted whole.
    void prepare(std::span<const DirectoryGroup> groups);

    // Then call for each group in order, and for each of its members. File
    // data is extracted in the background, chunks in parallel.
    void enter(const DirectoryGroup& group);
    void extract(const Entry& entry);

    // Waits for all file data, then creates symlinks and applies directory
    // metadata, which wait until everything else is in place.
    ExtractStats finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zar
