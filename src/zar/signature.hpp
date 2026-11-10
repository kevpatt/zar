// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// SIGNATURE blocks (FORMAT.md §5.6): a signed statement naming the root
// index and its hash, and one or more signatures of it.
#pragma once

#include "zar/hash.hpp"
#include "zar/signing.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace zar {

// What a signature vouches for.
struct SignedRoot {
    std::int64_t root = kNoOffset;    // the root index block
    Digest root_hash{};               // its block hash
    std::optional<std::uint64_t> sequence;
    std::optional<std::int64_t> time;
    std::optional<std::string> name;

    bool operator==(const SignedRoot&) const = default;
};

struct SignatureBlock {
    struct Signer {
        PublicKey key;
        Ed25519Signature signature;
    };

    SignedRoot statement;
    std::vector<Signer> signers;          // the Ed25519 ones
    std::size_t other_signers = 0;        // with algorithms zar doesn't know
    Bytes message;                        // what each signer signed

    // The first of `keys` with a valid signature here, if any.
    std::optional<PublicKey> signed_by(std::span<const PublicKey> keys) const;
};

// A SIGNATURE block's payload, signed by each of `keys` (at least one).
Bytes encode_signature(const SignedRoot& statement, std::span<const SecretKey> keys);

// Throws FormatError if the payload is malformed. Signatures aren't checked
// here: see signed_by().
SignatureBlock decode_signature(ByteView payload);

} // namespace zar
