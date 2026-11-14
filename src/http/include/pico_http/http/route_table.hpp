// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "pico_http/http/detail/parsing.hpp"
#include "pico_http/http/message.hpp"
#include "pico_util/containers/fixed_string.hpp"

namespace pico_http {

/// Exact routes take precedence over trailing-* prefix routes. Method
/// matching ignores case; * matches any method.
/// Registration allocates and rejects excess method or pattern length. Lookup
/// allocates nothing. Re-registration replaces the handler (AR-1, PR-2).
class RouteTable {
   public:
    /// Usable method-token capacity in characters, also bounding Allow
    /// (BR-2).
    static constexpr size_t METHOD_CAPACITY = 16U;

   private:
    static constexpr size_t URI_CAPACITY = 128U;

    struct Route {
        pico_util::FixedString<METHOD_CAPACITY + 1> method;
        pico_util::FixedString<URI_CAPACITY> uri_pattern;
        RequestHandler handler;
    };

    std::vector<Route> exact_routes_;
    std::vector<Route> wildcard_routes_;

   public:
    /// Register a handler. Returns false, registering nothing, if the method or
    /// pattern exceeds its fixed capacity.
    [[nodiscard]] bool add_route(std::string_view method,
                                 std::string_view uri_pattern,
                                 RequestHandler handler) {
        Route route;
        if (!route.method.assign(method) ||
            !route.uri_pattern.assign(uri_pattern)) {
            return false;
        }
        route.handler = std::move(handler);

        std::vector<Route>& table =
            uri_pattern.ends_with("*") ? wildcard_routes_ : exact_routes_;
        for (Route& existing : table) {
            if (detail::iequals(existing.method.view(), route.method.view()) &&
                existing.uri_pattern.view() == route.uri_pattern.view()) {
                existing = std::move(route);  // re-registration replaces
                return true;
            }
        }
        table.push_back(std::move(route));
        return true;
    }

    /// Find the handler for method + uri. Exact matches win over wildcard
    /// prefix matches; among wildcards, registration order decides.
    [[nodiscard]] const Route* find_route(std::string_view method,
                                          std::string_view uri) const {
        for (const Route& route : exact_routes_) {
            if (matches_method(route.method.view(), method) &&
                route.uri_pattern.view() == uri) {
                return &route;
            }
        }
        for (const Route& route : wildcard_routes_) {
            if (matches_method(route.method.view(), method) &&
                matches_wildcard_uri(route.uri_pattern.view(), uri)) {
                return &route;
            }
        }
        return nullptr;
    }

    /// Collect distinct methods for matching URI patterns, bounded by
    /// out.size(). Wildcard methods are skipped (RFC 9110 §10.2.1).
    /// The views remain valid while the table lives and no route is added.
    /// The caller owns out.
    [[nodiscard]] std::size_t allowed_methods(
        std::string_view uri, std::span<std::string_view> out) const {
        std::size_t count = 0;
        auto already_listed = [&](std::string_view method) {
            return std::ranges::any_of(out.first(count),
                                       [&](std::string_view seen) {
                                           return detail::iequals(seen, method);
                                       });
        };
        auto collect = [&](const Route& route) {
            const std::string_view method = route.method.view();
            if (method == "*" ||
                !matches_wildcard_uri(route.uri_pattern.view(), uri)) {
                return;
            }
            if (count < out.size() && !already_listed(method)) {
                out[count++] = method;
            }
        };
        for (const Route& route : exact_routes_) {
            collect(route);
        }
        for (const Route& route : wildcard_routes_) {
            collect(route);
        }
        return count;
    }

   private:
    static bool matches_method(std::string_view pattern_method,
                               std::string_view request_method) {
        return pattern_method == "*" ||
               detail::iequals(pattern_method, request_method);
    }

    static bool matches_wildcard_uri(std::string_view pattern,
                                     std::string_view uri) {
        if (!pattern.ends_with("*")) {
            return pattern == uri;
        }

        const std::string_view prefix = pattern.substr(0, pattern.size() - 1);
        return uri.starts_with(prefix);
    }
};

}  // namespace pico_http
