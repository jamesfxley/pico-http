// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string_view>

#include "pico_http/http/detail/parsing.hpp"

/// \file keep_alive.hpp
/// HTTP/1.0-vs-1.1 keep-alive negotiation (spec NR-2) used by HttpService, kept
/// in one place so the policy lives in exactly one spot. Not part of the stable
/// public API.

namespace pico_http::detail {

inline constexpr std::string_view HTTP_VERSION_1_1 = "HTTP/1.1";
inline constexpr std::string_view CONNECTION_CLOSE = "close";
inline constexpr std::string_view CONNECTION_KEEP_ALIVE = "keep-alive";

/// Decide whether a connection stays open after this request/response.
///   - keep-alive disabled          -> always close
///   - HTTP/1.1 (or unversioned)     -> keep-alive unless the request said
///   close
///   - HTTP/1.0                      -> close unless the request asked to keep
///     alive (and did not also say close)
///   - an explicit response Connection header overrides the version default
///   - a per-connection request cap  -> close once it is reached, and it is
///     applied last so a handler cannot override a resource bound
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

    // The per-connection request cap is applied last, after the response
    // header has had its say. It is a resource bound rather than a preference —
    // it limits how long one peer can hold a slot (CXX-12.7) — so a handler
    // that sets Connection: keep-alive outranks the version default but must
    // not be able to switch off a limit it may not know exists.
    if (max_requests > 0U && requests_served >= max_requests) {
        allowed = false;
    }
    return allowed;
}

}  // namespace pico_http::detail
