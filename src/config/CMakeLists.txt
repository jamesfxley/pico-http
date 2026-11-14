# Profile definitions propagate as usage requirements because they determine
# consumer class layouts.
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

# Header capacities are set on the shared config target so the archive and
# consumers agree.
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
