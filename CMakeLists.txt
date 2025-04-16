cmake_minimum_required(VERSION 3.15)

#
# Project Setup
#
set(CMAKE_C_COMPILER /usr/bin/clang) # optionally use gcc as well
set(CMAKE_CXX_COMPILER /usr/bin/clang++)
project(starpack VERSION 1.0 LANGUAGES CXX)

# Use C++20 features
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Verbose makes it easier to see compiler/linker commands
# set(CMAKE_VERBOSE_MAKEFILE ON)

#
# Source & Include Directories
#
# Collect all .cpp files in src/ recursively, to form the main starpack executable.
file(GLOB_RECURSE SOURCES src/*.cpp)
include_directories(include)

#
# Linker Search Paths
#
# The user might have installed libs in /usr/local/lib or /usr/lib
# We'll add those paths so the linker can find them more easily.
link_directories(/usr/local/lib /usr/lib)

#
# Find Common Libraries
#
# 1. OpenSSL (dynamically linked)
find_package(OpenSSL REQUIRED)

# 2. ZLIB (dynamically linked)
find_package(ZLIB REQUIRED)

# 3. PThreads
find_package(Threads REQUIRED)

#
# Find Libarchive (dynamically linked)
#
# We use pkg-config to locate libarchive. Setting PKG_CONFIG_USE_STATIC_LIBS OFF
# ensures we prefer dynamic linking for this library.
find_package(PkgConfig REQUIRED)
set(PKG_CONFIG_USE_STATIC_LIBS OFF)
pkg_check_modules(LIBARCHIVE REQUIRED libarchive)
include_directories(${LIBARCHIVE_INCLUDE_DIRS})

#
# Find cURL (dynamically linked)
#
# If you have a system-wide cURL dev package, you can locate it with find_path / find_library
# or a simpler find_package if a config module is available. We'll do a basic approach here:
#
find_path(CURL_INCLUDE_DIR curl/curl.h PATHS /usr/local/include /usr/include)
if(NOT CURL_INCLUDE_DIR)
    message(FATAL_ERROR "cURL include directory not found!")
endif()

find_library(CURL_LIBRARY NAMES curl PATHS /usr/local/lib /usr/lib)
if(NOT CURL_LIBRARY)
    message(FATAL_ERROR "Dynamic cURL library (libcurl.so) not found!")
endif()

include_directories(${CURL_INCLUDE_DIR})

#
# YAML-CPP (statically linked)
#
# This expects a typical FindYAML-CPP.cmake or config module for dynamic usage,
# but we want a static link. We'll forcibly link it statically by controlling
# the linker flags below.
#
find_package(yaml-cpp REQUIRED)

#
# Additional Libraries (dynamically linked)
#
find_library(BROTLIDEC_LIBRARY brotlidec REQUIRED)
find_library(BROTLIENC_LIBRARY brotlienc REQUIRED)
find_library(NGHTTP2_LIBRARY nghttp2 REQUIRED)
find_library(PCRE_LIBRARY pcre2-8 REQUIRED)
find_library(ZSTD_LIBRARY zstd REQUIRED)

#
# Locate dynamic openldap, and lber
#
find_library(OPENLDAP_LIBRARY ldap REQUIRED)
find_library(LBER_LIBRARY lber REQUIRED)

# system uses IDN2, PSL, unistring, etc. dynamically:
find_library(LIBIDN2_LIBRARY idn2 REQUIRED)
find_library(LIBPSL_LIBRARY psl REQUIRED)
find_library(UNISTRING_LIBRARY unistring REQUIRED)

#
# Prepare the Executable
#
add_executable(starpack ${SOURCES})

#
# Linking Logic
#
# We want only yaml-cpp to be statically linked.
# Everything else is dynamic. We'll use the linker flags:
#   -Wl,-Bstatic -> subsequent libs are linked statically
#   -Wl,-Bdynamic -> revert to dynamic linking for subsequent libs
#
target_link_libraries(starpack PRIVATE
    ${CURL_LIBRARY}
    ${ZLIB_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    ${BROTLIDEC_LIBRARY}
    ${BROTLIENC_LIBRARY}
    ${NGHTTP2_LIBRARY}
    ${PCRE_LIBRARY}
    ${ZSTD_LIBRARY}
    ${OPENLDAP_LIBRARY}
    ${LBER_LIBRARY}
    ${LIBIDN2_LIBRARY}
    ${LIBPSL_LIBRARY}
    ${UNISTRING_LIBRARY}
    ${LIBARCHIVE_LIBRARIES}  # from pkg-config
    Threads::Threads
    -Wl,-Bstatic
        yaml-cpp
    -Wl,-Bdynamic
)

set(CMAKE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} -ldl -lrt -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,-O2"
)

#
# Install Rules
install(TARGETS starpack DESTINATION bin)
