// Buffered, error-checked archive output.
#pragma once

#include "zar/bytes.hpp"

#include <filesystem>
#include <memory>
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
    // Without overwrite, fails if `path` already exists.
    static Output create_file(const std::filesystem::path& path, bool overwrite);
    static Output standard_output();

    // An anonymous temporary file for staging data, beside this output's file
    // (so it's on the same filesystem) or, for stdout, in $TMPDIR.
    Output make_spool() const;

    // An output that compresses everything written to it, as a single zstd
    // frame, into `dest`. Throws SizeLimitExceeded as soon as the compressed
    // data passes `limit` bytes. It reports `identity`'s device and inode, so
    // an archive written through it still skips its own file.
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

    // Flushes, syncs, and (for files) atomically moves the result into place.
    // A compressing output finishes its frame.
    void commit();

    // Appends everything written to this output so far to `dest`.
    void copy_to(Output& dest);

    // Identity of the file being written, so the archive can skip itself.
    dev_t device() const noexcept { return device_; }
    ino_t inode() const noexcept { return inode_; }

private:
    struct Zstd;

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
    dev_t device_ = 0;
    ino_t inode_ = 0;
    std::unique_ptr<Zstd> zstd_;
};

} // namespace zar
