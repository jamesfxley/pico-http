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
/// Assertions for application-owned lwIP options (AR-10). This header must
/// not define options.
/// lwip_tcp_transport.cpp includes it. A second inclusion is redundant.

// Named constants avoid identical-literal comparisons after macro expansion.
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

// The raw adapter runs in the SDK async context without an OS thread.
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

// NOCOPY output needs PBUF_ROM headers for every queued ring fragment (AR-2).
// TCP_SND_QUEUELEN bounds how many pbufs a connection may queue.
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

// The PCB pool has to cover MAX_CONNECTIONS. TIME_WAIT needs extra headroom.
static_assert(pico_http::lwipopts::MEMP_NUM_TCP_PCB_VALUE >=
                  static_cast<int>(pico_http::MAX_CONNECTIONS),
              "MEMP_NUM_TCP_PCB must cover MAX_CONNECTIONS, or accept() "
              "starves before the service reaches its own limit");
static_assert(pico_http::lwipopts::MEMP_NUM_TCP_PCB_LISTEN_VALUE >= 1,
              "the service binds one listener");

// The pbuf pool backs all advertised windows concurrently to avoid upload
// stalls.
static_assert(
    pico_http::lwipopts::PBUF_POOL_SIZE_VALUE *
            pico_http::lwipopts::PBUF_POOL_BUFSIZE_VALUE >=
        pico_http::MAX_CONNECTIONS * pico_http::lwipopts::TCP_WND_VALUE,
    "PBUF_POOL_SIZE must back MAX_CONNECTIONS x TCP_WND of advertised window");
