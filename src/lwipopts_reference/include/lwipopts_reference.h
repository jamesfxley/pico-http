// SPDX-License-Identifier: MIT
#pragma once

// cppcoreguidelines-macro-usage: lwIP reads its option macros during
// preprocessing.
// NOLINTBEGIN(cppcoreguidelines-macro-usage)

// 8 concurrent application connections (P8 profile) plus headroom for PCBs
// lingering in TIME_WAIT / used by mDNS, so churn never starves accept().
#define MEMP_NUM_TCP_PCB 12        // Number of simultaneously active TCP conns
#define MEMP_NUM_TCP_PCB_LISTEN 2  // Listener + spare
#define TCP_MSL 3000               // Shorten TIME_WAIT (ms) to free PCBs faster
// Initial retransmission timeout, used until a connection has an RTT sample.
// lwIP's opt.h documents 3000 ms as the traditional TCP value and 1 s (RFC
// 6298 §2.1) as the faster-recovery option. The value matters here because a
// simultaneous MAX_CONNECTIONS burst can lose one connection's acks, and the
// whole cost of that is one initial RTO (UNCONFIRMED: no committed run record).
// This is a configuration an application owns (AR-10)
// rather than a constraint this library imposes -- a deployment across a WAN
// or a marginal link may want lwIP's more conservative default, and pico-http
// cannot tell which it is in.
#define LWIP_TCP_RTO_TIME 1000
#define TCP_MSS 1460               // 1500 MTU minus IP and TCP headers
#define TCP_SND_BUF (4 * TCP_MSS)  // Send buffer size per connection
// Receive window in bytes, bound to ADVERTISED_RX_WINDOW by the transport
// (AR-10, PR-5, AR-6).
#ifdef PICO_HTTP_PROFILE_P8_RP2350
#define TCP_WND (4 * TCP_MSS)  // P8-RP2350: 4*MSS receive window
#else
#define TCP_WND (2 * TCP_MSS)  // default: 2*MSS receive window
#endif
#define TCP_SND_QUEUELEN \
    ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))  // Allow 4x send buffer
#define TCP_SEGMENTS_PER_CONN \
    (((TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))  // Round up per-conn budget
#define MEMP_NUM_TCP_SEG \
    (MEMP_NUM_TCP_PCB *  \
     (TCP_SEGMENTS_PER_CONN + 8))  // Leave retransmit headroom

#define NO_SYS 1       // Use no operating system (NO_SYS=1)
#define LWIP_SOCKET 0  // Disable socket API

// Bound the out-of-order reassembly queue so a hostile peer cannot pin RAM.
#define TCP_QUEUE_OOSEQ 1
#define TCP_OOSEQ_MAX_PBUFS 4

#define MEM_LIBC_MALLOC 0  // Use lwIP's own memory management
#define MEM_ALIGNMENT 4    // Memory alignment (4-byte alignment)
// A zero-copy segment queues a heap header pbuf and a PBUF_ROM data pbuf
// against TCP_SND_QUEUELEN, so at most TCP_SND_QUEUELEN / 2 headers per PCB.
// The heap budget covers these headers and one persist-timer split of up to
// an MSS plus its header per PCB, and 2 KiB for DHCP, DNS and ICMP (AR-10).
// The split in tcp_out.c, tcp_split_unsent_seg, is UNCONFIRMED by lwIP
// documentation.
// /lwip_stats reports mem.max and err, so a run checks the bound against load.
// A header pbuf is struct pbuf (16 B) and 54 B of header room, aligned to 72 B,
// plus struct mem's 8 B.
#define PICO_HTTP_REF_TCP_HEADER_BYTES 80
#define MEM_SIZE                                                              \
    (MEMP_NUM_TCP_PCB *                                                       \
         ((TCP_SND_QUEUELEN / 2) * PICO_HTTP_REF_TCP_HEADER_BYTES + TCP_MSS + \
          PICO_HTTP_REF_TCP_HEADER_BYTES) +                                   \
     2048)
#define MEMP_NUM_ARP_QUEUE 10  // Number of ARP queued packets
// PBUF_ROM headers for every pbuf a connection may queue (AR-10).
#define MEMP_NUM_PBUF (MEMP_NUM_TCP_PCB * TCP_SND_QUEUELEN)

// The pool must cover eight receive windows plus packets in flight through
// the driver (AR-10, PR-5).
#ifdef PICO_HTTP_PROFILE_P8_RP2350
#define PBUF_POOL_SIZE 40  // P8-RP2350: backs 8 × 4*MSS window
#else
#define PBUF_POOL_SIZE 24  // default: backs 8 × 2*MSS window
#endif

#define LWIP_ARP 1       // Enable ARP support
#define LWIP_ETHERNET 1  // Enable Ethernet support
#define LWIP_ICMP 1      // Enable ICMP support
#define LWIP_RAW 1       // Enable raw API support
#define LWIP_NETIF_STATUS_CALLBACK \
    1                               // Enable network interface status callbacks
#define LWIP_NETIF_LINK_CALLBACK 1  // Enable network interface link callbacks
#define LWIP_NETIF_HOSTNAME 1       // Enable network interface hostname support
#define LWIP_NETCONN 0              // Disable netconn API

// Heap and pool high-water and error counts for /lwip_stats. Collected
// whenever LWIP_STATS is enabled.
#define MEM_STATS 1              // Heap high-water tracking
#define SYS_STATS 0              // Disable system statistics
#define MEMP_STATS 1             // Pool high-water tracking
#define LINK_STATS 0             // Disable link layer statistics
#define LWIP_CHKSUM_ALGORITHM 3  // Use Fletcher-16 checksum algorithm
#define LWIP_DHCP 1              // Enable DHCP support
#define LWIP_IPV4 1              // Enable IPv4 support
#define LWIP_TCP 1               // Enable TCP support
#define LWIP_UDP 1               // Enable UDP support
#define LWIP_DNS 1               // Enable DNS support
#define LWIP_TCP_KEEPALIVE 0     // Disable TCP keepalive support
// Zero keeps tcp_write from forcing TCP_WRITE_FLAG_COPY (AR-2). lwIP's opt.h
// says a driver must not rely on single pbufs. That the CYW43 driver does not
// need them is UNCONFIRMED by its documentation.
#define LWIP_NETIF_TX_SINGLE_PBUF 0
#define DHCP_DOES_ARP_CHECK 0  // Disable ARP check for DHCP
#define LWIP_DHCP_DOES_ACD_CHECK \
    0  // Disable Address Conflict Detection for DHCP

#ifndef NDEBUG
#define LWIP_DEBUG 1          // Enable debug messages
#define LWIP_STATS 1          // Enable statistics collection
#define LWIP_STATS_DISPLAY 1  // Enable statistics display
#endif

// Debugging options
#define ETHARP_DEBUG LWIP_DBG_OFF
#define NETIF_DEBUG LWIP_DBG_OFF
#define PBUF_DEBUG LWIP_DBG_OFF
#define API_LIB_DEBUG LWIP_DBG_OFF
#define API_MSG_DEBUG LWIP_DBG_OFF
#define SOCKETS_DEBUG LWIP_DBG_OFF
#define ICMP_DEBUG LWIP_DBG_OFF
#define INET_DEBUG LWIP_DBG_OFF
#define IP_DEBUG LWIP_DBG_OFF
#define IP_REASS_DEBUG LWIP_DBG_OFF
#define RAW_DEBUG LWIP_DBG_OFF
#define MEM_DEBUG LWIP_DBG_OFF
#define MEMP_DEBUG LWIP_DBG_OFF
#define SYS_DEBUG LWIP_DBG_OFF
#define TCP_DEBUG LWIP_DBG_OFF
#define TCP_INPUT_DEBUG LWIP_DBG_OFF
#define TCP_OUTPUT_DEBUG LWIP_DBG_OFF
#define TCP_RTO_DEBUG LWIP_DBG_OFF
#define TCP_CWND_DEBUG LWIP_DBG_OFF
#define TCP_WND_DEBUG LWIP_DBG_OFF
#define TCP_FR_DEBUG LWIP_DBG_OFF
#define TCP_QLEN_DEBUG LWIP_DBG_OFF
#define TCP_RST_DEBUG LWIP_DBG_OFF
#define UDP_DEBUG LWIP_DBG_OFF
#define TCPIP_DEBUG LWIP_DBG_OFF
#define PPP_DEBUG LWIP_DBG_OFF
#define SLIP_DEBUG LWIP_DBG_OFF
#define DHCP_DEBUG LWIP_DBG_OFF

// The following is needed to test mDns
#define LWIP_MDNS_RESPONDER 1
#define LWIP_IGMP 1
#define LWIP_NUM_NETIF_CLIENT_DATA 1
#define MDNS_RESP_USENETIF_EXTCALLBACK 1
#define MEMP_NUM_SYS_TIMEOUT (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 3)

// cppcoreguidelines-macro-usage: scope ends here.
// NOLINTEND(cppcoreguidelines-macro-usage)
