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
#include <type_traits>
#include <utility>
#include <vector>

namespace zar {

// A thread that is joined when it's destroyed, like std::jthread, which
// some standard libraries still lack (that of the macOS 15 SDK, say).
class JoiningThread {
public:
    template <typename F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, JoiningThread>)
    explicit JoiningThread(F&& f) : thread_(std::forward<F>(f))
    {
    }
    JoiningThread(JoiningThread&&) noexcept = default;
    JoiningThread& operator=(JoiningThread&& other)    // joins this one first
    {
        join();
        thread_ = std::move(other.thread_);
        return *this;
    }
    ~JoiningThread() { join(); }

    void join()
    {
        if (thread_.joinable())
            thread_.join();
    }

private:
    std::thread thread_;
};

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
    void work();
    void stop() noexcept;    // lets the workers finish the queue, and joins them

    std::mutex mutex_;
    std::condition_variable work_ready_;
    std::condition_variable idle_;
    std::deque<std::function<void()>> jobs_;
    std::size_t running_ = 0;
    bool stopping_ = false;
    std::exception_ptr failure_;
    std::vector<JoiningThread> workers_;
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
