// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// Tagged fields (FORMAT.md §6.1): varint tag, varint length, value. Index
// entries and SIGNATURE blocks are made of them.
#pragma once

#include "zar/bytes.hpp"
#include "zar/error.hpp"

#include <concepts>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace zar {

// Appends fields to `out`. Tags are any enum or integer.
class FieldWriter {
public:
    explicit FieldWriter(Bytes& out) : out_(out) {}

    template <typename T>
    void bytes(T tag, std::string_view value)
    {
        start(tag, value.size());
        out_.insert(out_.end(), value.begin(), value.end());
    }

    template <typename T>
    void bytes(T tag, ByteView value)
    {
        start(tag, value.size());
        out_.insert(out_.end(), value.begin(), value.end());
    }

    template <typename T, std::unsigned_integral V>
    void fixed(T tag, V value)
    {
        start(tag, sizeof(V));
        append_le(out_, value);
    }

    // 4 bytes when the value fits, otherwise 8 (§6.4).
    template <typename T>
    void unsigned_number(T tag, std::uint64_t value)
    {
        if (value <= std::numeric_limits<std::uint32_t>::max())
            fixed(tag, static_cast<std::uint32_t>(value));
        else
            fixed(tag, value);
    }

    template <typename T>
    void signed_number(T tag, std::int64_t value)
    {
        if (value >= std::numeric_limits<std::int32_t>::min() &&
            value <= std::numeric_limits<std::int32_t>::max())
            fixed(tag, static_cast<std::uint32_t>(static_cast<std::int32_t>(value)));
        else
            fixed(tag, static_cast<std::uint64_t>(value));
    }

    template <typename T>
    void offsets(T tag, const std::vector<std::int64_t>& values)
    {
        start(tag, values.size() * 8);
        for (const auto value : values)
            append_le(out_, static_cast<std::uint64_t>(value));
    }

private:
    template <typename T>
    void start(T tag, std::size_t length)
    {
        append_varint(out_, static_cast<std::uint64_t>(tag));
        append_varint(out_, length);
    }

    Bytes& out_;
};

struct Field {
    std::uint64_t tag;
    ByteView value;
};

// Takes the next field off the front of `in`; `what` names the container.
inline Field read_field(ByteView& in, std::string_view what)
{
    const auto tag = read_varint(in);
    const auto length = read_varint(in);
    if (length > in.size())
        throw FormatError(std::string(what) + " field overruns its end");
    const auto value = in.first(static_cast<std::size_t>(length));
    in = in.subspan(static_cast<std::size_t>(length));
    return {tag, value};
}

inline std::uint64_t as_unsigned(ByteView value)
{
    switch (value.size()) {
    case 4: return load_le<std::uint32_t>(value.data());
    case 8: return load_le<std::uint64_t>(value.data());
    default: throw FormatError("numeric field must be 4 or 8 bytes");
    }
}

inline std::int64_t as_signed(ByteView value)
{
    switch (value.size()) {
    case 4: return static_cast<std::int32_t>(load_le<std::uint32_t>(value.data()));
    case 8: return static_cast<std::int64_t>(load_le<std::uint64_t>(value.data()));
    default: throw FormatError("numeric field must be 4 or 8 bytes");
    }
}

template <std::unsigned_integral T>
T as_fixed(ByteView value)
{
    if (value.size() != sizeof(T))
        throw FormatError("field must be " + std::to_string(sizeof(T)) + " bytes");
    return load_le<T>(value.data());
}

inline std::int64_t as_offset(ByteView value)
{
    return static_cast<std::int64_t>(as_fixed<std::uint64_t>(value));
}

inline std::vector<std::int64_t> as_offsets(ByteView value)
{
    if (value.size() % 8 != 0)
        throw FormatError("offset list length is not a multiple of 8");
    std::vector<std::int64_t> offsets;
    offsets.reserve(value.size() / 8);
    for (std::size_t i = 0; i < value.size(); i += 8)
        offsets.push_back(static_cast<std::int64_t>(load_le<std::uint64_t>(&value[i])));
    return offsets;
}

inline std::string as_string(ByteView value)
{
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

} // namespace zar
