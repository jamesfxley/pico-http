cmake_minimum_required(VERSION 3.26)

include(cmake/pico_family_bootstrap.cmake)

# Firmware needs CYW43 for its radio and RP2350 for pico-logging; only the
# consumer-selected board is checked (AR-12).
pico_family_bootstrap(PICO_HTTP PLATFORM rp2350 CYW43)

project(pico_http VERSION 0.1.0 LANGUAGES C CXX ASM)

house_cxx_common_settings(${PICO_HTTP_BUILD_FIRMWARE})
pico_family_init_sdk(${PICO_HTTP_BUILD_FIRMWARE})

# The SDK adds vendor sources to firmware targets, so owned sources take
# -Werror per source. Host targets take it directly. Freestanding hardening
# follows the toolchain.
house_cxx_add_strict_target(pico_http_strict
    VENDOR_SOURCES ${PICO_HTTP_BUILD_FIRMWARE}
)

# Profile definitions propagate through pico_http_config because they
# determine consumer class layouts.
pico_family_declare_profile(PICO_HTTP NAME PROFILE
    DEFAULT default VALUES default p8-rp2350
    DOC "pico-http sizing profile: default or p8-rp2350")

# Sanitizers are set before creating GoogleTest so both sides of library
# boundaries are instrumented.
house_cxx_enable_sanitizers(PICO_HTTP)

# Options are declared before subdirectories read them.
house_cxx_declare_tests_option(PICO_HTTP)

# Fuzzing is declared here so unsupported configurations report why it is
# unavailable.
house_cxx_declare_fuzzers_option(PICO_HTTP)

# Example firmware images (benchmark app, etc.) are firmware-only.
set(PICO_HTTP_ENABLE_EXAMPLES_DEFAULT OFF)
if(PROJECT_IS_TOP_LEVEL AND PICO_HTTP_BUILD_FIRMWARE)
    set(PICO_HTTP_ENABLE_EXAMPLES_DEFAULT ON)
endif()
option(PICO_HTTP_ENABLE_EXAMPLES "Build the firmware examples"
    ${PICO_HTTP_ENABLE_EXAMPLES_DEFAULT})

# The top-level project owns package exports for host and firmware.
house_cxx_declare_install_option(PICO_HTTP)

# Exported targets need their dependencies in an export set, so dependency
# install rules are enabled before fetching.
pico_family_propagate_install(PICO_HTTP PICO_LOGGING)

pico_family_propagate_install(PICO_HTTP PICO_UTIL)
pico_family_dependency(pico_logging
    REPOSITORY https://github.com/jamesfxley/pico-logging.git
    REVISION 724f4555863d60fb68b1ddff944c0ce89398c16a)
pico_family_dependency(pico_util
    REPOSITORY https://github.com/jamesfxley/pico-util.git
    REVISION 870e68b33012c9052d3336be4ad9abd58d44d4d1)
pico_family_make_available(pico_logging pico_util)

set(PICO_LWIPOPTS_TARGET "" CACHE STRING "Target carrying the application's lwipopts.h")
set(PICO_HTTP_WIFI_SSID "" CACHE STRING "Wi-Fi SSID the example firmware should join")
set(PICO_HTTP_WIFI_PASSWORD "" CACHE STRING "Wi-Fi password (WPA2-PSK)")

# The device-test application offers an explicitly selected whole-build lwIP
# configuration (AR-10).
if(PROJECT_IS_TOP_LEVEL AND PICO_HTTP_BUILD_FIRMWARE)
    include(tests/device/lwipopts.cmake)
endif()

add_subdirectory(src)

if(PROJECT_IS_TOP_LEVEL AND PICO_HTTP_BUILD_FIRMWARE)
    add_subdirectory(tests/device)
endif()

if(PICO_HTTP_ENABLE_TESTS)
    house_cxx_enable_gtest()
    add_subdirectory(tests)
endif()

# Host builds ignore PICO_HTTP_ENABLE_EXAMPLES because the images need the
# SDK.
if(PICO_HTTP_ENABLE_EXAMPLES AND PICO_HTTP_BUILD_FIRMWARE)
    add_subdirectory(examples)
endif()

if(PICO_HTTP_ENABLE_INSTALL)
    # house_cxx_install_package strips the namespace prefix so imported
    # targets match source targets. Host test support is exported for
    # downstream tests.
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

    # Internal export needed for the static library link closure. Its device
    # include paths and options are build-only, not an installed module API.
    if(TARGET pico_http_device_lwipopts)
        list(APPEND PICO_HTTP_EXPORT_TARGETS pico_http_device_lwipopts)
    endif()

    # Only targets built in this configuration are exported: firmware lwIP or
    # host test support.
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
