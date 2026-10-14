// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// A fixed set of worker threads running queued jobs.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace zar {

class ThreadPool {
public:
    explicit ThreadPool(unsigned threads);
    ~ThreadPool();    // waits for queued jobs to finish

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(std::function<void()> job);

    // Blocks until every submitted job has finished. Rethrows the first
    // exception a job let escape, if any.
    void wait();

private:
    void work(std::stop_token stop);

    std::mutex mutex_;
    std::condition_variable_any work_ready_;
    std::condition_variable_any idle_;
    std::deque<std::function<void()>> jobs_;
    std::size_t running_ = 0;
    std::exception_ptr failure_;
    std::vector<std::jthread> workers_;
};

// Limits work in flight, both in items and in bytes. An item bigger than the
// byte limit doesn't count against it, but only one such item is admitted at
// a time: at most max_bytes plus the largest item are in flight.
class Budget {
public:
    Budget(std::size_t max_items, std::uint64_t max_bytes) : max_items_(max_items), max_bytes_(max_bytes) {}

    void acquire(std::uint64_t bytes);
    void release(std::uint64_t bytes);

private:
    std::mutex mutex_;
    std::condition_variable released_;
    const std::size_t max_items_;
    const std::uint64_t max_bytes_;
    std::size_t items_ = 0;
    std::uint64_t bytes_ = 0;
    bool oversized_ = false;
};

// One per core, or 1 if that can't be determined.
unsigned default_thread_count() noexcept;

} // namespace zar
