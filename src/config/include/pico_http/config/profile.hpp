// SPDX-License-Identifier: MIT
#pragma once

#include <concepts>
#include <cstddef>

#include "pico_http/config/limits.hpp"
#include "pico_http/config/sizing.hpp"

/// \file profile.hpp
/// Named compile-time sizing policies for HttpService. A profile bundles the
/// parser's boundary limits with the per-slot buffer capacities and the slot
/// count, so a downstream target retunes every static allocation from a single
/// struct. Select one by defining PICO_HTTP_ACTIVE_PROFILE (the top-level
/// CMake PICO_HTTP_PROFILE option does this for the shipped profiles, carrying
/// the definition on the pico_http_config target so consumers inherit it).

namespace pico_http {

/// The compile-time interface a limits policy must supply (BR-1..5).
template <typename L>
concept HttpLimits = requires {
    { L::MAX_REQUEST_LINE } -> std::convertible_to<std::size_t>;
    { L::MAX_METHOD } -> std::convertible_to<std::size_t>;
    { L::MAX_URI } -> std::convertible_to<std::size_t>;
    { L::MAX_VERSION } -> std::convertible_to<std::size_t>;
    { L::MAX_HEADER_LINE } -> std::convertible_to<std::size_t>;
    { L::MAX_HEADERS } -> std::convertible_to<std::size_t>;
    { L::MAX_REQUEST_HEAD } -> std::convertible_to<std::size_t>;
    { L::MAX_BUFFERED_BODY } -> std::convertible_to<std::size_t>;
};

/// The compile-time interface a sizing profile must supply. Stating it as a
/// concept turns a mis-specified downstream profile into one diagnostic at the
/// point of use rather than a wall of substitution failures inside the parser.
template <typename P>
concept HttpProfile = HttpLimits<typename P::Limits> && requires {
    { P::MAX_CONNECTIONS } -> std::convertible_to<std::size_t>;
    { P::MAX_PENDING_RESPONSES } -> std::convertible_to<std::size_t>;
    { P::HEAD_CAPACITY } -> std::convertible_to<std::size_t>;
    { P::GENERATOR_CHUNK } -> std::convertible_to<std::size_t>;
    { P::RX_CAPACITY } -> std::convertible_to<std::size_t>;
};

/// The default profile. Its invariants (`RX_CAPACITY >=
/// ADVERTISED_RX_WINDOW`; `MAX_CONNECTIONS` == the transport's slot count) are
/// static_asserted inside HttpService.
struct DefaultHttpProfile {
    /// The parser capacity set this profile bundles.
    using Limits = DefaultHttpLimits;
    /// Connection slots — shares the transport's owner (config/sizing.hpp) so
    /// the two layers' fixed arrays cannot drift apart.
    static constexpr std::size_t MAX_CONNECTIONS = pico_http::MAX_CONNECTIONS;
    /// BR-12: concurrent deferred responses (AR-9). Service-level pool, not
    /// per-slot — see sizing.hpp for why it is sized independently of the slot
    /// count. HttpService static_asserts it against MAX_CONNECTIONS.
    static constexpr std::size_t MAX_PENDING_RESPONSES =
        pico_http::MAX_PENDING_RESPONSES;
    /// Per-slot serialized response head (status line + headers); heap-free.
    static constexpr std::size_t HEAD_CAPACITY = 1024;
    /// Scratch pulled from a generator body one chunk at a time.
    static constexpr std::size_t GENERATOR_CHUNK = 512;
    /// RX assembly buffer; must be >= the advertised receive window (AR-6).
    static constexpr std::size_t RX_CAPACITY = ADVERTISED_RX_WINDOW;
};

/// BR-5 raised to 8 KiB for the P8-RP2350 profile.
struct P8Rp2350HttpLimits : DefaultHttpLimits {
    /// BR-5: largest body buffered before the handler runs. Over → 413.
    static constexpr std::size_t MAX_BUFFERED_BODY = 8192;
};

/// P8-RP2350 profile. Larger buffers exploiting the RP2350's 520 KB SRAM: an
/// 8 KiB buffered body, a 4×MSS TX ring and receive window (paired in lwipopts
/// via PICO_HTTP_PROFILE_P8_RP2350, and in the transport via the TX_RING and
/// RX_WINDOW MSS-factor macros). Header caps stay 64/256; slot count and head
/// capacity are unchanged from the default, which is why this inherits them
/// rather than restating them — a new knob added to the default profile reaches
/// this one instead of silently leaving it behind.
struct P8Rp2350HttpProfile : DefaultHttpProfile {
    /// The raised capacity set; every other member is inherited unchanged.
    using Limits = P8Rp2350HttpLimits;
};

static_assert(HttpProfile<DefaultHttpProfile>);
static_assert(HttpProfile<P8Rp2350HttpProfile>);

}  // namespace pico_http

/// The profile HttpService instantiates. Defined here rather than in
/// http_service.hpp so every layer that sizes storage from a profile — the
/// service, the transports, the drift tests — resolves the same selection.
/// Supply an alternative by defining it on the compile line.
#ifndef PICO_HTTP_ACTIVE_PROFILE
// A compile-line -D override seam.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PICO_HTTP_ACTIVE_PROFILE ::pico_http::DefaultHttpProfile
#endif
