// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

/// \file sizing.hpp
/// Build-wide sizing constants shared by the HTTP layer, the transports and
/// lwipopts. These determine static array extents and therefore class layout,
/// so every translation unit in a build must see identical values: the
/// pico_http_config target carries the override macros as INTERFACE compile
/// definitions, and HttpService checks the resulting binding at construction.

namespace pico_http {

/// Single source of truth for the connection-slot count, shared by the
/// transports (their SlotTable/TimeoutScheduler) and the HTTP service (its
/// per-connection Conn array). Sizing everything from one constant makes it
/// impossible for the two layers' fixed arrays to drift apart, so an id minted
/// by the transport always indexes a valid service slot (AR-4).
inline constexpr std::size_t MAX_CONNECTIONS = 8;

/// TCP maximum segment size assumed by the host-safe layer. The firmware's real
/// value comes from lwipopts (TCP_MSS); the lwIP transport static_asserts the
/// binding so a divergence is caught.
inline constexpr std::size_t TCP_MSS_BYTES = 1460;

/// Advertised TCP receive window as a multiple of MSS. Default 2×MSS (lwipopts
/// TCP_WND); the P8-RP2350 profile raises it to 4×MSS, kept paired with
/// lwipopts TCP_WND.
#ifndef PICO_HTTP_RX_WINDOW_MSS_FACTOR
// A compile-line -D override seam.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PICO_HTTP_RX_WINDOW_MSS_FACTOR 2
#endif

/// Transmit ring capacity as a multiple of MSS. 4×MSS in both profiles: the
/// reference TCP_SND_BUF, so the ring keeps as much in flight as lwIP will, and
/// PR-5's throughput depends on it. The lwIP transport sizes its per-slot ring
/// from it.
#ifndef PICO_HTTP_TX_RING_MSS_FACTOR
// A compile-line -D override seam.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PICO_HTTP_TX_RING_MSS_FACTOR 4
#endif

/// Header-storage caps. A deployment serving browsers with large cookies can
/// raise the value cap with the CMake cache variable of the same name, which
/// puts it on pico_http_config as an INTERFACE definition so the library and
/// every consumer compile against one layout. A bare -D on a consumer's own
/// target reaches only that consumer: the archive it links keeps the default,
/// and ProfileBinding aborts in the HttpService constructor rather than let the
/// two disagree about the size of every HTTPHeaders that crosses between them.
#ifndef PICO_HTTP_MAX_HEADER_NAME
// A compile-line -D override seam.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PICO_HTTP_MAX_HEADER_NAME 64
#endif
/// Longest header value stored, in bytes. A longer value is a capacity failure
/// reported as 431, never a truncation (BR-2). DefaultHttpLimits derives
/// MAX_HEADER_LINE from this and the name cap, so raising it raises what the
/// parser will accept rather than leaving storage no request can reach.
#ifndef PICO_HTTP_MAX_HEADER_VALUE
// A compile-line -D override seam.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PICO_HTTP_MAX_HEADER_VALUE 256
#endif

/// Advertised TCP receive window. The HTTP service's RX assembly buffer must be
/// at least this large so a full window always fits and no delivered byte is
/// dropped (AR-6); HttpService static_asserts it, and the lwIP transport
/// static_asserts this equals the real TCP_WND so the two cannot diverge.
inline constexpr std::size_t ADVERTISED_RX_WINDOW =
    static_cast<std::size_t>(PICO_HTTP_RX_WINDOW_MSS_FACTOR) * TCP_MSS_BYTES;

/// Per-slot transmit ring capacity.
inline constexpr std::size_t TX_RING_CAPACITY =
    static_cast<std::size_t>(PICO_HTTP_TX_RING_MSS_FACTOR) * TCP_MSS_BYTES;

/// Concurrent deferred responses the service can hold (BR-12). Sized by
/// pending-handler concurrency rather than by connection count: an HTTPResponse
/// carries a full HTTPHeaders, the dominant per-slot cost, so one per slot
/// would add several tens of KiB for a resource only a deferring handler holds.
/// Exhausting the pool is a 503, never a wait.
///
/// A plain constant with no -D seam, like MAX_CONNECTIONS above: a deployment
/// that wants a different pool supplies its own profile, which is how every
/// other service-enforced limit here is retuned.
inline constexpr std::size_t MAX_PENDING_RESPONSES = 4;

/// How long a drain-close lingers waiting for the peer to acknowledge the
/// queued response before the connection is torn down (TR-5).
inline constexpr std::uint32_t DRAIN_LINGER_MS = 2000;

}  // namespace pico_http
