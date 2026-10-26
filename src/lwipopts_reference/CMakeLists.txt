# A reference lwIP configuration meeting src/lwipopts/'s assertions, and
# deliberately NOT in the automatic link graph.
#
# An application owns its lwipopts.h (AR-10). This is a copy to start from, or
# a target to name directly in PICO_LWIPOPTS_TARGET -- which is what a
# standalone build of this repository does. Naming it is what puts an
# lwipopts.h on the include path; nothing else does.
#
# The header is carried as lwipopts_reference.h and reaches lwIP's own
# `#include "lwipopts.h"` through the shim directory below, so an application
# that copies the file rather than linking this target is not competing with an
# installed header of that name.
add_library(pico_http_lwipopts_fallback INTERFACE)
add_library(pico_http::lwipopts_fallback ALIAS pico_http_lwipopts_fallback)

target_sources(pico_http_lwipopts_fallback INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
    include/lwipopts_reference.h
)

target_include_directories(pico_http_lwipopts_fallback INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/shim>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# The reference configuration branches on PICO_HTTP_PROFILE_P8_RP2350 to size
# TCP_WND and PBUF_POOL_SIZE, and that macro is a usage requirement of
# pico_http::config. An application that copies the file supplies the values
# directly and needs no such link -- the assertions are what hold it to them.
target_link_libraries(pico_http_lwipopts_fallback INTERFACE pico_http::config)
