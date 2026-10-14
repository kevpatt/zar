// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// Small wrappers over POSIX calls shared by the reader, writer, and extractor.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <sys/types.h>
#include <utility>

namespace zar {

// Owns a file descriptor, closing it on destruction.
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        std::swap(fd_, other.fd_);
        return *this;
    }
    ~UniqueFd() { reset(); }

    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }
    int release() noexcept { return std::exchange(fd_, -1); }
    void reset() noexcept;

private:
    int fd_ = -1;
};

// Throws std::system_error for the current errno: "what: <description>".
[[noreturn]] void throw_errno(const std::string& what);

// Reads until `out` is full or the file ends, retrying interrupted calls.
// Returns the number of bytes read; throws std::system_error on failure.
std::size_t read_full(int fd, std::span<std::uint8_t> out, const std::string& what);
std::size_t pread_full(int fd, std::span<std::uint8_t> out, std::uint64_t position, const std::string& what);

// Writes all of `data`, retrying short and interrupted writes.
void write_all(int fd, std::span<const std::uint8_t> data, const std::string& what);
void pwrite_all(int fd, std::span<const std::uint8_t> data, std::uint64_t position, const std::string& what);

// Bytes available to unprivileged users on the filesystem holding `path`.
std::optional<std::uint64_t> free_space(const std::filesystem::path& path);

// $TMPDIR, or /tmp.
std::filesystem::path temp_directory();

// Creates a file in `dir` and unlinks it at once, so it disappears when
// closed. `prefix` starts its (briefly visible) name.
UniqueFd anonymous_temp_file(const std::filesystem::path& dir, const std::string& prefix);

// User and group database lookups. `buffer_size` is the initial buffer, grown
// as needed; tests set it small to exercise that.
std::optional<std::string> user_name(uid_t uid, std::size_t buffer_size = 1024);
std::optional<std::string> group_name(gid_t gid, std::size_t buffer_size = 1024);
std::optional<uid_t> user_id(const std::string& name, std::size_t buffer_size = 1024);
std::optional<gid_t> group_id(const std::string& name, std::size_t buffer_size = 1024);

} // namespace zar
