// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// File-name encoding and archive path rules (FORMAT.md §7).
#pragma once

#include <string>
#include <string_view>

namespace zar {

// Well-formed UTF-8: no overlong forms, surrogates, or code points past U+10FFFF.
bool is_valid_utf8(std::string_view text) noexcept;

// Every byte is a Latin-1 code point, so this always succeeds and is reversible.
std::string latin1_to_utf8(std::string_view text);

// Relative, '/'-separated, no empty/"."/".." components, no NUL (§7).
bool is_valid_archive_path(std::string_view path) noexcept;

// Orders paths component by component, so a directory's contents sort
// directly after it ("a", "a/b", "a-c" rather than "a", "a-c", "a/b").
bool path_less(std::string_view a, std::string_view b) noexcept;

// path_less() for ordered containers, which can then be searched with any
// string-like key.
struct PathLess {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return path_less(a, b); }
};

// The path without its last component: "" for "a", "a" for "a/b".
std::string_view parent_path(std::string_view path) noexcept;

} // namespace zar
