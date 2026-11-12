# Reference application configuration, selected explicitly through
# PICO_LWIPOPTS_TARGET (AR-10). The shim supplies lwipopts.h without
# installing a header of that name.
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

# The reference header reads the profile macro carried by pico_http::config.
# Applications copying it supply their own values.
target_link_libraries(pico_http_lwipopts_fallback INTERFACE pico_http::config)
