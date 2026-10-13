// Extracting, testing, and scanning archive entries.
#pragma once

#include "zar/archive_reader.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
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
    Replace,    // delete each existing top-level directory first
};

struct ExtractOptions {
    ExtractMode mode = ExtractMode::Extract;
    std::filesystem::path directory = ".";
    bool overwrite = false;    // required if anything to be extracted already exists
    DirMode dirs = DirMode::Merge;
    unsigned threads = 0;      // 0: one per CPU core
    std::function<void(std::string_view)> on_warning;    // may be called from any thread
};

struct ExtractStats {
    std::uint64_t files = 0;
    std::uint64_t directories = 0;
    std::uint64_t symlinks = 0;
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

// The last component of an entry's path.
std::string_view base_name(std::string_view path);

class Extractor {
public:
    // A streaming reader is read forward as finish() extracts the data.
    Extractor(ArchiveReader& reader, const ExtractOptions& options);
    ~Extractor();

    // Checks the destination before anything is written. Throws if any path
    // to be extracted already exists and options.overwrite isn't set; with
    // DirMode::Replace, deletes existing top-level directories.
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
