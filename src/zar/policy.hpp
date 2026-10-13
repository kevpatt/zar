// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// Writer-side decisions that aren't part of the format itself.
#pragma once

#include <string_view>

namespace zar {

// True if `filename` has an extension (case-insensitive) whose content is
// normally already compressed — JPEG, MP4, ZIP, and so on — so the writer
// should store it raw instead of spending time on zstd.
bool is_precompressed(std::string_view filename) noexcept;

} // namespace zar
