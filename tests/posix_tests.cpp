// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "check.hpp"
#include "util.hpp"

#include "zar/output.hpp"
#include "zar/posix.hpp"
#include "zar/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

using namespace zar;
using namespace zar::test;

TEST(user_and_group_lookups_grow_their_buffers)
{
    // A one-byte buffer can't hold any record, so these only succeed by
    // retrying with a larger one.
    const auto user = user_name(::getuid());
    CHECK(user.has_value());
    CHECK(user_name(::getuid(), 1) == user);
    CHECK(group_name(::getgid(), 1) == group_name(::getgid()));
    if (user)
        CHECK(user_id(*user, 1) == ::getuid());
    CHECK(!user_id("no-such-user-zar-test", 1));
}

TEST(budget_limits_items_and_bytes)
{
    Budget budget(4, 100);
    std::atomic<bool> admitted = false;
    const auto wait_for = [&](std::uint64_t bytes) {
        return JoiningThread([&, bytes] {
            budget.acquire(bytes);
            admitted = true;
            budget.release(bytes);
        });
    };

    // Over the byte limit.
    budget.acquire(60);
    auto waiter = wait_for(60);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!admitted);
    budget.release(60);
    waiter.join();
    CHECK(admitted);

    // An item bigger than the limit runs beside ordinary ones, but alone
    // among big ones.
    budget.acquire(1000);
    budget.acquire(100);
    admitted = false;
    waiter = wait_for(2000);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!admitted);
    budget.release(1000);
    waiter.join();
    CHECK(admitted);
    budget.release(100);

    for (int i = 0; i < 4; ++i)
        budget.acquire(1);
    admitted = false;
    JoiningThread fifth([&] {
        budget.acquire(1);
        admitted = true;
        budget.release(1);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!admitted);    // over the item limit
    budget.release(1);
    fifth.join();
    CHECK(admitted);
    for (int i = 0; i < 3; ++i)
        budget.release(1);
}

// The temporary files Output left in `dir`: "*.zar.XXXXXX".
std::vector<Bytes> kept_archives(const fs::path& dir)
{
    std::vector<Bytes> kept;
    for (const auto& entry : fs::directory_iterator(dir))
        if (entry.path().filename().string().find(".zar.") != std::string::npos)
            kept.push_back(read_file(entry.path()));
    return kept;
}

TEST(output_never_replaces_a_file_that_appeared_without_overwrite)
{
    TempDir tmp;
    const auto path = tmp.path() / "a.zar";
    auto out = Output::create_file(path, false);
    out.write(text("new"));
    write_file(path, text("someone else's"));    // appears while the archive is written
    CHECK_THROWS_WITH(out.commit(), "already exists");
    CHECK(read_file(path) == text("someone else's"));
    CHECK(kept_archives(tmp.path()) == std::vector<Bytes>{text("new")});    // finished, so kept
    for (const auto& kept : fs::directory_iterator(tmp.path()))
        if (kept.path() != path)
            fs::remove(kept.path());

    auto replacing = Output::create_file(path, true);
    replacing.write(text("new"));
    replacing.commit();
    CHECK(read_file(path) == text("new"));
}

TEST(only_disks_have_a_device_size)
{
    TempDir tmp;
    write_file(tmp.path() / "f", text("not a disk"));
    for (const auto& path : {tmp.path() / "f", fs::path("/dev/null"), fs::path("/dev/zero")}) {
        const UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
        CHECK(fd && !device_size(fd.get()));
    }
}

// A finished archive that can't be renamed into place is kept, and the
// error says where: it may have taken hours to make. An unfinished one is
// removed.
TEST(output_keeps_a_finished_archive_it_cannot_rename)
{
    TempDir tmp;
    const auto path = tmp.path() / "a.zar";
    fs::create_directories(path / "in-the-way");    // rename() can't replace a directory
    {
        auto out = Output::create_file(path, true);
        out.write(text("finished"));
        CHECK_THROWS_WITH(out.commit(), "the finished archive was kept as " + path.string() + ".");
    }
    CHECK(kept_archives(tmp.path()) == std::vector<Bytes>{text("finished")});

    for (const auto& kept : fs::directory_iterator(tmp.path()))
        if (kept.path() != path)
            fs::remove(kept.path());
    {
        auto out = Output::create_file(tmp.path() / "b.zar", false);
        out.write(text("abandoned"));
    }
    CHECK(kept_archives(tmp.path()).empty());
}
