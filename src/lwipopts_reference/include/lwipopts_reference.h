// SPDX-License-Identifier: MIT
#pragma once

// lwIP reads its whole configuration through #if and #ifdef at preprocessing
// time, so every option here has to be a macro; a constant of any kind is
// invisible to it. The names and their meanings are lwIP's, not ours.
// NOLINTBEGIN(cppcoreguidelines-macro-usage)

// 8 concurrent application connections (P8 profile) plus headroom for PCBs
// lingering in TIME_WAIT / used by mDNS, so churn never starves accept().
#define MEMP_NUM_TCP_PCB 12        // Number of simultaneously active TCP conns
#define MEMP_NUM_TCP_PCB_LISTEN 2  // Listener + spare
#define TCP_MSL 3000               // Shorten TIME_WAIT (ms) to free PCBs faster
// Initial retransmission timeout, used until a connection has an RTT sample.
// lwIP defaults to 3000 ms because it is a portable stack that must also suit
// links this device will never see; RFC 6298 s2.1 prescribes 1 second for the
// case before any measurement exists. It matters here because a simultaneous
// MAX_CONNECTIONS burst loses one connection's acks in the contention it
// creates, and the whole cost of that is one initial RTO. This is a
// configuration an application owns (AR-10) rather than a constraint this
// library imposes -- a deployment across a WAN or a marginal link may want
// lwIP's more conservative default, and pico-http cannot tell which it is in.
#define LWIP_TCP_RTO_TIME 1000
#define TCP_MSS 1460               // 1500 MTU minus IP and TCP headers
#define TCP_SND_BUF (4 * TCP_MSS)  // Send buffer size per connection
// Receive window per connection (AR-10; the per-connection sizing in PR-5). The
// advertised window is the RX flow-control lever (AR-6): a smaller window is
// backed by less RAM and makes backpressure the norm under paced uploads.
// ADVERTISED_RX_WINDOW in tcp_transport.hpp mirrors this and is static_asserted
// equal via the lwIP transport, so the HTTP RX buffer and the window can never
// diverge — the P8-RP2350 profile raises both together
// (PICO_HTTP_RX_WINDOW_MSS_FACTOR=4).
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
// lwIP heap, both profiles (AR-10), sized by arithmetic rather than from a
// measured peak. A zero-copy segment queues two pbufs against
// TCP_SND_QUEUELEN -- its header, from this heap, and its PBUF_ROM data -- so
// a PCB holds at most TCP_SND_QUEUELEN / 2 headers of
// PICO_HTTP_REF_TCP_HEADER_BYTES. lwIP's persist timer can also copy up to an
// MSS of a segment into a new header pbuf from this heap
// (tcp_split_unsent_seg in tcp_out.c; lwIP's documentation is silent), one
// per PCB. 2 KiB covers DHCP, DNS and ICMP. /lwip_stats reports mem.max and
// err, and the baselines record lwip_stats_end, so the bound is checked
// against load.
//
// A heap TCP header pbuf: struct pbuf (16 B) and 54 B of link, IP and TCP
// header room, aligned to 72, plus struct mem's 8.
#define PICO_HTTP_REF_TCP_HEADER_BYTES 80
#define MEM_SIZE                                                              \
    (MEMP_NUM_TCP_PCB *                                                       \
         ((TCP_SND_QUEUELEN / 2) * PICO_HTTP_REF_TCP_HEADER_BYTES + TCP_MSS + \
          PICO_HTTP_REF_TCP_HEADER_BYTES) +                                   \
     2048)
#define MEMP_NUM_ARP_QUEUE 10  // Number of ARP queued packets
// PBUF_ROM headers (AR-10). Zero-copy tcp_write takes one for every piece of
// ring it queues, and TCP_SND_QUEUELEN caps the pbufs a connection may queue,
// so TCP's own use can never exhaust them.
#define MEMP_NUM_PBUF (MEMP_NUM_TCP_PCB * TCP_SND_QUEUELEN)

// pbuf pool (AR-10; the per-connection sizing in PR-5). Must cover 8 × TCP_WND
// of advertised window plus driver in-flight. 24 @ ~1,536 B/pbuf ≈ 36.9 KB
// covers 8 × 2×MSS (default); the P8-RP2350 profile raises the window to
// 4×MSS and the pool to 40 to match.
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

// MEM_STATS / MEMP_STATS drive the /lwip_stats endpoint's heap/pool
// high-water marks (mem.max, pool max/err), which check the bounds MEM_SIZE
// and the pools are sized by against measured load. Collected whenever
// LWIP_STATS is on, which is lwIP's default in every build type.
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
// Off, so tcp_write keeps the zero-copy TX path AR-2 relies on: at 1 lwIP
// forces TCP_WRITE_FLAG_COPY and copies every sent byte into its heap. lwIP's
// opt.h documents the option as a compatibility aid for DMA MACs without
// scatter-gather and says a driver must not rely on single pbufs; that the
// CYW43 driver needs none is UNCONFIRMED by its documentation.
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

// Closes the region opened at the top of the file; the reason is there.
// NOLINTEND(cppcoreguidelines-macro-usage)
