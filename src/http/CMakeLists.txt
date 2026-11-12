add_library(pico_http_http STATIC
    http_service.cpp
)
add_library(pico_http::http ALIAS pico_http_http)

set(PICO_HTTP_PUBLIC_HEADERS
    include/pico_http/http/default_service.hpp
    include/pico_http/http/detail/body_source.hpp
    include/pico_http/http/detail/default_handlers.hpp
    include/pico_http/http/http_parser.hpp
    include/pico_http/http/detail/keep_alive.hpp
    include/pico_http/http/detail/parsing.hpp
    include/pico_http/http/detail/pending_pool.hpp
    include/pico_http/http/detail/response_writer.hpp
    include/pico_http/http/headers.hpp
    include/pico_http/http/http_service.hpp
    include/pico_http/http/message.hpp
    include/pico_http/http/route_table.hpp
)

target_sources(pico_http_http PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES ${PICO_HTTP_PUBLIC_HEADERS}
)

# Version facts and the umbrella include path are private. The core target
# depends on this module.
house_cxx_version_definitions(pico_http_http pico_http PRIVATE)

target_include_directories(pico_http_http PRIVATE
    $<BUILD_INTERFACE:${PICO_HTTP_PUBLIC_INCLUDE_DIR}>
)

target_include_directories(pico_http_http PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pico_http_http PUBLIC cxx_std_20)

target_link_libraries(pico_http_http PUBLIC
    pico_logging::core
    pico_http::config
    pico_http::util
    pico_http::tcp
)

target_link_libraries(pico_http_http PRIVATE
    pico_http_strict
)

if(PICO_HTTP_BUILD_FIRMWARE)
    # Only firmware defines the factory that constructs LwipTcpTransport.
    target_sources(pico_http_http PRIVATE
        default_service.cpp
    )
    # Apply -Werror per owned source so SDK sources keep their diagnostics.
    house_cxx_werror_sources(http_service.cpp default_service.cpp)
    target_link_libraries(pico_http_http PUBLIC
        pico_http::tcp_lwip
    )
    # The family helper makes SDK links exportable; a private direct link
    # would still expose unexported targets through LINK_ONLY.
    pico_family_sdk_link_libraries(pico_http_http
        PRIVATE
            pico_stdlib
            pico_cyw43_arch_lwip_threadsafe_background
    )
endif()
