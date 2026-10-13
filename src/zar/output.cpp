#include "zar/output.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace zar {
namespace {

constexpr std::size_t kBufferSize = 1 << 20;

[[noreturn]] void throw_errno(const std::string& what)
{
    throw std::system_error(errno, std::generic_category(), what);
}

void write_all(int fd, const std::uint8_t* data, std::size_t size, const std::string& name)
{
    while (size) {
        const auto n = ::write(fd, data, size);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            throw_errno(name);
        }
        data += n;
        size -= static_cast<std::size_t>(n);
    }
}

mode_t current_umask()
{
    const mode_t mask = ::umask(0);
    ::umask(mask);
    return mask;
}

} // namespace

Output::Output(int fd, std::filesystem::path temp, std::filesystem::path final_path)
    : fd_(fd), temp_path_(std::move(temp)), final_path_(std::move(final_path))
{
    buffer_.reserve(kBufferSize);
    struct stat st {};
    if (::fstat(fd_, &st) == 0) {
        // pwrite() ignores the offset on O_APPEND descriptors, so treat those
        // (e.g. `zar -o - >> file`) as unseekable.
        seekable_ = S_ISREG(st.st_mode) && ::lseek(fd_, 0, SEEK_CUR) == 0 &&
                    !(::fcntl(fd_, F_GETFL) & O_APPEND);
        device_ = st.st_dev;
        inode_ = st.st_ino;
    }
}

Output Output::create_file(const std::filesystem::path& path, bool overwrite)
{
    if (!overwrite && std::filesystem::exists(std::filesystem::symlink_status(path)))
        throw std::runtime_error(path.string() + " already exists (use -y to overwrite)");

    std::string temp = path.string() + ".XXXXXX";
    const int fd = ::mkstemp(temp.data());
    if (fd < 0)
        throw_errno(path.string());
    ::fchmod(fd, 0666 & ~current_umask());    // mkstemp creates 0600
    return Output(fd, temp, path);
}

Output Output::standard_output()
{
    if (::isatty(STDOUT_FILENO))
        throw std::runtime_error("refusing to write an archive to a terminal");
    return Output(STDOUT_FILENO, {}, {});
}

Output::Output(Output&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      temp_path_(std::move(other.temp_path_)),
      final_path_(std::move(other.final_path_)),
      buffer_(std::move(other.buffer_)),
      position_(other.position_),
      seekable_(other.seekable_),
      device_(other.device_),
      inode_(other.inode_)
{
    other.temp_path_.clear();
}

Output::~Output()
{
    if (!temp_path_.empty()) {
        ::close(fd_);
        ::unlink(temp_path_.c_str());
    }
}

void Output::write(ByteView bytes)
{
    if (buffer_.size() + bytes.size() > kBufferSize)
        flush();
    if (bytes.size() >= kBufferSize)
        write_all(fd_, bytes.data(), bytes.size(), final_path_.string());
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
    if (::pwrite(fd_, bytes.data(), bytes.size(), static_cast<off_t>(position)) !=
        static_cast<ssize_t>(bytes.size()))
        throw_errno(final_path_.string());
}

void Output::flush()
{
    write_all(fd_, buffer_.data(), buffer_.size(), final_path_.string());
    buffer_.clear();
}

void Output::commit()
{
    flush();
    if (temp_path_.empty())
        return;
    if (::fsync(fd_) != 0 || ::close(fd_) != 0) {
        fd_ = -1;
        throw_errno(final_path_.string());
    }
    fd_ = -1;
    if (::rename(temp_path_.c_str(), final_path_.c_str()) != 0)
        throw_errno(final_path_.string());
    temp_path_.clear();
}

} // namespace zar
