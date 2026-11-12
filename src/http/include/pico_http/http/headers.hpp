// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "pico_http/config/sizing.hpp"
#include "pico_http/http/detail/parsing.hpp"
#include "pico_util/containers/fixed_string.hpp"

namespace pico_http {

/// Storage for HTTP headers with small-vector characteristics. The name/value
/// caps are profile knobs carried by the config module.
class HTTPHeaders {
   private:
    static constexpr size_t INLINE_CAPACITY = 16;

   public:
    /// Usable header-name capacity in characters. A consumer must be able to
    /// check its compiled value with ProfileBinding.
    static constexpr size_t MAX_HEADER_NAME = PICO_HTTP_MAX_HEADER_NAME;

    /// Usable header-value capacity in characters, including values exactly
    /// at the cap.
    static constexpr size_t MAX_HEADER_VALUE = PICO_HTTP_MAX_HEADER_VALUE;

    /// Header slots available to the parser.
    static constexpr size_t MAX_HEADERS = INLINE_CAPACITY;

    // One byte over the advertised cap for pico_util::FixedString's terminator,
    // so a name or value of exactly MAX_HEADER_* characters is stored whole.
    /// Storage for a header name, one byte over MAX_HEADER_NAME.
    using NameStorage = pico_util::FixedString<MAX_HEADER_NAME + 1>;
    /// Storage for a header value, one byte over MAX_HEADER_VALUE.
    using ValueStorage = pico_util::FixedString<MAX_HEADER_VALUE + 1>;

    /// One stored header. Both fields are fixed-capacity and inline, so a
    /// Header costs the same whatever it holds and nothing is allocated.
    struct Header {
        /// Field name as received; comparison against it is case-insensitive.
        NameStorage name;
        /// Field value, with any leading and trailing whitespace already
        /// stripped by the parser.
        ValueStorage value;
    };

    /// Drop every header. Storage is fixed, so this only resets the count.
    void clear() noexcept { size_ = 0; }

    /// Replace a header. Failure changes nothing when either length or the
    /// count exceeds capacity (BR-7).
    bool set(std::string_view name, std::string_view value) {
        if (value.size() > MAX_HEADER_VALUE) {
            return false;
        }
        Header* existing = find(name);
        if (existing != nullptr) {
            return existing->value.assign(value);
        }
        return emplace(name, value);
    }

    /// Append a comma-separated value for a repeated name (NR-10; RFC 9110
    /// §5.2).
    /// Failure leaves storage unchanged when a name, value or count exceeds
    /// capacity (BR-7).
    bool add(std::string_view name, std::string_view value) {
        Header* existing = find(name);
        if (existing == nullptr) {
            return emplace(name, value);
        }
        if (existing->value.empty()) {
            // assign() truncates rather than returning failure, so the cap is
            // checked here: an over-long value must leave the stored one
            // untouched.
            if (value.size() > MAX_HEADER_VALUE) {
                return false;
            }
            return existing->value.assign(value);
        }
        ValueStorage combined = existing->value;
        if (!combined.append(", ") || !combined.append(value)) {
            return false;
        }
        existing->value = combined;
        return true;
    }

    /// Returns a case-insensitive value match. An empty view can mean absent
    /// or empty. The view is invalidated by mutation; has() distinguishes
    /// absence.
    [[nodiscard]] std::string_view get(std::string_view name) const {
        const Header* header = find(name);
        return header != nullptr ? header->value.view() : std::string_view{};
    }

    /// True when a header with this name is stored, even with an empty value.
    [[nodiscard]] bool has(std::string_view name) const {
        return find(name) != nullptr;
    }

    /// Headers stored, at most MAX_HEADERS.
    [[nodiscard]] size_t size() const noexcept { return size_; }

    /// The stored headers in insertion order. Invalidated by any mutating call.
    [[nodiscard]] std::span<const Header> entries() const noexcept {
        return {inline_headers_.data(), size_};
    }

    /// Begin iterator over entries(), so the collection works in a range-for.
    [[nodiscard]] auto begin() const noexcept { return entries().begin(); }

    /// End iterator over entries().
    [[nodiscard]] auto end() const noexcept { return entries().end(); }

   private:
    std::array<Header, INLINE_CAPACITY> inline_headers_{};
    size_t size_ = 0;

    Header* find(std::string_view name) {
        for (size_t i = 0; i < size_; ++i) {
            if (detail::iequals(inline_headers_.at(i).name.view(), name)) {
                return &inline_headers_.at(i);
            }
        }
        return nullptr;
    }

    [[nodiscard]] const Header* find(std::string_view name) const {
        for (size_t i = 0; i < size_; ++i) {
            if (detail::iequals(inline_headers_.at(i).name.view(), name)) {
                return &inline_headers_.at(i);
            }
        }
        return nullptr;
    }

    /// Store a new header, or return false without changes when any cap is
    /// exceeded (BR-7).
    bool emplace(std::string_view name, std::string_view value) {
        if (size_ >= INLINE_CAPACITY || name.size() > MAX_HEADER_NAME ||
            value.size() > MAX_HEADER_VALUE) {
            return false;
        }
        // Both lengths were checked against usable FixedString capacity
        // before assignment.
        static_cast<void>(inline_headers_.at(size_).name.assign(name));
        static_cast<void>(inline_headers_.at(size_).value.assign(value));
        ++size_;
        return true;
    }
};

}  // namespace pico_http
