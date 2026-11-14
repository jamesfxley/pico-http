// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "pico_http/tcp/connection_id.hpp"

/// \file slot_table.hpp
/// Fixed slots with generation-checked connection handles (AR-4).

namespace pico_http::tcp::detail {

/// Owns N default-constructed Slot payloads. Capacity must fit the 8-bit slot
/// field of ConnectionId.
template <typename Slot, std::size_t Capacity>
class SlotTable {
    static_assert(Capacity > 0, "SlotTable capacity must be non-zero");
    static_assert(Capacity <= ConnectionId::SLOT_CAPACITY,
                  "slot index is 8 bits");

   public:
    static constexpr std::size_t CAPACITY = Capacity;

    /// Claim a slot, or return an invalid id when full. The caller
    /// initialises the retained payload.
    ConnectionId acquire() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) {
            if (!entries_[i].occupied) {
                entries_[i].occupied = true;
                ++active_count_;
                high_watermark_ = std::max(active_count_, high_watermark_);
                return ConnectionId::make(static_cast<std::uint8_t>(i),
                                          entries_[i].generation);
            }
        }
        return ConnectionId{};
    }

    /// Release a slot and advance its generation. Return false for a stale or
    /// invalid id.
    bool release(ConnectionId id) noexcept {
        Entry* entry = live_entry(id);
        if (entry == nullptr) {
            return false;
        }
        entry->occupied = false;
        entry->generation = next_generation(entry->generation);
        --active_count_;
        return true;
    }

    /// Resolve id to its live slot payload, or nullptr if stale/invalid.
    Slot* get(ConnectionId id) noexcept {
        Entry* entry = live_entry(id);
        return entry != nullptr ? &entry->slot : nullptr;
    }

    [[nodiscard]] const Slot* get(ConnectionId id) const noexcept {
        const Entry* entry = live_entry(id);
        return entry != nullptr ? &entry->slot : nullptr;
    }

    /// The current ConnectionId for a physical slot index (invalid if free).
    [[nodiscard]] ConnectionId id_at(std::size_t index) const noexcept {
        if (index >= Capacity || !entries_[index].occupied) {
            return ConnectionId{};
        }
        return ConnectionId::make(static_cast<std::uint8_t>(index),
                                  entries_[index].generation);
    }

    /// Invoke fn for each occupied slot without forwarding a repeatedly used
    /// callable.
    template <typename Fn>
    // cppcoreguidelines-missing-std-forward: fn is called repeatedly.
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
    void for_each_active(Fn&& fn) {
        for (std::size_t i = 0; i < Capacity; ++i) {
            if (entries_[i].occupied) {
                fn(ConnectionId::make(static_cast<std::uint8_t>(i),
                                      entries_[i].generation),
                   entries_[i].slot);
            }
        }
    }

    [[nodiscard]] std::size_t active_count() const noexcept {
        return active_count_;
    }

    [[nodiscard]] std::size_t high_watermark() const noexcept {
        return high_watermark_;
    }

    [[nodiscard]] bool full() const noexcept {
        return active_count_ == Capacity;
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

   private:
    struct Entry {
        Slot slot{};
        std::uint16_t generation =
            1;  // never 0: keeps the invalid sentinel free
        bool occupied = false;
    };

    static std::uint16_t next_generation(std::uint16_t gen) noexcept {
        const auto next = static_cast<std::uint16_t>(gen + 1U);
        return next == 0U ? 1U : next;  // wrap past the reserved 0
    }

    Entry* live_entry(ConnectionId id) noexcept {
        if (!id.valid() || id.slot() >= Capacity) {
            return nullptr;
        }
        Entry& entry = entries_[id.slot()];
        if (!entry.occupied || entry.generation != id.generation()) {
            return nullptr;
        }
        return &entry;
    }

    [[nodiscard]] const Entry* live_entry(ConnectionId id) const noexcept {
        if (!id.valid() || id.slot() >= Capacity) {
            return nullptr;
        }
        const Entry& entry = entries_[id.slot()];
        if (!entry.occupied || entry.generation != id.generation()) {
            return nullptr;
        }
        return &entry;
    }

    std::array<Entry, Capacity> entries_{};
    std::size_t active_count_ = 0;
    std::size_t high_watermark_ = 0;
};

}  // namespace pico_http::tcp::detail
