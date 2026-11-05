// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "zar/editor.hpp"

#include "zar/names.hpp"

#include <map>
#include <optional>
#include <set>

namespace zar {
namespace {

struct Pattern {
    std::string text;    // as listed
    std::string path;    // without the trailing "/" or "/**"
    Addition::Kind kind;
};

using Kind = Addition::Kind;

class Planner {
public:
    explicit Planner(std::span<const Entry> entries) : entries_(entries)
    {
        for (const auto& entry : entries) {
            by_path_.emplace(entry.path, &entry);
            if (entry.type == EntryType::Dir)
                dirs_.insert(entry.path);
            for (auto dir = parent_path(entry.path); !dir.empty() && dirs_.emplace(dir).second;)
                dir = parent_path(dir);    // implied, though it may have no entry
        }
    }

    EditPlan plan(const EditLists& lists)
    {
        for (const auto& text : lists.remove)
            if (const auto p = take(text); p && exists(*p, false)) {
                remove(*p);
                if (p->kind == Kind::Directory)
                    alone_.push_back(*p);
            }
        const auto deleted = removed_.size();

        // Deleted entries are gone from here on.
        for (const auto& text : lists.replace)
            if (const auto p = take(text); p && exists(*p, true)) {
                remove(*p);
                plan_.additions.push_back({p->kind, p->path});
            }
        for (const auto& text : lists.add_replace)
            if (const auto p = take(text)) {
                if (still_there(p->path)) {
                    if (!exists(*p, true))
                        continue;
                    remove(*p);
                }
                plan_.additions.push_back({p->kind, p->path});
            }
        for (const auto& text : lists.add)
            if (const auto p = take(text)) {
                if (still_there(p->path))
                    error(p->text, "already in the archive (to replace it, list it with replace or add-replace)");
                else
                    plan_.additions.push_back({p->kind, p->path});
            }

        check_alone();
        check_inside_trees();
        if (!errors_.empty()) {
            std::string message;
            for (const auto& e : errors_)
                message += (message.empty() ? "" : "\n") + e;
            throw EditError(message);
        }

        for (const auto& entry : entries_) {
            if (removed_.contains(entry.path))
                continue;
            auto& kept = plan_.keep.emplace_back(entry);
            // It carries its target's data (§6.5), so it keeps the old content.
            if (kept.type == EntryType::Hardlink && kept.link_target && removed_.contains(*kept.link_target)) {
                kept.type = EntryType::File;
                kept.link_target.reset();
            }
        }
        plan_.removed.assign(removed_.begin(), removed_.end());
        plan_.deleted = deleted;
        return std::move(plan_);
    }

private:
    void error(std::string_view text, std::string_view problem)
    {
        errors_.push_back(std::string(text) + ": " + std::string(problem));
    }

    // Parses a listed path, which may appear only once in all the lists.
    std::optional<Pattern> take(const std::string& text)
    {
        Pattern p{text, text, Kind::File};
        if (text.ends_with("/**")) {
            p.kind = Kind::Tree;
            p.path.resize(text.size() - 3);
        } else if (text.ends_with('/')) {
            p.kind = Kind::Directory;
            p.path.pop_back();
        }
        if (!is_valid_archive_path(p.path)) {
            error(text, "not a valid archive path (relative, with no empty, \".\", or \"..\" parts)");
            return std::nullopt;
        }
        if (!listed_.insert(p.path).second) {
            error(text, "listed more than once");
            return std::nullopt;
        }
        return p;
    }

    // Whether `path` is in the archive: an entry, or a directory with an
    // entry or with something in it, that hasn't been removed.
    bool still_there(const std::string& path) const
    {
        if (by_path_.contains(path) && !removed_.contains(path))
            return true;
        if (!dirs_.contains(path))
            return false;
        bool found = false;
        below(path, [&](const std::string& p) { found = found || !removed_.contains(p); });
        return found;
    }

    // Whether `p` names something in the archive, of the kind it says.
    // `after_deletes`: not counting what's been removed.
    bool exists(const Pattern& p, bool after_deletes)
    {
        const bool there = after_deletes ? still_there(p.path) : by_path_.contains(p.path) || dirs_.contains(p.path);
        if (!there) {
            error(p.text, "not in the archive");
            return false;
        }
        const bool directory = dirs_.contains(p.path);
        if (p.kind == Kind::File && directory) {
            error(p.text, "is a directory; list it as " + p.path + "/ (alone) or " + p.path + "/** (with its contents)");
            return false;
        }
        if (p.kind != Kind::File && !directory) {
            error(p.text, "not a directory");
            return false;
        }
        return true;
    }

    void remove(const Pattern& p)
    {
        if (by_path_.contains(p.path))
            removed_.insert(p.path);
        if (p.kind == Kind::Tree)
            below(p.path, [&](const std::string& path) { removed_.insert(path); });
    }

    // Calls `f` for every entry below the directory `dir`.
    template <typename F>
    void below(const std::string& dir, F f) const
    {
        // Paths below "a" sort together, directly after "a/".
        const auto prefix = dir + '/';
        for (auto it = by_path_.lower_bound(prefix); it != by_path_.end() && it->first.starts_with(prefix); ++it)
            f(it->first);
    }

    // A directory deleted alone must end up empty.
    void check_alone()
    {
        for (const auto& p : alone_) {
            bool empty = true;
            below(p.path, [&](const std::string& path) { empty = empty && removed_.contains(path); });
            if (!empty)
                error(p.text, "not empty (to delete it with everything in it, list " + p.path + "/**)");
        }
    }

    // A path below one added with "/**" would be added twice.
    void check_inside_trees()
    {
        std::set<std::string, PathLess> trees;
        for (const auto& a : plan_.additions)
            if (a.kind == Kind::Tree)
                trees.insert(a.path);
        for (const auto& a : plan_.additions)
            for (auto dir = parent_path(a.path); !dir.empty(); dir = parent_path(dir))
                if (trees.contains(dir)) {
                    error(a.path, "already added with " + std::string(dir) + "/**");
                    break;
                }
    }

    std::span<const Entry> entries_;
    std::map<std::string, const Entry*, PathLess> by_path_;
    std::set<std::string, PathLess> dirs_;     // with an entry, or implied by one below
    std::set<std::string, PathLess> listed_;
    std::set<std::string, PathLess> removed_;
    std::vector<Pattern> alone_;               // directories deleted alone
    std::vector<std::string> errors_;
    EditPlan plan_;
};

} // namespace

EditPlan plan_edit(std::span<const Entry> entries, const EditLists& lists)
{
    return Planner(entries).plan(lists);
}

std::vector<std::string> split_list(std::string_view text, bool null_separated)
{
    const char separator = null_separated ? '\0' : '\n';
    std::vector<std::string> items;
    while (!text.empty()) {
        const auto end = text.find(separator);
        if (const auto item = text.substr(0, end); !item.empty())
            items.emplace_back(item);
        if (end == std::string_view::npos)
            break;
        text.remove_prefix(end + 1);
    }
    return items;
}

} // namespace zar
