// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// Training a zstd dictionary on the files being archived.
#pragma once

#include "zar/bytes.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace zar {

// The default largest dictionary, as with `zstd --train`.
inline constexpr std::uint64_t kDefaultDictSize = 110 << 10;

// Collects training samples: the start of each chunk, as `zstd --train` does
// with files. Past a fixed budget it keeps an evenly spread subset, chosen
// deterministically, so the same input always trains the same dictionary.
class SampleCollector {
public:
    void add(ByteView chunk);

    std::uint64_t total() const noexcept { return data_.size(); }
    std::size_t count() const noexcept { return sizes_.size(); }

    // Trains a dictionary of at most `max_size` bytes, scaled down for
    // small inputs. Returns nullopt if there is too little data to train on.
    std::optional<Bytes> train(std::uint64_t max_size) const;

private:
    static constexpr std::size_t kMaxSample = 128 << 10;
    static constexpr std::size_t kBudget = 32 << 20;

    void thin();

    Bytes data_;                     // the samples, back to back
    std::vector<std::size_t> sizes_;
    std::uint64_t seen_ = 0;         // chunks offered
    std::uint64_t stride_ = 1;       // keep one chunk in this many
};

} // namespace zar
