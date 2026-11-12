# Firmware adapter for raw lwIP and the cyw43 async context.
add_library(pico_http_tcp_lwip STATIC
    lwip_tcp_transport.cpp
)
add_library(pico_http::tcp_lwip ALIAS pico_http_tcp_lwip)

# Apply -Werror per owned source so SDK and lwIP sources keep their
# diagnostics.
house_cxx_werror_sources(lwip_tcp_transport.cpp)

target_sources(pico_http_tcp_lwip PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/pico_http/tcp/lwip_tcp_transport.hpp
)

target_include_directories(pico_http_tcp_lwip PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pico_http_tcp_lwip PUBLIC cxx_std_20)

target_link_libraries(pico_http_tcp_lwip PUBLIC
    pico_http::tcp
    pico_http::lwipopts_assertions
)

# Propagate application lwIP options with the SDK INTERFACE libraries so each
# target compiling lwIP sources receives the configuration.
target_link_libraries(pico_http_tcp_lwip PUBLIC ${PICO_LWIPOPTS_TARGET})

target_link_libraries(pico_http_tcp_lwip PRIVATE
    pico_http_strict
)

# The family helper hides SDK targets during export and restores them after
# the consumer calls pico_sdk_init().
pico_family_sdk_link_libraries(pico_http_tcp_lwip
    PUBLIC
        pico_lwip_nosys
        pico_cyw43_arch
        pico_cyw43_arch_lwip_threadsafe_background
    PRIVATE
        pico_stdlib
)
