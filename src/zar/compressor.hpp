// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// Parallel chunk compression with results delivered in submission order.
#pragma once

#include "zar/bytes.hpp"
#include "zar/format.hpp"

#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

struct ZSTD_CDict_s;

namespace zar {

struct CompressedChunk {
    BlockType type;    // DataZstd, or DataRaw if zstd didn't help
    Bytes payload;
};

class Compressor {
public:
    Compressor(int level, unsigned threads);
    ~Compressor();

    Compressor(const Compressor&) = delete;
    Compressor& operator=(const Compressor&) = delete;

    // Compresses every chunk submitted from now on with this dictionary
    // (its frames omit the dictionary ID; the index records the dictionary).
    void use_dictionary(ByteView dict);

    // With `compress` false the chunk is stored raw without trying zstd.
    std::future<CompressedChunk> submit(Bytes data, bool compress);

private:
    struct Job {
        Bytes data;
        bool compress;
        std::shared_ptr<ZSTD_CDict_s> dict;
        std::promise<CompressedChunk> result;
    };

    void work(std::stop_token stop);

    int level_;
    std::shared_ptr<ZSTD_CDict_s> dict_;
    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<Job> jobs_;
    std::vector<std::jthread> workers_;
};

// One-shot compression on the calling thread, for index blocks.
Bytes zstd_compress(ByteView data, int level);

// One-shot decompression of a frame that should hold exactly `size` bytes,
// for reading back blocks. Returns nullopt if it doesn't.
std::optional<Bytes> zstd_decompress(ByteView frame, std::size_t size, ByteView dict = {});

} // namespace zar
