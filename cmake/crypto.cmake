# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 Kevin H. Patterson
#
# BLAKE3 (block hashes) and Monocypher's Ed25519 (signatures): the copies in
# third_party/, or with ZAR_USE_SYSTEM_BLAKE3 / ZAR_USE_SYSTEM_MONOCYPHER,
# installed ones found through pkg-config. Sets ZAR_BLAKE3_TARGET and
# ZAR_MONOCYPHER_TARGET.

if(ZAR_USE_SYSTEM_BLAKE3)
    pkg_check_modules(BLAKE3 REQUIRED IMPORTED_TARGET libblake3>=1.0)
    set(ZAR_BLAKE3_TARGET PkgConfig::BLAKE3)
else()
    set(dir ${CMAKE_CURRENT_SOURCE_DIR}/third_party/blake3)
    add_library(zar_blake3 STATIC ${dir}/blake3.c ${dir}/blake3_dispatch.c ${dir}/blake3_portable.c)
    target_include_directories(zar_blake3 SYSTEM PUBLIC ${dir})
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" processor)
    if(MSVC)
        # Not yet a supported platform; the portable code works anywhere.
        target_compile_definitions(zar_blake3 PRIVATE BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512)
    elseif(processor MATCHES "^(x86_64|amd64|i[3-6]86|x86)$")
        # Each SIMD level in its own file, compiled for that level and chosen
        # at run time by what the CPU supports.
        target_sources(zar_blake3 PRIVATE ${dir}/blake3_sse2.c ${dir}/blake3_sse41.c
                                          ${dir}/blake3_avx2.c ${dir}/blake3_avx512.c)
        set_source_files_properties(${dir}/blake3_sse2.c PROPERTIES COMPILE_OPTIONS -msse2)
        set_source_files_properties(${dir}/blake3_sse41.c PROPERTIES COMPILE_OPTIONS -msse4.1)
        set_source_files_properties(${dir}/blake3_avx2.c PROPERTIES COMPILE_OPTIONS -mavx2)
        set_source_files_properties(${dir}/blake3_avx512.c PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512vl")
    elseif(processor MATCHES "^(aarch64|arm64)$")
        target_sources(zar_blake3 PRIVATE ${dir}/blake3_neon.c)
        target_compile_definitions(zar_blake3 PRIVATE BLAKE3_USE_NEON=1)
    else()
        target_compile_definitions(zar_blake3 PRIVATE
            BLAKE3_USE_NEON=0 BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512)
    endif()
    set(ZAR_BLAKE3_TARGET zar_blake3)
endif()

if(ZAR_USE_SYSTEM_MONOCYPHER)
    pkg_check_modules(MONOCYPHER REQUIRED IMPORTED_TARGET monocypher>=4.0)
    set(ZAR_MONOCYPHER_TARGET PkgConfig::MONOCYPHER)
else()
    set(dir ${CMAKE_CURRENT_SOURCE_DIR}/third_party/monocypher)
    add_library(zar_monocypher STATIC ${dir}/monocypher.c ${dir}/monocypher-ed25519.c)
    target_include_directories(zar_monocypher SYSTEM PUBLIC ${dir})
    set(ZAR_MONOCYPHER_TARGET zar_monocypher)
endif()
