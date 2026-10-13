// A fixed set of worker threads running queued jobs.
#pragma once

#include <condition_variable>
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

// One per core, or 1 if that can't be determined.
unsigned default_thread_count() noexcept;

} // namespace zar
