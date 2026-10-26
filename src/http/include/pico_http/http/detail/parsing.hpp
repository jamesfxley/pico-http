// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string_view>

/// \file parsing.hpp
/// Low-level byte-scanning helpers shared by the HTTP request parser and its
/// tests. Not part of the stable public API.

namespace pico_http::detail {

/// ASCII-only lower-case (HTTP field names/tokens are ASCII; std::tolower is
/// locale-dependent and UB on negative char values).
constexpr char ascii_to_lower(char ch) noexcept {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

/// Case-insensitive ASCII string compare.
inline bool iequals(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (ascii_to_lower(lhs[i]) != ascii_to_lower(rhs[i])) {
            return false;
        }
    }
    return true;
}

/// Strip leading/trailing optional whitespace (SP / HT), RFC 9110 OWS.
inline std::string_view trim_ows(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/// True when a comma-separated header list (e.g. Connection) contains `token`
/// as a whole element, case-insensitively, ignoring OWS around each element.
inline bool header_contains_token(std::string_view header_value,
                                  std::string_view token) noexcept {
    std::size_t start = 0;
    while (start < header_value.size()) {
        while (start < header_value.size() &&
               (header_value[start] == ' ' || header_value[start] == '\t' ||
                header_value[start] == ',')) {
            ++start;
        }
        if (start >= header_value.size()) {
            break;
        }
        std::size_t end = start;
        while (end < header_value.size() && header_value[end] != ',') {
            ++end;
        }
        const std::string_view candidate =
            trim_ows(header_value.substr(start, end - start));
        if (!candidate.empty() && iequals(candidate, token)) {
            return true;
        }
        start = (end < header_value.size()) ? end + 1 : header_value.size();
    }
    return false;
}

}  // namespace pico_http::detail
