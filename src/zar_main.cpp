// zar — create an archive.
#include "zar/format.hpp"

#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view kUsage = R"(usage: zar [options] -o ARCHIVE PATH...

  -o, --output=FILE      archive to write ('-' for stdout)
  -l, --level=N          zstd compression level (default 3)
  -j, --threads=N        worker threads (default: one per CPU)
      --chunk-size=SIZE  split files into independently compressed chunks
                         of SIZE bytes (default 1M)
      --align[=N]        align block payloads to N bytes (default 8; with no
                         N, the CPU's cache-line size)
      --owner            store uid/gid and owner/group names
      --nsec             store timestamps to the nanosecond
      --names=MODE       non-UTF-8 file names: raw (default), skip, latin1
      --normalize=FORM   Unicode-normalize names: nfc or nfd (default: none)
      --compress-all     compress even files that are usually precompressed
  -v, --verbose          list files as they are added
  -h, --help             show this help
      --version          show version information
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
            std::cout << "zar " << ZAR_VERSION << " (format "
                      << zar::kFormatVersion.to_string() << ")\n";
            return 0;
        }
    }
    std::cerr << "zar: not implemented yet\n" << kUsage;
    return 2;
}
