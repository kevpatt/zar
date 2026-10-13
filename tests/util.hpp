// Helpers shared by the end-to-end tests.
#pragma once

#include "zar/bytes.hpp"

#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

namespace zar::test {

namespace fs = std::filesystem;

class TempDir {
public:
    TempDir()
    {
        std::string pattern = (fs::temp_directory_path() / "zar-test-XXXXXX").string();
        if (!::mkdtemp(pattern.data()))
            throw std::runtime_error("mkdtemp failed");
        path_ = pattern;
    }
    ~TempDir() { fs::remove_all(path_); }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

inline void write_file(const fs::path& path, const Bytes& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
}

inline Bytes read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

inline Bytes text(std::string_view s) { return {s.begin(), s.end()}; }

inline Bytes random_bytes(std::size_t n, unsigned seed)
{
    std::mt19937 rng(seed);
    Bytes out(n);
    for (auto& b : out)
        b = static_cast<std::uint8_t>(rng());
    return out;
}

} // namespace zar::test
