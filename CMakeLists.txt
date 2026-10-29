# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 Kevin H. Patterson

cmake_minimum_required(VERSION 3.20)

project(zar
    VERSION 0.5.1
    DESCRIPTION "Random-access zstd archiver"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)    # bin, share/man, share/doc/zar, ...

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(ZAR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(ZAR_BUILD_TESTS "Build the test suite" ON)
option(ZAR_USE_SYSTEM_XXHASH "Link an installed libxxhash instead of compiling in the bundled copy" OFF)

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

# libxxhash, if asked for: the same way. Otherwise third_party/xxhash is
# compiled in, header-only.
if(ZAR_USE_SYSTEM_XXHASH)
    if(PkgConfig_FOUND)
        pkg_check_modules(XXHASH IMPORTED_TARGET libxxhash>=0.8.0)
    endif()
    if(TARGET PkgConfig::XXHASH)
        set(ZAR_XXHASH_TARGET PkgConfig::XXHASH)
    else()
        find_package(xxHash 0.8 CONFIG REQUIRED)
        set(ZAR_XXHASH_TARGET xxHash::xxhash)
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
    src/zar/dedup.cpp
    src/zar/dictionary.cpp
    src/zar/creator.cpp
    src/zar/extractor.cpp
    src/zar/format.cpp
    src/zar/index.cpp
    src/zar/names.cpp
    src/zar/output.cpp
    src/zar/policy.cpp
    src/zar/posix.cpp
    src/zar/thread_pool.cpp)
set_target_properties(libzar PROPERTIES OUTPUT_NAME zar)

set(ZAR_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
configure_file(src/zar/version.hpp.in ${ZAR_GENERATED_DIR}/zar/version.hpp @ONLY)
target_include_directories(libzar PUBLIC src ${ZAR_GENERATED_DIR})
if(ZAR_USE_SYSTEM_XXHASH)
    target_compile_definitions(libzar PRIVATE ZAR_SYSTEM_XXHASH)
    target_link_libraries(libzar PRIVATE ${ZAR_XXHASH_TARGET})
else()
    target_include_directories(libzar SYSTEM PRIVATE third_party/xxhash)
endif()
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

option(ZAR_BUILD_FUZZERS "Build the fuzz target; this disables CRC checks, so such a build is never installed" OFF)
if(ZAR_BUILD_FUZZERS)
    target_compile_definitions(libzar PUBLIC FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION)
    add_executable(zar_fuzz fuzz/fuzz_archive.cpp)
    target_link_libraries(zar_fuzz PRIVATE libzar zar_warnings)

    # libFuzzer if the compiler has it (Apple's doesn't); otherwise the
    # target's own driver.
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS -fsanitize=fuzzer)
    check_cxx_source_compiles([[
        #include <cstddef>
        #include <cstdint>
        extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) { return 0; }
    ]] ZAR_HAVE_LIBFUZZER)
    unset(CMAKE_REQUIRED_FLAGS)
    if(ZAR_HAVE_LIBFUZZER)
        target_compile_options(zar_fuzz PRIVATE -fsanitize=fuzzer)
        target_link_options(zar_fuzz PRIVATE -fsanitize=fuzzer)
    else()
        target_compile_definitions(zar_fuzz PRIVATE ZAR_FUZZ_STANDALONE)
    endif()
else()
    # Manual pages, with the version filled in.
    foreach(page zar.1 unzar.1)
        configure_file(man/${page}.in ${CMAKE_CURRENT_BINARY_DIR}/man/${page} @ONLY)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/man/${page} DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
    endforeach()

    install(TARGETS zar unzar RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES LICENSE.txt README.md FORMAT.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
    if(NOT ZAR_USE_SYSTEM_XXHASH)
        install(FILES third_party/xxhash/LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR} RENAME LICENSE.xxhash)
    endif()

    # A Debian package of the above, under /usr: `cpack -G DEB` in the build
    # directory, on Linux. Its library dependencies come from dpkg-shlibdeps.
    set(CPACK_PACKAGE_NAME zar)
    set(CPACK_PACKAGE_VENDOR "Kevin H. Patterson")
    set(CPACK_PACKAGE_CONTACT "Kevin H. Patterson <kevpatt@khptech.com>")
    set(CPACK_PACKAGE_HOMEPAGE_URL https://github.com/kevpatt/zar)
    set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt)
    set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)    # zar_<version>_<arch>.deb
    set(CPACK_DEBIAN_PACKAGE_SECTION utils)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "random-access archiver built on zstd
zar packs a file or directory into a single .zar archive, and unzar lists,
verifies, and extracts it. Every file, and every chunk of a large file, is
compressed and checksummed on its own, so any file can be read without
decompressing the rest. Archives can be streamed through pipes, keep sparse
files sparse, and store duplicate data once.")
    include(CPack)
endif()

if(ZAR_BUILD_TESTS)
    enable_testing()
    add_executable(zar_tests
        tests/main.cpp
        tests/cli_tests.cpp
        tests/format_tests.cpp
        tests/index_tests.cpp
        tests/policy_tests.cpp
        tests/posix_tests.cpp
        tests/writer_tests.cpp
        tests/extractor_tests.cpp)
    target_link_libraries(zar_tests PRIVATE libzar zar_warnings)
    add_test(NAME zar_tests COMMAND zar_tests)
    add_test(NAME cli_test
             COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_test.sh $<TARGET_FILE:zar> $<TARGET_FILE:unzar>)
endif()
