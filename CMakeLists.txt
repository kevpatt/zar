cmake_minimum_required(VERSION 3.20)

project(zar
    VERSION 0.1.0
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
    src/zar/crc32c.cpp
    src/zar/format.cpp
    src/zar/policy.cpp)
set_target_properties(libzar PROPERTIES OUTPUT_NAME zar)
target_include_directories(libzar PUBLIC src)
target_compile_definitions(libzar PUBLIC ZAR_VERSION="${PROJECT_VERSION}")
target_link_libraries(libzar PUBLIC ${ZAR_ZSTD_TARGET} PRIVATE zar_warnings)

add_executable(zar src/zar_main.cpp)
target_link_libraries(zar PRIVATE libzar zar_warnings)

add_executable(unzar src/unzar_main.cpp)
target_link_libraries(unzar PRIVATE libzar zar_warnings)

install(TARGETS zar unzar RUNTIME DESTINATION bin)

if(ZAR_BUILD_TESTS)
    enable_testing()
    add_executable(zar_tests tests/format_tests.cpp tests/policy_tests.cpp tests/main.cpp)
    target_link_libraries(zar_tests PRIVATE libzar zar_warnings)
    add_test(NAME zar_tests COMMAND zar_tests)
endif()
