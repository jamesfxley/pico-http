// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>

#include "pico_http/config/sizing.hpp"

/// \file limits.hpp
/// Compile-time boundary limits for the HTTP request parser (BR-1..5). Bundled
/// as a policy struct so a downstream profile can supply its own caps;
/// DefaultHttpLimits matches the default profile. Tests instantiate the
/// parser with tiny limits to exercise boundary rejection deterministically.
/// The HttpLimits concept in profile.hpp states the contract a replacement
/// must satisfy.

namespace pico_http {

/// Bytes a header line spends on syntax between the two stored fields: the
/// ':' and the single OWS that conventionally follows it. The line length the
/// parser bounds excludes the CRLF, so nothing else is counted here.
inline constexpr std::size_t HEADER_LINE_SYNTAX = 2;

/// The default parser capacity set. Each member is a hard ceiling whose breach
/// produces a status code rather than a truncated field (AR-1): the parser has
/// no heap to grow into, so "too long" is an answer, not a resize.
///
/// A consumer supplies its own struct with the same members to change the
/// sizing; see P8Rp2350HttpLimits in profile.hpp for one the library ships.
struct DefaultHttpLimits {
    /// BR-1: request line (method + SP + URI + SP + version). Over → 414.
    static constexpr std::size_t MAX_REQUEST_LINE = 576;
    /// Longest method token stored. Over → 400.
    static constexpr std::size_t MAX_METHOD = 16;
    /// BR-1: URI within the request line. Over → 414.
    static constexpr std::size_t MAX_URI = 512;
    /// Longest HTTP-version token stored.
    static constexpr std::size_t MAX_VERSION = 16;
    /// BR-2: single header line (name + ':' + value + OWS). Over → 431.
    ///
    /// Derived from the storage caps rather than chosen: a line cap below
    /// `name + ':' + value` rejects at the line before the value cap can ever
    /// be reached, so the advertised cap would not be the enforced one and the
    /// difference would be per-slot storage no request could fill. Raising
    /// PICO_HTTP_MAX_HEADER_VALUE now raises what the parser accepts, which is
    /// what a deployment serving large cookies is asking for.
    static constexpr std::size_t MAX_HEADER_LINE = PICO_HTTP_MAX_HEADER_NAME +
                                                   PICO_HTTP_MAX_HEADER_VALUE +
                                                   HEADER_LINE_SYNTAX;
    /// BR-3: number of headers. Over → 431 (enforced via header storage).
    static constexpr std::size_t MAX_HEADERS = 16;
    /// BR-4: total request head (request line + headers + terminator). Over →
    /// 431.
    static constexpr std::size_t MAX_REQUEST_HEAD = 2048;
    /// BR-5: buffered request body. Declared length over this → 413.
    static constexpr std::size_t MAX_BUFFERED_BODY = 2048;
};

}  // namespace pico_http
