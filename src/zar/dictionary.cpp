// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "zar/dictionary.hpp"

#include <algorithm>
#include <zdict.h>

namespace zar {

void SampleCollector::add(ByteView chunk)
{
    if (chunk.empty() || seen_++ % stride_ != 0)
        return;
    const auto sample = chunk.first(std::min(chunk.size(), kMaxSample));
    data_.insert(data_.end(), sample.begin(), sample.end());
    sizes_.push_back(sample.size());
    if (data_.size() > kBudget)
        thin();
}

// Drops every other sample and halves how often new ones are taken.
void SampleCollector::thin()
{
    std::size_t from = 0, to = 0, kept = 0;
    for (std::size_t i = 0; i < sizes_.size(); ++i) {
        const auto size = sizes_[i];
        if (i % 2 == 0) {
            std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(from), size,
                        data_.begin() + static_cast<std::ptrdiff_t>(to));
            to += size;
            sizes_[kept++] = size;
        }
        from += size;
    }
    data_.resize(to);
    sizes_.resize(kept);
    stride_ *= 2;
}

std::optional<Bytes> SampleCollector::train(std::uint64_t max_size) const
{
    // zstd suggests about 100 times as much sample data as dictionary; with
    // less, a smaller dictionary pays for itself sooner.
    constexpr std::uint64_t kMinSize = 1 << 10;
    const auto size = std::clamp<std::uint64_t>(total() / 10, kMinSize, std::max(max_size, kMinSize));
    if (count() < 8 || total() < 2 * kMinSize)
        return std::nullopt;

    Bytes dict(static_cast<std::size_t>(size));
    const auto n = ZDICT_trainFromBuffer(dict.data(), dict.size(), data_.data(), sizes_.data(),
                                         static_cast<unsigned>(sizes_.size()));
    if (ZDICT_isError(n))
        return std::nullopt;    // usually: not enough data, or too uniform
    dict.resize(n);
    return dict;
}

} // namespace zar
