add_library(pico_http_util INTERFACE)
add_library(pico_http::util ALIAS pico_http_util)

target_sources(pico_http_util INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/pico_http/util/tx_ring.hpp
)

target_include_directories(pico_http_util INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pico_http_util INTERFACE cxx_std_20)

# PUBLIC: tx_ring.hpp takes its wrap arithmetic from pico_util::ring_index, and
# the http module's headers carry pico_util::FixedString and as_bytes, so those
# headers reach a consumer through this library's own. Every module that needs
# them links pico_http::util already.
target_link_libraries(pico_http_util INTERFACE pico_util::core)
