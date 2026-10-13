// unzar — extract, test, scan, or list an archive.
#include "cli.hpp"
#include "zar/extractor.hpp"
#include "zar/format.hpp"
#include "zar/names.hpp"

#include <ctime>
#include <functional>
#include <getopt.h>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <zstd.h>

namespace {

constexpr std::string_view kUsage = R"(usage: unzar [options] ARCHIVE [PATH...]

ARCHIVE may be '-' for standard input. Reading from a pipe works for
archives created with zar --head-index.

  -C, --directory=DIR    extract into DIR (default: current directory)
  -l, --list             print a listing: each directory on its own line,
                         followed by its files with their size and mtime
      --scan             check everything except decompression: block and
                         index checksums, sizes, and paths; write nothing
  -t, --test             like --scan, but also decompress and verify data
  -y, --yes              overwrite files and directories that already exist
      --dirs=MODE        existing directories: merge (default) extracts into
                         them; replace deletes them first. Either needs -y.
  -j, --threads[=N]      worker threads (default: one per CPU core)
      --max-size=SIZE    refuse to decompress more than SIZE bytes in all
                         (e.g. 10G): the selected files' total is checked
                         before anything is written, and a solid archive
                         stops decompressing once it passes SIZE
  -v, --verbose          with --list, also show permissions and owner;
                         print a summary at the end
  -h, --help             show this help
      --version          show version information

If PATHs are given, only those entries (and directories' contents) are
processed. Nothing is extracted if any target already exists, unless -y is
given. --list combines with any mode: `unzar --list --scan` lists an
archive while checking it, without extracting.
)";

using zar::cli::UsageError;

struct Command {
    zar::ExtractOptions options;
    std::string archive;
    std::vector<std::string> selections;
    bool list = false;
    bool verbose = false;
};

enum LongOnly { kScan = 256, kDirs, kMaxSize, kVersion };

std::optional<Command> parse(int argc, char** argv)
{
    static const option kOptions[] = {
        {"directory", required_argument, nullptr, 'C'},
        {"list", no_argument, nullptr, 'l'},
        {"scan", no_argument, nullptr, kScan},
        {"test", no_argument, nullptr, 't'},
        {"yes", no_argument, nullptr, 'y'},
        {"dirs", required_argument, nullptr, kDirs},
        {"threads", optional_argument, nullptr, 'j'},
        {"max-size", required_argument, nullptr, kMaxSize},
        {"verbose", no_argument, nullptr, 'v'},
        {"help", no_argument, nullptr, 'h'},
        {"version", no_argument, nullptr, kVersion},
        {nullptr, 0, nullptr, 0},
    };

    Command cmd;
    int c;
    while ((c = ::getopt_long(argc, argv, "C:ltyj::vh", kOptions, nullptr)) != -1) {
        switch (c) {
        case 'C': cmd.options.directory = optarg; break;
        case 'l': cmd.list = true; break;
        case kScan: cmd.options.mode = zar::ExtractMode::Scan; break;
        case 't': cmd.options.mode = zar::ExtractMode::Test; break;
        case 'y': cmd.options.overwrite = true; break;
        case 'j': cmd.options.threads = zar::cli::parse_threads(optarg); break;
        case kMaxSize: cmd.options.size_limit = zar::cli::parse_size(optarg, "size limit"); break;
        case kDirs:
            if (std::string_view(optarg) == "merge")
                cmd.options.dirs = zar::DirMode::Merge;
            else if (std::string_view(optarg) == "replace")
                cmd.options.dirs = zar::DirMode::Replace;
            else
                throw UsageError("--dirs must be merge or replace");
            break;
        case 'v': cmd.verbose = true; break;
        case 'h':
            std::cout << kUsage;
            return std::nullopt;
        case kVersion:
            std::cout << "unzar " << ZAR_VERSION << " (format " << zar::kFormatVersion.to_string()
                      << ", zstd " << ZSTD_versionString() << ")\n";
            return std::nullopt;
        default:
            throw UsageError("");
        }
    }
    if (optind >= argc)
        throw UsageError("no archive given");
    cmd.archive = argv[optind++];
    for (int i = optind; i < argc; ++i) {
        std::string selection = argv[i];
        while (selection.size() > 1 && selection.back() == '/')
            selection.pop_back();
        cmd.selections.push_back(std::move(selection));
    }
    return cmd;
}

// Escapes control characters, backslashes, and (in names that aren't valid
// UTF-8) non-ASCII bytes, so every name prints on one line without
// disturbing the tab-separated columns.
std::string printable(std::string_view name)
{
    const bool utf8 = zar::is_valid_utf8(name);
    std::string out;
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte == '\\') {
            out += "\\\\";
        } else if (byte < 0x20 || byte == 0x7f || (!utf8 && byte >= 0x80)) {
            constexpr char kHex[] = "0123456789abcdef";
            out += "\\x";
            out += kHex[byte >> 4];
            out += kHex[byte & 0xf];
        } else {
            out += c;
        }
    }
    return out;
}

std::string format_time(const std::optional<zar::Timestamp>& t)
{
    if (!t)
        return "-";
    const auto seconds = static_cast<std::time_t>(t->seconds);
    std::tm local{};
    char text[32];
    if (!::localtime_r(&seconds, &local) ||
        !std::strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S", &local))
        return std::to_string(t->seconds);
    return text;
}

std::string format_mode(const zar::Entry& e)
{
    std::string out = e.type == zar::EntryType::Dir       ? "d"
                      : e.type == zar::EntryType::Symlink ? "l"
                                                          : "-";
    if (!e.mode)
        return out + "?????????";
    constexpr const char* kBits = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i)
        out += (*e.mode >> (8 - i)) & 1 ? kBits[i] : '-';
    return out;
}

std::string format_owner(const zar::Entry& e)
{
    const auto user = e.uname ? *e.uname : e.uid ? std::to_string(*e.uid) : "-";
    const auto group = e.gname ? *e.gname : e.gid ? std::to_string(*e.gid) : "-";
    return user + ':' + group;
}

class Listing {
public:
    explicit Listing(bool verbose) : verbose_(verbose) {}

    void directory(const zar::DirectoryGroup& group)
    {
        if (group.path.empty())
            return;    // the root has no heading
        if (!group.entry && group.members.empty())
            return;    // only an ancestor of what was selected
        if (printed_)
            std::cout << '\n';
        std::cout << printable(group.path) << "/\n";
        printed_ = true;
    }

    void member(const zar::Entry& e)
    {
        std::string name = printable(zar::base_name(e.path));
        if (e.type == zar::EntryType::Symlink && e.link_target)
            name += " -> " + printable(*e.link_target);
        const auto size = e.type == zar::EntryType::Symlink ? "-" : std::to_string(e.size.value_or(0));

        std::cout << "  " << name << '\t' << size << '\t' << format_time(e.mtime);
        if (verbose_)
            std::cout << '\t' << format_mode(e) << '\t' << format_owner(e);
        std::cout << '\n';
        printed_ = true;
    }

private:
    bool verbose_;
    bool printed_ = false;
};

bool selected(std::string_view path, const std::vector<std::string>& selections,
              std::set<std::string_view>& matched)
{
    if (selections.empty())
        return true;
    for (const auto& s : selections) {
        if (path == s || (path.starts_with(s) && path[s.size()] == '/')) {
            matched.insert(s);
            return true;
        }
    }
    return false;
}

// "-" reads stdin: at random if it's a file, otherwise as a stream.
zar::ArchiveReader open_archive(const std::string& archive,
                                const std::function<void(std::string_view)>& warn,
                                std::optional<std::uint64_t> size_limit)
{
    if (archive != "-")
        return zar::ArchiveReader::open(archive, warn, size_limit);

    struct stat st {};
    if (::fstat(STDIN_FILENO, &st) == 0 && S_ISREG(st.st_mode))
        return zar::ArchiveReader::open("/dev/fd/0", warn, size_limit);
    if (::isatty(STDIN_FILENO))
        throw std::runtime_error("refusing to read an archive from a terminal");
    return zar::ArchiveReader::open_stream(STDIN_FILENO, "standard input", warn, size_limit);
}

int run(Command& cmd)
{
    std::uint64_t warnings = 0;
    const auto warn = [&](std::string_view message) {
        ++warnings;
        std::cerr << "unzar: " << message << '\n';
    };
    cmd.options.on_warning = warn;

    auto reader = open_archive(cmd.archive, warn, cmd.options.size_limit);

    std::vector<zar::Entry> chosen;
    std::set<std::string_view> matched;
    for (const auto& entry : reader.entries())
        if (selected(entry.path, cmd.selections, matched))
            chosen.push_back(entry);
    for (const auto& s : cmd.selections)
        if (!matched.contains(s))
            warn(s + ": not found in archive");

    const auto groups = zar::group_by_directory(chosen);
    Listing listing(cmd.verbose);
    zar::Extractor extractor(reader, cmd.options);
    extractor.prepare(groups);
    for (const auto& group : groups) {
        if (cmd.list)
            listing.directory(group);
        extractor.enter(group);
        for (const auto* entry : group.members) {
            if (cmd.list)
                listing.member(*entry);
            extractor.extract(*entry);
        }
    }
    const auto stats = extractor.finish();

    if (cmd.verbose) {
        const char* verb = cmd.options.mode == zar::ExtractMode::Extract ? "extracted"
                           : cmd.options.mode == zar::ExtractMode::Test  ? "tested"
                                                                         : "scanned";
        std::cerr << verb << ' ' << stats.files << " files, " << stats.directories
                  << " directories, " << stats.symlinks << " symlinks (" << stats.bytes
                  << " bytes)" << (warnings ? "" : "; no problems found") << '\n';
    }
    return warnings ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        auto cmd = parse(argc, argv);
        return cmd ? run(*cmd) : 0;
    } catch (const UsageError& e) {
        if (*e.what())
            std::cerr << "unzar: " << e.what() << '\n';
        std::cerr << "Try 'unzar --help' for more information.\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "unzar: " << e.what() << '\n';
        return 2;
    }
}
