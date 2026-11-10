// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "zar/index.hpp"

#include "zar/fields.hpp"
#include "zar/names.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <utility>

namespace zar {
namespace {

constexpr std::uint32_t kNanosPerSecond = 1'000'000'000;

// An algorithm byte, then the digests (§6.7).
void write_digests(FieldWriter& field, Tag tag, std::span<const Digest> digests)
{
    Bytes value{static_cast<std::uint8_t>(HashAlgorithm::Blake3)};
    for (const auto& digest : digests)
        value.insert(value.end(), digest.begin(), digest.end());
    field.bytes(tag, ByteView(value));
}

// The digests in a hash field, or nullopt if its algorithm isn't one zar
// knows.
std::optional<std::vector<Digest>> as_digests(ByteView value)
{
    if (value.empty())
        throw FormatError("empty hash field");
    if (value[0] != static_cast<std::uint8_t>(HashAlgorithm::Blake3))
        return std::nullopt;
    value = value.subspan(1);
    if (value.size() % kDigestSize != 0)
        throw FormatError("hash field length is not a whole number of digests");
    std::vector<Digest> digests(value.size() / kDigestSize);
    for (std::size_t i = 0; i < digests.size(); ++i)
        std::copy_n(value.begin() + static_cast<std::ptrdiff_t>(i * kDigestSize), kDigestSize, digests[i].begin());
    return digests;
}

// One digest, as DICT_HASH and SUBINDEX_HASH hold.
std::optional<Digest> as_digest(ByteView value, bool& unknown)
{
    const auto digests = as_digests(value);
    if (!digests) {
        unknown = true;
        return std::nullopt;
    }
    if (digests->size() != 1)
        throw FormatError("hash field must hold one digest");
    return digests->front();
}

// A time and, if it has one, its sub-second part (§6.5).
void write_time(FieldWriter& field, Tag seconds_tag, Tag nanos_tag, const Timestamp& t)
{
    field.signed_number(seconds_tag, t.seconds);
    if (t.nanoseconds)
        field.fixed(nanos_tag, *t.nanoseconds);
}

// ---- decoding ---------------------------------------------------------------

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
    // Tags seen so far: every known tag is below 64; others are rare.
    std::uint64_t seen_low = 0;
    std::set<std::uint64_t> seen_high;
    const auto first_sight = [&](std::uint64_t tag) {
        if (tag >= 64)
            return seen_high.insert(tag).second;
        const auto bit = std::uint64_t{1} << tag;
        return !(std::exchange(seen_low, seen_low | bit) & bit);
    };

    while (!body.empty()) {
        const auto [raw_tag, value] = read_field(body, "index");
        const auto tag = static_cast<Tag>(raw_tag);

        if (!first_sight(raw_tag))
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
        case Tag::ChunkHashes:
            if (auto digests = as_digests(value))
                entry.chunk_hashes = std::move(*digests);
            else
                entry.unknown_hashes = true;
            break;
        case Tag::DictHash: entry.dict_hash = as_digest(value, entry.unknown_hashes); break;
        case Tag::SubindexHash: entry.subindex_hash = as_digest(value, entry.unknown_hashes); break;
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
    if (!entry.chunk_hashes.empty())
        write_digests(field, Tag::ChunkHashes, entry.chunk_hashes);
    if (entry.dict_hash)
        write_digests(field, Tag::DictHash, std::span(&*entry.dict_hash, 1));
    if (entry.subindex_hash)
        write_digests(field, Tag::SubindexHash, std::span(&*entry.subindex_hash, 1));
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
        write_time(field, Tag::Mtime, Tag::MtimeNsec, *entry.mtime);
    if (entry.atime)
        write_time(field, Tag::Atime, Tag::AtimeNsec, *entry.atime);
    if (entry.ctime)
        write_time(field, Tag::Ctime, Tag::CtimeNsec, *entry.ctime);
    if (entry.btime)
        write_time(field, Tag::Btime, Tag::BtimeNsec, *entry.btime);

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
