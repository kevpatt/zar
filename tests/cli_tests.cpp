// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson

#include "check.hpp"

#include "cli.hpp"

using zar::cli::human_size;

TEST(human_readable_sizes)
{
    CHECK(human_size(0) == "0");
    CHECK(human_size(1023) == "1023");
    CHECK(human_size(1024) == "1.0K");
    CHECK(human_size(1536) == "1.5K");
    CHECK(human_size(999'999) == "976.6K");
    CHECK(human_size((1 << 20) - 1) == "1.0M");    // rounds up into the next unit
    CHECK(human_size(32'896'812) == "31.4M");
    CHECK(human_size(std::uint64_t{5} << 30) == "5.0G");
    CHECK(human_size(std::uint64_t{3} << 40) == "3.0T");
    CHECK(human_size(~std::uint64_t{0}) == "16.0E");
}

TEST(size_arguments)
{
    using zar::cli::parse_size;
    CHECK(parse_size("512", "x") == 512);
    CHECK(parse_size("4K", "x") == 4096);
    CHECK(parse_size("1M", "x") == 1 << 20);
    CHECK(parse_size("2g", "x") == std::uint64_t{2} << 30);
    CHECK_THROWS(parse_size("", "x"));
    CHECK_THROWS(parse_size("1.5M", "x"));
    CHECK_THROWS(parse_size("M", "x"));
    CHECK_THROWS(parse_size("99999999999T", "x"));
}

TEST(printable_names_cannot_control_the_terminal)
{
    using zar::cli::printable;
    CHECK(printable("plain/name.txt") == "plain/name.txt");
    CHECK(printable("caf\xc3\xa9") == "caf\xc3\xa9");                     // valid UTF-8 is kept
    CHECK(printable("x\x1b[31mred") == "x\\x1b[31mred");                   // escape sequences aren't
    CHECK(printable("tab\there\nnew") == "tab\\x09here\\x0anew");
    CHECK(printable("back\\slash") == "back\\\\slash");
    CHECK(printable("latin\xe9") == "latin\\xe9");                         // not UTF-8: bytes escaped
}
