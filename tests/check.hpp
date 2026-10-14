// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 Kevin H. Patterson
//
// A deliberately tiny test harness: TEST(name) { CHECK(...); }
#pragma once

#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace zar::test {

struct Case {
    std::string_view name;
    std::function<void()> body;
};

inline std::vector<Case>& registry()
{
    static std::vector<Case> cases;
    return cases;
}

inline int failures = 0;

struct Register {
    Register(std::string_view name, std::function<void()> body)
    {
        registry().push_back({name, std::move(body)});
    }
};

inline void fail(const char* file, int line, std::string_view what)
{
    ++failures;
    std::cerr << file << ':' << line << ": check failed: " << what << '\n';
}

} // namespace zar::test

#define ZAR_CONCAT_(a, b) a##b
#define ZAR_CONCAT(a, b) ZAR_CONCAT_(a, b)

#define TEST(name)                                                                       \
    static void ZAR_CONCAT(test_, name)();                                               \
    static const ::zar::test::Register ZAR_CONCAT(register_, name){#name,                \
                                                                   ZAR_CONCAT(test_, name)}; \
    static void ZAR_CONCAT(test_, name)()

#define CHECK(expr)                                                                      \
    do {                                                                                 \
        if (!(expr))                                                                     \
            ::zar::test::fail(__FILE__, __LINE__, #expr);                                \
    } while (0)

#define CHECK_THROWS(expr)                                                               \
    do {                                                                                 \
        bool threw_ = false;                                                             \
        try {                                                                            \
            (void)(expr);                                                                \
        } catch (...) {                                                                  \
            threw_ = true;                                                               \
        }                                                                                \
        if (!threw_)                                                                     \
            ::zar::test::fail(__FILE__, __LINE__, "expected exception: " #expr);         \
    } while (0)

// Like CHECK_THROWS, but the exception's message must also contain `text`.
#define CHECK_THROWS_WITH(expr, text)                                                    \
    do {                                                                                 \
        std::string what_;                                                               \
        try {                                                                            \
            (void)(expr);                                                                \
        } catch (const std::exception& e_) {                                             \
            what_ = e_.what();                                                           \
        }                                                                                \
        if (what_.find(text) == std::string::npos)                                       \
            ::zar::test::fail(__FILE__, __LINE__,                                        \
                              "expected exception mentioning \"" + std::string(text) +   \
                                  "\": " #expr + (what_.empty() ? "" : " threw: " + what_)); \
    } while (0)
