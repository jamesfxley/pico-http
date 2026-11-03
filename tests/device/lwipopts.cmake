# The device image is the application owning this whole-build lwIP configuration
# (AR-10). Keep its options off the fallback target and out of installed usage.
add_library(pico_http_device_lwipopts INTERFACE)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/device-lwipopts/lwipopts.h
    "#pragma once\n#include <lwipopts_reference.h> // IWYU pragma: export\n"
    "#undef MEMP_NUM_TCP_PCB\n#define MEMP_NUM_TCP_PCB 24\n"
    # Idle loopback peers need extra PCBs, not twice the simultaneous TX load.
    # Keep the reference's heap, segment and ROM-header budgets for eight
    # transmitting application slots; the peer tests serialize their writes.
    "#undef MEM_SIZE\n#define MEM_SIZE (12 * ((TCP_SND_QUEUELEN / 2) * PICO_HTTP_REF_TCP_HEADER_BYTES + TCP_MSS + PICO_HTTP_REF_TCP_HEADER_BYTES) + 2048)\n"
    "#undef MEMP_NUM_TCP_SEG\n#define MEMP_NUM_TCP_SEG (12 * (TCP_SEGMENTS_PER_CONN + 8))\n"
    "#undef MEMP_NUM_PBUF\n#define MEMP_NUM_PBUF (12 * TCP_SND_QUEUELEN)\n")
target_include_directories(pico_http_device_lwipopts INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/device-lwipopts>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/lwipopts_reference/include>)
target_compile_definitions(pico_http_device_lwipopts INTERFACE
    $<BUILD_INTERFACE:LWIP_NETIF_LOOPBACK=1>
    $<BUILD_INTERFACE:LWIP_HAVE_LOOPIF=1>
    $<BUILD_INTERFACE:LWIP_SINGLE_NETIF=0>
    $<BUILD_INTERFACE:LWIP_NETIF_LOOPBACK_MULTITHREADING=0>)
target_link_libraries(pico_http_device_lwipopts INTERFACE pico_http_config)
