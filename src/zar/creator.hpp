// Builds an archive from files on disk.
#pragma once

#include "zar/format.hpp"
#include "zar/output.hpp"

#include <filesystem>
#include <functional>
#include <span>
#include <string_view>

namespace zar {

// What to do with file names that aren't valid UTF-8.
enum class NameMode { Raw, Skip, Latin1 };

enum class OwnerMode { None, Ids, Names, Both };

struct CreateOptions {
    int level = 3;
    unsigned threads = 0;    // 0: one per CPU
    std::uint64_t chunk_size = 1 << 20;
    std::uint8_t align_log2 = kAlignLog2Default;
    OwnerMode owner = OwnerMode::None;
    bool nanoseconds = false;
    bool all_times = false;    // atime, ctime, and btime as well as mtime
    NameMode names = NameMode::Raw;
    bool compress_all = false;

    std::function<void(std::string_view)> on_warning;
    std::function<void(std::string_view)> on_entry;    // each archive path as it's added
};

struct CreateStats {
    std::uint64_t files = 0;
    std::uint64_t directories = 0;
    std::uint64_t symlinks = 0;
    std::uint64_t hardlinks = 0;
    std::uint64_t input_bytes = 0;
    std::uint64_t archive_bytes = 0;
    std::uint64_t warnings = 0;
};

// Writes a complete archive of `inputs` to `out` (without committing it).
//
// A single directory input is stored by its contents, so its files appear
// at the top level of the archive. Otherwise each input is stored under its
// own name. Problems with individual files are reported through
// options.on_warning and skipped; anything else, including every input
// being unreadable, throws.
CreateStats create_archive(Output& out, std::span<const std::filesystem::path> inputs,
                           const CreateOptions& options);

} // namespace zar
