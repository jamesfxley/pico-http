add_library(pico_http_testing STATIC
    fake_tcp_transport.cpp
)

add_library(pico_http::testing ALIAS pico_http_testing)

target_sources(pico_http_testing PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/pico_http/testing/fake_tcp_transport.hpp
        include/pico_http/testing/service_harness.hpp
)

target_include_directories(pico_http_testing PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(pico_http_testing PUBLIC
    pico_http::config
    pico_http::tcp
    pico_http::http
)

target_compile_features(pico_http_testing PUBLIC cxx_std_20)

target_link_libraries(pico_http_testing PRIVATE
    pico_http_strict
)
