// unzar — list, verify, or extract an archive.
#include "zar/format.hpp"

#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view kUsage = R"(usage: unzar [options] ARCHIVE [PATH...]

  -C, --directory=DIR    extract into DIR (default: current directory)
  -l, --list             list contents instead of extracting
  -t, --test             verify all checksums without writing anything
  -k, --keep             don't overwrite existing files
  -j, --threads=N        worker threads (default: one per CPU)
  -v, --verbose          list files as they are extracted
  -h, --help             show this help
      --version          show version information

If PATHs are given, only those entries (and directories' contents) are
processed.
)";

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            std::cout << kUsage;
            return 0;
        }
        if (arg == "--version") {
            std::cout << "unzar " << ZAR_VERSION << " (format "
                      << zar::kFormatVersion.to_string() << ")\n";
            return 0;
        }
    }
    std::cerr << "unzar: not implemented yet\n" << kUsage;
    return 2;
}
