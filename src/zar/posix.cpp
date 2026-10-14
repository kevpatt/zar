// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "zar/posix.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <grp.h>
#include <pwd.h>
#include <sys/statvfs.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace zar {
namespace {

// Calls a getpw*_r or getgr*_r function `get`, growing the buffer while it
// reports ERANGE (large groups need far more than any fixed size), and
// returns what `extract` takes from the record.
template <typename Record, typename Get, typename Extract>
auto lookup(std::size_t buffer_size, Get get, Extract extract)
    -> std::optional<decltype(extract(std::declval<const Record&>()))>
{
    constexpr std::size_t kMaxBuffer = std::size_t{1} << 24;
    std::vector<char> buffer(std::max<std::size_t>(buffer_size, 1));
    for (;;) {
        Record record{};
        Record* result = nullptr;
        const int rc = get(&record, buffer.data(), buffer.size(), &result);
        if (rc == ERANGE && buffer.size() < kMaxBuffer) {
            buffer.resize(buffer.size() * 2);
            continue;
        }
        if (rc != 0 || !result)
            return std::nullopt;
        return extract(*result);
    }
}

} // namespace

void UniqueFd::reset() noexcept
{
    if (fd_ >= 0)
        ::close(std::exchange(fd_, -1));
}

void throw_errno(const std::string& what)
{
    throw std::system_error(errno, std::generic_category(), what);
}

std::size_t read_full(int fd, std::span<std::uint8_t> out, const std::string& what)
{
    std::size_t done = 0;
    while (done < out.size()) {
        const auto n = ::read(fd, out.data() + done, out.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            throw_errno(what);
        if (n == 0)
            break;
        done += static_cast<std::size_t>(n);
    }
    return done;
}

std::size_t pread_full(int fd, std::span<std::uint8_t> out, std::uint64_t position, const std::string& what)
{
    std::size_t done = 0;
    while (done < out.size()) {
        const auto n = ::pread(fd, out.data() + done, out.size() - done, static_cast<off_t>(position + done));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            throw_errno(what);
        if (n == 0)
            break;
        done += static_cast<std::size_t>(n);
    }
    return done;
}

void write_all(int fd, std::span<const std::uint8_t> data, const std::string& what)
{
    while (!data.empty()) {
        const auto n = ::write(fd, data.data(), data.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            throw_errno(what);
        data = data.subspan(static_cast<std::size_t>(n));
    }
}

void pwrite_all(int fd, std::span<const std::uint8_t> data, std::uint64_t position, const std::string& what)
{
    while (!data.empty()) {
        const auto n = ::pwrite(fd, data.data(), data.size(), static_cast<off_t>(position));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            throw_errno(what);
        data = data.subspan(static_cast<std::size_t>(n));
        position += static_cast<std::uint64_t>(n);
    }
}

std::optional<std::uint64_t> free_space(const std::filesystem::path& path)
{
    struct statvfs fs {};
    if (::statvfs(path.c_str(), &fs) != 0)
        return std::nullopt;
    return static_cast<std::uint64_t>(fs.f_bavail) * fs.f_frsize;
}

std::filesystem::path temp_directory()
{
    const char* dir = std::getenv("TMPDIR");
    return dir && *dir ? dir : "/tmp";
}

UniqueFd anonymous_temp_file(const std::filesystem::path& dir, const std::string& prefix)
{
    std::string path = (dir / (prefix + "XXXXXX")).string();
    UniqueFd fd(::mkstemp(path.data()));
    if (!fd)
        throw_errno(path);
    ::unlink(path.c_str());
    return fd;
}

std::optional<std::string> user_name(uid_t uid, std::size_t buffer_size)
{
    return lookup<passwd>(
        buffer_size, [&](auto... args) { return ::getpwuid_r(uid, args...); },
        [](const passwd& pw) { return std::string(pw.pw_name); });
}

std::optional<std::string> group_name(gid_t gid, std::size_t buffer_size)
{
    return lookup<group>(
        buffer_size, [&](auto... args) { return ::getgrgid_r(gid, args...); },
        [](const group& gr) { return std::string(gr.gr_name); });
}

std::optional<uid_t> user_id(const std::string& name, std::size_t buffer_size)
{
    return lookup<passwd>(
        buffer_size, [&](auto... args) { return ::getpwnam_r(name.c_str(), args...); },
        [](const passwd& pw) { return pw.pw_uid; });
}

std::optional<gid_t> group_id(const std::string& name, std::size_t buffer_size)
{
    return lookup<group>(
        buffer_size, [&](auto... args) { return ::getgrnam_r(name.c_str(), args...); },
        [](const group& gr) { return gr.gr_gid; });
}

} // namespace zar
