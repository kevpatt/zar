// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// Fuzzes everything unzar does with an archive except writing files: index
// decoding, opening (from a file and from a pipe), scanning, and testing.
//
// Built with -DZAR_BUILD_FUZZERS=ON. With a compiler that has libFuzzer
// this is an ordinary libFuzzer target; otherwise a small driver below
// mutates seed archives itself:
//
//   zar_fuzz [-runs=N] [-seed=S] [-artifact=PATH] [FILE...]
//
// runs each FILE, then N mutations of them (or of archives it makes, if no
// FILE is given). With -artifact, each input is saved to PATH before it
// runs, so after a crash PATH reproduces it. Fuzzing builds skip CRC checks,
// so never install one.
#include "zar/archive_reader.hpp"
#include "zar/extractor.hpp"
#include "zar/index.hpp"
#include "zar/posix.hpp"

#include <cstdint>
#include <cstdio>
#include <span>
#include <thread>
#include <unistd.h>

namespace {

constexpr std::uint64_t kSizeLimit = 64 << 20;

void check(zar::ArchiveReader& reader, zar::ExtractMode mode)
{
    zar::ExtractOptions options;
    options.mode = mode;
    options.threads = 2;
    options.size_limit = kSizeLimit;
    const auto groups = zar::group_by_directory(reader.entries());
    zar::Extractor extractor(reader, options);
    extractor.prepare(groups);
    for (const auto& group : groups) {
        extractor.enter(group);
        for (const auto* member : group.members)
            extractor.extract(*member);
    }
    extractor.finish();
}

void from_file(std::span<const std::uint8_t> data)
{
    const auto dir = zar::temp_directory();
    std::string path = (dir / "zar-fuzz-XXXXXX").string();
    const zar::UniqueFd fd(::mkstemp(path.data()));
    if (!fd)
        return;
    zar::write_all(fd.get(), data, path);
    try {
        auto reader = zar::ArchiveReader::open(path, {}, kSizeLimit);
        check(reader, zar::ExtractMode::Scan);
        check(reader, zar::ExtractMode::Test);
    } catch (const std::exception&) {
    }
    ::unlink(path.c_str());
}

void from_pipe(std::span<const std::uint8_t> data)
{
    int fds[2];
    if (::pipe(fds) != 0)
        return;
    std::jthread feeder([data, fd = fds[1]] {
        try {
            zar::write_all(fd, data, "pipe");
        } catch (const std::exception&) {
            // the reader stopped early
        }
        ::close(fd);
    });
    try {
        auto reader = zar::ArchiveReader::open_stream(fds[0], "pipe", {}, kSizeLimit);
        check(reader, zar::ExtractMode::Test);
    } catch (const std::exception&) {
    }
    ::close(fds[0]);    // unblocks the feeder if the reader gave up
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::span bytes(data, size);
    try {
        zar::decode_index(bytes);
    } catch (const std::exception&) {
    }
    from_file(bytes);
    from_pipe(bytes);
    return 0;
}

#ifdef ZAR_FUZZ_STANDALONE

#include "zar/creator.hpp"

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using zar::Bytes;

Bytes read(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void write(const fs::path& path, const Bytes& bytes)
{
    if (path.has_parent_path())
        fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// A few small archives in different shapes, to mutate.
std::vector<Bytes> make_seeds()
{
    const auto dir = zar::temp_directory() / ("zar-fuzz-seeds-" + std::to_string(::getpid()));
    std::mt19937 rng(1);
    Bytes noise(5000);
    for (auto& b : noise)
        b = static_cast<std::uint8_t>(rng());
    const std::string text = "the quick brown fox jumps over the lazy dog\n";
    Bytes words;
    for (int i = 0; i < 200; ++i)
        words.insert(words.end(), text.begin(), text.end());
    write(dir / "src/a.txt", words);
    write(dir / "src/sub/b.bin", noise);
    write(dir / "src/sub/c.txt", words);
    write(dir / "src/empty", {});
    fs::create_symlink("a.txt", dir / "src/link");
    fs::create_hard_link(dir / "src/a.txt", dir / "src/sub/hard");

    std::vector<Bytes> seeds;
    const auto add = [&](zar::CreateOptions options) {
        const auto path = dir / "seed.zar";
        auto out = zar::Output::create_file(path, true);
        options.chunk_size = 4096;
        zar::create_archive(out, std::vector{dir / "src"}, options);
        out.commit();
        seeds.push_back(read(path));
    };
    add({});
    zar::CreateOptions head;
    head.head_index = true;
    add(head);
    zar::CreateOptions dict = head;
    dict.dict_size = 1024;
    add(dict);
    zar::CreateOptions solid;
    solid.solid_limit = 1 << 20;
    solid.dict_over_solid = 0;
    add(solid);
    fs::remove_all(dir);
    return seeds;
}

void mutate(Bytes& bytes, std::mt19937_64& rng)
{
    const auto pick = [&](std::size_t n) { return n ? static_cast<std::size_t>(rng() % n) : 0; };
    static constexpr std::uint64_t kInteresting[] = {
        0, 1, 7, 8, 31, 32, 33, 64, 4096, 0x7fffffff, 0x80000000, 0xffffffff,
        std::uint64_t{1} << 32, std::uint64_t{1} << 40, 0x7fffffffffffffff, 0x8000000000000000,
        ~std::uint64_t{0}, ~std::uint64_t{0} - 31,
    };
    for (int edits = 1 + static_cast<int>(pick(4)); edits > 0; --edits) {
        if (bytes.empty())
            bytes.push_back(0);
        switch (pick(6)) {
        case 0: bytes[pick(bytes.size())] ^= static_cast<std::uint8_t>(1u << pick(8)); break;
        case 1: bytes[pick(bytes.size())] = static_cast<std::uint8_t>(rng()); break;
        case 2: {    // an interesting little-endian integer, 1 to 8 bytes wide
            const auto value = kInteresting[pick(std::size(kInteresting))];
            const auto width = std::size_t{1} << pick(4);
            const auto at = pick(bytes.size());
            for (std::size_t i = 0; i < width && at + i < bytes.size(); ++i)
                bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
            break;
        }
        case 3: bytes.resize(pick(bytes.size() + 1)); break;
        case 4: {    // repeat a slice
            const auto from = pick(bytes.size());
            const auto length = std::min<std::size_t>(pick(64) + 1, bytes.size() - from);
            const Bytes slice(bytes.begin() + static_cast<std::ptrdiff_t>(from),
                              bytes.begin() + static_cast<std::ptrdiff_t>(from + length));
            bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(pick(bytes.size())), slice.begin(), slice.end());
            break;
        }
        default: {    // delete a slice
            const auto from = pick(bytes.size());
            const auto length = std::min<std::size_t>(pick(64) + 1, bytes.size() - from);
            bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(from),
                        bytes.begin() + static_cast<std::ptrdiff_t>(from + length));
            break;
        }
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::signal(SIGPIPE, SIG_IGN);
    std::uint64_t runs = 0, seed = 1;
    std::string artifact;
    std::vector<Bytes> corpus;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.starts_with("-runs="))
            runs = std::stoull(arg.substr(6));
        else if (arg.starts_with("-seed="))
            seed = std::stoull(arg.substr(6));
        else if (arg.starts_with("-artifact="))
            artifact = arg.substr(10);
        else
            corpus.push_back(read(arg));
    }
    if (corpus.empty())
        corpus = make_seeds();

    for (const auto& input : corpus)
        LLVMFuzzerTestOneInput(input.data(), input.size());

    std::mt19937_64 rng(seed);
    for (std::uint64_t run = 1; run <= runs; ++run) {
        auto input = corpus[rng() % corpus.size()];
        mutate(input, rng);
        if (!artifact.empty())
            write(artifact, input);
        LLVMFuzzerTestOneInput(input.data(), input.size());
        if (run % 1000 == 0)
            std::cerr << "zar_fuzz: " << run << " runs\n";
    }
    std::cerr << "zar_fuzz: done, " << corpus.size() << " inputs and " << runs << " mutations\n";
    return 0;
}

#endif
