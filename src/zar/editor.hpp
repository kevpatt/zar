// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// What rezar changes in an archive: from its lists of paths to add, replace,
// and delete, the entries to keep and the paths to add (rebuild_archive()).
#pragma once

#include "zar/creator.hpp"
#include "zar/index.hpp"

#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zar {

// Archive paths, one list per operation. Each is written:
//
//   a/b      a file or symlink
//   a/d/     a directory alone: deleted only if it's empty
//   a/d/**   a directory and everything below it
struct EditLists {
    std::vector<std::string> add;            // must not be in the archive yet
    std::vector<std::string> replace;        // must be
    std::vector<std::string> add_replace;    // either
    std::vector<std::string> remove;         // must be
};

struct EditPlan {
    std::vector<Entry> keep;    // hard links to removed files become plain files
    std::vector<Addition> additions;
    std::vector<std::string> removed;     // entries removed, deleted or to be replaced
    std::size_t deleted = 0;              // of them, not replaced
};

// The lists are wrong for this archive; what() describes every problem
// found, one per line.
struct EditError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Works out the changes. All deletions happen before all additions, so the
// order of lines doesn't matter, but a path may appear only once in all.
EditPlan plan_edit(std::span<const Entry> entries, const EditLists& lists);

// Splits a list file into its paths: one per line, or with `null_separated`,
// per NUL byte. Empty items are skipped; nothing else is trimmed.
std::vector<std::string> split_list(std::string_view text, bool null_separated);

} // namespace zar
