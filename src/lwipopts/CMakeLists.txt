# This module must not define lwIP options. It checks the application
# configuration (AR-10). The separate reference module is a configuration an
# application may copy.
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

# The assertions use the shared profile constants.
target_link_libraries(pico_http_lwipopts_assertions
    INTERFACE pico_http::config)
