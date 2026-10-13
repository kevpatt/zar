// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#pragma once

#include <stdexcept>

namespace zar {

// The archive's contents violate the format (FORMAT.md) or are damaged.
struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The archive ends before data it refers to.
struct TruncatedError : FormatError {
    TruncatedError() : FormatError("archive is truncated") {}
};

} // namespace zar
