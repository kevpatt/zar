// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// Ed25519 keys (RFC 8032) and the key files zar keeps them in:
//
//   KEYFILE       zar-ed25519-secret <64 hex digits: the 32-byte seed>
//   KEYFILE.pub   zar-ed25519-public <64 hex digits: the public key>
//
// one line each. The secret file is created with mode 0600.
#pragma once

#include "zar/bytes.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace zar {

inline constexpr std::size_t kPublicKeySize = 32;
inline constexpr std::size_t kSignatureSize = 64;
using PublicKey = std::array<std::uint8_t, kPublicKeySize>;
using Signature = std::array<std::uint8_t, kSignatureSize>;

class SecretKey {
public:
    // A new key from the system's random number generator.
    static SecretKey generate();
    static SecretKey from_seed(std::span<const std::uint8_t, 32> seed);

    SecretKey(const SecretKey&);
    SecretKey& operator=(const SecretKey&);
    ~SecretKey();    // wipes the key from memory

    const PublicKey& public_key() const noexcept { return public_; }
    Signature sign(ByteView message) const;

    // The key file's text, which holds the secret.
    std::string to_text() const;

private:
    SecretKey() = default;

    std::array<std::uint8_t, 64> secret_{};    // Monocypher's form: seed, then public key
    PublicKey public_{};
};

// Whether `signature` is `key`'s valid signature of `message`.
bool check_signature(const PublicKey& key, const Signature& signature, ByteView message);

// A short name for a key, for people to compare: 16 hex digits of the
// key's BLAKE3 hash.
std::string fingerprint(const PublicKey& key);

std::string public_key_text(const PublicKey& key);

// Read key files, throwing std::runtime_error if one is missing or malformed.
SecretKey read_secret_key(const std::filesystem::path& path);
PublicKey read_public_key(const std::filesystem::path& path);

// Writes `path` (the secret, mode 0600) and `path`.pub. Neither may exist
// unless `overwrite`.
void write_key_files(const std::filesystem::path& path, const SecretKey& key, bool overwrite);

} // namespace zar
