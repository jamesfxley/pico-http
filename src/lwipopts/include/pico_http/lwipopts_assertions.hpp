// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>

#include "lwip/opt.h"
// PBUF_POOL_BUFSIZE's default expands to PBUF_IP_HLEN and PBUF_TRANSPORT_HLEN,
// which this header provides; misc-include-cleaner does not count a use inside
// another macro's expansion.
#include "lwip/pbuf.h"  // IWYU pragma: keep
#include "lwipopts.h"
#include "pico_http/config/sizing.hpp"

/// \file lwipopts_assertions.hpp
/// What this library requires of the application's `lwipopts.h` (AR-10).
///
/// **Assertions only. This header defines no lwIP option and must not.** lwIP
/// admits exactly one configuration per image and the Pico SDK compiles the
/// whole lwIP core into whichever target links its INTERFACE libraries, so two
/// libraries each supplying one produce two lwIP builds whose struct layouts
/// and memory-pool tables disagree. Which copy the linker resolves depends on
/// archive order, and the failure is silent memory corruption a long way from
/// the cause (`CXX-10.3`). Ownership belongs to the application, which is the
/// only party that can see the whole image.
///
/// **Include this in exactly one translation unit.** It includes `lwip/opt.h`,
/// which supplies the macros checked below from the application's
/// `lwipopts.h`. `lwip_tcp_transport.cpp` is that unit. Included more than once
/// it is merely redundant rather than wrong, but one place to look is the
/// point.
///
/// A configuration that does not meet these is a compile error naming the
/// setting, which is the whole reason this file exists: the alternative is a
/// runtime fault under load, on a device, a long way from the option that
/// caused it.

// The values are the application's. What is normative here is the relation
// each one has to something this library sizes from it.
//
// Each option is named once as a constant before being compared. These are
// macros expanding to integer arithmetic, so a comparison of two options can
// read after preprocessing as one literal against an identical literal, which
// misc-redundant-expression reports -- correctly, for the text it sees. Naming
// them is the fix rather than a suppression (EN-00.3), and it is the idiom
// tests/config/profile_binding_test.cpp already uses for the same reason.
namespace pico_http::lwipopts {

/// lwIP's NO_SYS, as a constant.
inline constexpr int NO_SYS_VALUE = NO_SYS;
/// lwIP's LWIP_TCP, as a constant.
inline constexpr int LWIP_TCP_VALUE = LWIP_TCP;
/// lwIP's LWIP_RAW, as a constant.
inline constexpr int LWIP_RAW_VALUE = LWIP_RAW;
/// lwIP's LWIP_SOCKET, as a constant.
inline constexpr int LWIP_SOCKET_VALUE = LWIP_SOCKET;
/// lwIP's LWIP_NETIF_TX_SINGLE_PBUF, as a constant.
inline constexpr int LWIP_NETIF_TX_SINGLE_PBUF_VALUE =
    LWIP_NETIF_TX_SINGLE_PBUF;
/// lwIP's MEMP_NUM_PBUF, as a constant.
inline constexpr std::size_t MEMP_NUM_PBUF_VALUE = MEMP_NUM_PBUF;
/// lwIP's TCP_SND_QUEUELEN, as a constant.
inline constexpr std::size_t TCP_SND_QUEUELEN_VALUE = TCP_SND_QUEUELEN;
/// lwIP's TCP_QUEUE_OOSEQ, as a constant.
inline constexpr int TCP_QUEUE_OOSEQ_VALUE = TCP_QUEUE_OOSEQ;
/// lwIP's TCP_OOSEQ_MAX_PBUFS, as a constant.
inline constexpr int TCP_OOSEQ_MAX_PBUFS_VALUE = TCP_OOSEQ_MAX_PBUFS;
/// lwIP's MEMP_NUM_TCP_PCB, as a constant.
inline constexpr int MEMP_NUM_TCP_PCB_VALUE = MEMP_NUM_TCP_PCB;
/// lwIP's MEMP_NUM_TCP_PCB_LISTEN, as a constant.
inline constexpr int MEMP_NUM_TCP_PCB_LISTEN_VALUE = MEMP_NUM_TCP_PCB_LISTEN;
/// lwIP's PBUF_POOL_SIZE, as a constant.
inline constexpr std::size_t PBUF_POOL_SIZE_VALUE = PBUF_POOL_SIZE;
/// lwIP's PBUF_POOL_BUFSIZE, as a constant.
inline constexpr std::size_t PBUF_POOL_BUFSIZE_VALUE = PBUF_POOL_BUFSIZE;
/// lwIP's TCP_WND, as a constant.
inline constexpr std::size_t TCP_WND_VALUE = TCP_WND;

}  // namespace pico_http::lwipopts

// The raw API, driven from the SDK's async context. Neither is a preference:
// the transport is written against tcp_* callbacks, and there is no OS thread
// for a socket or netconn API to run on.
static_assert(pico_http::lwipopts::NO_SYS_VALUE == 1,
              "pico-http requires NO_SYS=1: the transport is the lwIP raw API "
              "driven from the SDK async context");
static_assert(pico_http::lwipopts::LWIP_TCP_VALUE == 1,
              "pico-http is an HTTP server over TCP");
static_assert(pico_http::lwipopts::LWIP_RAW_VALUE == 1,
              "pico-http uses the lwIP raw (callback) API, not sockets");
static_assert(pico_http::lwipopts::LWIP_SOCKET_VALUE == 0,
              "pico-http does not use the socket API; enabling it costs flash "
              "and a thread this image does not have");

// Zero-copy transmit (AR-2). The TX path hands tcp_write a pointer into the
// connection's own ring without TCP_WRITE_FLAG_COPY; LWIP_NETIF_TX_SINGLE_PBUF
// makes lwIP force that flag and copy every sent byte into its heap. Each
// piece of ring queued then takes a PBUF_ROM header, and TCP_SND_QUEUELEN caps
// the pbufs one connection may queue, so a pool of MAX_CONNECTIONS times that
// cannot refuse one: a refused write waits for the transport's poll, which
// shows up as a stall rather than as a configuration error.
static_assert(pico_http::lwipopts::LWIP_NETIF_TX_SINGLE_PBUF_VALUE == 0,
              "pico-http's transmit path is zero-copy (AR-2): "
              "LWIP_NETIF_TX_SINGLE_PBUF=1 makes tcp_write copy every sent "
              "byte into lwIP's heap");
static_assert(pico_http::lwipopts::MEMP_NUM_PBUF_VALUE >=
                  pico_http::MAX_CONNECTIONS *
                      pico_http::lwipopts::TCP_SND_QUEUELEN_VALUE,
              "MEMP_NUM_PBUF must cover MAX_CONNECTIONS x TCP_SND_QUEUELEN "
              "PBUF_ROM headers for the zero-copy transmit path");

// The out-of-order queue is bounded, so a peer that withholds one segment
// cannot pin RAM indefinitely.
static_assert(pico_http::lwipopts::TCP_QUEUE_OOSEQ_VALUE == 0 ||
                  pico_http::lwipopts::TCP_OOSEQ_MAX_PBUFS_VALUE > 0,
              "an unbounded out-of-order queue lets a hostile peer pin RAM: "
              "set TCP_OOSEQ_MAX_PBUFS, or TCP_QUEUE_OOSEQ=0");

// Capacity, expressed against what this library was sized for rather than
// against a number. MAX_CONNECTIONS is the profile's; the PCB pool has to
// cover it, with headroom for connections lingering in TIME_WAIT.
static_assert(pico_http::lwipopts::MEMP_NUM_TCP_PCB_VALUE >=
                  static_cast<int>(pico_http::MAX_CONNECTIONS),
              "MEMP_NUM_TCP_PCB must cover MAX_CONNECTIONS, or accept() "
              "starves before the service reaches its own limit");
static_assert(pico_http::lwipopts::MEMP_NUM_TCP_PCB_LISTEN_VALUE >= 1,
              "the service binds one listener");

// The pbuf pool backs every connection's advertised window at once. Below that
// the window is advertised and cannot be filled, which shows up as a stall
// under concurrent upload rather than as a configuration error.
static_assert(
    pico_http::lwipopts::PBUF_POOL_SIZE_VALUE *
            pico_http::lwipopts::PBUF_POOL_BUFSIZE_VALUE >=
        pico_http::MAX_CONNECTIONS * pico_http::lwipopts::TCP_WND_VALUE,
    "PBUF_POOL_SIZE must back MAX_CONNECTIONS x TCP_WND of advertised window");
