// zar — create an archive.
#include "zar/creator.hpp"
#include "zar/format.hpp"

#include <bit>
#include <charconv>
#include <cstdio>
#include <getopt.h>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>
#include <zstd.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kUsage = R"(usage: zar [options] PATH
       zar [options] -m -o ARCHIVE PATH...

PATH is stored under its own name, so extracting recreates it:
  zar proj      proj (file, directory, or symlink) is stored as proj
  zar proj/     the same, but proj must be a directory (symlinks followed)
  zar proj/.    the contents of proj are stored, without proj itself
  zar .         the contents of the current directory

  -o, --output=FILE      archive to write ('-' for stdout); defaults to
                         PATH.zar beside PATH, or NAME.zar here for '.'
  -m, --many             archive several PATHs into one archive (needs -o)
  -y, --yes              overwrite an existing archive
  -l, --level=N          zstd compression level (default 3)
  -j, --threads[=N]      worker threads (default: one per CPU core)
      --chunk-size=SIZE  split files into independently compressed chunks
                         of SIZE bytes: a power of two from 4K to 1G, with
                         an optional K, M, or G suffix (default 1M)
      --align[=N]        align block payloads to N bytes (default 8; with no
                         N, the CPU's cache-line size)
      --owner[=WHICH]    store file ownership: ids, names, or both (default)
      --nsec             store timestamps to the nanosecond
      --all-times        store access, change, and birth times too
      --names=MODE       non-UTF-8 file names: raw (default), skip, latin1
      --compress-all     compress even files that are usually precompressed
      --head-index       also put the index at the front, so the archive can
                         be extracted from a pipe (data is written twice)
  -v, --verbose          list files as they are added
  -h, --help             show this help
      --version          show version information
)";

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::uint64_t parse_number(std::string_view text, std::string_view what)
{
    std::uint64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size())
        throw UsageError("invalid " + std::string(what) + ": " + std::string(text));
    return value;
}

// "1M", "64K", "512", ... with binary multipliers.
std::uint64_t parse_size(std::string_view text, std::string_view what)
{
    int shift = 0;
    if (!text.empty()) {
        switch (text.back()) {
        case 'k': case 'K': shift = 10; break;
        case 'm': case 'M': shift = 20; break;
        case 'g': case 'G': shift = 30; break;
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

std::uint64_t cache_line_size()
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

std::uint8_t parse_alignment(const char* arg)
{
    const auto bytes = arg ? parse_size(arg, "alignment") : cache_line_size();
    if (!std::has_single_bit(bytes) || bytes > (std::uint64_t{1} << zar::kAlignLog2Max))
        throw UsageError("alignment must be a power of two up to 1M");
    return static_cast<std::uint8_t>(std::countr_zero(bytes));
}

int parse_level(std::string_view text)
{
    const bool negative = text.starts_with('-');
    const auto magnitude = parse_number(negative ? text.substr(1) : text, "level");
    const auto level = negative ? -static_cast<long long>(magnitude) : static_cast<long long>(magnitude);
    if (level < ZSTD_minCLevel() || level > ZSTD_maxCLevel())
        throw UsageError("level must be between " + std::to_string(ZSTD_minCLevel()) + " and " +
                         std::to_string(ZSTD_maxCLevel()));
    return static_cast<int>(level);
}

struct Command {
    zar::CreateOptions options;
    std::string output;
    bool overwrite = false;
    bool many = false;
    bool verbose = false;
    std::vector<fs::path> inputs;
};

enum LongOnly {
    kChunkSize = 256,
    kAlign,
    kOwner,
    kNsec,
    kAllTimes,
    kNames,
    kCompressAll,
    kHeadIndex,
    kVersion,
};

// Returns nullopt when the command was fully handled (--help, --version).
std::optional<Command> parse(int argc, char** argv)
{
    static const option kOptions[] = {
        {"output", required_argument, nullptr, 'o'},
        {"yes", no_argument, nullptr, 'y'},
        {"many", no_argument, nullptr, 'm'},
        {"level", required_argument, nullptr, 'l'},
        {"threads", optional_argument, nullptr, 'j'},
        {"chunk-size", required_argument, nullptr, kChunkSize},
        {"align", optional_argument, nullptr, kAlign},
        {"owner", optional_argument, nullptr, kOwner},
        {"nsec", no_argument, nullptr, kNsec},
        {"all-times", no_argument, nullptr, kAllTimes},
        {"names", required_argument, nullptr, kNames},
        {"compress-all", no_argument, nullptr, kCompressAll},
        {"head-index", no_argument, nullptr, kHeadIndex},
        {"verbose", no_argument, nullptr, 'v'},
        {"help", no_argument, nullptr, 'h'},
        {"version", no_argument, nullptr, kVersion},
        {nullptr, 0, nullptr, 0},
    };

    Command cmd;
    auto& opt = cmd.options;
    int c;
    while ((c = ::getopt_long(argc, argv, "o:myl:j::vh", kOptions, nullptr)) != -1) {
        const std::string_view arg = optarg ? optarg : "";
        switch (c) {
        case 'o': cmd.output = arg; break;
        case 'y': cmd.overwrite = true; break;
        case 'm': cmd.many = true; break;
        case 'l': opt.level = parse_level(arg); break;
        case 'j':
            opt.threads = 0;    // one per core
            if (optarg) {
                const auto threads = parse_number(arg, "thread count");
                if (threads == 0 || threads > 1024)
                    throw UsageError("thread count must be between 1 and 1024");
                opt.threads = static_cast<unsigned>(threads);
            }
            break;
        case kChunkSize:
            opt.chunk_size = parse_size(arg, "chunk size");
            if (!std::has_single_bit(opt.chunk_size) || opt.chunk_size < (1 << 12) ||
                opt.chunk_size > (1 << 30))
                throw UsageError("chunk size must be a power of two from 4K to 1G");
            break;
        case kAlign: opt.align_log2 = parse_alignment(optarg); break;
        case kOwner:
            if (!optarg || arg == "both")
                opt.owner = zar::OwnerMode::Both;
            else if (arg == "ids")
                opt.owner = zar::OwnerMode::Ids;
            else if (arg == "names")
                opt.owner = zar::OwnerMode::Names;
            else
                throw UsageError("--owner must be ids, names, or both");
            break;
        case kNsec: opt.nanoseconds = true; break;
        case kAllTimes: opt.all_times = true; break;
        case kNames:
            if (arg == "raw")
                opt.names = zar::NameMode::Raw;
            else if (arg == "skip")
                opt.names = zar::NameMode::Skip;
            else if (arg == "latin1")
                opt.names = zar::NameMode::Latin1;
            else
                throw UsageError("--names must be raw, skip, or latin1");
            break;
        case kCompressAll: opt.compress_all = true; break;
        case kHeadIndex: opt.head_index = true; break;
        case 'v': cmd.verbose = true; break;
        case 'h':
            std::cout << kUsage;
            return std::nullopt;
        case kVersion:
            std::cout << "zar " << ZAR_VERSION << " (format " << zar::kFormatVersion.to_string()
                      << ", zstd " << ZSTD_versionString() << ")\n";
            return std::nullopt;
        default:
            throw UsageError("");    // getopt has already described the problem
        }
    }

    for (int i = optind; i < argc; ++i)
        cmd.inputs.emplace_back(argv[i]);
    if (cmd.inputs.empty())
        throw UsageError("nothing to archive");
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

std::string human_size(std::uint64_t bytes)
{
    constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024 && unit + 1 < std::size(kUnits)) {
        value /= 1024;
        ++unit;
    }
    char text[32];
    std::snprintf(text, sizeof text, unit ? "%.1f %s" : "%.0f %s", value, kUnits[unit]);
    return text;
}

int run(Command& cmd)
{
    cmd.options.on_warning = [](std::string_view message) {
        std::cerr << "zar: " << message << '\n';
    };
    if (cmd.verbose)
        cmd.options.on_entry = [](std::string_view path) { std::cerr << path << '\n'; };

    auto out = cmd.output == "-" ? zar::Output::standard_output()
                                 : zar::Output::create_file(cmd.output, cmd.overwrite);
    const auto stats = zar::create_archive(out, cmd.inputs, cmd.options);
    out.commit();

    if (cmd.verbose) {
        std::cerr << stats.files << " files, " << stats.directories << " directories, "
                  << stats.symlinks << " symlinks, " << stats.hardlinks << " hard links: "
                  << human_size(stats.input_bytes) << " -> " << human_size(stats.archive_bytes)
                  << '\n';
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
        std::cerr << "zar: " << e.what() << '\n';
        return 2;
    }
}
