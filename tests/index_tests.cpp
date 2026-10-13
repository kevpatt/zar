// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "check.hpp"

#include "zar/index.hpp"
#include "zar/names.hpp"

using namespace zar;

TEST(utf8_validation)
{
    CHECK(is_valid_utf8(""));
    CHECK(is_valid_utf8("plain ascii"));
    CHECK(is_valid_utf8("caf\xc3\xa9"));                 // é
    CHECK(is_valid_utf8("\xf0\x9f\x93\xa6"));            // 📦
    CHECK(!is_valid_utf8("caf\xe9"));                    // Latin-1 é
    CHECK(!is_valid_utf8("\xc0\xaf"));                   // overlong '/'
    CHECK(!is_valid_utf8("\xed\xa0\x80"));               // surrogate
    CHECK(!is_valid_utf8("\xf4\x90\x80\x80"));           // past U+10FFFF
    CHECK(!is_valid_utf8("\xe2\x82"));                   // truncated
}

TEST(latin1_conversion)
{
    CHECK(latin1_to_utf8("caf\xe9") == "caf\xc3\xa9");
    CHECK(latin1_to_utf8("plain") == "plain");
    CHECK(is_valid_utf8(latin1_to_utf8("\x80\xff")));
}

TEST(archive_paths)
{
    CHECK(is_valid_archive_path("a"));
    CHECK(is_valid_archive_path("a/b/c.txt"));
    CHECK(is_valid_archive_path("..hidden/x"));
    CHECK(!is_valid_archive_path(""));
    CHECK(!is_valid_archive_path("/etc/passwd"));
    CHECK(!is_valid_archive_path("a/../b"));
    CHECK(!is_valid_archive_path(".."));
    CHECK(!is_valid_archive_path("./a"));
    CHECK(!is_valid_archive_path("a//b"));
    CHECK(!is_valid_archive_path("a/"));
    CHECK(!is_valid_archive_path(std::string_view("a\0b", 3)));
}

TEST(index_round_trip)
{
    Entry file;
    file.path = "src/main.cpp";
    file.size = 5'000'000'000;    // needs 8 bytes
    file.chunk_size = 1 << 20;
    file.chunks = {32, 1'048'640, -4096};
    file.content_crc = 0x12345678;
    file.mode = 0644;
    file.uid = 501;
    file.gname = "staff";
    file.mtime = Timestamp{1'760'000'000, 123'456'789};
    file.btime = Timestamp{-86400, {}};    // before the epoch

    Entry dir;
    dir.type = EntryType::Dir;
    dir.path = "src";
    dir.subindex = 4096;

    Entry link;
    link.type = EntryType::Symlink;
    link.flags = entry_flags::kPathRaw;
    link.path = "caf\xe9";
    link.link_target = "../elsewhere";

    Bytes index;
    for (const auto& e : {file, dir, link})
        encode_entry(index, e);

    const auto decoded = decode_index(index);
    CHECK(decoded.size() == 3);
    CHECK(decoded[0] == file);
    CHECK(decoded[1] == dir);
    CHECK(decoded[2] == link);
}

TEST(index_skips_unknown_entries_and_fields)
{
    Bytes index;
    // An entry of unknown type 99.
    index.insert(index.end(), {99, 0, 3, 0x7f, 1, 0xaa});
    // A FILE with an unknown tag 0x7e between known ones.
    Bytes body;
    append_varint(body, 0x01);
    append_varint(body, 1);
    body.push_back('x');
    append_varint(body, 0x7e);
    append_varint(body, 2);
    body.insert(body.end(), {0xde, 0xad});
    index.push_back(static_cast<std::uint8_t>(EntryType::File));
    index.push_back(0);
    append_varint(index, body.size());
    index.insert(index.end(), body.begin(), body.end());

    const auto decoded = decode_index(index);
    CHECK(decoded.size() == 1);
    CHECK(decoded[0].path == "x");
}

TEST(index_rejects_malformed)
{
    const auto entry_with = [](std::string_view path, std::uint8_t flags = 0) {
        Entry e;
        e.path = path;
        e.flags = flags;
        Bytes index;
        encode_entry(index, e);
        return index;
    };
    CHECK_THROWS(decode_index(entry_with("/abs")));
    CHECK_THROWS(decode_index(entry_with("a/../b")));
    CHECK_THROWS(decode_index(entry_with("caf\xe9")));                 // raw, not flagged
    CHECK(decode_index(entry_with("caf\xe9", entry_flags::kPathRaw)).size() == 1);
    CHECK_THROWS(decode_index(entry_with("ok", 0x80)));                // unknown flag

    auto truncated = entry_with("abc");
    truncated.pop_back();
    CHECK_THROWS(decode_index(truncated));

    Bytes no_path{static_cast<std::uint8_t>(EntryType::File), 0, 0};
    CHECK_THROWS(decode_index(no_path));
}
