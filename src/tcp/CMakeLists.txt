# The host-safe transport core is header-only. The firmware adapter is a
# separate target.
add_library(pico_http_tcp INTERFACE)
add_library(pico_http::tcp ALIAS pico_http_tcp)

target_sources(pico_http_tcp INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/pico_http/tcp/tcp_transport.hpp
        include/pico_http/tcp/connection_id.hpp
        # TransportBase is installed for downstream transport implementations.
        include/pico_http/tcp/transport_base.hpp
        include/pico_http/tcp/detail/slot_table.hpp
        include/pico_http/tcp/detail/timeout_scheduler.hpp
)

target_include_directories(pico_http_tcp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pico_http_tcp INTERFACE cxx_std_20)

target_link_libraries(pico_http_tcp INTERFACE
    pico_logging::core
    pico_http::config
    pico_http::util
)

if(PICO_HTTP_BUILD_FIRMWARE)
    add_subdirectory(lwip)
endif()

# Host tests cover the portable core; device tests cover the lwIP adapter.
