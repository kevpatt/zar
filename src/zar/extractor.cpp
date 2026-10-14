// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "zar/extractor.hpp"

#include "zar/crc32c.hpp"
#include "zar/posix.hpp"
#include "zar/thread_pool.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace zar {
namespace {

struct ExtractError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A block that a broken stream will never deliver.
struct StreamLost : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void throw_path_error(const std::string& what)
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

timespec to_timespec(const std::optional<Timestamp>& t)
{
    if (!t)
        return {0, UTIME_OMIT};
    return {static_cast<time_t>(t->seconds), static_cast<long>(t->nanoseconds.value_or(0))};
}

// Names win over ids, so ownership carries across systems.
std::optional<uid_t> owner_of(const Entry& e)
{
    if (e.uname)
        if (const auto uid = user_id(*e.uname))
            return uid;
    if (e.uid)
        return static_cast<uid_t>(*e.uid);
    return std::nullopt;
}

std::optional<gid_t> group_of(const Entry& e)
{
    if (e.gname)
        if (const auto gid = group_id(*e.gname))
            return gid;
    if (e.gid)
        return static_cast<gid_t>(*e.gid);
    return std::nullopt;
}

} // namespace

mode_t permitted_mode(std::uint16_t mode, bool owner_restored)
{
    return mode & (owner_restored ? 07777 : 01777);
}

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

        // Records the first failure. `lost` marks one caused by a broken
        // stream, which is reported in a single summary instead.
        void fail(std::string_view what, bool lost = false)
        {
            std::lock_guard lock(mutex);
            if (!failed.exchange(true)) {
                error = what;
                lost_to_stream = lost;
            }
        }

        const Entry& entry;
        std::vector<std::uint64_t> lengths;
        std::vector<std::uint32_t> crcs;    // per chunk, combined at the end
        std::atomic<std::size_t> remaining;
        std::atomic<bool> failed = false;
        std::mutex mutex;
        std::string error;
        bool lost_to_stream = false;
        std::string dir_path;    // archive path of its directory
        std::string name;
        bool opened = false;     // stream: creation was attempted
        UniqueFd dir;    // its directory, so a failed file can be removed
        UniqueFd out;
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
    std::string stream_error;    // main thread only
    std::uint64_t lost_to_stream = 0;

    ArchiveReader& reader;
    const ExtractOptions& options;
    const bool writing = options.mode == ExtractMode::Extract;
    const bool decompress = options.mode != ExtractMode::Scan;
    const bool privileged = ::geteuid() == 0;
    const unsigned threads = options.threads ? options.threads : default_thread_count();

    std::mutex mutex;    // guards stats and warnings, which workers update too
    ExtractStats stats;

    UniqueFd root;
    UniqueFd current;    // directory of the group being extracted
    std::string current_path;

    // Created last, so nothing can be written through them (§7).
    std::vector<std::pair<std::string, const Entry*>> symlinks;
    // Linked once their targets are complete.
    std::vector<std::pair<std::string, const Entry*>> hardlinks;
    std::set<std::string_view> selected_files;    // what hard links may link to
    std::set<std::string_view> extracted;         // files extracted intact
    // Metadata applied last, deepest first, so writing contents doesn't disturb it.
    std::vector<const Entry*> directories;

    // Bounds the chunks in memory, by count (and so the open files) and by
    // size: a chunk can be as large as a whole file (zar --contiguous).
    static constexpr std::uint64_t kBytesPerThread = 16 << 20;
    Budget in_flight{std::size_t{threads} * 4, std::uint64_t{threads} * kBytesPerThread};
    ThreadPool pool{threads};    // last: destroyed first, while what its jobs use still exists

    Impl(ArchiveReader& r, const ExtractOptions& o) : reader(r), options(o) {}

    // The destination is created only once there's something to put in it,
    // so a refused extraction leaves no trace.
    int root_fd()
    {
        if (!root) {
            std::error_code ec;
            std::filesystem::create_directories(options.directory, ec);
            root = UniqueFd(::open(options.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            if (!root)
                throw std::system_error(errno, std::generic_category(), options.directory.string());
        }
        return root.get();
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
    UniqueFd open_directory(std::string_view path, bool create)
    {
        UniqueFd dir(::dup(root_fd()));
        while (!path.empty()) {
            const auto slash = path.find('/');
            const std::string component(path.substr(0, slash));
            path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);

            constexpr int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
            int fd = ::openat(dir.get(), component.c_str(), flags);
            if (fd < 0 && errno == ENOENT && create) {
                if (::mkdirat(dir.get(), component.c_str(), 0777) != 0 && errno != EEXIST)
                    throw_path_error(component);
                fd = ::openat(dir.get(), component.c_str(), flags);
            }
            if (fd < 0)
                throw_path_error(component);
            dir = UniqueFd(fd);
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
            throw_path_error("cannot replace existing file");
    }

    bool exists(std::string_view path) const
    {
        std::error_code ec;
        return std::filesystem::exists(std::filesystem::symlink_status(options.directory / path, ec));
    }

    void prepare(std::span<const DirectoryGroup> groups)
    {
        for (const auto& group : groups)
            for (const auto* member : group.members)
                if (member->type == EntryType::File)
                    selected_files.insert(member->path);

        if (decompress) {
            std::uint64_t total = 0;
            for (const auto& group : groups)
                for (const auto* member : group.members)
                    total += std::min(member->size.value_or(0), ~std::uint64_t{0} - total);
            check_size(total);
        }
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
            // Only directories extracted whole: those with their own entry,
            // which selection includes only with everything below them. Not
            // ancestors of a selection, which would take unrelated files
            // with them. remove_all() never follows symlinks.
            std::vector<std::string_view> replaced;
            for (const auto& group : groups) {
                if (!group.entry || !exists(group.path))
                    continue;
                const auto inside = [&](std::string_view dir) {
                    return group.path.starts_with(dir) && group.path[dir.size()] == '/';
                };
                if (std::ranges::any_of(replaced, inside))
                    continue;    // already gone with its parent
                std::error_code ec;
                std::filesystem::remove_all(options.directory / group.path, ec);
                if (ec)
                    throw ExtractError(group.path + ": cannot replace: " + ec.message());
                replaced.push_back(group.path);
            }
        }
    }

    // Refuses to decompress more than the size limit or, when extracting
    // without one, more than the destination has room for: a small archive
    // can describe far more data than that.
    void check_size(std::uint64_t total) const
    {
        const auto describe = [&](std::string_view limit, std::uint64_t bytes) {
            return "the files total " + std::to_string(total) + " bytes, more than " + std::string(limit) +
                   " (" + std::to_string(bytes) + " bytes)";
        };
        if (options.size_limit) {
            if (total > *options.size_limit)
                throw ExtractError(describe("the size limit", *options.size_limit));
            return;
        }
        if (!writing)
            return;
        auto dir = std::filesystem::absolute(options.directory);
        while (!std::filesystem::exists(dir) && dir.has_relative_path())
            dir = dir.parent_path();    // it will be created inside this
        if (const auto room = free_space(dir); room && total > *room)
            throw ExtractError(describe("the free space in " + dir.string(), *room) +
                               "; set --max-size to try anyway");
    }

    // Returns whether the archive's owner was applied.
    bool apply_owner(int dir, const char* name, int fd, const Entry& e) const
    {
        if (!privileged)
            return false;
        const auto uid = owner_of(e);
        const auto gid = group_of(e);
        if (!uid && !gid)
            return false;
        const auto u = uid.value_or(static_cast<uid_t>(-1));
        const auto g = gid.value_or(static_cast<gid_t>(-1));
        const int rc = fd >= 0 ? ::fchown(fd, u, g) : ::fchownat(dir, name, u, g, AT_SYMLINK_NOFOLLOW);
        if (rc != 0)
            throw_path_error("cannot set owner");
        return true;
    }

    void apply_metadata(int fd, const Entry& e) const
    {
        const bool owned = apply_owner(-1, nullptr, fd, e);    // before chmod: chown clears set-id bits
        if (e.mode) {
            if (::fchmod(fd, permitted_mode(*e.mode, owned)) != 0)
                throw_path_error("cannot set permissions");
        }
        const timespec times[2] = {to_timespec(e.atime), to_timespec(e.mtime)};
        if (::futimens(fd, times) != 0)
            throw_path_error("cannot set times");
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
            current = UniqueFd();
            warn(group.path + ": " + e.what());
        }
    }

    void extract(const Entry& entry)
    {
        try {
            switch (entry.type) {
            case EntryType::Hardlink:
                if (writing && options.hard_links && entry.link_target &&
                    *entry.link_target != entry.path && selected_files.contains(*entry.link_target)) {
                    hardlinks.emplace_back(current_path, &entry);
                    break;
                }
                extract_file(entry);    // as an independent copy
                break;
            case EntryType::File:
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
            in_flight.acquire(job->lengths[i]);
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
            job.out = UniqueFd(::openat(dir, job.name.c_str(),
                                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
            if (job.out || errno != EEXIST)
                break;
            remove_existing(dir, job.name);
        }
        if (!job.out)
            throw_path_error("cannot create file");
        job.dir = UniqueFd(::dup(dir));
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
                    pwrite_all(job.out.get(), data, position, "cannot write");
            } catch (const StreamLost& e) {
                job.fail(e.what(), true);
            } catch (const std::exception& e) {
                job.fail(e.what());
            }
        }
        in_flight.release(job.lengths[index]);
        if (--job.remaining == 0)
            complete(job);
    }

    // Stream only: reads every needed block in archive order, handing each
    // chunk that uses it to the pool.
    void run_stream()
    {
        for (const auto& [offset, refs] : streamed) {
            std::shared_ptr<const std::pair<BlockHeader, Bytes>> block;
            std::string error;    // why `block` is missing
            if (stream_error.empty()) {
                try {
                    block = std::make_shared<const std::pair<BlockHeader, Bytes>>(reader.next_block(offset));
                } catch (const FormatError& e) {
                    error = e.what();    // just this block is bad; the next can still be found
                    if (dynamic_cast<const TruncatedError*>(&e))
                        stream_error = error;
                } catch (const std::exception& e) {
                    stream_error = e.what();    // e.g. a read error
                }
            }
            const bool lost = !block && !stream_error.empty();
            if (lost)
                error = stream_error;

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
                in_flight.acquire(job.lengths[ref.index]);
                pool.submit([this, ref, block, error, lost] {
                    const auto& job = *ref.job;
                    run_chunk(*ref.job, ref.index, ref.position, [&] {
                        if (lost)
                            throw StreamLost(error);
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
                if (!crc_matches(*job.entry.content_crc, crc))
                    throw FormatError("content checksum mismatch");
            }
        } catch (const std::exception& e) {
            if (job.out)
                ::unlinkat(job.dir.get(), job.name.c_str(), 0);    // don't leave a partial file
            if (job.lost_to_stream) {
                std::lock_guard lock(mutex);
                ++lost_to_stream;
            } else {
                warn(job.entry.path + ": " + e.what());
            }
            return;
        }

        const bool written = static_cast<bool>(job.out);
        if (written) {
            try {
                apply_metadata(job.out.get(), job.entry);
            } catch (const std::exception& e) {
                warn(job.entry.path + ": " + e.what());
            }
            job.out.reset();    // close now, not when the last reference goes
            job.dir.reset();
        }
        std::lock_guard lock(mutex);
        ++stats.files;
        stats.bytes += job.entry.size.value_or(0);
        if (written)
            extracted.insert(job.entry.path);
    }

    // Links `entry` to its already-extracted target.
    void create_hardlink(const std::string& dir_path, const Entry& entry)
    {
        const auto& target = *entry.link_target;
        if (!extracted.contains(target))
            throw ExtractError("its target was not extracted");
        const auto from = open_directory(parent_of(target), false);
        const auto to = open_directory(dir_path, true);
        const std::string from_name(base_name(target));
        const std::string name(base_name(entry.path));

        // Without AT_SYMLINK_FOLLOW, linkat() never follows a symlink.
        if (::linkat(from.get(), from_name.c_str(), to.get(), name.c_str(), 0) != 0) {
            if (errno != EEXIST)
                throw_path_error("cannot create hard link");
            remove_existing(to.get(), name);
            if (::linkat(from.get(), from_name.c_str(), to.get(), name.c_str(), 0) != 0)
                throw_path_error("cannot create hard link");
        }
    }

    // Hard links whose targets are now complete. One that can't be made is
    // extracted as a copy instead, unless the stream has moved past its data.
    void link_hardlinks()
    {
        std::vector<std::pair<std::string, const Entry*>> copies;
        for (const auto& [dir_path, entry] : hardlinks) {
            try {
                create_hardlink(dir_path, *entry);
                ++stats.hardlinks;
            } catch (const std::exception& e) {
                if (reader.streaming())
                    warn(entry->path + ": " + e.what());
                else
                    copies.emplace_back(dir_path, entry);
            }
        }
        for (const auto& [dir_path, entry] : copies) {
            try {
                current_path = dir_path;
                current = open_directory(dir_path, true);
                extract_file(*entry);
            } catch (const std::exception& e) {
                warn(entry->path + ": " + e.what());
            }
        }
        pool.wait();
    }

    void create_symlink(const std::string& dir_path, const Entry& entry)
    {
        const auto dir = open_directory(dir_path, true);
        const std::string name(base_name(entry.path));
        const auto& target = *entry.link_target;

        if (::symlinkat(target.c_str(), dir.get(), name.c_str()) != 0) {
            if (errno != EEXIST)
                throw_path_error("cannot create symlink");
            remove_existing(dir.get(), name);
            if (::symlinkat(target.c_str(), dir.get(), name.c_str()) != 0)
                throw_path_error("cannot create symlink");
        }
        apply_owner(dir.get(), name.c_str(), -1, entry);
        const timespec times[2] = {to_timespec(entry.atime), to_timespec(entry.mtime)};
        if (::utimensat(dir.get(), name.c_str(), times, AT_SYMLINK_NOFOLLOW) != 0)
            throw_path_error("cannot set times");
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
        link_hardlinks();
        current = UniqueFd();

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
