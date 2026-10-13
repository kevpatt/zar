#include "zar/crc32c.hpp"

#include <array>
#include <cstring>

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define ZAR_CRC32C_ARM 1
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

std::uint32_t update(std::uint32_t crc, const std::uint8_t* p, std::size_t n) noexcept
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

} // namespace

std::uint32_t crc32c(std::uint32_t crc, std::span<const std::uint8_t> data) noexcept
{
    return ~update(~crc, data.data(), data.size());
}

} // namespace zar
