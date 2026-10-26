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
    /// Max stored header name length, in characters. Public alongside the value
    /// cap because both are profile knobs that change this type's layout, so a
    /// consumer must be able to check the values it was compiled against
    /// (HttpService::ProfileBinding).
    ///
    /// This is the usable length, not the underlying array extent:
    /// pico_util::FixedString reserves one byte for its terminator, so the
    /// storage below is declared one larger. Stating the cap as the usable
    /// length is what lets a caller size a buffer against it (see
    /// MAX_HEADER_VALUE) and have the value fit.
    static constexpr size_t MAX_HEADER_NAME = PICO_HTTP_MAX_HEADER_NAME;

    /// Max stored header value length, in characters; exposed so a caller
    /// assembling a value (e.g. the 405 Allow list) can bound its buffer to
    /// what set() retains. A value of exactly this length is stored whole.
    static constexpr size_t MAX_HEADER_VALUE = PICO_HTTP_MAX_HEADER_VALUE;

    /// Number of header slots this storage holds — the true cap the parser
    /// enforces (a set() past it fails -> 431). Exposed so a limits policy can
    /// static_assert its advertised MAX_HEADERS matches (see http_parser.hpp),
    /// preventing /limits from reporting a count the storage does not honour.
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

    /// Set a header, replacing any existing value under the same name.
    /// Returns false — changing nothing — if the name or value exceeds its cap,
    /// or if the header count is already at capacity. A rejected set never
    /// leaves a partially written entry behind (BR-7; CXX-07.2).
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

    /// Add a header, combining with any existing value under the same name as
    /// a comma-separated list (NR-10; RFC 9110 §5.2 field-line combination).
    /// Returns false if the combined value (or a new name/value) does not fit —
    /// the caller turns that into a 431 (BR-7); the stored value is never
    /// partially updated or silently replaced.
    bool add(std::string_view name, std::string_view value) {
        Header* existing = find(name);
        if (existing == nullptr) {
            return emplace(name, value);
        }
        if (existing->value.empty()) {
            // assign() truncates rather than refusing, so the cap is checked
            // here: an over-long value must leave the stored one untouched.
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

    /// Value stored under `name`, matched case-insensitively, or an empty view
    /// if there is none. An empty view is therefore ambiguous between absent
    /// and present-but-empty; use has() to distinguish them. The view points
    /// into this object and is invalidated by any mutating call.
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

    /// Store a new header in the next free slot. Returns false — storing
    /// nothing and leaving the count unchanged — when the count cap is hit or
    /// the name/value exceeds its own cap. Both lengths are checked before the
    /// first byte is written: committing a truncated entry and reporting
    /// failure would leave the caller's 431 rejection sitting on top of storage
    /// that silently disagrees with the request (BR-7; CXX-07.2).
    bool emplace(std::string_view name, std::string_view value) {
        if (size_ >= INLINE_CAPACITY || name.size() > MAX_HEADER_NAME ||
            value.size() > MAX_HEADER_VALUE) {
            return false;
        }
        // Discarded, not unchecked: the guard above rejects both lengths
        // before a byte is written, and each FixedString is declared one
        // larger than its cap, so neither assign can truncate here.
        static_cast<void>(inline_headers_.at(size_).name.assign(name));
        static_cast<void>(inline_headers_.at(size_).value.assign(value));
        ++size_;
        return true;
    }
};

}  // namespace pico_http
