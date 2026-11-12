// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>

#include "pico_http/config/sizing.hpp"

/// \file limits.hpp
/// Parser capacity policies. A replacement must satisfy HttpLimits (BR-1..5).

namespace pico_http {

/// Bytes reserved for ':' and one space, excluding CRLF.
inline constexpr std::size_t HEADER_LINE_SYNTAX = 2;

/// Default capacities. Exceeding a cap returns a status without truncation
/// (AR-1).
struct DefaultHttpLimits {
    /// Request-line cap in bytes. Exceeding it returns 414 (BR-1).
    static constexpr std::size_t MAX_REQUEST_LINE = 576;
    /// Method-token cap in bytes. Exceeding it returns 400.
    static constexpr std::size_t MAX_METHOD = 16;
    /// URI cap in bytes. Exceeding it returns 414 (BR-1).
    static constexpr std::size_t MAX_URI = 512;
    /// Longest HTTP-version token stored.
    static constexpr std::size_t MAX_VERSION = 16;
    /// Header-line cap in bytes. Exceeding it returns 431 (BR-2).
    static constexpr std::size_t MAX_HEADER_LINE = PICO_HTTP_MAX_HEADER_NAME +
                                                   PICO_HTTP_MAX_HEADER_VALUE +
                                                   HEADER_LINE_SYNTAX;
    /// Header-count cap. Exceeding it returns 431 (BR-3).
    static constexpr std::size_t MAX_HEADERS = 16;
    /// Request-head cap in bytes. Exceeding it returns 431 (BR-4).
    static constexpr std::size_t MAX_REQUEST_HEAD = 2048;
    /// Buffered-body cap in bytes. Excess declared length returns 413 (BR-5).
    static constexpr std::size_t MAX_BUFFERED_BODY = 2048;
};

}  // namespace pico_http
