#pragma once

#include <stdexcept>

namespace zar {

// The archive's contents violate the format (FORMAT.md) or are damaged.
struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

} // namespace zar
