# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 Kevin H. Patterson
#
# Writes OUTPUT, a C++ file recording the build time and git revision.
# Run at build time (cmake -P) so both stay current between configures.
#
# Inputs: SOURCE_DIR, OUTPUT, and GIT_EXECUTABLE (may be empty).

set(revision "unknown")
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse --short=12 HEAD
        OUTPUT_VARIABLE head
        RESULT_VARIABLE status
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(status EQUAL 0)
        set(revision "${head}")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" status --porcelain --untracked-files=no
            OUTPUT_VARIABLE changes
            ERROR_QUIET)
        if(changes)
            string(APPEND revision "-dirty")
        endif()
    endif()
endif()

# string(TIMESTAMP) honors SOURCE_DATE_EPOCH, for reproducible builds.
string(TIMESTAMP build_time "%Y-%m-%d %H:%M:%S UTC" UTC)

set(content "// Generated at build time by cmake/build_info.cmake.
#include \"zar/version.hpp\"

namespace zar::version {

extern const std::string_view kGitRevision = \"${revision}\";
extern const std::string_view kBuildTime = \"${build_time}\";

} // namespace zar::version
")

if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" previous)
endif()
if(NOT content STREQUAL previous)
    file(WRITE "${OUTPUT}" "${content}")
endif()
