// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// Builds an archive from files on disk.
#pragma once

#include "zar/format.hpp"
#include "zar/index.hpp"
#include "zar/output.hpp"
#include "zar/signing.hpp"

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
    // Store each distinct chunk once: a chunk identical to one already
    // written reuses that block. Found by hash, confirmed by reading the
    // block back, so it needs an output that can be read back (a file, or
    // head_index). Unset: on whenever it can be; true: required.
    std::optional<bool> dedup;
    // rezar: also compare new chunks with the blocks copied from the source
    // archive, decompressing those of the same length to hash them.
    bool dedup_copied = false;
    bool head_index = false;    // also put the index first, for streaming (§5.4)

    // Store each block's hash (§6.7), so the archive can be verified, and
    // signed, now or later.
    bool hash = false;
    // Sign the archive with these keys (§5.6), which implies hash. Not with
    // solid_limit: a solid archive can't be signed.
    std::vector<SecretKey> sign;
    std::optional<std::uint64_t> sequence;    // what the signature says
    std::optional<std::int64_t> sign_time;
    std::optional<std::string> sign_name;

    // Store each file of at most this many bytes as a single block, whatever
    // the chunk size.
    std::optional<std::uint64_t> contiguous_limit;

    // Train a zstd dictionary of at most this many bytes on the files, and
    // compress them with it.
    std::optional<std::uint64_t> dict_size;

    // Try a solid archive (§5.5) first, and keep it if it compresses to at
    // most this many bytes. Otherwise, write a normal archive.
    std::optional<std::uint64_t> solid_limit;

    // When the solid archive fits, also build a normal archive compressed
    // with a trained dictionary, and keep that instead if it is at most this
    // percentage of the solid archive's size (and also fits). 0: don't try.
    unsigned dict_over_solid = 120;

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
    std::uint64_t deduplicated_bytes = 0;    // uncompressed data not stored again
    std::uint64_t hole_bytes = 0;            // holes in sparse files, stored as such
    std::uint64_t copied_entries = 0;        // rezar: entries kept from the source archive
    std::uint64_t copied_bytes = 0;          // and the payload bytes of their blocks
    std::size_t signed_by = 0;               // keys that signed it
    std::uint64_t warnings = 0;
    bool solid = false;         // the solid form was kept
    bool dictionary = false;    // files were compressed with a trained dictionary
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

// The archive name zar uses when -o isn't given, in the current directory:
// "a/b/proj", "a/b/proj/" and "a/b/proj/." all become "proj.zar", and "."
// becomes "<current directory's name>.zar".
std::filesystem::path default_archive_path(const InputSpec& input);

// Writes a complete archive of `inputs` to `out` (without committing it).
//
// Problems with individual files below an input are reported through
// options.on_warning and skipped. Anything else throws: a missing input, an
// input with a trailing slash that isn't a directory, or two inputs that
// would store the same path.
CreateStats create_archive(Output& out, std::span<const std::filesystem::path> inputs,
                           const CreateOptions& options);

class ArchiveReader;

// A path for rezar to add, read from below a source directory, where it has
// the same relative path as in the archive.
struct Addition {
    enum class Kind {
        File,         // a file or symlink (not followed)
        Directory,    // a directory's own entry, without its contents
        Tree,         // a directory and everything below it
    };
    Kind kind = Kind::File;
    std::string path;    // the archive path

    bool operator==(const Addition&) const = default;
};

// Writes a new archive (without committing it) holding `keep`, entries of
// `source` whose data is copied block for block, never decompressed or
// recompressed, and `additions`, read from `source_dir`. Missing parent
// directories of additions are added too. New files are compressed with the
// source's dictionary, if the kept entries use one. Hashes the kept entries
// have are checked as their blocks are copied, and kept; if any have them, or
// with options.hash or sign, every block in the new archive is hashed.
//
// Throws for a solid source, whose data can't be copied without
// recompressing it, and, like create_archive(), for problems with an
// addition itself rather than with something below it.
CreateStats rebuild_archive(Output& out, const ArchiveReader& source, std::span<const Entry> keep,
                            std::span<const Addition> additions,
                            const std::filesystem::path& source_dir, const CreateOptions& options);

// For tests: called just before each file is opened to be read, so a test
// can change it at that moment.
inline void (*before_open_for_test)(const std::filesystem::path&) = nullptr;

} // namespace zar
