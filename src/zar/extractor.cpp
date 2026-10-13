#include "zar/extractor.hpp"

#include "zar/crc32c.hpp"
#include "zar/thread_pool.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <grp.h>
#include <map>
#include <mutex>
#include <optional>
#include <pwd.h>
#include <semaphore>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace zar {
namespace {

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept
    {
        std::swap(fd_, other.fd_);
        return *this;
    }
    ~Fd()
    {
        if (fd_ >= 0)
            ::close(fd_);
    }
    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};

struct ExtractError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void throw_errno(const std::string& what)
{
    if (errno == ELOOP)
        throw ExtractError(what + ": is a symbolic link; not following it");
    throw ExtractError(what + ": " + std::generic_category().message(errno));
}

std::string_view parent_of(std::string_view path)
{
    const auto slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
}

// Orders paths component by component, so a directory's contents sort
// directly after it ("a", "a/b", "a-c" rather than "a", "a-c", "a/b").
struct PathLess {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const
    {
        const auto key = [](char c) { return c == '/' ? 0 : static_cast<unsigned char>(c) + 1; };
        return std::ranges::lexicographical_compare(
            a, b, [&](char x, char y) { return key(x) < key(y); });
    }
};

void write_all_at(int fd, ByteView data, std::uint64_t position)
{
    while (!data.empty()) {
        const auto n = ::pwrite(fd, data.data(), data.size(), static_cast<off_t>(position));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            throw ExtractError(std::generic_category().message(errno));
        data = data.subspan(static_cast<std::size_t>(n));
        position += static_cast<std::uint64_t>(n);
    }
}

timespec to_timespec(const std::optional<Timestamp>& t)
{
    if (!t)
        return {0, UTIME_OMIT};
    return {static_cast<time_t>(t->seconds), static_cast<long>(t->nanoseconds.value_or(0))};
}

std::optional<uid_t> owner_of(const Entry& e)
{
    if (e.uname) {
        std::vector<char> buffer(4096);
        struct passwd pw {};
        struct passwd* result = nullptr;
        if (::getpwnam_r(e.uname->c_str(), &pw, buffer.data(), buffer.size(), &result) == 0 && result)
            return result->pw_uid;
    }
    if (e.uid)
        return static_cast<uid_t>(*e.uid);
    return std::nullopt;
}

std::optional<gid_t> group_of(const Entry& e)
{
    if (e.gname) {
        std::vector<char> buffer(4096);
        struct group gr {};
        struct group* result = nullptr;
        if (::getgrnam_r(e.gname->c_str(), &gr, buffer.data(), buffer.size(), &result) == 0 && result)
            return result->gr_gid;
    }
    if (e.gid)
        return static_cast<gid_t>(*e.gid);
    return std::nullopt;
}

} // namespace

std::string_view base_name(std::string_view path)
{
    const auto slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::vector<DirectoryGroup> group_by_directory(std::span<const Entry> entries)
{
    std::map<std::string, DirectoryGroup, PathLess> groups;
    const auto group = [&](std::string_view path) -> DirectoryGroup& {
        auto [it, inserted] = groups.try_emplace(std::string(path));
        if (inserted)
            it->second.path = path;
        return it->second;
    };

    group("");
    for (const auto& entry : entries) {
        if (entry.type == EntryType::Dir)
            group(entry.path).entry = &entry;
        else
            group(parent_of(entry.path)).members.push_back(&entry);
        for (auto p = parent_of(entry.path); !p.empty(); p = parent_of(p))
            group(p);
    }

    std::vector<DirectoryGroup> out;
    out.reserve(groups.size());
    for (auto& [path, g] : groups) {
        std::ranges::sort(g.members, {}, [](const Entry* e) { return base_name(e->path); });
        out.push_back(std::move(g));
    }
    return out;
}

struct Extractor::Impl {
    // A file whose chunks are being extracted in parallel. Whichever worker
    // finishes the last chunk completes the file.
    struct FileJob {
        FileJob(const Entry& e, std::vector<std::uint64_t> chunk_lengths)
            : entry(e), lengths(std::move(chunk_lengths)), crcs(lengths.size()), remaining(lengths.size())
        {
        }

        void fail(std::string_view what)
        {
            std::lock_guard lock(mutex);
            if (!failed.exchange(true))
                error = what;
        }

        const Entry& entry;
        std::vector<std::uint64_t> lengths;
        std::vector<std::uint32_t> crcs;    // per chunk, combined at the end
        std::atomic<std::size_t> remaining;
        std::atomic<bool> failed = false;
        std::mutex mutex;
        std::string error;
        std::string dir_path;    // archive path of its directory
        std::string name;
        bool opened = false;     // stream: creation was attempted
        Fd dir;    // its directory, so a failed file can be removed
        Fd out;
    };

    // Stream: a chunk waiting for its block to be read.
    struct StreamedChunk {
        std::shared_ptr<FileJob> job;
        std::size_t index;
        std::uint64_t position;
    };

    std::map<std::int64_t, std::vector<StreamedChunk>> streamed;    // by block offset
    // Once a stream breaks (it was truncated, say), every later block is lost
    // too; those files are counted and reported once rather than one by one.
    std::string stream_error;
    std::uint64_t lost_to_stream = 0;

    ArchiveReader& reader;
    const ExtractOptions& options;
    const bool writing = options.mode == ExtractMode::Extract;
    const bool decompress = options.mode != ExtractMode::Scan;
    const bool privileged = ::geteuid() == 0;
    const unsigned threads = options.threads ? options.threads : default_thread_count();

    std::mutex mutex;    // guards stats and warnings, which workers update too
    ExtractStats stats;

    Fd root;
    Fd current;    // directory of the group being extracted
    std::string current_path;

    // Created last, so nothing can be written through them (§7).
    std::vector<std::pair<std::string, const Entry*>> symlinks;
    // Metadata applied last, deepest first, so writing contents doesn't disturb it.
    std::vector<const Entry*> directories;

    // Bounds the chunks in memory, and so the open files, at any moment.
    std::counting_semaphore<> in_flight{static_cast<std::ptrdiff_t>(threads) * 4};
    ThreadPool pool{threads};    // last: destroyed first, while what its jobs use still exists

    Impl(ArchiveReader& r, const ExtractOptions& o) : reader(r), options(o)
    {
        if (!writing)
            return;
        std::error_code ec;
        std::filesystem::create_directories(options.directory, ec);
        root = Fd(::open(options.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!root)
            throw std::system_error(errno, std::generic_category(), options.directory.string());
    }

    void warn(const std::string& message)
    {
        std::lock_guard lock(mutex);
        ++stats.warnings;
        if (options.on_warning)
            options.on_warning(message);
    }

    // Opens a directory below the root, one component at a time, never
    // following symlinks. Missing directories are created if `create`.
    Fd open_directory(std::string_view path, bool create) const
    {
        Fd dir(::dup(root.get()));
        while (!path.empty()) {
            const auto slash = path.find('/');
            const std::string component(path.substr(0, slash));
            path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);

            constexpr int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
            int fd = ::openat(dir.get(), component.c_str(), flags);
            if (fd < 0 && errno == ENOENT && create) {
                if (::mkdirat(dir.get(), component.c_str(), 0777) != 0 && errno != EEXIST)
                    throw_errno(component);
                fd = ::openat(dir.get(), component.c_str(), flags);
            }
            if (fd < 0)
                throw_errno(component);
            dir = Fd(fd);
        }
        return dir;
    }

    // Clears the way for a new file or symlink.
    void remove_existing(int dir, const std::string& name)
    {
        if (!options.overwrite)
            throw ExtractError("already exists (use -y to overwrite)");
        struct stat st {};
        if (::fstatat(dir, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode))
            throw ExtractError("a directory is in the way");
        if (::unlinkat(dir, name.c_str(), 0) != 0)
            throw_errno("cannot replace existing file");
    }

    bool exists(std::string_view path) const
    {
        std::error_code ec;
        return std::filesystem::exists(std::filesystem::symlink_status(options.directory / path, ec));
    }

    void prepare(std::span<const DirectoryGroup> groups)
    {
        if (!writing)
            return;

        std::vector<std::string> existing;
        for (const auto& group : groups) {
            if (!group.path.empty() && exists(group.path))
                existing.push_back(group.path + '/');
            for (const auto* member : group.members)
                if (exists(member->path))
                    existing.push_back(member->path);
        }
        if (existing.empty())
            return;

        if (!options.overwrite) {
            std::string message = std::to_string(existing.size()) +
                                  (existing.size() == 1 ? " path already exists: "
                                                        : " paths already exist, including ");
            for (std::size_t i = 0; i < std::min<std::size_t>(existing.size(), 3); ++i)
                message += (i ? ", " : "") + existing[i];
            throw ExtractError(message + " (use -y to overwrite)");
        }

        if (options.dirs == DirMode::Replace) {
            // Only top-level directories: any deeper existing directory lies
            // inside one of them. remove_all() never follows symlinks.
            for (const auto& group : groups) {
                if (group.path.empty() || group.path.find('/') != std::string::npos ||
                    !exists(group.path))
                    continue;
                std::error_code ec;
                std::filesystem::remove_all(options.directory / group.path, ec);
                if (ec)
                    throw ExtractError(group.path + ": cannot replace: " + ec.message());
            }
        }
    }

    void apply_owner(int dir, const char* name, int fd, const Entry& e) const
    {
        if (!privileged)
            return;
        const auto uid = owner_of(e);
        const auto gid = group_of(e);
        if (!uid && !gid)
            return;
        const auto u = uid.value_or(static_cast<uid_t>(-1));
        const auto g = gid.value_or(static_cast<gid_t>(-1));
        const int rc = fd >= 0 ? ::fchown(fd, u, g) : ::fchownat(dir, name, u, g, AT_SYMLINK_NOFOLLOW);
        if (rc != 0)
            throw_errno("cannot set owner");
    }

    void apply_metadata(int fd, const Entry& e) const
    {
        apply_owner(-1, nullptr, fd, e);    // before chmod: chown clears set-id bits
        if (e.mode) {
            const mode_t allowed = privileged ? 07777 : 01777;
            if (::fchmod(fd, *e.mode & allowed) != 0)
                throw_errno("cannot set permissions");
        }
        const timespec times[2] = {to_timespec(e.atime), to_timespec(e.mtime)};
        if (::futimens(fd, times) != 0)
            throw_errno("cannot set times");
    }

    void enter(const DirectoryGroup& group)
    {
        current_path = group.path;
        if (group.entry) {
            std::lock_guard lock(mutex);
            ++stats.directories;
            directories.push_back(group.entry);
        }
        if (!writing)
            return;
        try {
            current = open_directory(group.path, true);
        } catch (const std::exception& e) {
            current = Fd();
            warn(group.path + ": " + e.what());
        }
    }

    void extract(const Entry& entry)
    {
        try {
            switch (entry.type) {
            case EntryType::File:
            case EntryType::Hardlink:    // extracted as an independent copy
                extract_file(entry);
                break;
            case EntryType::Symlink:
                if (!entry.link_target || entry.link_target->empty() ||
                    entry.link_target->find('\0') != std::string::npos)
                    throw FormatError("symlink has no valid target");
                {
                    std::lock_guard lock(mutex);
                    ++stats.symlinks;
                }
                if (writing)
                    symlinks.emplace_back(current_path, &entry);
                break;
            case EntryType::Dir:
                break;    // handled by enter()
            }
        } catch (const std::exception& e) {
            warn(entry.path + ": " + e.what());
        }
    }

    // Creates the file and queues its chunks; the pool does the rest. When
    // reading a stream, chunks wait for their blocks to come by, and the file
    // is created when its first one does.
    void extract_file(const Entry& entry)
    {
        auto job = std::make_shared<FileJob>(entry, ArchiveReader::chunk_lengths(entry));
        job->dir_path = current_path;
        if (writing && !current)
            throw ExtractError("its directory could not be created");

        if (reader.streaming() && !job->lengths.empty()) {
            std::uint64_t position = 0;
            for (std::size_t i = 0; i < job->lengths.size(); ++i) {
                streamed[entry.chunks[i]].push_back({job, i, position});
                position += job->lengths[i];
            }
            return;
        }

        if (writing)
            create_file(*job, current.get());
        if (job->lengths.empty()) {
            complete(*job);
            return;
        }
        std::uint64_t position = 0;
        for (std::size_t i = 0; i < job->lengths.size(); ++i) {
            in_flight.acquire();
            pool.submit([this, job, i, position] {
                run_chunk(*job, i, position,
                          [&] { return reader.chunk(job->entry, i, job->lengths[i], decompress); });
            });
            position += job->lengths[i];
        }
    }

    void create_file(FileJob& job, int dir)
    {
        job.name = base_name(job.entry.path);
        for (;;) {
            job.out = Fd(::openat(dir, job.name.c_str(),
                                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
            if (job.out || errno != EEXIST)
                break;
            remove_existing(dir, job.name);
        }
        if (!job.out)
            throw_errno("cannot create file");
        job.dir = Fd(::dup(dir));
    }

    // Runs on a worker thread: `load` produces the chunk's (decompressed) data.
    template <typename Load>
    void run_chunk(FileJob& job, std::size_t index, std::uint64_t position, Load load)
    {
        if (!job.failed) {
            try {
                const auto data = load();
                job.crcs[index] = crc32c(data);
                if (job.out)
                    write_all_at(job.out.get(), data, position);
            } catch (const std::exception& e) {
                job.fail(e.what());
            }
        }
        in_flight.release();
        if (--job.remaining == 0)
            complete(job);
    }

    // Stream only: reads every needed block in archive order, handing each
    // chunk that uses it to the pool.
    void run_stream()
    {
        for (const auto& [offset, refs] : streamed) {
            std::shared_ptr<const std::pair<BlockHeader, Bytes>> block;
            std::string error = stream_error;
            if (stream_error.empty()) {
                try {
                    block = std::make_shared<const std::pair<BlockHeader, Bytes>>(reader.next_block(offset));
                } catch (const TruncatedError& e) {
                    error = stream_error = e.what();
                } catch (const FormatError& e) {
                    error = e.what();    // just this block is bad; the next can still be found
                } catch (const std::exception& e) {
                    error = stream_error = e.what();    // e.g. a read error
                }
            }

            for (const auto& ref : refs) {
                auto& job = *ref.job;
                if (writing && !job.opened) {
                    job.opened = true;
                    try {
                        create_file(job, open_directory(job.dir_path, true).get());
                    } catch (const std::exception& e) {
                        job.fail(e.what());
                    }
                }
                in_flight.acquire();
                pool.submit([this, ref, block, error] {
                    const auto& job = *ref.job;
                    run_chunk(*ref.job, ref.index, ref.position, [&] {
                        if (!block)
                            throw FormatError(error);
                        return reader.decode_chunk(job.entry, block->first, block->second,
                                                   job.lengths[ref.index], decompress);
                    });
                });
            }
        }
        streamed.clear();
    }

    // Runs once all of a file's chunks are done, on whichever thread finished last.
    void complete(FileJob& job)
    {
        try {
            if (job.failed)
                throw ExtractError(job.error);
            if (decompress && job.entry.content_crc) {
                std::uint32_t crc = 0;
                for (std::size_t i = 0; i < job.crcs.size(); ++i)
                    crc = crc32c_combine(crc, job.crcs[i], job.lengths[i]);
                if (crc != *job.entry.content_crc)
                    throw FormatError("content checksum mismatch");
            }
        } catch (const std::exception& e) {
            if (job.out)
                ::unlinkat(job.dir.get(), job.name.c_str(), 0);    // don't leave a partial file
            if (!stream_error.empty() && job.error == stream_error) {
                std::lock_guard lock(mutex);
                ++lost_to_stream;
            } else {
                warn(job.entry.path + ": " + e.what());
            }
            return;
        }

        if (job.out) {
            try {
                apply_metadata(job.out.get(), job.entry);
            } catch (const std::exception& e) {
                warn(job.entry.path + ": " + e.what());
            }
            job.out = Fd();    // close now, not when the last reference goes
            job.dir = Fd();
        }
        std::lock_guard lock(mutex);
        ++stats.files;
        stats.bytes += job.entry.size.value_or(0);
    }

    void create_symlink(const std::string& dir_path, const Entry& entry)
    {
        const auto dir = open_directory(dir_path, true);
        const std::string name(base_name(entry.path));
        const auto& target = *entry.link_target;

        if (::symlinkat(target.c_str(), dir.get(), name.c_str()) != 0) {
            if (errno != EEXIST)
                throw_errno("cannot create symlink");
            remove_existing(dir.get(), name);
            if (::symlinkat(target.c_str(), dir.get(), name.c_str()) != 0)
                throw_errno("cannot create symlink");
        }
        apply_owner(dir.get(), name.c_str(), -1, entry);
        const timespec times[2] = {to_timespec(entry.atime), to_timespec(entry.mtime)};
        if (::utimensat(dir.get(), name.c_str(), times, AT_SYMLINK_NOFOLLOW) != 0)
            throw_errno("cannot set times");
    }

    ExtractStats finish()
    {
        if (reader.streaming())
            run_stream();
        pool.wait();
        if (lost_to_stream)
            warn(stream_error + "; " + std::to_string(lost_to_stream) +
                 (lost_to_stream == 1 ? " file was" : " files were") + " not extracted");
        if (!writing)
            return stats;
        current = Fd();

        for (const auto& [dir_path, entry] : symlinks) {
            try {
                create_symlink(dir_path, *entry);
            } catch (const std::exception& e) {
                warn(entry->path + ": " + e.what());
            }
        }
        for (auto it = directories.rbegin(); it != directories.rend(); ++it) {
            try {
                apply_metadata(open_directory((*it)->path, false).get(), **it);
            } catch (const std::exception& e) {
                warn((*it)->path + ": " + e.what());
            }
        }
        return stats;
    }
};

Extractor::Extractor(ArchiveReader& reader, const ExtractOptions& options)
    : impl_(std::make_unique<Impl>(reader, options))
{
}

Extractor::~Extractor() = default;

void Extractor::prepare(std::span<const DirectoryGroup> groups) { impl_->prepare(groups); }
void Extractor::enter(const DirectoryGroup& group) { impl_->enter(group); }
void Extractor::extract(const Entry& entry) { impl_->extract(entry); }
ExtractStats Extractor::finish() { return impl_->finish(); }

} // namespace zar
