// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

/// \file connection_id.hpp
/// Host-safe packed connection handles with generation checks.

namespace pico_http::tcp {

/// Packed slot:8, generation:16 and tag:8 fields. Zero is invalid; slot
/// allocation skips generation zero.
class ConnectionId {
    static constexpr std::uint32_t SLOT_MASK = 0xFFU;
    static constexpr std::uint32_t GEN_SHIFT = 8U;
    static constexpr std::uint32_t GEN_MASK = 0xFFFFU;
    static constexpr std::uint32_t TAG_SHIFT = 24U;
    static constexpr std::uint32_t TAG_MASK = 0xFFU;

   public:
    /// Distinct slot indices the packed slot field can address. A slot table
    /// larger than this could not encode its own indices.
    static constexpr std::size_t SLOT_CAPACITY = SLOT_MASK + 1U;

    constexpr ConnectionId() = default;

    /// Pack a slot, generation and optional tag. Releasing a slot changes its
    /// generation.
    static constexpr ConnectionId make(std::uint8_t slot,
                                       std::uint16_t generation,
                                       std::uint8_t tag = 0) noexcept {
        return ConnectionId(
            static_cast<std::uint32_t>(slot) |
            (static_cast<std::uint32_t>(generation) << GEN_SHIFT) |
            (static_cast<std::uint32_t>(tag) << TAG_SHIFT));
    }

    /// Rebuild a packed id without validation. A nonzero value need not name
    /// a live slot.
    static constexpr ConnectionId from_raw(std::uint32_t raw) noexcept {
        return ConnectionId(raw);
    }

    /// The packed form, for storage in a C callback's void* or a plain integer.
    [[nodiscard]] constexpr std::uint32_t raw() const noexcept { return raw_; }

    /// Index into the transport's slot table.
    [[nodiscard]] constexpr std::uint8_t slot() const noexcept {
        return static_cast<std::uint8_t>(raw_ & SLOT_MASK);
    }

    /// Reuse counter for this slot; see make() for what it defends against.
    [[nodiscard]] constexpr std::uint16_t generation() const noexcept {
        return static_cast<std::uint16_t>((raw_ >> GEN_SHIFT) & GEN_MASK);
    }

    /// Caller-defined discriminator carried alongside the id, untouched by the
    /// transport.
    [[nodiscard]] constexpr std::uint8_t tag() const noexcept {
        return static_cast<std::uint8_t>((raw_ >> TAG_SHIFT) & TAG_MASK);
    }

    /// True for any nonzero packed value, including values not issued by a
    /// slot table.
    [[nodiscard]] constexpr bool valid() const noexcept { return raw_ != 0; }

    /// Equality over the whole packed value, tag included — two ids for the
    /// same slot and generation but different tags are distinct.
    constexpr bool operator==(const ConnectionId&) const noexcept = default;

   private:
    explicit constexpr ConnectionId(std::uint32_t raw) noexcept : raw_(raw) {}

    std::uint32_t raw_ = 0;
};

}  // namespace pico_http::tcp
