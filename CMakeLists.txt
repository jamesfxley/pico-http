cmake_minimum_required(VERSION 3.26)

include(cmake/pico_family_bootstrap.cmake)

# AR-12. Firmware serves over the CYW43 radio, and pico-logging needs an
# RP2350. As a dependency the board is the consumer's, and only that is checked
# of it.
pico_family_bootstrap(PICO_HTTP PLATFORM rp2350 CYW43)

project(pico_http VERSION 0.1.0 LANGUAGES C CXX ASM)

house_cxx_common_settings(${PICO_HTTP_BUILD_FIRMWARE})
pico_family_init_sdk(${PICO_HTTP_BUILD_FIRMWARE})

# One source tree serves host and firmware, so both switches follow the
# configure rather than being fixed.
#
# VENDOR_SOURCES takes the firmware flag: the Pico SDK attaches its own sources
# through INTERFACE libraries that compile into whichever target links them, so
# a target-scoped -Werror there would land on SDK and lwIP code. The four
# cross-compiled sources this repository owns get it per source instead, in
# src/*/CMakeLists.txt. The host tree links no SDK sources, so every diagnostic
# there is this repository's to fix and the target carries the flag directly.
#
# FREESTANDING follows the toolchain, which is the helper's default: a firmware
# image has no hosted libc behind _FORTIFY_SOURCE and no dynamic loader behind
# the RELRO link options.
house_cxx_add_strict_target(pico_http_strict
    VENDOR_SOURCES ${PICO_HTTP_BUILD_FIRMWARE}
)

# Named sizing profile. "default" has a 2 KiB buffered body and a 2×MSS receive
# window; "p8-rp2350" raises them to 8 KiB and 4×MSS with a 40-entry pbuf pool.
# Both have a 4×MSS TX ring. The choice is whole-build: one set of compile
# definitions feeds the HttpService profile, the transport buffer factors, and
# lwipopts together, so the library, firmware examples (including their lwIP
# sources), and tests are sized consistently.
#
# The definitions themselves are INTERFACE usage requirements on
# pico_http_config (src/config), not directory-scoped compile definitions: they
# determine class layout, and directory scope reaches only this repo's own
# targets, which would leave a FetchContent or installed-package consumer
# compiling its translation units against a different layout than the archive it
# links.
pico_family_declare_profile(PICO_HTTP NAME PROFILE
    DEFAULT default VALUES default p8-rp2350
    DOC "pico-http sizing profile: default or p8-rp2350")

# Declares PICO_HTTP_SANITIZE. Build-wide rather than target-scoped, and ahead
# of house_cxx_enable_gtest, so the fetched GoogleTest is instrumented too --
# ASan's container-overflow detector needs both sides of a std::string boundary,
# and add_compile_options reaches only targets created after it.
house_cxx_enable_sanitizers(PICO_HTTP)

# Every knob this build has, declared before anything is added: a subdirectory
# reading an option that has not been declared yet reads an empty string.
house_cxx_declare_tests_option(PICO_HTTP)

# libFuzzer targets (Clang only). Declared here rather than beside the targets
# themselves so that enabling it on a configure that cannot honour it reports
# the reason instead of silently building nothing.
house_cxx_declare_fuzzers_option(PICO_HTTP)

# Example firmware images (benchmark app, etc.) are firmware-only.
set(PICO_HTTP_ENABLE_EXAMPLES_DEFAULT OFF)
if(PROJECT_IS_TOP_LEVEL AND PICO_HTTP_BUILD_FIRMWARE)
    set(PICO_HTTP_ENABLE_EXAMPLES_DEFAULT ON)
endif()
option(PICO_HTTP_ENABLE_EXAMPLES "Build the firmware examples"
    ${PICO_HTTP_ENABLE_EXAMPLES_DEFAULT})

# Install/export only when this is the top-level project: as a subproject the
# parent owns the export set. Not host-only any more -- pico_http_tcp_lwip's SDK
# links go through pico_family_sdk_link_libraries, which is the form an export
# set can carry, so a firmware consumer can resolve this package through
# find_package rather than only through FetchContent.
house_cxx_declare_install_option(PICO_HTTP)

# When pico-http's targets are exported, the fetched pico_logging target they
# link must be in an export set too, so its install rules must be on. Before the
# fetch below, or the dependency is configured with its default first.
pico_family_propagate_install(PICO_HTTP PICO_LOGGING)

pico_family_propagate_install(PICO_HTTP PICO_UTIL)
pico_family_dependency(pico_logging
    REPOSITORY https://github.com/jamesfxley/pico-logging.git
    REVISION fe23964417288a0dedfa27ddbd7e2f85b0915bae)
pico_family_dependency(pico_util
    REPOSITORY https://github.com/jamesfxley/pico-util.git
    REVISION 2afe3150a52429cb61c2330041cb4ec2e26c00ef)
pico_family_make_available(pico_logging pico_util)

set(PICO_LWIPOPTS_TARGET "" CACHE STRING "Target carrying the application's lwipopts.h")
set(PICO_HTTP_WIFI_SSID "" CACHE STRING "Wi-Fi SSID the example firmware should join")
set(PICO_HTTP_WIFI_PASSWORD "" CACHE STRING "Wi-Fi password (WPA2-PSK)")

add_subdirectory(src)

if(PICO_HTTP_ENABLE_TESTS)
    house_cxx_enable_gtest()
    add_subdirectory(tests)
endif()

# A host configure with -DPICO_HTTP_ENABLE_EXAMPLES=ON is a silent no-op rather
# than an error: the images need the SDK.
if(PICO_HTTP_ENABLE_EXAMPLES AND PICO_HTTP_BUILD_FIRMWARE)
    add_subdirectory(examples)
endif()

if(PICO_HTTP_ENABLE_INSTALL)
    # house_cxx_install_package strips the namespace prefix from each exported
    # name, so `pico_http::core` resolves identically through find_package and
    # through FetchContent.
    #
    # pico_http_testing ships with the package. A downstream project writing
    # tests against this library needs the same fakes this repository's own
    # tests use; without them in the export set it has to build from source or
    # write its own, and a hand-written double drifts from the seam it stands
    # in for.
    set(PICO_HTTP_EXPORT_TARGETS
        pico_http_strict
        pico_http_core
        pico_http_lwipopts_assertions
        pico_http_lwipopts_fallback
        pico_http_config
        pico_http_util
        pico_http_tcp
        pico_http_http
    )

    # The export set is what this configuration actually built, and the two
    # configurations differ at both ends. The lwIP transport is firmware-only:
    # it is the one component touching the lwIP raw API and the cyw43 async
    # context. The host-side fakes are host-only: they stand in for a transport
    # an image already has. Naming a target that was never created fails the
    # configure, so neither can be listed unconditionally.
    if(PICO_HTTP_BUILD_FIRMWARE)
        list(APPEND PICO_HTTP_EXPORT_TARGETS pico_http_tcp_lwip)
    else()
        list(APPEND PICO_HTTP_EXPORT_TARGETS pico_http_testing)
    endif()

    pico_family_sdk_config_snippet(PICO_HTTP_SDK_CONFIG TARGETS ${PICO_HTTP_EXPORT_TARGETS})
    house_cxx_install_package(
        PACKAGE pico_http VERSION ${PROJECT_VERSION}
        CONFIG_SNIPPET "${PICO_HTTP_SDK_CONFIG}"
        NAMESPACE pico_http
        TARGETS ${PICO_HTTP_EXPORT_TARGETS}
        DEPENDS pico_logging pico_util
    )
endif()
