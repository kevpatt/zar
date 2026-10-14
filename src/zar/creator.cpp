// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "zar/creator.hpp"

#include "zar/archive_writer.hpp"
#include "zar/compressor.hpp"
#include "zar/crc32c.hpp"
#include "zar/dedup.hpp"
#include "zar/dictionary.hpp"
#include "zar/index.hpp"
#include "zar/names.hpp"
#include "zar/policy.hpp"
#include "zar/posix.hpp"
#include "zar/thread_pool.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <unordered_map>

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

std::string without_trailing_slashes(std::string path)
{
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    return path;
}

std::string last_component(const std::string& path)
{
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The real name of a directory reached through "..".
std::string resolved_name(const fs::path& path)
{
    std::error_code ec;
    auto name = fs::weakly_canonical(path, ec).filename().native();
    if (ec || name.empty())
        throw std::runtime_error(path.string() + ": can't determine this directory's name");
    return name;
}

// ---- the creator ------------------------------------------------------------

// What a pass over the inputs produces.
enum class Pass {
    Normal,    // an archive
    Raw,       // an archive with nothing compressed, to compress whole (§5.5)
    Sample,    // nothing: just training samples
};

class Creator {
public:
    // `identity` is the archive being made, which is skipped if found among
    // the inputs; `out` may be a temporary file. Chunks that would be
    // compressed are offered to `samples`, and compressed with `dict`.
    Creator(Output& out, const Output& identity, const CreateOptions& options,
            bool check_duplicates, Pass pass, SampleCollector* samples, const Bytes* dict)
        : check_duplicates_(check_duplicates),
          pass_(pass),
          samples_(samples),
          out_(out),
          identity_(identity),
          options_(options),
          threads_(options.threads ? options.threads : default_thread_count()),
          compressor_(options.level, threads_),
          dict_(dict)
    {
        if (pass_ == Pass::Sample)
            return;    // writes nothing
        writer_.emplace(out, options.align_log2, options.head_index);
        // Neither a pipe nor a solid archive's frame can be read back; in a
        // solid archive, zstd finds repeats itself.
        dedup_ = options.dedup.value_or(true) && pass_ == Pass::Normal && writer_->can_read_back();
        if (dict) {
            dict_offset_ = writer_->write_dictionary(*dict);
            compressor_.use_dictionary(*dict);
        }
    }

    void add_input(const InputSpec& input);
    CreateStats finish();

private:
    // An archive path plus the entry flags it implies (kPathRaw).
    struct Name {
        std::string path;
        std::uint8_t flags = 0;
    };

    // A block already written.
    struct Stored {
        std::int64_t offset;
        BlockType type;
    };

    struct PendingChunk {
        std::future<CompressedChunk> chunk;    // unless `reuse`
        std::size_t entry;
        std::size_t size;    // uncompressed, if being compressed
        std::optional<ChunkHash> hash;    // to remember the block by, once written
        std::optional<Stored> reuse;      // a duplicate of this block
    };

    // A block that dedup may reuse, by the hash of its uncompressed data.
    struct Known {
        std::uint64_t sequence;    // its place among all chunks queued
        std::optional<Stored> stored;
        std::uint64_t payload_size = 0;
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

    void queue_chunk(Bytes chunk, std::size_t entry, bool compress);
    std::optional<Stored> find_duplicate(const ChunkHash& hash, ByteView chunk);
    void drain(std::size_t keep);
    void write_next();
    void warn(const std::string& message);

    // Several inputs (or Latin-1 conversion) can map two files to one path;
    // remember where each path came from to report that.
    bool check_duplicates_;
    Pass pass_;
    SampleCollector* samples_;
    std::unordered_map<std::string, std::string> origins_;

    Output& out_;
    const Output& identity_;
    const CreateOptions& options_;
    unsigned threads_;
    std::optional<ArchiveWriter> writer_;    // not for Pass::Sample
    Compressor compressor_;
    const Bytes* dict_;
    std::optional<std::int64_t> dict_offset_;
    bool dedup_ = false;
    std::unordered_map<ChunkHash, Known, ChunkHash::Hasher> known_;
    std::uint64_t queued_ = 0;     // chunks queued so far
    std::uint64_t written_ = 0;    // chunks written so far, in queue order

    std::vector<Entry> entries_;
    std::set<std::size_t> dropped_;              // entries abandoned after a read error
    std::deque<PendingChunk> pending_;           // chunks being compressed, in file order
    std::uint64_t pending_bytes_ = 0;
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

void Creator::add_input(const InputSpec& input)
{
    // As in POSIX, a trailing slash (or "/.") resolves through a symlink.
    struct stat st {};
    const int rc = input.must_be_directory ? ::stat(input.path.c_str(), &st)
                                           : ::lstat(input.path.c_str(), &st);
    if (rc != 0)
        throw std::runtime_error(input.path.string() + ": " + errno_text());
    if (input.must_be_directory && !S_ISDIR(st.st_mode))
        throw std::runtime_error(input.path.string() + ": not a directory");

    if (input.contents)
        add_directory_contents(input.path, Name{});
    else if (auto name = child_name(Name{}, input.name, input.path))
        add(input.path, *name, st);
}

void Creator::add(const fs::path& path, const Name& name, const struct stat& st)
{
    if (identity_.is_file(st.st_dev, st.st_ino))
        return;    // the archive being written, or the one it replaces

    if (check_duplicates_) {
        const auto [it, inserted] = origins_.try_emplace(name.path, path.string());
        if (!inserted)
            throw std::runtime_error(name.path + ": would be archived twice, from " + it->second +
                                     " and " + path.string());
    }

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

    if (before_open_for_test)
        before_open_for_test(path);
    // Without O_NONBLOCK, a FIFO swapped in since lstat() would block forever.
    const UniqueFd fd(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (!fd) {
        warn(path.string() + ": " + errno_text());
        return;
    }
    struct stat opened {};
    if (::fstat(fd.get(), &opened) != 0 || !S_ISREG(opened.st_mode) || opened.st_dev != st.st_dev ||
        opened.st_ino != st.st_ino) {
        warn(path.string() + ": skipped; it was replaced while being archived");
        return;
    }
    if (options_.on_entry)
        options_.on_entry(name.path);

    // Chunk block offsets are filled in as the compressed blocks are written.
    const std::size_t index = entries_.size();
    entries_.push_back(make_entry(EntryType::File, name, opened));

    const bool compressible = options_.compress_all || !is_precompressed(name.path);
    const bool compress = compressible && pass_ == Pass::Normal;
    const auto expected = static_cast<std::uint64_t>(opened.st_size);
    const bool contiguous = options_.contiguous_limit && expected <= *options_.contiguous_limit;
    const auto chunk_size = contiguous ? std::max<std::uint64_t>(expected, 1) : options_.chunk_size;
    std::uint64_t total = 0;
    std::uint64_t chunk_count = 0;
    std::uint32_t crc = 0;
    const auto read = [&](std::span<std::uint8_t> buffer) -> std::optional<std::size_t> {
        try {
            return read_full(fd.get(), buffer, path.string());
        } catch (const std::system_error& e) {
            warn(std::string(e.what()) + "; skipped");
            dropped_.insert(index);
            return std::nullopt;
        }
    };

    // Archive exactly the size stat() reported, so every chunk but the last is
    // full (§6.5) even if the file changes while we read it.
    while (total < expected) {
        const auto want = static_cast<std::size_t>(std::min(chunk_size, expected - total));
        Bytes chunk(want);
        const auto n = read(chunk);
        if (!n)
            return;
        chunk.resize(*n);
        if (!chunk.empty()) {
            crc = crc32c(crc, chunk);
            total += chunk.size();
            ++chunk_count;
            if (samples_ && compressible)
                samples_->add(chunk);
            if (pass_ != Pass::Sample)
                queue_chunk(std::move(chunk), index, compress);
        }
        if (*n < want) {
            warn(path.string() + ": file shrank while being read; archived " +
                 std::to_string(total) + " bytes");
            break;
        }
    }
    const auto grew = [&] {
        try {
            std::uint8_t probe;
            return read_full(fd.get(), {&probe, 1}, path.string()) > 0;
        } catch (const std::system_error&) {
            return false;    // it was read in full; this was only a check
        }
    };
    if (total == expected && grew())
        warn(path.string() + ": file grew while being read; archived the first " +
             std::to_string(total) + " bytes");

    auto& entry = entries_[index];
    if (total > 0) {
        entry.size = total;
        entry.content_crc = crc;
        if (chunk_count > 1)
            entry.chunk_size = chunk_size;
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

// Hands a chunk to the compressor or, if it duplicates a block already in
// the archive, queues a reference to that block instead.
void Creator::queue_chunk(Bytes chunk, std::size_t entry, bool compress)
{
    std::optional<ChunkHash> hash;
    if (dedup_) {
        hash = hash_chunk(chunk);
        if (const auto stored = find_duplicate(*hash, chunk)) {
            stats_.deduplicated_bytes += chunk.size();
            pending_.push_back({{}, entry, 0, std::nullopt, stored});
            ++queued_;
            return;
        }
        // Remember it, unless a different chunk with the same hash came first.
        if (!known_.try_emplace(*hash, Known{queued_, std::nullopt}).second)
            hash.reset();
    }

    const auto size = chunk.size();
    pending_.push_back({compressor_.submit(std::move(chunk), compress), entry, size, hash, std::nullopt});
    pending_bytes_ += size;
    ++queued_;
    drain(threads_ * 4);
}

// A block already written (or queued) whose contents equal `chunk`.
std::optional<Creator::Stored> Creator::find_duplicate(const ChunkHash& hash, ByteView chunk)
{
    const auto it = known_.find(hash);
    if (it == known_.end())
        return std::nullopt;
    while (written_ <= it->second.sequence)    // it's still being compressed
        write_next();

    // Equal hashes are very likely equal contents, but compare to be sure.
    const auto& known = it->second;
    if (!known.stored)
        return std::nullopt;
    const auto [offset, type] = *known.stored;
    if (type == BlockType::DataRaw && known.payload_size != chunk.size())
        return std::nullopt;
    auto payload = writer_->read_payload(offset, static_cast<std::size_t>(known.payload_size));
    if (type == BlockType::DataZstd) {
        auto data = zstd_decompress(payload, chunk.size(), dict_ ? ByteView(*dict_) : ByteView{});
        if (!data)
            return std::nullopt;
        payload = std::move(*data);
    }
    if (!std::ranges::equal(payload, chunk))
        return std::nullopt;
    return known.stored;
}

// Writes finished chunks, in order, until at most `keep` remain in flight,
// holding no more than `keep` full chunks' worth of data (contiguous files
// can be much larger than a chunk).
void Creator::drain(std::size_t keep)
{
    const auto budget = keep * options_.chunk_size;
    while (!pending_.empty() && (pending_.size() > keep || pending_bytes_ > budget))
        write_next();
}

// Writes the oldest queued chunk, waiting for its compression if need be.
void Creator::write_next()
{
    auto pending = std::move(pending_.front());
    pending_.pop_front();
    pending_bytes_ -= pending.size;
    ++written_;

    Stored stored{};
    if (pending.reuse) {
        stored = *pending.reuse;
    } else {
        const auto chunk = pending.chunk.get();
        stored = {writer_->write_block(chunk.type, chunk.payload), chunk.type};
        if (pending.hash) {
            auto& known = known_.at(*pending.hash);
            known.stored = stored;
            known.payload_size = chunk.payload.size();
        }
    }

    auto& entry = entries_[pending.entry];
    entry.chunks.push_back(stored.offset);
    if (stored.type == BlockType::DataZstd && dict_offset_) {
        entry.dict = dict_offset_;
        stats_.dictionary = true;
    }
}

CreateStats Creator::finish()
{
    if (pass_ == Pass::Sample)
        return stats_;
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

    const auto packed = pass_ == Pass::Raw ? Bytes{} : zstd_compress(index, options_.level);
    if (!packed.empty() && packed.size() < index.size())
        writer_->finish(BlockType::IndexZstd, packed);
    else
        writer_->finish(BlockType::IndexRaw, index);

    stats_.archive_bytes = out_.position();
    return stats_;
}

} // namespace

InputSpec parse_input(const fs::path& input)
{
    const auto& given = input.native();
    if (given.empty())
        throw std::runtime_error("empty path");

    const auto path = without_trailing_slashes(given);
    const auto last = last_component(path);
    InputSpec spec{input, {}, false, path.size() < given.size()};

    if (last == ".") {
        spec.contents = true;
        spec.must_be_directory = true;
    } else if (path == "/") {
        throw std::runtime_error("/: can't archive the root directory by name; use /. for its contents");
    } else {
        spec.name = last == ".." ? resolved_name(path) : last;
    }
    return spec;
}

fs::path default_archive_path(const InputSpec& input)
{
    if (!input.contents)
        return input.name + ".zar";

    // "proj/." is named after proj; "." and ".." after the directories they are.
    auto path = without_trailing_slashes(input.path.native());
    while (path != "." && last_component(path) == ".")
        path = without_trailing_slashes(path.substr(0, path.size() - 1));
    if (path == "/")
        throw std::runtime_error("can't name an archive of the root directory; use -o");
    const auto last = last_component(path);
    return (last == "." || last == ".." ? resolved_name(path) : last) + ".zar";
}

namespace {

CreateStats create(Output& out, const Output& identity, std::span<const InputSpec> specs,
                   const CreateOptions& options, Pass pass, SampleCollector* samples = nullptr,
                   const Bytes* dict = nullptr)
{
    const bool check_duplicates = specs.size() > 1 || options.names == NameMode::Latin1;
    Creator creator(out, identity, options, check_duplicates, pass, samples, dict);
    for (const auto& spec : specs)
        creator.add_input(spec);
    return creator.finish();
}

// What a trial pass reports, held back until it's known whether the result
// is kept: whichever pass is kept reports for real.
class HeldReports {
public:
    CreateOptions quiet(const CreateOptions& options)
    {
        auto quiet = options;
        quiet.on_warning = [this](std::string_view m) { warnings_.emplace_back(m); };
        quiet.on_entry = [this](std::string_view m) { entries_.emplace_back(m); };
        return quiet;
    }

    void replay(const CreateOptions& options) const
    {
        for (const auto& entry : entries_)
            if (options.on_entry)
                options.on_entry(entry);
        for (const auto& warning : warnings_)
            if (options.on_warning)
                options.on_warning(warning);
    }

private:
    std::vector<std::string> warnings_, entries_;
};

std::optional<Bytes> train_dictionary(Output& out, std::span<const InputSpec> specs,
                                      const CreateOptions& options, std::uint64_t dict_size)
{
    HeldReports ignored;
    SampleCollector samples;
    create(out, out, specs, ignored.quiet(options), Pass::Sample, &samples);
    return samples.train(dict_size);
}

// A solid archive's compressed frame, in a temporary file.
struct SolidFrame {
    Output frame;
    std::uint32_t crc;
    CreateStats stats;
    HeldReports reports;
};

// Builds the archive with nothing compressed, compressing it whole on the
// fly, and gives up as soon as that passes the limit. Returns nullopt then.
// Training samples are collected on the way, though they're incomplete if
// it gives up.
std::optional<SolidFrame> try_solid(Output& out, std::span<const InputSpec> specs,
                                    const CreateOptions& options, SampleCollector* samples)
{
    SolidFrame solid{out.make_spool(), 0, {}, {}};
    auto inner = solid.reports.quiet(options);
    inner.align_log2 = 0;    // padding would only be compressed away
    inner.head_index = false;
    // The limit is on the whole archive: the frame plus its framing.
    const auto overhead = solid_archive_size(0);
    const auto limit = *options.solid_limit;
    if (limit <= overhead)
        return std::nullopt;
    try {
        const auto threads = options.threads ? options.threads : default_thread_count();
        auto sink = Output::compressing(solid.frame, options.level, threads, limit - overhead, out);
        solid.stats = create(sink, out, specs, inner, Pass::Raw, samples);
        sink.commit();
        solid.crc = sink.compressed_crc();
    } catch (const SizeLimitExceeded&) {
        return std::nullopt;
    }
    if (solid_archive_size(solid.frame.position()) > limit)    // rounding in the framing
        return std::nullopt;
    return solid;
}

// With --solid: the solid archive if it fits, or a dictionary-compressed
// archive if that's nearly as small. Returns nullopt, having written
// nothing, if neither is kept.
std::optional<CreateStats> try_small(Output& out, std::span<const InputSpec> specs,
                                     const CreateOptions& options)
{
    SampleCollector samples;
    auto solid = try_solid(out, specs, options, options.dict_over_solid ? &samples : nullptr);
    if (!solid)
        return std::nullopt;

    const auto solid_size = solid_archive_size(solid->frame.position());
    const auto dict = options.dict_over_solid
                          ? samples.train(options.dict_size.value_or(kDefaultDictSize))
                          : std::nullopt;
    if (dict) {
        HeldReports reports;
        auto spool = out.make_spool();
        auto stats = create(spool, out, specs, reports.quiet(options), Pass::Normal, nullptr, &*dict);
        const auto size = spool.position();
        if (size <= *options.solid_limit && size * 100 <= solid_size * options.dict_over_solid) {
            spool.copy_to(out);
            reports.replay(options);
            stats.archive_bytes = out.position();
            return stats;
        }
    }

    write_solid_archive(out, solid->frame, solid->crc);
    solid->reports.replay(options);
    solid->stats.solid = true;
    solid->stats.archive_bytes = out.position();
    return solid->stats;
}

} // namespace

CreateStats create_archive(Output& out, std::span<const fs::path> inputs,
                           const CreateOptions& options)
{
    std::vector<InputSpec> specs;    // check every name before writing anything
    for (const auto& input : inputs)
        specs.push_back(parse_input(input));

    // Checked first, so nothing is written.
    if (options.dedup == true && !options.head_index && !out.readable())
        throw std::runtime_error("--dedup=on reads back what it has written, so it can't write "
                                 "to a pipe (unless with --head-index)");

    if (options.solid_limit)
        if (auto stats = try_small(out, specs, options))
            return *stats;

    std::optional<Bytes> dict;
    if (options.dict_size)
        dict = train_dictionary(out, specs, options, *options.dict_size);
    return create(out, out, specs, options, Pass::Normal, nullptr, dict ? &*dict : nullptr);
}

} // namespace zar
