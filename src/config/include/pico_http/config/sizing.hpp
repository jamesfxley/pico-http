// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

/// \file sizing.hpp
/// Build-wide capacities. Every translation unit must see identical values.

namespace pico_http {

/// Connection slots shared by the transport and HTTP service (AR-4).
inline constexpr std::size_t MAX_CONNECTIONS = 8;

/// Host-safe TCP maximum segment size in bytes, checked against TCP_MSS.
inline constexpr std::size_t TCP_MSS_BYTES = 1460;

/// Advertised receive window in multiples of MSS.
#ifndef PICO_HTTP_RX_WINDOW_MSS_FACTOR
// cppcoreguidelines-macro-usage: the build can override this macro.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PICO_HTTP_RX_WINDOW_MSS_FACTOR 2
#endif

/// Transmit-ring capacity in multiples of MSS (PR-5).
#ifndef PICO_HTTP_TX_RING_MSS_FACTOR
// cppcoreguidelines-macro-usage: the build can override this macro.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PICO_HTTP_TX_RING_MSS_FACTOR 4
#endif

/// Maximum stored header-name length in bytes.
#ifndef PICO_HTTP_MAX_HEADER_NAME
// cppcoreguidelines-macro-usage: the build can override this macro.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PICO_HTTP_MAX_HEADER_NAME 64
#endif
/// Maximum stored header-value length in bytes. Exceeding it returns 431
/// (BR-2).
#ifndef PICO_HTTP_MAX_HEADER_VALUE
// cppcoreguidelines-macro-usage: the build can override this macro.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PICO_HTTP_MAX_HEADER_VALUE 256
#endif

/// Advertised receive window in bytes. The RX buffer must be at least this
/// large (AR-6).
inline constexpr std::size_t ADVERTISED_RX_WINDOW =
    static_cast<std::size_t>(PICO_HTTP_RX_WINDOW_MSS_FACTOR) * TCP_MSS_BYTES;

/// Per-slot transmit ring capacity.
inline constexpr std::size_t TX_RING_CAPACITY =
    static_cast<std::size_t>(PICO_HTTP_TX_RING_MSS_FACTOR) * TCP_MSS_BYTES;

/// Concurrent deferred responses. Exhaustion returns 503 (BR-12).
inline constexpr std::size_t MAX_PENDING_RESPONSES = 4;

/// Drain-close wait for acknowledgement, in milliseconds (TR-5).
inline constexpr std::uint32_t DRAIN_LINGER_MS = 2000;

}  // namespace pico_http
