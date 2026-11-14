// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

/// \file timeout_scheduler.hpp
/// Per-slot deadlines with wrap-safe millisecond arithmetic (AR-7).

namespace pico_http::tcp::detail {

/// True when now reaches deadline across 32-bit millisecond wrap.
/// Requires the two times to be less than 2^31 ms (about 24.8 days) apart.
inline bool time_reached(std::uint32_t now, std::uint32_t deadline) noexcept {
    return static_cast<std::int32_t>(now - deadline) >= 0;
}

template <std::size_t Capacity>
class TimeoutScheduler {
   public:
    static constexpr std::size_t CAPACITY = Capacity;

    /// Arm slot `index` to expire at `deadline_ms` with an opaque `kind` tag
    /// (the caller assigns kind meanings). Replaces any existing deadline.
    void arm(std::size_t index, std::uint32_t deadline_ms,
             std::uint8_t kind) noexcept {
        if (index >= Capacity) {
            return;
        }
        entries_[index].active = true;
        entries_[index].deadline_ms = deadline_ms;
        entries_[index].kind = kind;
    }

    void disarm(std::size_t index) noexcept {
        if (index < Capacity) {
            entries_[index].active = false;
        }
    }

    [[nodiscard]] bool armed(std::size_t index) const noexcept {
        return index < Capacity && entries_[index].active;
    }

    /// Disarm expired slots before invoking fn, so fn may re-arm them. The
    /// repeatedly used callable is not forwarded.
    template <typename Fn>
    // cppcoreguidelines-missing-std-forward: fn is called repeatedly.
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
    void for_each_expired(std::uint32_t now_ms, Fn&& fn) {
        for (std::size_t i = 0; i < Capacity; ++i) {
            Entry& entry = entries_[i];
            if (entry.active && time_reached(now_ms, entry.deadline_ms)) {
                const std::uint8_t kind = entry.kind;
                entry.active = false;
                fn(i, kind);
            }
        }
    }

   private:
    struct Entry {
        std::uint32_t deadline_ms = 0;
        std::uint8_t kind = 0;
        bool active = false;
    };

    std::array<Entry, Capacity> entries_{};
};

}  // namespace pico_http::tcp::detail
