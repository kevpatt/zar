// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// Command-line helpers shared by zar and unzar.
#pragma once

#include "zar/format.hpp"
#include "zar/version.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <zstd.h>

namespace zar::cli {

// A mistake on the command line; main() adds a pointer to --help.
struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline std::uint64_t parse_number(std::string_view text, std::string_view what)
{
    std::uint64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size())
        throw UsageError("invalid " + std::string(what) + ": " + std::string(text));
    return value;
}

// "1M", "64K", "512", ... with binary multipliers.
inline std::uint64_t parse_size(std::string_view text, std::string_view what)
{
    int shift = 0;
    if (!text.empty()) {
        switch (text.back()) {
        case 'k': case 'K': shift = 10; break;
        case 'm': case 'M': shift = 20; break;
        case 'g': case 'G': shift = 30; break;
        case 't': case 'T': shift = 40; break;
        default: break;
        }
        if (shift)
            text.remove_suffix(1);
    }
    const auto value = parse_number(text, what);
    if (value > (~std::uint64_t{0} >> shift))
        throw UsageError(std::string(what) + " is too large");
    return value << shift;
}

// Sizes for people: plain bytes below 1 KiB, then one decimal place and a
// binary suffix: "512", "1.0K", "31.4M", "2.0G".
inline std::string human_size(std::uint64_t bytes)
{
    if (bytes < 1024)
        return std::to_string(bytes);

    constexpr char kSuffixes[] = "KMGTPE";
    double value = static_cast<double>(bytes) / 1024;
    std::size_t unit = 0;
    // Compare after rounding, so 1023.97K shows as 1.0M rather than 1024.0K.
    while (std::round(value * 10) / 10 >= 1024 && unit + 2 < sizeof kSuffixes) {
        value /= 1024;
        ++unit;
    }
    char text[32];
    std::snprintf(text, sizeof text, "%.1f%c", value, kSuffixes[unit]);
    return text;
}

// -j/--threads[=N]: 0 (one per core) without N.
inline unsigned parse_threads(const char* arg)
{
    if (!arg)
        return 0;
    const auto threads = parse_number(arg, "thread count");
    if (threads == 0 || threads > 1024)
        throw UsageError("thread count must be between 1 and 1024");
    return static_cast<unsigned>(threads);
}

// The top of --help: name, version, build, copyright, and what the tool does.
inline void print_banner(std::ostream& out, std::string_view program, std::string_view description)
{
    out << program << ' ' << version::kVersion << " (git " << version::kGitRevision << ", built "
        << version::kBuildTime << ")\n"
        << version::kCopyright << "\n\n"
        << description << "\n\n";
}

inline void print_version(std::ostream& out, std::string_view program)
{
    out << program << ' ' << version::kVersion << '\n'
        << "archive format " << kFormatVersion.to_string() << ", zstd " << ZSTD_versionString() << '\n'
        << "git " << version::kGitRevision << ", built " << version::kBuildTime << '\n'
        << version::kCopyright << '\n';
}

} // namespace zar::cli
