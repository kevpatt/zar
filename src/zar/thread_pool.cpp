// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "zar/thread_pool.hpp"

#include <algorithm>
#include <utility>

namespace zar {

unsigned default_thread_count() noexcept
{
    return std::max(1u, std::thread::hardware_concurrency());
}

ThreadPool::ThreadPool(unsigned threads)
{
    threads = std::max(threads, 1u);
    workers_.reserve(threads);
    try {
        for (unsigned i = 0; i < threads; ++i)
            workers_.emplace_back([this] { work(); });
    } catch (...) {
        stop();    // the workers already started
        throw;
    }
}

ThreadPool::~ThreadPool()
{
    {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return jobs_.empty() && running_ == 0; });
    }
    stop();
}

void ThreadPool::stop() noexcept
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    work_ready_.notify_all();
    for (auto& worker : workers_)
        worker.join();
}

void ThreadPool::submit(std::function<void()> job)
{
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    work_ready_.notify_one();
}

void ThreadPool::wait()
{
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return jobs_.empty() && running_ == 0; });
    if (failure_)
        std::rethrow_exception(std::exchange(failure_, nullptr));
}

void ThreadPool::work()
{
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            work_ready_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (jobs_.empty())
                return;    // stopping, with nothing left to do
            job = std::move(jobs_.front());
            jobs_.pop_front();
            ++running_;
        }

        std::exception_ptr failure;
        try {
            job();
        } catch (...) {
            failure = std::current_exception();
        }

        {
            std::lock_guard lock(mutex_);
            --running_;
            if (failure && !failure_)
                failure_ = failure;
        }
        idle_.notify_all();
    }
}

void Budget::acquire(std::uint64_t bytes)
{
    const bool oversized = bytes > max_bytes_;
    std::unique_lock lock(mutex_);
    released_.wait(lock, [&] {
        return items_ < max_items_ && (oversized ? !oversized_ : bytes <= max_bytes_ - bytes_);
    });
    ++items_;
    if (oversized)
        oversized_ = true;
    else
        bytes_ += bytes;
}

void Budget::release(std::uint64_t bytes)
{
    {
        std::lock_guard lock(mutex_);
        --items_;
        if (bytes > max_bytes_)
            oversized_ = false;
        else
            bytes_ -= bytes;
    }
    released_.notify_all();
}

} // namespace zar
