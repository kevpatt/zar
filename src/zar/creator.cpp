#include "zar/creator.hpp"

#include "zar/archive_writer.hpp"
#include "zar/compressor.hpp"
#include "zar/crc32c.hpp"
#include "zar/index.hpp"
#include "zar/names.hpp"
#include "zar/policy.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <grp.h>
#include <map>
#include <optional>
#include <pwd.h>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace zar {
namespace {

namespace fs = std::filesystem;

// ---- platform details -------------------------------------------------------

#ifdef __APPLE__
timespec mtime_of(const struct stat& st) { return st.st_mtimespec; }
timespec atime_of(const struct stat& st) { return st.st_atimespec; }
timespec ctime_of(const struct stat& st) { return st.st_ctimespec; }
std::optional<timespec> btime_of(const struct stat& st) { return st.st_birthtimespec; }
#else
timespec mtime_of(const struct stat& st) { return st.st_mtim; }
timespec atime_of(const struct stat& st) { return st.st_atim; }
timespec ctime_of(const struct stat& st) { return st.st_ctim; }
std::optional<timespec> btime_of(const struct stat&) { return std::nullopt; }
#endif

std::string errno_text() { return std::strerror(errno); }

class FileDescriptor {
public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() { if (fd_ >= 0) ::close(fd_); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }

private:
    int fd_;
};

// Reads until `buffer` is full or EOF. Returns the byte count, or nullopt on error.
std::optional<std::size_t> read_fully(int fd, std::span<std::uint8_t> buffer)
{
    std::size_t total = 0;
    while (total < buffer.size()) {
        const auto n = ::read(fd, buffer.data() + total, buffer.size() - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return std::nullopt;
        }
        if (n == 0)
            break;
        total += static_cast<std::size_t>(n);
    }
    return total;
}

std::optional<std::string> user_name(uid_t uid)
{
    std::vector<char> buffer(4096);
    struct passwd pw {};
    struct passwd* result = nullptr;
    if (::getpwuid_r(uid, &pw, buffer.data(), buffer.size(), &result) != 0 || !result)
        return std::nullopt;
    return std::string(result->pw_name);
}

std::optional<std::string> group_name(gid_t gid)
{
    std::vector<char> buffer(4096);
    struct group gr {};
    struct group* result = nullptr;
    if (::getgrgid_r(gid, &gr, buffer.data(), buffer.size(), &result) != 0 || !result)
        return std::nullopt;
    return std::string(result->gr_name);
}

// The name an input is stored under: its last component, or for paths like
// "." and "dir/", the name of the directory they resolve to.
std::string root_component(const fs::path& input)
{
    auto name = input.filename();
    if (name.empty() || name == "." || name == "..") {
        std::error_code ec;
        name = fs::weakly_canonical(input, ec).filename();
    }
    return name.native();
}

// ---- the creator ------------------------------------------------------------

class Creator {
public:
    Creator(Output& out, const CreateOptions& options)
        : out_(out),
          options_(options),
          threads_(options.threads ? options.threads : std::max(1u, std::thread::hardware_concurrency())),
          writer_(out, options.align_log2),
          compressor_(options.level, threads_)
    {
    }

    // Returns false if the input couldn't be read at all.
    bool add_input(const fs::path& input, bool contents_only);
    CreateStats finish();

private:
    // An archive path plus the entry flags it implies (kPathRaw).
    struct Name {
        std::string path;
        std::uint8_t flags = 0;
    };

    struct PendingChunk {
        std::future<CompressedChunk> chunk;
        std::size_t entry;
    };

    struct FileId {
        dev_t device;
        ino_t inode;
        auto operator<=>(const FileId&) const = default;
    };

    std::optional<Name> child_name(const Name& parent, std::string_view component,
                                   const fs::path& where);
    void add(const fs::path& path, const Name& name, const struct stat& st);
    void add_directory_contents(const fs::path& dir, const Name& name);
    void add_file(const fs::path& path, const Name& name, const struct stat& st);
    void add_symlink(const fs::path& path, const Name& name, const struct stat& st);

    Entry make_entry(EntryType type, const Name& name, const struct stat& st);
    Timestamp timestamp(timespec ts) const;
    const std::optional<std::string>& cached_user(uid_t uid);
    const std::optional<std::string>& cached_group(gid_t gid);

    void drain(std::size_t keep);
    void warn(const std::string& message);

    Output& out_;
    const CreateOptions& options_;
    unsigned threads_;
    ArchiveWriter writer_;
    Compressor compressor_;

    std::vector<Entry> entries_;
    std::set<std::size_t> dropped_;              // entries abandoned after a read error
    std::deque<PendingChunk> pending_;           // chunks being compressed, in file order
    std::map<FileId, std::size_t> first_link_;   // first entry for each multiply-linked file
    std::vector<std::pair<std::size_t, std::size_t>> hardlinks_;    // (link, target)
    std::map<uid_t, std::optional<std::string>> users_;
    std::map<gid_t, std::optional<std::string>> groups_;
    CreateStats stats_;
};

void Creator::warn(const std::string& message)
{
    ++stats_.warnings;
    if (options_.on_warning)
        options_.on_warning(message);
}

std::optional<Creator::Name> Creator::child_name(const Name& parent, std::string_view component,
                                                 const fs::path& where)
{
    std::string part(component);
    std::uint8_t flags = parent.flags;

    if (!is_valid_utf8(part)) {
        switch (options_.names) {
        case NameMode::Skip:
            warn(where.string() + ": skipped; name is not valid UTF-8");
            return std::nullopt;
        case NameMode::Latin1:
            part = latin1_to_utf8(part);
            warn(where.string() + ": name is not valid UTF-8; converted from Latin-1");
            break;
        case NameMode::Raw:
            flags |= entry_flags::kPathRaw;
            warn(where.string() + ": name is not valid UTF-8; stored as raw bytes");
            break;
        }
    }
    return Name{parent.path.empty() ? part : parent.path + '/' + part, flags};
}

bool Creator::add_input(const fs::path& input, bool contents_only)
{
    struct stat st {};
    if (::stat(input.c_str(), &st) != 0) {    // command-line symlinks are followed
        warn(input.string() + ": " + errno_text());
        return false;
    }
    if (contents_only) {
        add_directory_contents(input, Name{});
        return true;
    }
    const auto component = root_component(input);
    if (component.empty()) {
        warn(input.string() + ": cannot archive a filesystem root by name");
        return false;
    }
    if (auto name = child_name(Name{}, component, input))
        add(input, *name, st);
    return true;
}

void Creator::add(const fs::path& path, const Name& name, const struct stat& st)
{
    if (st.st_dev == out_.device() && st.st_ino == out_.inode())
        return;    // the archive being written

    switch (st.st_mode & S_IFMT) {
    case S_IFREG:
        add_file(path, name, st);
        break;
    case S_IFDIR:
        entries_.push_back(make_entry(EntryType::Dir, name, st));
        ++stats_.directories;
        if (options_.on_entry)
            options_.on_entry(name.path + '/');
        add_directory_contents(path, name);
        break;
    case S_IFLNK:
        add_symlink(path, name, st);
        break;
    default:
        warn(path.string() + ": skipped; not a regular file, directory, or symlink");
        break;
    }
}

void Creator::add_directory_contents(const fs::path& dir, const Name& name)
{
    std::vector<std::string> children;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        children.push_back(it->path().filename().native());
    if (ec)
        warn(dir.string() + ": " + ec.message());
    std::ranges::sort(children);    // deterministic archives

    for (const auto& child : children) {
        const auto path = dir / child;
        struct stat st {};
        if (::lstat(path.c_str(), &st) != 0) {
            warn(path.string() + ": " + errno_text());
            continue;
        }
        if (auto child_path = child_name(name, child, path))
            add(path, *child_path, st);
    }
}

void Creator::add_file(const fs::path& path, const Name& name, const struct stat& st)
{
    const FileId id{st.st_dev, st.st_ino};
    const bool multiply_linked = st.st_nlink > 1;

    if (multiply_linked) {
        if (const auto it = first_link_.find(id); it != first_link_.end()) {
            auto entry = make_entry(EntryType::Hardlink, name, st);
            const auto& target = entries_[it->second];
            entry.link_target = target.path;
            entry.flags |= target.flags & entry_flags::kPathRaw;
            hardlinks_.emplace_back(entries_.size(), it->second);
            entries_.push_back(std::move(entry));
            ++stats_.hardlinks;
            if (options_.on_entry)
                options_.on_entry(name.path);
            return;
        }
    }

    const FileDescriptor fd(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (!fd) {
        warn(path.string() + ": " + errno_text());
        return;
    }
    if (options_.on_entry)
        options_.on_entry(name.path);

    // Chunk block offsets are filled in as the compressed blocks are written.
    const std::size_t index = entries_.size();
    entries_.push_back(make_entry(EntryType::File, name, st));

    const bool compress = options_.compress_all || !is_precompressed(name.path);
    const auto expected = static_cast<std::uint64_t>(st.st_size);
    std::uint64_t total = 0;
    std::uint64_t chunk_count = 0;
    std::uint32_t crc = 0;

    // Archive exactly the size stat() reported, so every chunk but the last is
    // full (§6.5) even if the file changes while we read it.
    while (total < expected) {
        const auto want = static_cast<std::size_t>(std::min(options_.chunk_size, expected - total));
        Bytes chunk(want);
        const auto n = read_fully(fd.get(), chunk);
        if (!n) {
            warn(path.string() + ": " + errno_text() + "; skipped");
            dropped_.insert(index);
            return;
        }
        chunk.resize(*n);
        if (!chunk.empty()) {
            crc = crc32c(crc, chunk);
            total += chunk.size();
            ++chunk_count;
            pending_.push_back({compressor_.submit(std::move(chunk), compress), index});
            drain(threads_ * 4);
        }
        if (*n < want) {
            warn(path.string() + ": file shrank while being read; archived " +
                 std::to_string(total) + " bytes");
            break;
        }
    }
    if (std::uint8_t probe; total == expected && read_fully(fd.get(), {&probe, 1}).value_or(0) > 0)
        warn(path.string() + ": file grew while being read; archived the first " +
             std::to_string(total) + " bytes");

    auto& entry = entries_[index];
    if (total > 0) {
        entry.size = total;
        entry.content_crc = crc;
        if (chunk_count > 1)
            entry.chunk_size = options_.chunk_size;
    }
    ++stats_.files;
    stats_.input_bytes += total;
    if (multiply_linked)
        first_link_.emplace(id, index);
}

void Creator::add_symlink(const fs::path& path, const Name& name, const struct stat& st)
{
    std::error_code ec;
    auto target = fs::read_symlink(path, ec).native();
    if (ec) {
        warn(path.string() + ": " + ec.message());
        return;
    }

    auto entry = make_entry(EntryType::Symlink, name, st);
    if (!is_valid_utf8(target)) {
        switch (options_.names) {
        case NameMode::Skip:
            warn(path.string() + ": skipped; link target is not valid UTF-8");
            return;
        case NameMode::Latin1:
            target = latin1_to_utf8(target);
            warn(path.string() + ": link target is not valid UTF-8; converted from Latin-1");
            break;
        case NameMode::Raw:
            entry.flags |= entry_flags::kPathRaw;
            warn(path.string() + ": link target is not valid UTF-8; stored as raw bytes");
            break;
        }
    }
    entry.link_target = std::move(target);
    entries_.push_back(std::move(entry));
    ++stats_.symlinks;
    if (options_.on_entry)
        options_.on_entry(name.path);
}

Timestamp Creator::timestamp(timespec ts) const
{
    Timestamp t{ts.tv_sec, {}};
    if (options_.nanoseconds && ts.tv_nsec != 0)
        t.nanoseconds = static_cast<std::uint32_t>(ts.tv_nsec);
    return t;
}

const std::optional<std::string>& Creator::cached_user(uid_t uid)
{
    auto it = users_.find(uid);
    if (it == users_.end())
        it = users_.emplace(uid, user_name(uid)).first;
    return it->second;
}

const std::optional<std::string>& Creator::cached_group(gid_t gid)
{
    auto it = groups_.find(gid);
    if (it == groups_.end())
        it = groups_.emplace(gid, group_name(gid)).first;
    return it->second;
}

Entry Creator::make_entry(EntryType type, const Name& name, const struct stat& st)
{
    Entry entry;
    entry.type = type;
    entry.flags = name.flags;
    entry.path = name.path;
    entry.mode = static_cast<std::uint16_t>(st.st_mode & 07777);
    entry.mtime = timestamp(mtime_of(st));

    if (options_.all_times) {
        entry.atime = timestamp(atime_of(st));
        entry.ctime = timestamp(ctime_of(st));
        if (const auto btime = btime_of(st))
            entry.btime = timestamp(*btime);
    }

    const auto owner = options_.owner;
    if (owner == OwnerMode::Ids || owner == OwnerMode::Both) {
        entry.uid = st.st_uid;
        entry.gid = st.st_gid;
    }
    if (owner == OwnerMode::Names || owner == OwnerMode::Both) {
        if (const auto& user = cached_user(st.st_uid); user && is_valid_utf8(*user))
            entry.uname = *user;
        if (const auto& group = cached_group(st.st_gid); group && is_valid_utf8(*group))
            entry.gname = *group;
    }
    return entry;
}

// Writes finished chunks, in order, until at most `keep` remain in flight.
void Creator::drain(std::size_t keep)
{
    while (pending_.size() > keep) {
        auto pending = std::move(pending_.front());
        pending_.pop_front();
        const auto chunk = pending.chunk.get();
        entries_[pending.entry].chunks.push_back(writer_.write_block(chunk.type, chunk.payload));
    }
}

CreateStats Creator::finish()
{
    drain(0);

    // Hard links share their target's data blocks (§6.5).
    for (const auto [link, target] : hardlinks_) {
        auto& entry = entries_[link];
        const auto& source = entries_[target];
        entry.size = source.size;
        entry.chunk_size = source.chunk_size;
        entry.chunks = source.chunks;
        entry.dict = source.dict;
        entry.content_crc = source.content_crc;
    }

    Bytes index;
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (!dropped_.contains(i))
            encode_entry(index, entries_[i]);

    const auto packed = zstd_compress(index, options_.level);
    const auto root = packed.size() < index.size()
                          ? writer_.write_block(BlockType::IndexZstd, packed)
                          : writer_.write_block(BlockType::IndexRaw, index);
    writer_.finish(root);

    stats_.archive_bytes = out_.position();
    return stats_;
}

} // namespace

CreateStats create_archive(Output& out, std::span<const fs::path> inputs,
                           const CreateOptions& options)
{
    Creator creator(out, options);
    std::error_code ec;
    const bool contents_only = inputs.size() == 1 && fs::is_directory(inputs[0], ec);
    bool any = false;
    for (const auto& input : inputs)
        any |= creator.add_input(input, contents_only);
    if (!any)
        throw std::runtime_error("no inputs could be archived");
    return creator.finish();
}

} // namespace zar
