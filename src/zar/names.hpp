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

} // namespace zar
