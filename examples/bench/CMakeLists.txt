
set(PICO_HTTP_WIFI_SSID "" CACHE STRING "Wi-Fi SSID the benchmark firmware should join")
set(PICO_HTTP_WIFI_PASSWORD "" CACHE STRING "Wi-Fi password (WPA2-PSK)")

add_executable(pico_http_bench main.cpp)

# Per-source -Werror excludes SDK sources compiled into this target.
house_cxx_werror_sources(main.cpp)

pico_set_program_name(pico_http_bench "pico_http_bench")
pico_set_program_version(pico_http_bench "${PROJECT_VERSION}")

# USB stdio carries Wi-Fi join diagnostics to the console.
pico_enable_stdio_usb(pico_http_bench 1)
pico_enable_stdio_uart(pico_http_bench 0)

# CMake supplies profile and build-type identity to the image (OR-4).
if(PICO_HTTP_PROFILE STREQUAL "")
    set(PICO_HTTP_BENCH_PROFILE_NAME "default")
else()
    set(PICO_HTTP_BENCH_PROFILE_NAME "${PICO_HTTP_PROFILE}")
endif()

target_compile_definitions(pico_http_bench PRIVATE
    PICO_HTTP_WIFI_SSID="${PICO_HTTP_WIFI_SSID}"
    PICO_HTTP_WIFI_PASSWORD="${PICO_HTTP_WIFI_PASSWORD}"
    PICO_HTTP_BENCH_VERSION="${PROJECT_VERSION}"
    PICO_HTTP_BENCH_BOARD="${PICO_BOARD}"
    PICO_HTTP_BENCH_PROFILE="${PICO_HTTP_BENCH_PROFILE_NAME}"
    PICO_HTTP_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)

target_link_libraries(pico_http_bench PRIVATE
    pico_http_strict
    pico_http::core
    pico_util::core
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
)

pico_add_extra_outputs(pico_http_bench)
