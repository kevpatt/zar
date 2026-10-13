#include "zar/index.hpp"

#include "zar/names.hpp"

#include <limits>
#include <set>

namespace zar {
namespace {

constexpr std::uint32_t kNanosPerSecond = 1'000'000'000;

// ---- encoding ---------------------------------------------------------------

class FieldWriter {
public:
    explicit FieldWriter(Bytes& out) : out_(out) {}

    void bytes(Tag tag, std::string_view value)
    {
        start(tag, value.size());
        out_.insert(out_.end(), value.begin(), value.end());
    }

    template <std::unsigned_integral T>
    void fixed(Tag tag, T value)
    {
        start(tag, sizeof(T));
        append_le(out_, value);
    }

    // 4 bytes when the value fits, otherwise 8 (§6.4).
    void unsigned_number(Tag tag, std::uint64_t value)
    {
        if (value <= std::numeric_limits<std::uint32_t>::max())
            fixed(tag, static_cast<std::uint32_t>(value));
        else
            fixed(tag, value);
    }

    void signed_number(Tag tag, std::int64_t value)
    {
        if (value >= std::numeric_limits<std::int32_t>::min() &&
            value <= std::numeric_limits<std::int32_t>::max())
            fixed(tag, static_cast<std::uint32_t>(static_cast<std::int32_t>(value)));
        else
            fixed(tag, static_cast<std::uint64_t>(value));
    }

    void offsets(Tag tag, const std::vector<std::int64_t>& values)
    {
        start(tag, values.size() * 8);
        for (const auto value : values)
            append_le(out_, static_cast<std::uint64_t>(value));
    }

    void time(Tag seconds_tag, Tag nanos_tag, const Timestamp& t)
    {
        signed_number(seconds_tag, t.seconds);
        if (t.nanoseconds)
            fixed(nanos_tag, *t.nanoseconds);
    }

private:
    void start(Tag tag, std::size_t length)
    {
        append_varint(out_, static_cast<std::uint64_t>(tag));
        append_varint(out_, length);
    }

    Bytes& out_;
};

// ---- decoding ---------------------------------------------------------------

std::uint64_t as_unsigned(ByteView value)
{
    switch (value.size()) {
    case 4: return load_le<std::uint32_t>(value.data());
    case 8: return load_le<std::uint64_t>(value.data());
    default: throw FormatError("numeric field must be 4 or 8 bytes");
    }
}

std::int64_t as_signed(ByteView value)
{
    switch (value.size()) {
    case 4: return static_cast<std::int32_t>(load_le<std::uint32_t>(value.data()));
    case 8: return static_cast<std::int64_t>(load_le<std::uint64_t>(value.data()));
    default: throw FormatError("numeric field must be 4 or 8 bytes");
    }
}

template <std::unsigned_integral T>
T as_fixed(ByteView value)
{
    if (value.size() != sizeof(T))
        throw FormatError("field must be " + std::to_string(sizeof(T)) + " bytes");
    return load_le<T>(value.data());
}

std::int64_t as_offset(ByteView value)
{
    return static_cast<std::int64_t>(as_fixed<std::uint64_t>(value));
}

std::vector<std::int64_t> as_offsets(ByteView value)
{
    if (value.size() % 8 != 0)
        throw FormatError("offset list length is not a multiple of 8");
    std::vector<std::int64_t> offsets;
    offsets.reserve(value.size() / 8);
    for (std::size_t i = 0; i < value.size(); i += 8)
        offsets.push_back(static_cast<std::int64_t>(load_le<std::uint64_t>(&value[i])));
    return offsets;
}

std::string as_string(ByteView value)
{
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::uint32_t as_nanos(ByteView value)
{
    const auto nanos = as_fixed<std::uint32_t>(value);
    if (nanos >= kNanosPerSecond)
        throw FormatError("nanoseconds field out of range");
    return nanos;
}

bool is_known(std::uint8_t type)
{
    return type >= static_cast<std::uint8_t>(EntryType::File) &&
           type <= static_cast<std::uint8_t>(EntryType::Hardlink);
}

// Sub-second fields are only meaningful next to their seconds field (§6.5).
void attach_nanos(std::optional<Timestamp>& time, std::optional<std::uint32_t> nanos)
{
    if (time && nanos)
        time->nanoseconds = nanos;
}

Entry decode_entry(std::uint8_t type, std::uint8_t flags, ByteView body)
{
    if (flags & ~entry_flags::kPathRaw)
        throw FormatError("index entry has unknown flags set");

    Entry entry;
    entry.type = static_cast<EntryType>(type);
    entry.flags = flags;

    bool have_path = false;
    std::optional<std::uint32_t> mtime_ns, atime_ns, ctime_ns, btime_ns;
    std::set<Tag> seen;

    while (!body.empty()) {
        const auto tag = static_cast<Tag>(read_varint(body));
        const auto length = read_varint(body);
        if (length > body.size())
            throw FormatError("index field overruns its entry");
        const auto value = body.first(static_cast<std::size_t>(length));
        body = body.subspan(static_cast<std::size_t>(length));

        if (!seen.insert(tag).second)
            throw FormatError("index entry repeats a field");

        switch (tag) {
        case Tag::Path:
            entry.path = as_string(value);
            have_path = true;
            break;
        case Tag::Size: entry.size = as_unsigned(value); break;
        case Tag::ChunkSize: entry.chunk_size = as_unsigned(value); break;
        case Tag::Chunks: entry.chunks = as_offsets(value); break;
        case Tag::Dict: entry.dict = as_offset(value); break;
        case Tag::ContentCrc: entry.content_crc = as_fixed<std::uint32_t>(value); break;
        case Tag::LinkTarget: entry.link_target = as_string(value); break;
        case Tag::Subindex: entry.subindex = as_offset(value); break;
        case Tag::Mode: entry.mode = as_fixed<std::uint16_t>(value); break;
        case Tag::Uid: entry.uid = as_unsigned(value); break;
        case Tag::Gid: entry.gid = as_unsigned(value); break;
        case Tag::Uname: entry.uname = as_string(value); break;
        case Tag::Gname: entry.gname = as_string(value); break;
        case Tag::Mtime: entry.mtime = Timestamp{as_signed(value), {}}; break;
        case Tag::Atime: entry.atime = Timestamp{as_signed(value), {}}; break;
        case Tag::Ctime: entry.ctime = Timestamp{as_signed(value), {}}; break;
        case Tag::Btime: entry.btime = Timestamp{as_signed(value), {}}; break;
        case Tag::MtimeNsec: mtime_ns = as_nanos(value); break;
        case Tag::AtimeNsec: atime_ns = as_nanos(value); break;
        case Tag::CtimeNsec: ctime_ns = as_nanos(value); break;
        case Tag::BtimeNsec: btime_ns = as_nanos(value); break;
        default: break;    // unknown tag: skip (§6.1)
        }
    }

    if (!have_path)
        throw FormatError("index entry has no path");
    if (!(flags & entry_flags::kPathRaw) && !is_valid_utf8(entry.path))
        throw FormatError("index entry path is not valid UTF-8");
    if (!is_valid_archive_path(entry.path))
        throw FormatError("index entry has an invalid path");

    attach_nanos(entry.mtime, mtime_ns);
    attach_nanos(entry.atime, atime_ns);
    attach_nanos(entry.ctime, ctime_ns);
    attach_nanos(entry.btime, btime_ns);
    return entry;
}

} // namespace

void encode_entry(Bytes& out, const Entry& entry)
{
    Bytes body;
    FieldWriter field(body);

    field.bytes(Tag::Path, entry.path);
    if (entry.size)
        field.unsigned_number(Tag::Size, *entry.size);
    if (entry.chunk_size)
        field.unsigned_number(Tag::ChunkSize, *entry.chunk_size);
    if (!entry.chunks.empty())
        field.offsets(Tag::Chunks, entry.chunks);
    if (entry.dict)
        field.fixed(Tag::Dict, static_cast<std::uint64_t>(*entry.dict));
    if (entry.content_crc)
        field.fixed(Tag::ContentCrc, *entry.content_crc);
    if (entry.link_target)
        field.bytes(Tag::LinkTarget, *entry.link_target);
    if (entry.subindex)
        field.fixed(Tag::Subindex, static_cast<std::uint64_t>(*entry.subindex));
    if (entry.mode)
        field.fixed(Tag::Mode, *entry.mode);
    if (entry.uid)
        field.unsigned_number(Tag::Uid, *entry.uid);
    if (entry.gid)
        field.unsigned_number(Tag::Gid, *entry.gid);
    if (entry.uname)
        field.bytes(Tag::Uname, *entry.uname);
    if (entry.gname)
        field.bytes(Tag::Gname, *entry.gname);
    if (entry.mtime)
        field.time(Tag::Mtime, Tag::MtimeNsec, *entry.mtime);
    if (entry.atime)
        field.time(Tag::Atime, Tag::AtimeNsec, *entry.atime);
    if (entry.ctime)
        field.time(Tag::Ctime, Tag::CtimeNsec, *entry.ctime);
    if (entry.btime)
        field.time(Tag::Btime, Tag::BtimeNsec, *entry.btime);

    out.push_back(static_cast<std::uint8_t>(entry.type));
    out.push_back(entry.flags);
    append_varint(out, body.size());
    out.insert(out.end(), body.begin(), body.end());
}

std::vector<Entry> decode_index(ByteView payload)
{
    std::vector<Entry> entries;
    while (!payload.empty()) {
        if (payload.size() < 2)
            throw FormatError("truncated index entry");
        const std::uint8_t type = payload[0];
        const std::uint8_t flags = payload[1];
        payload = payload.subspan(2);

        const auto length = read_varint(payload);
        if (length > payload.size())
            throw FormatError("index entry overruns its block");
        const auto body = payload.first(static_cast<std::size_t>(length));
        payload = payload.subspan(static_cast<std::size_t>(length));

        if (is_known(type))
            entries.push_back(decode_entry(type, flags, body));
    }
    return entries;
}

} // namespace zar
