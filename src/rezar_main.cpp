// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// rezar — add, replace, or delete paths in an archive, writing a new one.
#include "cli.hpp"
#include "create_options.hpp"
#include "zar/archive_reader.hpp"
#include "zar/creator.hpp"
#include "zar/editor.hpp"

#include <fstream>
#include <getopt.h>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kDescription =
    "Writes a new archive: a copy of ARCHIVE with paths added, replaced, or\n"
    "deleted. What's kept is copied as it is, never decompressed or\n"
    "recompressed, and the space of what's deleted is reclaimed.";

constexpr std::string_view kUsage = R"(usage: rezar [options] -o OUTPUT ARCHIVE

  -o, --output=FILE      the new archive (required; never ARCHIVE itself)
  -y, --yes              overwrite OUTPUT if it exists
  -C, --directory=DIR    read the paths to add from DIR (default: the
                         current directory)
      --add-list=FILE    add the paths listed in FILE ('-' for standard
                         input), which must not be in the archive yet
      --replace-list=FILE
                         replace the paths listed; they must be there
      --add-replace-list=FILE
                         add the paths listed, or replace them if they're
                         there
      --delete-list=FILE delete the paths listed
      --add=PATH, --replace=PATH, --add-replace=PATH, --delete=PATH
                         the same for one PATH; each may be repeated
  -0, --null             lists are separated by NUL bytes, not newlines
  -n, --dry-run          show what would change, and write nothing
      --dedup=on|off|deep
                         store identical new chunks only once (on, the
                         default); deep also reuses identical blocks already
                         in the archive, decompressing those of the same
                         length to compare them
      --align[=N]        align block payloads to N bytes (default: as in
                         ARCHIVE; with no N, the CPU's cache-line size)
      --head-index[=on|off]
                         also put the index at the front, so the archive
                         can be extracted from a pipe (default: as in
                         ARCHIVE; on without a value)
  -v, --verbose          list paths as they are added, and print a summary
      --help             show this help
      --version          show version information

For the files added, as with zar:
)";

constexpr std::string_view kUsageTail = R"(
PATH is a path in the archive, and what's added there is read from DIR/PATH:
  a/b       the file or symlink a/b (not followed)
  a/d/      the directory a/d alone; deleted only if it's empty
  a/d/**    the directory a/d and everything below it
Deletions come first, then additions, so the order of lines doesn't matter,
but a path may be listed only once. Missing parent directories are added
too. A hard link to a file that's deleted or replaced keeps the old content.
On the command line, quote 'a/d/**', or the shell may expand it. Solid
archives can't be edited: everything in them would be recompressed.

Short options can be combined, as in -yv0; -j's count must be attached (-j4).
)";

using zar::cli::printable;
using zar::cli::UsageError;

enum LongOnly {
    kAddList = 256,    // past every short option
    kReplaceList,
    kAddReplaceList,
    kDeleteList,
    kAdd,
    kReplace,
    kAddReplace,
    kDelete,
    kDedup,
    kHeadIndex,
    kHelp,
    kVersion,
};

struct Command {
    zar::CreateOptions options;
    std::string archive;
    std::string output;
    fs::path directory = ".";
    bool overwrite = false;
    bool null_separated = false;
    bool dry_run = false;
    bool verbose = false;
    std::optional<std::uint8_t> align_log2;    // unset: as in the archive
    std::optional<bool> head_index;            // unset: as in the archive
    zar::EditLists lists;
    // (which list, file), read once the options are all known
    std::vector<std::pair<std::vector<std::string> zar::EditLists::*, std::string>> list_files;
};

void print_help()
{
    zar::cli::print_banner(std::cout, "rezar", kDescription);
    std::cout << kUsage << zar::cli::kCreateOptionsHelp << kUsageTail;
}

// Returns nullopt when the command was fully handled (--help, --version).
std::optional<Command> parse(int argc, char** argv)
{
    static const auto kOptions = zar::cli::option_table({
        {"output", required_argument, nullptr, 'o'},
        {"yes", no_argument, nullptr, 'y'},
        {"directory", required_argument, nullptr, 'C'},
        {"add-list", required_argument, nullptr, kAddList},
        {"replace-list", required_argument, nullptr, kReplaceList},
        {"add-replace-list", required_argument, nullptr, kAddReplaceList},
        {"delete-list", required_argument, nullptr, kDeleteList},
        {"add", required_argument, nullptr, kAdd},
        {"replace", required_argument, nullptr, kReplace},
        {"add-replace", required_argument, nullptr, kAddReplace},
        {"delete", required_argument, nullptr, kDelete},
        {"null", no_argument, nullptr, '0'},
        {"dry-run", no_argument, nullptr, 'n'},
        {"dedup", required_argument, nullptr, kDedup},
        {"head-index", optional_argument, nullptr, kHeadIndex},
        {"verbose", no_argument, nullptr, 'v'},
        {"help", no_argument, nullptr, kHelp},
        {"version", no_argument, nullptr, kVersion},
    });

    Command cmd;
    auto& lists = cmd.lists;
    int c;
    while ((c = ::getopt_long(argc, argv, "o:yC:0nvl:j::", kOptions.data(), nullptr)) != -1) {
        if (zar::cli::parse_create_option(c, optarg, cmd.options))
            continue;
        const std::string_view arg = optarg ? optarg : "";
        switch (c) {
        case 'o': cmd.output = arg; break;
        case 'y': cmd.overwrite = true; break;
        case 'C': cmd.directory = std::string(arg); break;
        case kAddList: cmd.list_files.emplace_back(&zar::EditLists::add, arg); break;
        case kReplaceList: cmd.list_files.emplace_back(&zar::EditLists::replace, arg); break;
        case kAddReplaceList: cmd.list_files.emplace_back(&zar::EditLists::add_replace, arg); break;
        case kDeleteList: cmd.list_files.emplace_back(&zar::EditLists::remove, arg); break;
        case kAdd: lists.add.emplace_back(arg); break;
        case kReplace: lists.replace.emplace_back(arg); break;
        case kAddReplace: lists.add_replace.emplace_back(arg); break;
        case kDelete: lists.remove.emplace_back(arg); break;
        case '0': cmd.null_separated = true; break;
        case 'n': cmd.dry_run = true; break;
        case kDedup:
            if (arg == "on" || arg == "deep")
                cmd.options.dedup = true;
            else if (arg == "off")
                cmd.options.dedup = false;
            else
                throw UsageError("--dedup must be on, off, or deep");
            cmd.options.dedup_copied = arg == "deep";
            break;
        case zar::cli::kAlign: cmd.align_log2 = zar::cli::parse_alignment(optarg); break;
        case kHeadIndex:
            if (!optarg || arg == "on")
                cmd.head_index = true;
            else if (arg == "off")
                cmd.head_index = false;
            else
                throw UsageError("--head-index must be on or off");
            break;
        case 'v': cmd.verbose = true; break;
        case kHelp:
            print_help();
            return std::nullopt;
        case kVersion:
            zar::cli::print_version(std::cout, "rezar");
            return std::nullopt;
        default:
            throw UsageError("");    // getopt has already described the problem
        }
    }

    if (optind >= argc)
        throw UsageError("no archive given");
    cmd.archive = argv[optind++];
    if (optind < argc)
        throw UsageError("one ARCHIVE at a time; paths go in lists, or after --add and the like");
    if (cmd.archive == "-")
        throw UsageError("the archive must be a file; rezar reads it at random");
    if (cmd.output.empty() && !cmd.dry_run)
        throw UsageError("-o is needed, to name the new archive");
    if (cmd.output == "-")
        throw UsageError("the new archive must be a file");
    std::size_t from_stdin = 0;
    for (const auto& [list, file] : cmd.list_files)
        from_stdin += file == "-";
    if (from_stdin > 1)
        throw UsageError("only one list can come from standard input");
    return cmd;
}

std::vector<std::string> read_list(const std::string& file, bool null_separated)
{
    std::string text;
    if (file == "-") {
        text.assign(std::istreambuf_iterator<char>(std::cin), {});
    } else {
        std::ifstream in(file, std::ios::binary);
        if (!in)
            throw std::runtime_error(file + ": can't read this list");
        text.assign(std::istreambuf_iterator<char>(in), {});
    }
    return zar::split_list(text, null_separated);
}

void print_warning(std::string_view message)
{
    std::cerr << "rezar: " << printable(message) << '\n';
}

// Whether `a` and `b` name the same existing file.
bool same_file(const std::string& a, const std::string& b)
{
    struct stat sa {}, sb {};
    return ::stat(a.c_str(), &sa) == 0 && ::stat(b.c_str(), &sb) == 0 && sa.st_dev == sb.st_dev &&
           sa.st_ino == sb.st_ino;
}

void print_plan(const zar::EditPlan& plan)
{
    for (const auto& path : plan.removed)
        std::cout << "- " << printable(path) << '\n';
    for (const auto& a : plan.additions)
        std::cout << "+ " << printable(a.path)
                  << (a.kind == zar::Addition::Kind::Tree        ? "/**"
                      : a.kind == zar::Addition::Kind::Directory ? "/"
                                                                 : "")
                  << '\n';
    std::cout << plan.keep.size() << " entries kept, " << plan.deleted << " deleted, "
              << plan.removed.size() - plan.deleted << " replaced, " << plan.additions.size()
              << " paths to add\n";
}

int run(Command& cmd)
{
    for (const auto& [list, file] : cmd.list_files) {
        auto items = read_list(file, cmd.null_separated);
        (cmd.lists.*list).insert((cmd.lists.*list).end(), items.begin(), items.end());
    }

    auto source = zar::ArchiveReader::open(cmd.archive, print_warning);
    const auto plan = zar::plan_edit(source.entries(), cmd.lists);
    if (cmd.dry_run) {
        print_plan(plan);
        return 0;
    }
    if (same_file(cmd.archive, cmd.output))
        throw std::runtime_error(cmd.output + ": that's the archive being edited; write the new one elsewhere");

    // Like the archive, unless asked otherwise.
    const auto& header = source.header();
    auto& options = cmd.options;
    options.align_log2 = cmd.align_log2.value_or(header.align_log2);
    options.head_index = cmd.head_index.value_or(header.index_offset < 0);
    options.on_warning = print_warning;
    if (cmd.verbose)
        options.on_entry = [](std::string_view path) { std::cerr << printable(path) << '\n'; };

    auto out = zar::Output::create_file(cmd.output, cmd.overwrite);
    const auto stats = zar::rebuild_archive(out, source, plan.keep, plan.additions, cmd.directory, options);
    out.commit();

    if (cmd.verbose) {
        std::cerr << "kept " << stats.copied_entries << " entries ("
                  << zar::cli::human_size(stats.copied_bytes) << " copied as is), deleted " << plan.deleted
                  << ", replaced " << plan.removed.size() - plan.deleted << "; added " << stats.files
                  << " files, " << stats.directories << " directories, " << stats.symlinks
                  << " symlinks, " << stats.hardlinks << " hard links ("
                  << zar::cli::human_size(stats.input_bytes) << ")";
        if (stats.deduplicated_bytes)
            std::cerr << ", " << zar::cli::human_size(stats.deduplicated_bytes) << " deduplicated";
        if (stats.hole_bytes)
            std::cerr << ", " << zar::cli::human_size(stats.hole_bytes) << " in holes";
        std::cerr << ": " << zar::cli::human_size(stats.archive_bytes) << '\n';
    }
    return stats.warnings ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        auto cmd = parse(argc, argv);
        return cmd ? run(*cmd) : 0;
    } catch (const UsageError& e) {
        if (*e.what())
            std::cerr << "rezar: " << e.what() << '\n';
        std::cerr << "Try 'rezar --help' for more information.\n";
        return 2;
    } catch (const zar::EditError& e) {
        // One problem per line, each naming a listed path.
        std::string_view problems = e.what();
        while (!problems.empty()) {
            const auto end = problems.find('\n');
            print_warning(problems.substr(0, end));
            problems = end == std::string_view::npos ? std::string_view{} : problems.substr(end + 1);
        }
        std::cerr << "rezar: nothing was written\n";
        return 2;
    } catch (const std::exception& e) {
        print_warning(e.what());
        return 2;
    }
}
