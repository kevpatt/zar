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
    for (unsigned i = 0; i < threads; ++i)
        workers_.emplace_back([this](std::stop_token stop) { work(stop); });
}

ThreadPool::~ThreadPool()
{
    {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return jobs_.empty() && running_ == 0; });
    }
    for (auto& worker : workers_)
        worker.request_stop();
    work_ready_.notify_all();
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

void ThreadPool::work(std::stop_token stop)
{
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            if (!work_ready_.wait(lock, stop, [this] { return !jobs_.empty(); }))
                return;
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

} // namespace zar
