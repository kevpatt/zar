// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// Buffered, error-checked archive output.
#pragma once

#include "zar/bytes.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <sys/types.h>

namespace zar {

// A compressing output (Output::compressing) went over its size limit.
struct SizeLimitExceeded : std::runtime_error {
    SizeLimitExceeded() : std::runtime_error("compressed output exceeds its size limit") {}
};

class Output {
public:
    // Writes to a temporary file beside `path`; commit() renames it into place.
    // Without overwrite, fails if `path` already exists, now or at commit().
    static Output create_file(const std::filesystem::path& path, bool overwrite);
    static Output standard_output();

    // An anonymous temporary file for staging data, beside this output's file
    // (so it's on the same filesystem) or, for stdout, in $TMPDIR.
    Output make_spool() const;

    // An output that compresses everything written to it, as a single zstd
    // frame, into `dest`. Throws SizeLimitExceeded as soon as the compressed
    // data passes `limit` bytes. It takes `identity`'s is_file(), so an
    // archive written through it still skips its own file.
    static Output compressing(Output& dest, int level, unsigned threads, std::uint64_t limit,
                              const Output& identity);

    // For a compressing output, after commit(): the CRC-32C of the compressed frame.
    std::uint32_t compressed_crc() const noexcept;

    Output(Output&& other) noexcept;
    Output& operator=(Output&&) = delete;
    ~Output();    // discards an uncommitted temporary file

    void write(ByteView bytes);
    void write_zeros(std::size_t count);
    std::uint64_t position() const noexcept { return position_; }

    bool seekable() const noexcept { return seekable_; }
    // Overwrites already-written bytes; requires seekable().
    void write_at(std::uint64_t position, ByteView bytes);

    // Whether read_at() works: a file opened for reading and writing, such as
    // an archive's temporary file or a spool, but not a pipe.
    bool readable() const noexcept { return readable_; }
    // Reads back already-written bytes; requires readable().
    void read_at(std::uint64_t position, std::span<std::uint8_t> out);

    // Flushes, syncs, and (for files) atomically moves the result into place.
    // A compressing output finishes its frame.
    void commit();

    // Appends everything written to this output so far to `dest`.
    void copy_to(Output& dest);

    // Whether a file (by device and inode) is the one being written, or the
    // one it will replace, so an archive never contains itself.
    bool is_file(dev_t device, ino_t inode) const noexcept;

private:
    struct Zstd;

    struct FileId {
        dev_t device;
        ino_t inode;
        bool operator==(const FileId&) const = default;
    };

    Output(int fd, bool owns_fd, std::filesystem::path temp, std::filesystem::path final_path);
    void flush();
    void emit(ByteView bytes);    // to the file, or through zstd

    int fd_ = -1;
    bool owns_fd_ = false;
    std::filesystem::path temp_path_;
    std::filesystem::path final_path_;
    Bytes buffer_;
    std::uint64_t position_ = 0;
    bool seekable_ = false;
    bool readable_ = false;
    bool replace_ = true;    // whether commit() may replace an existing file
    std::optional<FileId> self_;
    std::optional<FileId> replaced_;
    std::unique_ptr<Zstd> zstd_;
};

} // namespace zar
