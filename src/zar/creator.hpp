// Builds an archive from files on disk.
#pragma once

#include "zar/format.hpp"
#include "zar/output.hpp"

#include <filesystem>
#include <functional>
#include <optional>
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
    bool head_index = false;    // also put the index first, for streaming (§5.4)

    // Try a solid archive (§5.5) first, and keep it if it compresses to at
    // most this many bytes. Otherwise, write a normal archive.
    std::optional<std::uint64_t> solid_limit;

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
    bool solid = false;    // the solid form was kept
};

// How a path given to zar is archived, following POSIX path resolution:
//
//   "proj"     a file, directory, or symlink (not followed), stored under its
//              own name, so extracting recreates proj
//   "proj/"    the same, but proj must be a directory (a symlink to one is
//              followed, and stored as a directory named proj)
//   "proj/."   the directory's contents, stored at the top level
//   "."        the current directory's contents
struct InputSpec {
    std::filesystem::path path;    // as given
    std::string name;              // the stored name; empty for contents
    bool contents = false;
    bool must_be_directory = false;
};

// Throws std::runtime_error for paths that can't be archived by name, like "/".
InputSpec parse_input(const std::filesystem::path& input);

// The archive name zar uses when -o isn't given: "proj" or "proj/." become
// "proj.zar" beside proj; "." becomes "<current directory's name>.zar" in
// the current directory.
std::filesystem::path default_archive_path(const InputSpec& input);

// Writes a complete archive of `inputs` to `out` (without committing it).
//
// Problems with individual files below an input are reported through
// options.on_warning and skipped. Anything else throws: a missing input, an
// input with a trailing slash that isn't a directory, or two inputs that
// would store the same path.
CreateStats create_archive(Output& out, std::span<const std::filesystem::path> inputs,
                           const CreateOptions& options);

} // namespace zar
