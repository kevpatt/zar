// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "zar/signature.hpp"

#include "zar/fields.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string_view>

namespace zar {
namespace {

constexpr std::uint8_t kVersion = 1;
constexpr std::string_view kContext = "zar signature v1";
constexpr std::uint8_t kEd25519 = 1;

enum class SigTag : std::uint64_t {
    Root = 0x01,
    RootHash = 0x02,
    Sequence = 0x03,
    Time = 0x04,
    Name = 0x05,
    Sig = 0x10,
};

// What every signer signs: the context, a NUL, and the payload up to its
// first SIG field.
Bytes message_for(ByteView signed_part)
{
    Bytes message(kContext.begin(), kContext.end());
    message.push_back(0);
    message.insert(message.end(), signed_part.begin(), signed_part.end());
    return message;
}

} // namespace

std::optional<PublicKey> SignatureBlock::signed_by(std::span<const PublicKey> keys) const
{
    for (const auto& signer : signers)
        if (std::ranges::find(keys, signer.key) != keys.end() &&
            check_signature(signer.key, signer.signature, message))
            return signer.key;
    return std::nullopt;
}

Bytes encode_signature(const SignedRoot& statement, std::span<const SecretKey> keys)
{
    if (keys.empty())
        throw std::invalid_argument("encode_signature needs a key");

    Bytes payload{kVersion};
    FieldWriter field(payload);
    field.fixed(SigTag::Root, static_cast<std::uint64_t>(statement.root));
    Bytes hash{static_cast<std::uint8_t>(HashAlgorithm::Blake3)};
    hash.insert(hash.end(), statement.root_hash.begin(), statement.root_hash.end());
    field.bytes(SigTag::RootHash, ByteView(hash));
    if (statement.sequence)
        field.unsigned_number(SigTag::Sequence, *statement.sequence);
    if (statement.time)
        field.signed_number(SigTag::Time, *statement.time);
    if (statement.name)
        field.bytes(SigTag::Name, *statement.name);

    const auto message = message_for(payload);
    for (const auto& key : keys) {
        Bytes sig{kEd25519};
        const auto& pub = key.public_key();
        const auto signature = key.sign(message);
        sig.insert(sig.end(), pub.begin(), pub.end());
        sig.insert(sig.end(), signature.begin(), signature.end());
        field.bytes(SigTag::Sig, ByteView(sig));
    }
    return payload;
}

SignatureBlock decode_signature(ByteView payload)
{
    const auto whole = payload;
    if (payload.empty() || payload[0] != kVersion)
        throw FormatError("unsupported signature version");
    payload = payload.subspan(1);

    SignatureBlock block;
    std::set<std::uint64_t> seen;
    bool have_root = false, have_hash = false;
    std::optional<std::size_t> signed_end;    // where the first SIG field starts
    while (!payload.empty()) {
        const auto start = whole.size() - payload.size();
        const auto [tag, value] = read_field(payload, "signature");
        if (tag == static_cast<std::uint64_t>(SigTag::Sig)) {
            if (!signed_end)
                signed_end = start;
            if (value.empty())
                throw FormatError("empty SIG field");
            if (value[0] != kEd25519) {
                ++block.other_signers;    // an algorithm zar doesn't know
                continue;
            }
            if (value.size() != 1 + kPublicKeySize + kSignatureSize)
                throw FormatError("Ed25519 SIG field has the wrong length");
            auto& signer = block.signers.emplace_back();
            std::copy_n(value.begin() + 1, kPublicKeySize, signer.key.begin());
            std::copy_n(value.begin() + 1 + kPublicKeySize, kSignatureSize, signer.signature.begin());
            continue;
        }
        if (signed_end)
            throw FormatError("signature has a field after its SIG fields");
        if (!seen.insert(tag).second)
            throw FormatError("signature repeats a field");
        switch (static_cast<SigTag>(tag)) {
        case SigTag::Root:
            block.statement.root = as_offset(value);
            have_root = true;
            break;
        case SigTag::RootHash:
            if (value.size() != 1 + kDigestSize || value[0] != static_cast<std::uint8_t>(HashAlgorithm::Blake3))
                throw FormatError("signature's root hash uses an unknown algorithm");
            std::copy_n(value.begin() + 1, kDigestSize, block.statement.root_hash.begin());
            have_hash = true;
            break;
        case SigTag::Sequence: block.statement.sequence = as_unsigned(value); break;
        case SigTag::Time: block.statement.time = as_signed(value); break;
        case SigTag::Name: block.statement.name = as_string(value); break;
        default: break;    // unknown: skipped, though it is signed
        }
    }
    if (!have_root || !have_hash)
        throw FormatError("signature doesn't name a root index and its hash");
    if (!signed_end)
        throw FormatError("signature block has no signatures");
    block.message = message_for(whole.first(*signed_end));
    return block;
}

} // namespace zar
