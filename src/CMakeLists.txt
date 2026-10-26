set(PICO_HTTP_PUBLIC_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# config before lwipopts: the reference configuration branches on the profile
# macro, and the target carrying that macro has to exist before the one
# carrying the header can link it. config depends on nothing here, so the order
# is free.
add_subdirectory(config)
add_subdirectory(lwipopts)
add_subdirectory(lwipopts_reference)

# AR-10. The application owns its lwipopts.h; this library asserts what it needs
# of one and provides none. The variable is the family's rather than this
# library's, so an application linking pico-http and a second networking
# library names its configuration once.
if(PICO_HTTP_BUILD_FIRMWARE)
    pico_family_require_lwipopts_target(pico-http
        FALLBACK pico_http_lwipopts_fallback)
endif()

add_subdirectory(util)
add_subdirectory(tcp)
add_subdirectory(http)

# Host-side fakes are pointless in firmware images; build them for host
# consumers and for the test suite.
if(NOT PICO_HTTP_BUILD_FIRMWARE)
    add_subdirectory(testing)
endif()

add_library(pico_http_core INTERFACE)
add_library(pico_http::core ALIAS pico_http_core)

target_sources(pico_http_core INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${PICO_HTTP_PUBLIC_INCLUDE_DIR}
    FILES
        ${PICO_HTTP_PUBLIC_INCLUDE_DIR}/pico_http/pico_http.hpp
)

target_include_directories(pico_http_core INTERFACE
    $<BUILD_INTERFACE:${PICO_HTTP_PUBLIC_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pico_http_core INTERFACE cxx_std_20)

target_link_libraries(pico_http_core INTERFACE
    pico_logging::core
    pico_http::config
    pico_http::util
    pico_http::tcp
    pico_http::http
)
