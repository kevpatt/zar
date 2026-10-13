// Buffered, error-checked archive output.
#pragma once

#include "zar/bytes.hpp"

#include <filesystem>
#include <sys/types.h>

namespace zar {

class Output {
public:
    // Writes to a temporary file beside `path`; commit() renames it into place.
    // Without overwrite, fails if `path` already exists.
    static Output create_file(const std::filesystem::path& path, bool overwrite);
    static Output standard_output();

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
    void commit();

    // Identity of the file being written, so the archive can skip itself.
    dev_t device() const noexcept { return device_; }
    ino_t inode() const noexcept { return inode_; }

private:
    Output(int fd, std::filesystem::path temp, std::filesystem::path final_path);
    void flush();

    int fd_ = -1;
    std::filesystem::path temp_path_;
    std::filesystem::path final_path_;
    Bytes buffer_;
    std::uint64_t position_ = 0;
    bool seekable_ = false;
    dev_t device_ = 0;
    ino_t inode_ = 0;
};

} // namespace zar
