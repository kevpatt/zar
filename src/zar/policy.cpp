// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "zar/policy.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace zar {
namespace {

// Sorted, lowercase, without the dot.
constexpr std::array<std::string_view, 70> kPrecompressed{
    "3g2",  "3gp",  "7z",   "aab",  "aac",  "apk",  "avi",  "avif", "br",   "bz2",
    "cab",  "deb",  "dmg",  "docx", "epub", "flac", "flv",  "gif",  "gz",   "heic",
    "heif", "ipa",  "jar",  "jp2",  "jpeg", "jpg",  "jxl",  "lz",   "lz4",  "lzma",
    "m2ts", "m4a",  "m4b",  "m4v",  "mkv",  "mov",  "mp3",  "mp4",  "mpeg", "mpg",
    "nupkg", "odp", "ods",  "odt",  "oga",  "ogg",  "ogv",  "opus", "png",  "pptx",
    "rar",  "rpm",  "tbz2", "tgz",  "txz",  "tzst", "war",  "webm", "webp", "whl",
    "wma",  "wmv",  "woff", "woff2", "xlsx", "xpi", "xz",   "zar",  "zip",  "zst",
};

static_assert(std::ranges::is_sorted(kPrecompressed));

} // namespace

bool is_precompressed(std::string_view filename) noexcept
{
    const auto slash = filename.rfind('/');
    if (slash != std::string_view::npos)
        filename.remove_prefix(slash + 1);

    const auto dot = filename.rfind('.');
    if (dot == std::string_view::npos || dot == 0)    // no extension, or a dotfile
        return false;
    const auto ext = filename.substr(dot + 1);

    char lower[8];
    if (ext.empty() || ext.size() > sizeof lower)
        return false;
    std::ranges::transform(ext, lower, [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return std::ranges::binary_search(kPrecompressed, std::string_view(lower, ext.size()));
}

} // namespace zar
