// SPDX-License-Identifier: MIT
#pragma once

#include <concepts>
#include <cstddef>

#include "pico_http/config/limits.hpp"
#include "pico_http/config/sizing.hpp"

/// \file profile.hpp
/// Compile-time limits, buffer capacities and connection counts for
/// HttpService.

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

/// The members a sizing profile must supply.
template <typename P>
concept HttpProfile = HttpLimits<typename P::Limits> && requires {
    { P::MAX_CONNECTIONS } -> std::convertible_to<std::size_t>;
    { P::MAX_PENDING_RESPONSES } -> std::convertible_to<std::size_t>;
    { P::HEAD_CAPACITY } -> std::convertible_to<std::size_t>;
    { P::GENERATOR_CHUNK } -> std::convertible_to<std::size_t>;
    { P::RX_CAPACITY } -> std::convertible_to<std::size_t>;
};

/// Default sizes, checked against the transport by HttpService.
struct DefaultHttpProfile {
    /// The parser capacity set this profile bundles.
    using Limits = DefaultHttpLimits;
    /// Connection slots shared with the transport.
    static constexpr std::size_t MAX_CONNECTIONS = pico_http::MAX_CONNECTIONS;
    /// Concurrent deferred responses across the service (BR-12, AR-9).
    static constexpr std::size_t MAX_PENDING_RESPONSES =
        pico_http::MAX_PENDING_RESPONSES;
    /// Per-slot serialised response head (status line + headers); heap-free.
    static constexpr std::size_t HEAD_CAPACITY = 1024;
    /// Scratch pulled from a generator body one chunk at a time.
    static constexpr std::size_t GENERATOR_CHUNK = 512;
    /// RX assembly buffer; must be >= the advertised receive window (AR-6).
    static constexpr std::size_t RX_CAPACITY = ADVERTISED_RX_WINDOW;
};

/// An 8 KiB buffered-body limit for P8-RP2350 (BR-5).
struct P8Rp2350HttpLimits : DefaultHttpLimits {
    /// Body cap in bytes before dispatch. Exceeding it returns 413 (BR-5).
    static constexpr std::size_t MAX_BUFFERED_BODY = 8192;
};

/// An 8 KiB body profile with a 4×MSS receive window and TX ring.
struct P8Rp2350HttpProfile : DefaultHttpProfile {
    /// The raised capacity set; every other member is inherited unchanged.
    using Limits = P8Rp2350HttpLimits;
};

static_assert(HttpProfile<DefaultHttpProfile>);
static_assert(HttpProfile<P8Rp2350HttpProfile>);

}  // namespace pico_http

/// The build-wide profile selected for HttpService.
#ifndef PICO_HTTP_ACTIVE_PROFILE
// cppcoreguidelines-macro-usage: the build can override this macro.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PICO_HTTP_ACTIVE_PROFILE ::pico_http::DefaultHttpProfile
#endif
