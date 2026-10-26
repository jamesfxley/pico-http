# Benchmark firmware image: exposes /ping, /payload/<n>, /upload, /stats,
# /info and the reset routes so the host-side bench.py can exercise the
# server on real hardware. Firmware-only (guarded by the parent examples/
# add_subdirectory, which only runs for firmware builds).

# Wi-Fi credentials: pass at configure time, e.g.
#   cmake -B build-firmware -DPICO_HTTP_WIFI_SSID=myssid -DPICO_HTTP_WIFI_PASSWORD=mypassword
set(PICO_HTTP_WIFI_SSID "" CACHE STRING "Wi-Fi SSID the benchmark firmware should join")
set(PICO_HTTP_WIFI_PASSWORD "" CACHE STRING "Wi-Fi password (WPA2-PSK)")

add_executable(pico_http_bench main.cpp)

# Warnings are errors for the code this project owns. Per source rather than on
# the target: the SDK compiles its own sources into this one, and
# pico_http_strict therefore carries WERROR OFF in a firmware build.
house_cxx_werror_sources(main.cpp)

pico_set_program_name(pico_http_bench "pico_http_bench")
pico_set_program_version(pico_http_bench "${PROJECT_VERSION}")

# Route stdio over USB CDC so the family console can watch the Wi-Fi join.
pico_enable_stdio_usb(pico_http_bench 1)
pico_enable_stdio_uart(pico_http_bench 0)

# OR-4 names the profile and the build type as part of build identity, and a
# run record inherits nothing (EN-06.7). Both are CMake-side facts, so they
# reach the image the same way the version and the board do rather than being
# inferred host-side from a measurement that happens to correlate.
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

# Generates pico_http_bench.uf2 (+ .bin/.hex/.elf.map) for flashing.
pico_add_extra_outputs(pico_http_bench)
