// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string_view>

#include "pico_http/http/detail/parsing.hpp"

/// \file keep_alive.hpp
/// Internal HTTP persistence negotiation (NR-2).

namespace pico_http::detail {

inline constexpr std::string_view HTTP_VERSION_1_1 = "HTTP/1.1";
inline constexpr std::string_view CONNECTION_CLOSE = "close";
inline constexpr std::string_view CONNECTION_KEEP_ALIVE = "keep-alive";

/// Decide persistence from configuration, version and Connection headers. The
/// request cap applies last.
inline bool keep_alive_allowed(bool enabled,
                               std::string_view request_connection,
                               std::string_view request_version,
                               std::string_view response_connection,
                               std::uint32_t requests_served,
                               std::uint32_t max_requests) noexcept {
    if (!enabled) {
        return false;
    }
    const bool request_has_close =
        header_contains_token(request_connection, CONNECTION_CLOSE);
    const bool request_has_keep_alive =
        header_contains_token(request_connection, CONNECTION_KEEP_ALIVE);
    const bool is_http11 =
        request_version.empty() || iequals(request_version, HTTP_VERSION_1_1);
    bool allowed = is_http11 ? !request_has_close
                             : (request_has_keep_alive && !request_has_close);

    if (!response_connection.empty()) {
        if (header_contains_token(response_connection, CONNECTION_CLOSE)) {
            allowed = false;
        } else if (header_contains_token(response_connection,
                                         CONNECTION_KEEP_ALIVE)) {
            allowed = true;
        }
    }

    // A handler must not override the request cap, even if it might not know
    // the cap exists.
    if (max_requests > 0U && requests_served >= max_requests) {
        allowed = false;
    }
    return allowed;
}

}  // namespace pico_http::detail
