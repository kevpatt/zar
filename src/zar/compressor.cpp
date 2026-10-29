// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "zar/compressor.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <zstd.h>

namespace zar {
namespace {

struct CCtxDeleter {
    void operator()(ZSTD_CCtx* cctx) const noexcept { ZSTD_freeCCtx(cctx); }
};
using CCtxPtr = std::unique_ptr<ZSTD_CCtx, CCtxDeleter>;

void check(std::size_t result, const char* what)
{
    if (ZSTD_isError(result))
        throw std::runtime_error(std::string(what) + ": " + ZSTD_getErrorName(result));
}

CCtxPtr make_context(int level)
{
    CCtxPtr cctx(ZSTD_createCCtx());
    if (!cctx)
        throw std::bad_alloc();
    check(ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_compressionLevel, level), "zstd level");
    // §4.2: content size in every frame header; payload_crc replaces the checksum.
    check(ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_contentSizeFlag, 1), "zstd parameter");
    check(ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_checksumFlag, 0), "zstd parameter");
    // An entry's DICT field says which dictionary its frames need (§6.5).
    check(ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_dictIDFlag, 0), "zstd parameter");
    return cctx;
}

Bytes compress_with(ZSTD_CCtx* cctx, ByteView data)
{
    Bytes out(ZSTD_compressBound(data.size()));
    const auto size = ZSTD_compress2(cctx, out.data(), out.size(), data.data(), data.size());
    check(size, "zstd compression failed");
    out.resize(size);
    return out;
}

} // namespace

Bytes zstd_compress(ByteView data, int level)
{
    return compress_with(make_context(level).get(), data);
}

std::optional<Bytes> zstd_decompress(ByteView frame, std::size_t size, ByteView dict)
{
    if (ZSTD_getFrameContentSize(frame.data(), frame.size()) != size)
        return std::nullopt;
    const std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> dctx(ZSTD_createDCtx(), ZSTD_freeDCtx);
    if (!dctx)
        throw std::bad_alloc();
    Bytes out(size);
    const auto n = ZSTD_decompress_usingDict(dctx.get(), out.data(), out.size(), frame.data(),
                                             frame.size(), dict.data(), dict.size());
    if (ZSTD_isError(n) || n != size)
        return std::nullopt;
    return out;
}

Compressor::Compressor(int level, unsigned threads) : level_(level)
{
    make_context(level);    // validate the level up front, on this thread
    workers_.reserve(threads);
    try {
        for (unsigned i = 0; i < std::max(threads, 1u); ++i)
            workers_.emplace_back([this] { work(); });
    } catch (...) {
        stop();    // the workers already started
        throw;
    }
}

Compressor::~Compressor()
{
    stop();
}

void Compressor::stop() noexcept
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    for (auto& worker : workers_)
        worker.join();
}

void Compressor::use_dictionary(ByteView dict)
{
    ZSTD_CDict* cdict = ZSTD_createCDict(dict.data(), dict.size(), level_);
    if (!cdict)
        throw std::runtime_error("zstd: invalid dictionary");
    dict_.reset(cdict, ZSTD_freeCDict);
}

std::future<CompressedChunk> Compressor::submit(Bytes data, bool compress)
{
    std::promise<CompressedChunk> promise;
    auto future = promise.get_future();
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back({std::move(data), compress, dict_, std::move(promise)});
    }
    ready_.notify_one();
    return future;
}

void Compressor::work()
{
    const auto cctx = make_context(level_);

    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (jobs_.empty())
                return;    // stopping, with nothing left to do
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }

        try {
            CompressedChunk chunk{BlockType::DataRaw, {}};
            if (job.compress) {
                check(ZSTD_CCtx_refCDict(cctx.get(), job.dict.get()), "zstd dictionary");
                auto packed = compress_with(cctx.get(), job.data);
                if (packed.size() < job.data.size())
                    chunk = {BlockType::DataZstd, std::move(packed)};
            }
            if (chunk.type == BlockType::DataRaw)
                chunk.payload = std::move(job.data);
            job.result.set_value(std::move(chunk));
        } catch (...) {
            job.result.set_exception(std::current_exception());
        }
    }
}

} // namespace zar
