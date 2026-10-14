# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 Kevin H. Patterson

cmake_minimum_required(VERSION 3.20)

project(zar
    VERSION 0.2.1
    DESCRIPTION "Random-access zstd archiver"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(ZAR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(ZAR_BUILD_TESTS "Build the test suite" ON)

# libzstd: prefer pkg-config, fall back to zstd's own CMake package.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(ZSTD IMPORTED_TARGET libzstd>=1.4.0)
endif()
if(TARGET PkgConfig::ZSTD)
    set(ZAR_ZSTD_TARGET PkgConfig::ZSTD)
else()
    find_package(zstd CONFIG REQUIRED)
    if(TARGET zstd::libzstd_shared)
        set(ZAR_ZSTD_TARGET zstd::libzstd_shared)
    else()
        set(ZAR_ZSTD_TARGET zstd::libzstd_static)
    endif()
endif()

add_library(zar_warnings INTERFACE)
if(MSVC)
    target_compile_options(zar_warnings INTERFACE /W4 $<$<BOOL:${ZAR_WARNINGS_AS_ERRORS}>:/WX>)
else()
    target_compile_options(zar_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        $<$<BOOL:${ZAR_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

add_library(libzar STATIC
    src/zar/archive_reader.cpp
    src/zar/archive_writer.cpp
    src/zar/compressor.cpp
    src/zar/crc32c.cpp
    src/zar/dictionary.cpp
    src/zar/creator.cpp
    src/zar/extractor.cpp
    src/zar/format.cpp
    src/zar/index.cpp
    src/zar/names.cpp
    src/zar/output.cpp
    src/zar/policy.cpp
    src/zar/thread_pool.cpp)
set_target_properties(libzar PROPERTIES OUTPUT_NAME zar)

set(ZAR_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
configure_file(src/zar/version.hpp.in ${ZAR_GENERATED_DIR}/zar/version.hpp @ONLY)
target_include_directories(libzar PUBLIC src ${ZAR_GENERATED_DIR})
find_package(Threads REQUIRED)
target_link_libraries(libzar PUBLIC ${ZAR_ZSTD_TARGET} Threads::Threads PRIVATE zar_warnings)

# The build time and git revision, refreshed on every build. Only the
# commands link this, so the library and tests don't rebuild for it.
find_package(Git QUIET)
add_custom_target(zar_build_info_source
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DOUTPUT=${ZAR_GENERATED_DIR}/build_info.cpp
        -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_info.cmake
    BYPRODUCTS ${ZAR_GENERATED_DIR}/build_info.cpp
    COMMENT "Recording build time and git revision")
add_library(zar_build_info STATIC ${ZAR_GENERATED_DIR}/build_info.cpp)
add_dependencies(zar_build_info zar_build_info_source)
target_include_directories(zar_build_info PUBLIC ${ZAR_GENERATED_DIR})

add_executable(zar src/zar_main.cpp)
target_link_libraries(zar PRIVATE libzar zar_build_info zar_warnings)

add_executable(unzar src/unzar_main.cpp)
target_link_libraries(unzar PRIVATE libzar zar_build_info zar_warnings)

install(TARGETS zar unzar RUNTIME DESTINATION bin)
install(FILES LICENSE.txt README.md FORMAT.md DESTINATION share/doc/zar)

if(ZAR_BUILD_TESTS)
    enable_testing()
    add_executable(zar_tests
        tests/main.cpp
        tests/cli_tests.cpp
        tests/format_tests.cpp
        tests/index_tests.cpp
        tests/policy_tests.cpp
        tests/writer_tests.cpp
        tests/extractor_tests.cpp)
    target_link_libraries(zar_tests PRIVATE libzar zar_warnings)
    add_test(NAME zar_tests COMMAND zar_tests)
endif()
