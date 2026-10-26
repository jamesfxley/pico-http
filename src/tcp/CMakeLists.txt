# The transport-independent core: the ITcpTransport seam, connection identity,
# the slot table and the timeout scheduler. Host-safe and header-only, so the
# target type no longer depends on whether this is a firmware build and no
# scope variable has to be threaded through the calls below. The concrete
# lwIP-backed transport is a separate firmware-only target (lwip/).
add_library(pico_http_tcp INTERFACE)
add_library(pico_http::tcp ALIAS pico_http_tcp)

target_sources(pico_http_tcp INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/pico_http/tcp/tcp_transport.hpp
        include/pico_http/tcp/connection_id.hpp
        # The documented porting seam (docs/usage-guide.md): a downstream
        # transport derives from TransportBase, so it ships with the package.
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

# Host-only unit tests for the transport-independent core (slot table,
# generation handles, timeout scheduler). The lwIP glue is validated on-device.