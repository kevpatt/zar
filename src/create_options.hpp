// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// The options that say how new files are stored, shared by zar and rezar:
// their getopt table, parsing, and help.
#pragma once

#include "cli.hpp"
#include "zar/creator.hpp"

#include <bit>
#include <cstdint>
#include <getopt.h>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>
#include <zstd.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace zar::cli {

inline std::uint64_t cache_line_size()
{
#ifdef __APPLE__
    std::int64_t size = 0;
    std::size_t length = sizeof size;
    if (::sysctlbyname("hw.cachelinesize", &size, &length, nullptr, 0) == 0 && size > 0)
        return static_cast<std::uint64_t>(size);
#elif defined(_SC_LEVEL1_DCACHE_LINESIZE)
    if (const long size = ::sysconf(_SC_LEVEL1_DCACHE_LINESIZE); size > 0)
        return static_cast<std::uint64_t>(size);
#endif
    return 64;
}

// --align[=N]: the cache-line size without N.
inline std::uint8_t parse_alignment(const char* arg)
{
    const auto bytes = arg ? parse_size(arg, "alignment") : cache_line_size();
    if (!std::has_single_bit(bytes) || bytes > (std::uint64_t{1} << kAlignLog2Max))
        throw UsageError("alignment must be a power of two up to 1M");
    return static_cast<std::uint8_t>(std::countr_zero(bytes));
}

inline int parse_level(std::string_view text)
{
    const bool negative = text.starts_with('-');
    const auto magnitude = parse_number(negative ? text.substr(1) : text, "level");
    const auto level = negative ? -static_cast<long long>(magnitude) : static_cast<long long>(magnitude);
    if (level < ZSTD_minCLevel() || level > ZSTD_maxCLevel())
        throw UsageError("level must be between " + std::to_string(ZSTD_minCLevel()) + " and " +
                         std::to_string(ZSTD_maxCLevel()));
    return static_cast<int>(level);
}

inline std::uint64_t parse_chunk_size(std::string_view text)
{
    const auto size = parse_size(text, "chunk size");
    if (!std::has_single_bit(size) || size < (1 << 12) || size > (1 << 30))
        throw UsageError("chunk size must be a power of two from 4K to 1G");
    return size;
}

// --owner[=WHICH]: both without WHICH.
inline OwnerMode parse_owner(const char* arg)
{
    const std::string_view text = arg ? arg : "both";
    if (text == "both")
        return OwnerMode::Both;
    if (text == "ids")
        return OwnerMode::Ids;
    if (text == "names")
        return OwnerMode::Names;
    throw UsageError("--owner must be ids, names, or both");
}

inline NameMode parse_names(std::string_view text)
{
    if (text == "raw")
        return NameMode::Raw;
    if (text == "skip")
        return NameMode::Skip;
    if (text == "latin1")
        return NameMode::Latin1;
    throw UsageError("--names must be raw, skip, or latin1");
}

// unzar reads blocks of up to 4G.
inline constexpr std::uint64_t kMaxContiguous = std::uint64_t{1} << 32;

// --contiguous[=SIZE]: files up to 4G without SIZE.
inline std::uint64_t parse_contiguous(const char* arg)
{
    const auto limit = arg ? parse_size(arg, "contiguous size") : kMaxContiguous;
    if (limit > kMaxContiguous)
        throw UsageError("--contiguous size can be at most 4G");
    return limit;
}

// Long-only options, numbered past every short option and past each tool's own.
enum CreateOption {
    kChunkSize = 512,
    kAlign,
    kOwner,
    kNsec,
    kAllTimes,
    kNames,
    kCompressAll,
    kContiguous,
};

// The tool's own options, then these: a getopt_long table. Short options:
// "l:j::".
inline std::vector<option> option_table(std::initializer_list<option> own)
{
    std::vector<option> table(own);
    table.insert(table.end(), {
        {"level", required_argument, nullptr, 'l'},
        {"threads", optional_argument, nullptr, 'j'},
        {"chunk-size", required_argument, nullptr, kChunkSize},
        {"align", optional_argument, nullptr, kAlign},
        {"owner", optional_argument, nullptr, kOwner},
        {"nsec", no_argument, nullptr, kNsec},
        {"all-times", no_argument, nullptr, kAllTimes},
        {"names", required_argument, nullptr, kNames},
        {"compress-all", no_argument, nullptr, kCompressAll},
        {"contiguous", optional_argument, nullptr, kContiguous},
        {nullptr, 0, nullptr, 0},
    });
    return table;
}

// Applies option `code` to `options`, if it's one of these; --align is left
// to the tool, whose default differs.
inline bool parse_create_option(int code, const char* arg, CreateOptions& options)
{
    const std::string_view text = arg ? arg : "";
    switch (code) {
    case 'l': options.level = parse_level(text); return true;
    case 'j': options.threads = parse_threads(arg); return true;
    case kChunkSize: options.chunk_size = parse_chunk_size(text); return true;
    case kOwner: options.owner = parse_owner(arg); return true;
    case kNsec: options.nanoseconds = true; return true;
    case kAllTimes: options.all_times = true; return true;
    case kNames: options.names = parse_names(text); return true;
    case kCompressAll: options.compress_all = true; return true;
    case kContiguous: options.contiguous_limit = parse_contiguous(arg); return true;
    default: return false;
    }
}

// Their --help lines.
inline constexpr std::string_view kCreateOptionsHelp =
    R"(  -l, --level=N          zstd compression level (default 3)
  -j, --threads[=N]      worker threads (default: one per CPU core)
      --chunk-size=SIZE  split files into independently compressed chunks
                         of SIZE bytes: a power of two from 4K to 1G, with
                         an optional K, M, or G suffix (default 1M). Holes
                         in sparse files are kept a chunk at a time.
      --owner[=WHICH]    store file ownership: ids, names, or both (default)
      --nsec             store timestamps to the nanosecond
      --all-times        store access, change, and birth times too
      --names=MODE       non-UTF-8 file names: raw (default), skip, latin1
      --compress-all     compress even files that are usually precompressed
      --contiguous[=SIZE]
                         store each file of at most SIZE bytes as one block
                         instead of in chunks (with no SIZE, files up to 4G)
)";

} // namespace zar::cli
