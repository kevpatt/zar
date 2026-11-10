// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// Hashes, Ed25519 keys and key files, and SIGNATURE blocks.
#include "check.hpp"
#include "util.hpp"

#include "zar/fields.hpp"
#include "zar/hash.hpp"
#include "zar/signature.hpp"
#include "zar/signing.hpp"

#include <sys/stat.h>

using namespace zar;
using namespace zar::test;

namespace {

Bytes from_hex(std::string_view hex)
{
    Bytes out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    return out;
}

SecretKey key_from_hex(std::string_view hex)
{
    const auto seed = from_hex(hex);
    return SecretKey::from_seed(std::span<const std::uint8_t, 32>(seed.data(), 32));
}

// RFC 8032 §7.1, tests 1 and 2.
const auto kRfcKey1 = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
const auto kRfcPub1 = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
const auto kRfcSig1 = "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b";
const auto kRfcKey2 = "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
const auto kRfcPub2 = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";
const auto kRfcSig2 = "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00";

SignedRoot sample_statement()
{
    SignedRoot s;
    s.root = 4096;
    s.root_hash = blake3(text("the root index"));
    s.sequence = 42;
    s.time = 1'800'000'000;
    s.name = "stable";
    return s;
}

} // namespace

TEST(blake3_known_vectors)
{
    CHECK(to_hex(blake3({})) == "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
    CHECK(to_hex(blake3(text("abc"))) == "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85");
    // Long enough for the SIMD paths and the chunk tree.
    const auto big = random_bytes(3 << 20, 9);
    CHECK(blake3(big) == blake3(big));
    CHECK(blake3(big) != blake3(ByteView(big).first(big.size() - 1)));
}

TEST(block_hashes_cover_the_type_and_the_payload)
{
    const auto payload = text("same bytes");
    Bytes typed{0x03, 0x00};    // DATA_RAW, little-endian
    typed.insert(typed.end(), payload.begin(), payload.end());
    CHECK(block_hash(BlockType::DataRaw, payload) == blake3(typed));
    CHECK(block_hash(BlockType::DataRaw, payload) != block_hash(BlockType::DataZstd, payload));
}

TEST(ed25519_matches_rfc_8032)
{
    const auto key1 = key_from_hex(kRfcKey1);
    CHECK(to_hex(key1.public_key()) == kRfcPub1);
    CHECK(to_hex(key1.sign({})) == kRfcSig1);

    const auto key2 = key_from_hex(kRfcKey2);
    const Bytes message{0x72};
    const auto sig = key2.sign(message);
    CHECK(to_hex(key2.public_key()) == kRfcPub2);
    CHECK(to_hex(sig) == kRfcSig2);
    CHECK(check_signature(key2.public_key(), sig, message));

    CHECK(!check_signature(key1.public_key(), sig, message));    // wrong key
    CHECK(!check_signature(key2.public_key(), sig, Bytes{0x73}));    // wrong message
    auto bad = sig;
    bad[10] ^= 1;
    CHECK(!check_signature(key2.public_key(), bad, message));

    const auto fresh = SecretKey::generate();
    CHECK(fresh.public_key() != SecretKey::generate().public_key());
    CHECK(check_signature(fresh.public_key(), fresh.sign(message), message));
    CHECK(fingerprint(key1.public_key()).size() == 16);
}

TEST(key_files_round_trip_and_stay_private)
{
    TempDir tmp;
    const auto path = tmp.path() / "release.key";
    const auto key = key_from_hex(kRfcKey1);
    write_key_files(path, key, false);

    struct stat st {};
    CHECK(::stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(read_file(path) == text(std::string("zar-ed25519-secret ") + kRfcKey1 + "\n"));
    CHECK(read_file(tmp.path() / "release.key.pub") == text(std::string("zar-ed25519-public ") + kRfcPub1 + "\n"));
    CHECK(read_secret_key(path).public_key() == key.public_key());
    CHECK(read_public_key(tmp.path() / "release.key.pub") == key.public_key());

    CHECK_THROWS_WITH(write_key_files(path, key, false), "already exists");
    write_key_files(path, SecretKey::generate(), true);
    CHECK(read_secret_key(path).public_key() != key.public_key());

    ::chmod(path.c_str(), 0640);
    CHECK_THROWS_WITH(read_secret_key(path), "chmod 600");
    CHECK_THROWS_WITH(read_secret_key(tmp.path() / "release.key.pub"), "not a zar secret key file");
    CHECK_THROWS_WITH(read_public_key(path), "not a zar public key file");
    write_file(tmp.path() / "short.pub", text("zar-ed25519-public abcd\n"));
    CHECK_THROWS_WITH(read_public_key(tmp.path() / "short.pub"), "malformed");
    CHECK_THROWS(read_public_key(tmp.path() / "missing.pub"));
}

TEST(signature_blocks_round_trip)
{
    const auto a = key_from_hex(kRfcKey1), b = key_from_hex(kRfcKey2);
    const auto statement = sample_statement();
    const SecretKey keys[] = {a, b};
    const auto payload = encode_signature(statement, keys);
    const auto block = decode_signature(payload);
    CHECK(block.statement == statement);
    CHECK(block.signers.size() == 2 && block.other_signers == 0);

    const PublicKey only_b[] = {b.public_key()};
    CHECK(block.signed_by(only_b) == b.public_key());
    const PublicKey stranger[] = {SecretKey::generate().public_key()};
    CHECK(!block.signed_by(stranger));

    // Each signature covers the statement: change any byte before the SIG
    // fields and none is valid.
    const PublicKey both[] = {a.public_key(), b.public_key()};
    for (std::size_t i = 0; i + 2 * (2 + 97) < payload.size(); ++i) {
        auto tampered = payload;
        tampered[i] ^= 0x40;
        bool accepted = false;
        try {
            accepted = decode_signature(tampered).signed_by(both).has_value();
        } catch (const FormatError&) {
        }
        CHECK(!accepted);
    }

    // Optional fields are optional.
    SignedRoot minimal;
    minimal.root = -64;
    const SecretKey one[] = {a};
    CHECK(decode_signature(encode_signature(minimal, one)).statement == minimal);
}

TEST(signature_blocks_reject_malformed_payloads)
{
    const auto key = key_from_hex(kRfcKey1);
    const SecretKey keys[] = {key};
    auto payload = encode_signature(sample_statement(), keys);

    auto version = payload;
    version[0] = 2;
    CHECK_THROWS_WITH(decode_signature(version), "version");

    auto trailing = payload;    // a field after the SIG fields
    FieldWriter(trailing).bytes(0x05, std::string_view("late"));
    CHECK_THROWS_WITH(decode_signature(trailing), "after its SIG");

    Bytes unsigned_payload{1};
    FieldWriter field(unsigned_payload);
    field.fixed(0x01, std::uint64_t{0});
    Bytes hash{1};
    hash.resize(1 + kDigestSize);
    field.bytes(0x02, ByteView(hash));
    CHECK_THROWS_WITH(decode_signature(unsigned_payload), "no signatures");

    Bytes no_root{1};
    FieldWriter(no_root).bytes(0x02, ByteView(hash));
    CHECK_THROWS_WITH(decode_signature(no_root), "root");

    // An unknown field before the signatures is skipped, but signed.
    Bytes extended{1};
    FieldWriter more(extended);
    more.fixed(0x01, std::uint64_t{0});
    more.bytes(0x02, ByteView(hash));
    more.bytes(0x7f, std::string_view("from the future"));
    Bytes message(std::string_view("zar signature v1").begin(), std::string_view("zar signature v1").end());
    message.push_back(0);
    message.insert(message.end(), extended.begin(), extended.end());
    Bytes sig{1};
    sig.insert(sig.end(), key.public_key().begin(), key.public_key().end());
    const auto signature = key.sign(message);
    sig.insert(sig.end(), signature.begin(), signature.end());
    more.bytes(0x10, ByteView(sig));
    const PublicKey trusted[] = {key.public_key()};
    CHECK(decode_signature(extended).signed_by(trusted));

    // A signature in an unknown algorithm is counted, not checked.
    auto other = payload;
    Bytes alien{200, 1, 2, 3};
    FieldWriter(other).bytes(0x10, ByteView(alien));
    const auto block = decode_signature(other);
    CHECK(block.other_signers == 1 && block.signers.size() == 1);
}
