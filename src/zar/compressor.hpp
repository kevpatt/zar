// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// Parallel chunk compression with results delivered in submission order.
#pragma once

#include "zar/bytes.hpp"
#include "zar/format.hpp"
#include "zar/hash.hpp"
#include "zar/thread_pool.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

struct ZSTD_CDict_s;

namespace zar {

struct CompressedChunk {
    BlockType type;    // DataZstd, or DataRaw if zstd didn't help
    Bytes payload;
    std::optional<Digest> hash;    // its block hash, if hash_blocks()
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

    // Also computes each block's hash (§6.7), on the worker threads.
    void hash_blocks() noexcept { hash_ = true; }

    // With `compress` false the chunk is stored raw without trying zstd.
    std::future<CompressedChunk> submit(Bytes data, bool compress);

private:
    struct Job {
        Bytes data;
        bool compress;
        std::shared_ptr<ZSTD_CDict_s> dict;
        std::promise<CompressedChunk> result;
    };

    void work();
    void stop() noexcept;    // lets the workers finish the queue, and joins them

    int level_;
    std::atomic<bool> hash_ = false;
    std::shared_ptr<ZSTD_CDict_s> dict_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Job> jobs_;
    bool stopping_ = false;
    std::vector<JoiningThread> workers_;
};

// One-shot compression on the calling thread, for index blocks.
Bytes zstd_compress(ByteView data, int level);

// One-shot decompression of a frame that should hold exactly `size` bytes,
// for reading back blocks. Returns nullopt if it doesn't.
std::optional<Bytes> zstd_decompress(ByteView frame, std::size_t size, ByteView dict = {});

} // namespace zar
