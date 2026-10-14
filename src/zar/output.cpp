// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "zar/output.hpp"

#include "zar/crc32c.hpp"
#include "zar/posix.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <zstd.h>

namespace zar {
namespace {

constexpr std::size_t kBufferSize = 1 << 20;

mode_t current_umask()
{
    const mode_t mask = ::umask(0);
    ::umask(mask);
    return mask;
}

// Renames `from` to `to` unless `to` exists, atomically where the system
// can; otherwise link() provides the same guarantee.
int rename_exclusive(const char* from, const char* to)
{
#if defined(__APPLE__)
    if (::renamex_np(from, to, RENAME_EXCL) == 0)
        return 0;
    if (errno != ENOTSUP && errno != EINVAL)
        return -1;
#elif defined(__linux__) && defined(RENAME_NOREPLACE)
    if (::renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE) == 0)
        return 0;
    if (errno != ENOSYS && errno != EINVAL)
        return -1;
#endif
    if (::link(from, to) != 0)
        return -1;
    ::unlink(from);
    return 0;
}

} // namespace

struct Output::Zstd {
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    Output* dest = nullptr;
    std::uint64_t start = 0;
    std::uint64_t limit = 0;
    std::uint32_t crc = 0;
    Bytes out = Bytes(ZSTD_CStreamOutSize());

    ~Zstd() { ZSTD_freeCCtx(cctx); }

    void check(std::size_t result) const
    {
        if (ZSTD_isError(result))
            throw std::runtime_error(std::string("zstd: ") + ZSTD_getErrorName(result));
    }

    void feed(ByteView data, ZSTD_EndDirective mode)
    {
        ZSTD_inBuffer in{data.data(), data.size(), 0};
        for (;;) {
            ZSTD_outBuffer chunk{out.data(), out.size(), 0};
            const auto remaining = ZSTD_compressStream2(cctx, &chunk, &in, mode);
            check(remaining);
            if (chunk.pos) {
                const ByteView produced(out.data(), chunk.pos);
                crc = crc32c(crc, produced);
                dest->write(produced);
                if (dest->position() - start > limit)
                    throw SizeLimitExceeded();
            }
            if (mode == ZSTD_e_end ? remaining == 0 : in.pos == in.size)
                return;
        }
    }
};

Output::Output(int fd, bool owns_fd, std::filesystem::path temp, std::filesystem::path final_path)
    : fd_(fd), owns_fd_(owns_fd), temp_path_(std::move(temp)), final_path_(std::move(final_path))
{
    buffer_.reserve(kBufferSize);
    struct stat st {};
    if (fd_ >= 0 && ::fstat(fd_, &st) == 0) {
        // pwrite() ignores the offset on O_APPEND descriptors, so treat those
        // (e.g. `zar -o - >> file`) as unseekable.
        const int flags = ::fcntl(fd_, F_GETFL);
        seekable_ = S_ISREG(st.st_mode) && ::lseek(fd_, 0, SEEK_CUR) == 0 && !(flags & O_APPEND);
        readable_ = seekable_ && (flags & O_ACCMODE) == O_RDWR;
        self_ = FileId{st.st_dev, st.st_ino};
    }
}

Output Output::create_file(const std::filesystem::path& path, bool overwrite)
{
    struct stat existing {};
    const bool exists = ::lstat(path.c_str(), &existing) == 0;
    if (exists && !overwrite)
        throw std::runtime_error(path.string() + " already exists (use -y to overwrite)");

    std::string temp = path.string() + ".XXXXXX";
    const int fd = ::mkstemp(temp.data());
    if (fd < 0)
        throw_errno(path.string());
    ::fchmod(fd, 0666 & ~current_umask());    // mkstemp creates 0600
    Output out(fd, true, temp, path);
    out.replace_ = overwrite;
    if (exists)
        out.replaced_ = FileId{existing.st_dev, existing.st_ino};
    return out;
}

Output Output::standard_output()
{
    if (::isatty(STDOUT_FILENO))
        throw std::runtime_error("refusing to write an archive to a terminal");
    return Output(STDOUT_FILENO, false, {}, "standard output");
}

Output Output::compressing(Output& dest, int level, unsigned threads, std::uint64_t limit,
                           const Output& identity)
{
    Output out(-1, false, {}, "compressed output");
    out.self_ = identity.self_;
    out.replaced_ = identity.replaced_;
    out.zstd_ = std::make_unique<Zstd>();
    auto& z = *out.zstd_;
    if (!z.cctx)
        throw std::bad_alloc();
    z.dest = &dest;
    z.start = dest.position();
    z.limit = limit;
    z.check(ZSTD_CCtx_setParameter(z.cctx, ZSTD_c_compressionLevel, level));
    z.check(ZSTD_CCtx_setParameter(z.cctx, ZSTD_c_checksumFlag, 0));
    if (threads > 1)    // fails harmlessly if libzstd lacks threading
        ZSTD_CCtx_setParameter(z.cctx, ZSTD_c_nbWorkers, static_cast<int>(threads));
    return out;
}

bool Output::is_file(dev_t device, ino_t inode) const noexcept
{
    const FileId id{device, inode};
    return id == self_ || id == replaced_;
}

std::uint32_t Output::compressed_crc() const noexcept
{
    return zstd_ ? zstd_->crc : 0;
}

Output Output::make_spool() const
{
    auto dir = temp_path_.empty() ? temp_directory() : final_path_.parent_path();
    if (dir.empty())
        dir = ".";
    return Output(anonymous_temp_file(dir, ".zar-spool-").release(), true, {}, "temporary file");
}

void Output::copy_to(Output& dest)
{
    flush();
    Bytes chunk(kBufferSize);
    for (std::uint64_t done = 0; done < position_;) {
        const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), position_ - done));
        if (pread_full(fd_, std::span(chunk).first(want), done, final_path_.string()) != want)
            throw std::runtime_error(final_path_.string() + ": ended early");
        dest.write(std::span(chunk).first(want));
        done += want;
    }
}

Output::Output(Output&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      owns_fd_(other.owns_fd_),
      temp_path_(std::move(other.temp_path_)),
      final_path_(std::move(other.final_path_)),
      buffer_(std::move(other.buffer_)),
      position_(other.position_),
      seekable_(other.seekable_),
      readable_(other.readable_),
      replace_(other.replace_),
      self_(other.self_),
      replaced_(other.replaced_),
      zstd_(std::move(other.zstd_))
{
    other.temp_path_.clear();
}

Output::~Output()
{
    if (owns_fd_ && fd_ >= 0)
        ::close(fd_);
    if (!temp_path_.empty())
        ::unlink(temp_path_.c_str());
}

void Output::write(ByteView bytes)
{
    if (buffer_.size() + bytes.size() > kBufferSize)
        flush();
    if (bytes.size() >= kBufferSize)
        emit(bytes);
    else
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    position_ += bytes.size();
}

void Output::write_zeros(std::size_t count)
{
    static constexpr std::uint8_t kZeros[64]{};
    while (count) {
        const auto n = std::min(count, sizeof kZeros);
        write({kZeros, n});
        count -= n;
    }
}

void Output::write_at(std::uint64_t position, ByteView bytes)
{
    flush();
    pwrite_all(fd_, bytes, position, final_path_.string());
}

void Output::read_at(std::uint64_t position, std::span<std::uint8_t> out)
{
    if (!readable_)
        throw std::logic_error("read_at() on an output that can't be read");
    flush();
    if (pread_full(fd_, out, position, final_path_.string()) != out.size())
        throw std::runtime_error(final_path_.string() + ": read past the end of what was written");
}

void Output::flush()
{
    emit(buffer_);
    buffer_.clear();
}

void Output::emit(ByteView bytes)
{
    if (zstd_)
        zstd_->feed(bytes, ZSTD_e_continue);
    else
        write_all(fd_, bytes, final_path_.string());
}

void Output::commit()
{
    flush();
    if (zstd_)
        zstd_->feed({}, ZSTD_e_end);
    if (temp_path_.empty())
        return;
    if (::fsync(fd_) != 0 || ::close(fd_) != 0) {
        fd_ = -1;
        throw_errno(final_path_.string());
    }
    fd_ = -1;
    const auto rc = replace_ ? ::rename(temp_path_.c_str(), final_path_.c_str())
                             : rename_exclusive(temp_path_.c_str(), final_path_.c_str());
    if (rc != 0 && errno == EEXIST && !replace_)
        throw std::runtime_error(final_path_.string() + " already exists (use -y to overwrite)");
    if (rc != 0)
        throw_errno(final_path_.string());
    temp_path_.clear();
}

} // namespace zar
