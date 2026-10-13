// Parallel chunk compression with results delivered in submission order.
#pragma once

#include "zar/bytes.hpp"
#include "zar/format.hpp"

#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

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

    // With `compress` false the chunk is stored raw without trying zstd.
    std::future<CompressedChunk> submit(Bytes data, bool compress);

private:
    struct Job {
        Bytes data;
        bool compress;
        std::promise<CompressedChunk> result;
    };

    void work(std::stop_token stop);

    int level_;
    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<Job> jobs_;
    std::vector<std::jthread> workers_;
};

// One-shot compression on the calling thread, for index blocks.
Bytes zstd_compress(ByteView data, int level);

} // namespace zar
