# The concrete lwIP-backed transport. Firmware-only: it is the one component
# that touches the lwIP raw API and the cyw43 async context, which is why it is
# a target of its own rather than a conditional arm of pico_http_tcp.
add_library(pico_http_tcp_lwip STATIC
    lwip_tcp_transport.cpp
)
add_library(pico_http::tcp_lwip ALIAS pico_http_tcp_lwip)

# Warnings are errors for the code this project owns. Per source rather than on
# the target: the SDK and lwIP compile their own sources into this one, and
# pico_http_strict therefore carries WERROR OFF in a firmware build, which is
# the only build this target has.
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

# The application's lwipopts.h, named once for the whole family by
# PICO_LWIPOPTS_TARGET. That is pico-tooling's requirement, cited by description
# because AR is this repository's own prefix (EN-02.8).
#
# PUBLIC, and that is not a weakening: the SDK's lwIP INTERFACE libraries
# propagate from here to whichever target links this one -- pico_http_http in a
# firmware build -- and it is that target which compiles the lwIP sources. The
# configuration has to travel the same edge they do or those sources compile
# without one. What changed is ownership, not visibility: the target named here
# is the application's, and this library ships none.
target_link_libraries(pico_http_tcp_lwip PUBLIC ${PICO_LWIPOPTS_TARGET})

target_link_libraries(pico_http_tcp_lwip PRIVATE
    pico_http_strict
)

# The SDK libraries go through this rather than target_link_libraries, because a
# target linking them cannot be exported: no SDK target is in any export set,
# and both export(TARGETS) and install(EXPORT) refuse a target whose link
# interface names one. They are wrapped in $<BUILD_LOCAL_INTERFACE:> and
# re-applied by the installed config file, once the consumer's own
# pico_sdk_init() has created them. pico-tooling's exportability requirement,
# cited by description because AR is this repository's own prefix (EN-02.8).
pico_family_sdk_link_libraries(pico_http_tcp_lwip
    PUBLIC
        pico_lwip_nosys
        pico_cyw43_arch
        pico_cyw43_arch_lwip_threadsafe_background
    PRIVATE
        pico_stdlib
)
