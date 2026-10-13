// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "zar/names.hpp"

#include <cstdint>

namespace zar {

bool is_valid_utf8(std::string_view text) noexcept
{
    const auto* p = reinterpret_cast<const std::uint8_t*>(text.data());
    const auto* const end = p + text.size();

    while (p < end) {
        const std::uint8_t lead = *p++;
        if (lead < 0x80)
            continue;

        int extra;
        std::uint32_t cp;
        std::uint32_t min;
        if ((lead & 0xe0) == 0xc0) {
            extra = 1, cp = lead & 0x1fu, min = 0x80;
        } else if ((lead & 0xf0) == 0xe0) {
            extra = 2, cp = lead & 0x0fu, min = 0x800;
        } else if ((lead & 0xf8) == 0xf0) {
            extra = 3, cp = lead & 0x07u, min = 0x10000;
        } else {
            return false;    // continuation byte or invalid lead
        }

        if (end - p < extra)
            return false;
        for (int i = 0; i < extra; ++i) {
            const std::uint8_t cont = *p++;
            if ((cont & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (cont & 0x3fu);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
    }
    return true;
}

std::string latin1_to_utf8(std::string_view text)
{
    std::string out;
    out.reserve(text.size() * 2);
    for (const char c : text) {
        const auto byte = static_cast<std::uint8_t>(c);
        if (byte < 0x80) {
            out += c;
        } else {
            out += static_cast<char>(0xc0 | (byte >> 6));
            out += static_cast<char>(0x80 | (byte & 0x3f));
        }
    }
    return out;
}

bool is_valid_archive_path(std::string_view path) noexcept
{
    if (path.empty() || path.find('\0') != std::string_view::npos)
        return false;

    for (;;) {
        const auto slash = path.find('/');
        const auto component = path.substr(0, slash);
        if (component.empty() || component == "." || component == "..")
            return false;
        if (slash == std::string_view::npos)
            return true;
        path.remove_prefix(slash + 1);
    }
}

} // namespace zar
