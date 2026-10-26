# Build-wide sizing policy: slot count, MSS-derived buffer extents, header caps,
# boundary limits and the named profiles. Header-only, but it is the target that
# carries the profile compile definitions as INTERFACE usage requirements, so
# every consumer — in-tree targets, FetchContent users and installed-package
# users alike — compiles against the same class layout. Setting them with a
# directory-scoped add_compile_definitions instead would reach only this repo's
# own targets and silently mismatch a consumer's translation units.
add_library(pico_http_config INTERFACE)
add_library(pico_http::config ALIAS pico_http_config)

target_sources(pico_http_config INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/pico_http/config/limits.hpp
        include/pico_http/config/profile.hpp
        include/pico_http/config/sizing.hpp
)

target_include_directories(pico_http_config INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pico_http_config INTERFACE cxx_std_20)

# The header-storage caps. sizing.hpp declares them with an #ifndef so a value
# can come from the compile line, but a bare -D on a consumer's own target
# reaches only that consumer's translation units: the archive it links was
# compiled with the default, the two disagree about the layout of every
# HTTPHeaders crossing the boundary, and ProfileBinding aborts in the
# HttpService constructor at boot. Setting them here puts one value on the
# target every consumer compiles against, which is the only place the override
# can be made whole.
set(PICO_HTTP_MAX_HEADER_NAME "" CACHE STRING
    "Longest stored header name in bytes (default 64). Empty keeps the default.")
set(PICO_HTTP_MAX_HEADER_VALUE "" CACHE STRING
    "Longest stored header value in bytes (default 256). Empty keeps the default.")

foreach(cap IN ITEMS PICO_HTTP_MAX_HEADER_NAME PICO_HTTP_MAX_HEADER_VALUE)
    if(NOT "${${cap}}" STREQUAL "")
        if(NOT "${${cap}}" MATCHES "^[1-9][0-9]*$")
            message(FATAL_ERROR
                "${cap} must be a positive integer, got '${${cap}}'")
        endif()
        target_compile_definitions(pico_http_config INTERFACE
            ${cap}=${${cap}})
    endif()
endforeach()

if(PICO_HTTP_PROFILE STREQUAL "p8-rp2350")
    target_compile_definitions(pico_http_config INTERFACE
        PICO_HTTP_ACTIVE_PROFILE=::pico_http::P8Rp2350HttpProfile
        PICO_HTTP_RX_WINDOW_MSS_FACTOR=4
        PICO_HTTP_TX_RING_MSS_FACTOR=4
        PICO_HTTP_PROFILE_P8_RP2350
    )
endif()
