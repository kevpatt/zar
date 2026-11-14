// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// --progress: one line on stderr, redrawn twice a second, showing how much
// of the data is done, the rate, the time left, and when compressing, the
// compression ratio so far. Only on a terminal. On success, finish() leaves
// a last line with the totals.
#pragma once

#include "cli.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>

namespace zar::cli {

class Progress {
public:
    // Whether a progress line can be shown: stderr is a terminal.
    static bool available() { return ::isatty(STDERR_FILENO) == 1; }

    Progress() : thread_([this] { tick(); }) { active_ = this; }

    // The run succeeded: stop, and leave a last line showing everything
    // done, the final ratio, the average rate and the time it all took.
    void finish()
    {
        {
            std::lock_guard lock(mutex_);
            const auto now = Clock::now();
            const auto done = done_.load(std::memory_order_relaxed);
            std::string line = head(done);
            const auto seconds = std::chrono::duration<double>(now - stage_start_).count();
            if (seconds > 0)
                line += "  " + human_size(static_cast<std::uint64_t>(static_cast<double>(done) / seconds)) + "/s";
            line += "  in " + clock(std::chrono::duration<double>(now - start_).count());
            std::fputs(("\r" + line + "\033[K\n").c_str(), stderr);
            std::fflush(stderr);
            shown_ = false;
            stopping_ = true;
        }
        wake_.notify_all();
    }

    ~Progress()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            erase();
        }
        wake_.notify_all();
        thread_.join();
        active_ = nullptr;
    }

    Progress(const Progress&) = delete;
    Progress& operator=(const Progress&) = delete;

    // Begins counting again from zero: a new pass over `total` bytes (0 if
    // unknown), described by `label` ("" for none).
    void stage(std::string_view label, std::uint64_t total)
    {
        std::lock_guard lock(mutex_);
        label_ = label;
        total_ = total;
        done_ = 0;
        stage_start_ = Clock::now();
        data_ = 0;
        stored_ = 0;
        samples_.clear();
        draw();
    }

    // From any thread.
    void add(std::uint64_t bytes) noexcept { done_.fetch_add(bytes, std::memory_order_relaxed); }

    // The data written so far in this stage, and the bytes it took: shown
    // as a cumulative compression ratio.
    void written(std::uint64_t data, std::uint64_t stored)
    {
        std::lock_guard lock(mutex_);
        data_ = data;
        stored_ = stored;
    }

    // 10.95:1 when smaller, 1:1.10 when larger (two decimal places).
    static std::string ratio(std::uint64_t data, std::uint64_t stored)
    {
        const auto fixed = [](std::uint64_t over, std::uint64_t under) {
            const auto hundredths = (over * 100 + under / 2) / under;    // rounded
            const auto cents = hundredths % 100;
            return std::to_string(hundredths / 100) + '.' + (cents < 10 ? "0" : "") + std::to_string(cents);
        };
        return data >= stored ? fixed(data, stored) + ":1" : "1:" + fixed(stored, data);
    }

    // Runs `print`, which writes other output to the terminal, with the
    // progress line out of its way: through the active Progress, if any.
    template <typename Print>
    static void around(Print&& print)
    {
        if (Progress* progress = active_) {
            std::lock_guard lock(progress->mutex_);
            progress->erase();
            std::forward<Print>(print)();
            std::cout.flush();
            progress->draw();
        } else {
            std::forward<Print>(print)();
        }
    }

private:
    using Clock = std::chrono::steady_clock;

    void tick()
    {
        std::unique_lock lock(mutex_);
        while (!wake_.wait_for(lock, std::chrono::milliseconds(500), [this] { return stopping_; }))
            draw();
    }

    // With mutex_ held.
    void erase()
    {
        if (shown_) {
            std::fputs("\r\033[K", stderr);
            std::fflush(stderr);
            shown_ = false;
        }
    }

    // With mutex_ held. The rate is over the last few seconds, so it follows
    // changes (a slow region of a disk, say) without jumping about.
    void draw()
    {
        if (stopping_)
            return;
        const auto now = Clock::now();
        const auto done = done_.load(std::memory_order_relaxed);
        samples_.emplace_back(now, done);
        while (samples_.size() > 2 && now - samples_.front().first > std::chrono::seconds(5))
            samples_.pop_front();

        std::string line = head(done);
        const auto elapsed = std::chrono::duration<double>(now - samples_.front().first).count();
        if (elapsed >= 1) {
            const auto rate = static_cast<double>(done - samples_.front().second) / elapsed;
            line += "  " + human_size(static_cast<std::uint64_t>(rate)) + "/s";
            if (total_ && rate > 0 && done < total_)
                line += "  " + clock(static_cast<double>(total_ - done) / rate) + " left";
        }
        std::fputs(("\r" + line + "\033[K").c_str(), stderr);
        std::fflush(stderr);
        shown_ = true;
    }

    // With mutex_ held: the label, how much is done, and the ratio.
    std::string head(std::uint64_t done) const
    {
        std::string line;
        if (!label_.empty())
            line += label_ + ": ";
        line += human_size(done);
        if (total_)
            line += " / " + human_size(total_) + "  " + percent(done);
        if (data_ && stored_)
            line += "  " + ratio(data_, stored_);
        return line;
    }

    std::string percent(std::uint64_t done) const
    {
        const auto tenths = std::min<std::uint64_t>(done, total_) * 1000 / total_;
        return std::to_string(tenths / 10) + '.' + std::to_string(tenths % 10) + '%';
    }

    // 0:42, 12:05, 3:07:45.
    static std::string clock(double seconds)
    {
        const auto s = static_cast<std::uint64_t>(seconds + 0.5);
        const auto two = [](std::uint64_t n) { return (n < 10 ? "0" : "") + std::to_string(n); };
        if (s >= 3600)
            return std::to_string(s / 3600) + ':' + two(s / 60 % 60) + ':' + two(s % 60);
        return std::to_string(s / 60) + ':' + two(s % 60);
    }

    inline static std::atomic<Progress*> active_ = nullptr;

    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    bool shown_ = false;
    const Clock::time_point start_ = Clock::now();
    Clock::time_point stage_start_ = start_;
    std::string label_;
    std::uint64_t total_ = 0;
    std::atomic<std::uint64_t> done_ = 0;
    std::uint64_t data_ = 0;      // with mutex_
    std::uint64_t stored_ = 0;
    std::deque<std::pair<Clock::time_point, std::uint64_t>> samples_;
    std::thread thread_;    // last: started once everything else is ready
};

} // namespace zar::cli
