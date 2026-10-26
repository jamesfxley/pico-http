# What this library asserts of whatever lwipopts.h the application supplied.
#
# Assertions only: this module defines no lwIP option and must not. lwIP admits
# exactly one configuration per image and the Pico SDK compiles the whole lwIP
# core into whichever target links its INTERFACE libraries, so two libraries
# each supplying one produce two lwIP builds whose struct layouts and
# memory-pool tables disagree -- resolved by archive order, and failing as
# silent memory corruption a long way from the cause (CXX-10.3). Ownership
# belongs to the application, which is the only party that can see the whole
# image (AR-10).
#
# The reference configuration an application may copy is a separate module,
# src/lwipopts_reference/, precisely so that this one carries no values.
add_library(pico_http_lwipopts_assertions INTERFACE)
add_library(pico_http::lwipopts_assertions ALIAS pico_http_lwipopts_assertions)

target_sources(pico_http_lwipopts_assertions INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
    include/pico_http/lwipopts_assertions.hpp
)

target_include_directories(pico_http_lwipopts_assertions INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pico_http_lwipopts_assertions INTERFACE cxx_std_20)

# The assertions are written against MAX_CONNECTIONS and the profile's sizing
# constants, so they need the module that carries them.
target_link_libraries(pico_http_lwipopts_assertions
    INTERFACE pico_http::config)
