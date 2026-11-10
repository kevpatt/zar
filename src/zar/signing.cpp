// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "zar/signing.hpp"

#include "zar/hash.hpp"
#include "zar/posix.hpp"

#include <cerrno>
#include <fcntl.h>
#include <monocypher-ed25519.h>
#include <monocypher.h>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#if __has_include(<sys/random.h>)
#include <sys/random.h>    // getentropy() on macOS
#endif

namespace zar {
namespace {

constexpr std::string_view kSecretPrefix = "zar-ed25519-secret ";
constexpr std::string_view kPublicPrefix = "zar-ed25519-public ";

std::optional<std::array<std::uint8_t, 32>> from_hex(std::string_view text)
{
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::array<std::uint8_t, 32> out{};
    if (text.size() != 2 * out.size())
        return std::nullopt;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int high = digit(text[2 * i]), low = digit(text[2 * i + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        out[i] = static_cast<std::uint8_t>(high << 4 | low);
    }
    return out;
}

// A key file's one line, without its prefix or trailing white space.
std::string read_key_line(const std::filesystem::path& path, std::string_view prefix, std::string_view kind,
                          struct stat* st)
{
    const UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd)
        throw_errno(path.string());
    if (st && ::fstat(fd.get(), st) != 0)
        throw_errno(path.string());
    std::string text(256, '\0');
    text.resize(read_full(fd.get(), std::span(reinterpret_cast<std::uint8_t*>(text.data()), text.size()),
                          path.string()));
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t'))
        text.pop_back();
    if (!text.starts_with(prefix))
        throw std::runtime_error(path.string() + ": not a zar " + std::string(kind) + " key file");
    return text.substr(prefix.size());
}

void write_key_file(const std::filesystem::path& path, const std::string& text, mode_t mode, bool overwrite)
{
    // A fresh file, so it never keeps an old file's looser permissions.
    if (overwrite && ::unlink(path.c_str()) != 0 && errno != ENOENT)
        throw_errno(path.string());
    const UniqueFd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode));
    if (!fd) {
        if (errno == EEXIST)
            throw std::runtime_error(path.string() + ": already exists (use -y to overwrite)");
        throw_errno(path.string());
    }
    write_all(fd.get(), ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), path.string());
    if (::fsync(fd.get()) != 0)
        throw_errno(path.string());
}

} // namespace

SecretKey SecretKey::generate()
{
    std::array<std::uint8_t, 32> seed;
    if (::getentropy(seed.data(), seed.size()) != 0)
        throw_errno("can't get random bytes for a key");
    auto key = from_seed(seed);
    crypto_wipe(seed.data(), seed.size());
    return key;
}

SecretKey SecretKey::from_seed(std::span<const std::uint8_t, 32> seed)
{
    SecretKey key;
    std::array<std::uint8_t, 32> copy;    // Monocypher wipes it
    std::ranges::copy(seed, copy.begin());
    crypto_ed25519_key_pair(key.secret_.data(), key.public_.data(), copy.data());
    return key;
}

SecretKey::SecretKey(const SecretKey&) = default;
SecretKey& SecretKey::operator=(const SecretKey&) = default;

SecretKey::~SecretKey()
{
    crypto_wipe(secret_.data(), secret_.size());
}

Signature SecretKey::sign(ByteView message) const
{
    Signature signature;
    crypto_ed25519_sign(signature.data(), secret_.data(), message.data(), message.size());
    return signature;
}

std::string SecretKey::to_text() const
{
    return std::string(kSecretPrefix) + to_hex(ByteView(secret_).first(32)) + '\n';
}

bool check_signature(const PublicKey& key, const Signature& signature, ByteView message)
{
    return crypto_ed25519_check(signature.data(), key.data(), message.data(), message.size()) == 0;
}

std::string fingerprint(const PublicKey& key)
{
    return to_hex(ByteView(blake3(key)).first(8));
}

std::string public_key_text(const PublicKey& key)
{
    return std::string(kPublicPrefix) + to_hex(key) + '\n';
}

SecretKey read_secret_key(const std::filesystem::path& path)
{
    struct stat st {};
    auto hex = read_key_line(path, kSecretPrefix, "secret", &st);
    // Like ssh: a secret others can read may no longer be one.
    if (st.st_mode & 077)
        throw std::runtime_error(path.string() + ": others can read this secret key (chmod 600 it)");
    auto seed = from_hex(hex);
    crypto_wipe(hex.data(), hex.size());
    if (!seed)
        throw std::runtime_error(path.string() + ": malformed secret key");
    auto key = SecretKey::from_seed(*seed);
    crypto_wipe(seed->data(), seed->size());
    return key;
}

PublicKey read_public_key(const std::filesystem::path& path)
{
    const auto key = from_hex(read_key_line(path, kPublicPrefix, "public", nullptr));
    if (!key)
        throw std::runtime_error(path.string() + ": malformed public key");
    return *key;
}

void write_key_files(const std::filesystem::path& path, const SecretKey& key, bool overwrite)
{
    auto pub = path;
    pub += ".pub";
    auto secret = key.to_text();
    try {
        write_key_file(path, secret, 0600, overwrite);
    } catch (...) {
        crypto_wipe(secret.data(), secret.size());
        throw;
    }
    crypto_wipe(secret.data(), secret.size());
    try {
        write_key_file(pub, public_key_text(key.public_key()), 0644, overwrite);
    } catch (...) {
        ::unlink(path.c_str());    // no secret without its public half
        throw;
    }
}

} // namespace zar
