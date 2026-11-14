// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// zar — create an archive.
#include "cli.hpp"
#include "create_options.hpp"
#include "progress.hpp"
#include "zar/creator.hpp"
#include "zar/dictionary.hpp"
#include "zar/format.hpp"

#include <charconv>
#include <cstdio>
#include <getopt.h>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kDescription =
    "Creates a zar archive of a file or directory. Files are compressed with\n"
    "zstd in independently checksummed chunks, so any file can be listed,\n"
    "verified, or extracted without decompressing the rest. Holes in sparse\n"
    "files are recorded rather than stored.";

constexpr std::string_view kUsage = R"(usage: zar [options] PATH
       zar [options] -m -o ARCHIVE PATH...
       zar [-y] --keygen=KEYFILE

PATH is stored under its own name, so extracting recreates it:
  zar proj      proj (file, directory, or symlink) is stored as proj
  zar proj/     the same, but proj must be a directory (symlinks followed)
  zar proj/.    the contents of proj are stored, without proj itself
  zar .         the contents of the current directory

  -o, --output=FILE      archive to write ('-' for stdout); defaults to
                         NAME.zar in the current directory, where NAME is
                         the last component of PATH (for '.', this
                         directory's name)
  -m, --many             archive several PATHs into one archive (needs -o)
  -y, --yes              overwrite an existing archive
      --read-devices     store a disk device given as PATH (/dev/sdb,
                         /dev/rdisk4, or a symlink to one) as a file holding
                         the disk's contents; devices in directories are
                         still skipped
)";

// After the options shared with rezar.
constexpr std::string_view kUsageTail = R"(      --align[=N]        align block payloads, and the archive's end, to N
                         bytes (default 8; with no N, the CPU's cache-line
                         size)
      --dedup=on|off     store identical chunks only once (default: on,
                         except when writing to a pipe without --head-index)
      --dict[=SIZE]      train a zstd dictionary of at most SIZE bytes
                         (default 110K) on the files and compress them with
                         it; helps most with many small, similar files
      --head-index       also put the index at the front, so the archive can
                         be extracted from a pipe (index is written twice)
      --solid=SIZE       first try compressing everything as one stream, and
                         keep that if it's at most SIZE bytes (e.g. 1M);
                         better for many small files, but without random
                         access. Otherwise a normal archive is written.
      --dict-over-solid=PERCENT
                         when the solid archive fits, also try a normal one
                         compressed with a dictionary, and keep it instead if
                         it's at most PERCENT of the solid size and fits too
                         (default 120; 0 never tries)
  -v, --verbose          list files as they are added, and print a summary
      --progress         show progress on the terminal: how much is done,
                         the rate, and the time left
      --help             show this help
      --version          show version information

Hashes and signatures:
)";

constexpr std::string_view kUsageEnd = R"(      --keygen=KEYFILE   make a new Ed25519 key pair for --sign: the secret
                         key in KEYFILE (mode 0600, kept private) and the
                         public key, for unzar --verify, in KEYFILE.pub

Short options can be combined, as in -myv; -l's level can be attached or
separate (-l19 or -l 19), but -j's count must be attached (-j4).
)";

using zar::cli::parse_number;
using zar::cli::printable;
using zar::cli::parse_size;
using zar::cli::UsageError;

struct Command {
    zar::CreateOptions options;
    std::string output;
    bool overwrite = false;
    bool many = false;
    bool verbose = false;
    bool progress = false;
    std::vector<fs::path> inputs;
    std::optional<fs::path> keygen;
};

enum LongOnly {
    kHeadIndex = 256,    // past every short option
    kSolid,
    kDedup,
    kDict,
    kDictOverSolid,
    kKeygen,
    kReadDevices,
    kProgress,
    kHelp,
    kVersion,
};

void print_help()
{
    zar::cli::print_banner(std::cout, "zar", kDescription);
    std::cout << kUsage << zar::cli::kCreateOptionsHelp << kUsageTail << zar::cli::kSigningOptionsHelp
              << kUsageEnd;
}

// Returns nullopt when the command was fully handled (--help, --version).
std::optional<Command> parse(int argc, char** argv)
{

    static const auto kOptions = zar::cli::option_table({
        {"output", required_argument, nullptr, 'o'},
        {"yes", no_argument, nullptr, 'y'},
        {"many", no_argument, nullptr, 'm'},
        {"head-index", no_argument, nullptr, kHeadIndex},
        {"solid", required_argument, nullptr, kSolid},
        {"dedup", required_argument, nullptr, kDedup},
        {"dict", optional_argument, nullptr, kDict},
        {"dict-over-solid", required_argument, nullptr, kDictOverSolid},
        {"verbose", no_argument, nullptr, 'v'},
        {"help", no_argument, nullptr, kHelp},
        {"keygen", required_argument, nullptr, kKeygen},
        {"read-devices", no_argument, nullptr, kReadDevices},
        {"progress", no_argument, nullptr, kProgress},
        {"version", no_argument, nullptr, kVersion},
    });

    Command cmd;
    auto& opt = cmd.options;
    int c;
    while ((c = ::getopt_long(argc, argv, "o:myl:j::v", kOptions.data(), nullptr)) != -1) {
        if (zar::cli::parse_create_option(c, optarg, opt))
            continue;
        const std::string_view arg = optarg ? optarg : "";
        switch (c) {
        case 'o': cmd.output = arg; break;
        case 'y': cmd.overwrite = true; break;
        case 'm': cmd.many = true; break;
        case zar::cli::kAlign: opt.align_log2 = zar::cli::parse_alignment(optarg); break;
        case kHeadIndex: opt.head_index = true; break;
        case kSolid:
            opt.solid_limit = parse_size(arg, "solid size");
            if (*opt.solid_limit == 0)
                throw UsageError("--solid needs a size above zero");
            break;
        case kDedup:
            if (arg == "on")
                opt.dedup = true;
            else if (arg == "off")
                opt.dedup = false;
            else
                throw UsageError("--dedup must be on or off");
            break;
        case kDict:
            opt.dict_size = optarg ? parse_size(arg, "dictionary size") : zar::kDefaultDictSize;
            if (*opt.dict_size < (1 << 10) || *opt.dict_size > (1 << 30))
                throw UsageError("dictionary size must be from 1K to 1G");
            break;
        case kDictOverSolid:
            opt.dict_over_solid = static_cast<unsigned>(parse_number(arg, "percentage"));
            if (opt.dict_over_solid > 1000)
                throw UsageError("--dict-over-solid must be from 0 to 1000");
            break;
        case 'v': cmd.verbose = true; break;
        case kHelp:
            print_help();
            return std::nullopt;
        case kKeygen: cmd.keygen = fs::path(std::string(arg)); break;
        case kReadDevices: opt.read_devices = true; break;
        case kProgress: cmd.progress = true; break;
        case kVersion:
            zar::cli::print_version(std::cout, "zar");
            return std::nullopt;
        default:
            throw UsageError("");    // getopt has already described the problem
        }
    }

    for (int i = optind; i < argc; ++i)
        cmd.inputs.emplace_back(argv[i]);
    if (cmd.keygen) {
        if (!cmd.inputs.empty())
            throw UsageError("--keygen makes a key pair; it doesn't archive anything");
        return cmd;
    }
    if (cmd.inputs.empty())
        throw UsageError("nothing to archive");
    zar::cli::finish_signing_options(cmd.options);
    if (cmd.many) {
        if (cmd.output.empty())
            throw UsageError("-m needs -o to name the archive");
    } else {
        if (cmd.inputs.size() > 1)
            throw UsageError("one PATH at a time; use -m -o ARCHIVE for several");
        if (cmd.output.empty())
            cmd.output = zar::default_archive_path(zar::parse_input(cmd.inputs.front()));
    }
    return cmd;
}

int keygen(const Command& cmd)
{
    const auto key = zar::SecretKey::generate();
    zar::write_key_files(*cmd.keygen, key, cmd.overwrite);
    std::cout << "secret key: " << printable(cmd.keygen->string()) << " (keep it private)\n"
              << "public key: " << printable(cmd.keygen->string()) << ".pub (fingerprint "
              << zar::fingerprint(key.public_key()) << ")\n";
    return 0;
}

int run(Command& cmd)
{
    if (cmd.keygen)
        return keygen(cmd);

    using zar::cli::Progress;
    // File names can hold anything, including terminal control sequences.
    cmd.options.on_warning = [](std::string_view message) {
        Progress::around([&] { std::cerr << "zar: " << printable(message) << '\n'; });
    };
    if (cmd.verbose)
        cmd.options.on_entry = [](std::string_view path) {
            Progress::around([&] { std::cerr << printable(path) << '\n'; });
        };

    auto out = cmd.output == "-" ? zar::Output::standard_output()
                                 : zar::Output::create_file(cmd.output, cmd.overwrite);
    std::optional<Progress> progress;
    if (cmd.progress && Progress::available()) {
        progress.emplace();
        progress->stage("measuring", 0);
        const auto total = zar::measure_inputs(cmd.inputs, cmd.options.read_devices);
        cmd.options.on_stage = [&progress, total](std::string_view stage) { progress->stage(stage, total); };
        cmd.options.on_progress = [&progress](std::uint64_t bytes) { progress->add(bytes); };
        cmd.options.on_written = [&progress](std::uint64_t data, std::uint64_t stored) {
            progress->written(data, stored);
        };
    }
    const auto stats = zar::create_archive(out, cmd.inputs, cmd.options);
    out.commit();
    progress.reset();

    if (cmd.verbose) {
        std::cerr << stats.files << " files, " << stats.directories << " directories, "
                  << stats.symlinks << " symlinks, " << stats.hardlinks << " hard links: "
                  << zar::cli::human_size(stats.input_bytes) << " -> "
                  << zar::cli::human_size(stats.archive_bytes)
                  << (stats.solid ? " (solid)" : stats.dictionary ? " (dictionary)" : "");
        if (stats.deduplicated_bytes)
            std::cerr << ", " << zar::cli::human_size(stats.deduplicated_bytes) << " deduplicated";
        if (stats.hole_bytes)
            std::cerr << ", " << zar::cli::human_size(stats.hole_bytes) << " in holes";
        if (stats.signed_by)
            std::cerr << ", signed";
        else if (cmd.options.hash)
            std::cerr << ", hashed";
        std::cerr << '\n';
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
            std::cerr << "zar: " << e.what() << '\n';
        std::cerr << "Try 'zar --help' for more information.\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "zar: " << printable(e.what()) << '\n';
        return 2;
    }
}
