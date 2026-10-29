// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "zar/crc32c.hpp"

#include <array>
#include <cstring>

#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#include <arm_acle.h>
#define ZAR_CRC32C_ARM 1
#if !defined(__ARM_FEATURE_CRC32) && defined(__linux__)
#include <sys/auxv.h>
#elif !defined(__ARM_FEATURE_CRC32) && defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <nmmintrin.h>
#define ZAR_CRC32C_X86 1
#endif

namespace zar {
namespace {

constexpr std::uint32_t kPolyReflected = 0x82f63b78;

constexpr auto kTable = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? kPolyReflected : 0);
        table[i] = crc;
    }
    return table;
}();

// Operates on the raw (pre-inverted) register value.
[[maybe_unused]] std::uint32_t update_portable(std::uint32_t crc, const std::uint8_t* p, std::size_t n) noexcept
{
    while (n--)
        crc = (crc >> 8) ^ kTable[(crc ^ *p++) & 0xff];
    return crc;
}

#if ZAR_CRC32C_ARM

__attribute__((target("+crc")))
std::uint32_t update_arm(std::uint32_t crc, const std::uint8_t* p, std::size_t n) noexcept
{
    for (; n >= 8; p += 8, n -= 8) {
        std::uint64_t word;
        std::memcpy(&word, p, 8);
        crc = __crc32cd(crc, word);
    }
    for (; n; ++p, --n)
        crc = __crc32cb(crc, *p);
    return crc;
}

// The CRC instructions are optional in ARMv8.0, so unless the compiler was
// told they're there, ask the system.
bool has_arm_crc() noexcept
{
#if defined(__ARM_FEATURE_CRC32)
    return true;
#elif defined(__linux__) && defined(HWCAP_CRC32)
    return (::getauxval(AT_HWCAP) & HWCAP_CRC32) != 0;
#elif defined(__APPLE__)
    int value = 0;
    std::size_t length = sizeof value;
    return ::sysctlbyname("hw.optional.armv8_crc32", &value, &length, nullptr, 0) == 0 && value;
#else
    return false;
#endif
}

std::uint32_t update(std::uint32_t crc, const std::uint8_t* p, std::size_t n) noexcept
{
    static const bool has_crc = has_arm_crc();
    return has_crc ? update_arm(crc, p, n) : update_portable(crc, p, n);
}

#elif ZAR_CRC32C_X86

__attribute__((target("sse4.2")))
std::uint32_t update_sse42(std::uint32_t crc, const std::uint8_t* p, std::size_t n) noexcept
{
#if defined(__x86_64__)
    std::uint64_t crc64 = crc;
    for (; n >= 8; p += 8, n -= 8) {
        std::uint64_t word;
        std::memcpy(&word, p, 8);
        crc64 = _mm_crc32_u64(crc64, word);
    }
    crc = static_cast<std::uint32_t>(crc64);
#endif
    for (; n; ++p, --n)
        crc = _mm_crc32_u8(crc, *p);
    return crc;
}

std::uint32_t update(std::uint32_t crc, const std::uint8_t* p, std::size_t n) noexcept
{
    static const bool has_sse42 = __builtin_cpu_supports("sse4.2");
    return has_sse42 ? update_sse42(crc, p, n) : update_portable(crc, p, n);
}

#else

std::uint32_t update(std::uint32_t crc, const std::uint8_t* p, std::size_t n) noexcept
{
    return update_portable(crc, p, n);
}

#endif

// CRC combination works in GF(2): appending n zero bits to a message is a
// linear operator on its CRC, represented as a 32x32 bit matrix (one column
// per word). Squaring the operator doubles n, so with the operators for each
// power of two at hand, appending length_b bytes takes one matrix-vector
// product per bit set in length_b (the method zlib uses, precomputed).
using Gf2Matrix = std::array<std::uint32_t, 32>;

std::uint32_t times(const Gf2Matrix& matrix, std::uint32_t vector) noexcept
{
    std::uint32_t sum = 0;
    for (std::size_t i = 0; vector; ++i, vector >>= 1)
        if (vector & 1)
            sum ^= matrix[i];
    return sum;
}

Gf2Matrix square(const Gf2Matrix& matrix) noexcept
{
    Gf2Matrix result;
    for (std::size_t i = 0; i < 32; ++i)
        result[i] = times(matrix, matrix[i]);
    return result;
}

} // namespace

std::uint32_t crc32c(std::uint32_t crc, std::span<const std::uint8_t> data) noexcept
{
    return ~update(~crc, data.data(), data.size());
}

std::uint32_t crc32c_combine(std::uint32_t crc_a, std::uint32_t crc_b, std::uint64_t length_b) noexcept
{
    // The operators for 1, 2, 4, ... zero bytes, computed once: holes in
    // sparse files make for many calls.
    static const auto operators = [] {
        std::array<Gf2Matrix, 64> ops;
        Gf2Matrix op;    // one zero bit, then squared up to one zero byte
        op[0] = kPolyReflected;
        for (std::size_t i = 1; i < 32; ++i)
            op[i] = std::uint32_t{1} << (i - 1);
        op = square(square(square(op)));
        for (auto& power : ops) {
            power = op;
            op = square(op);
        }
        return ops;
    }();

    for (std::size_t bit = 0; length_b; ++bit, length_b >>= 1)
        if (length_b & 1)
            crc_a = times(operators[bit], crc_a);
    return crc_a ^ crc_b;
}

std::uint32_t crc32c_zeros(std::uint32_t crc, std::uint64_t length) noexcept
{
    // Zeros only shift the (pre-inverted) register, which is what combining
    // does to its first argument.
    return ~crc32c_combine(~crc, 0, length);
}

} // namespace zar
