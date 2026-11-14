// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>

#include "pico_http/http/message.hpp"
#include "pico_http/tcp/connection_id.hpp"
#include "pico_util/containers/flat_map.hpp"

/// \file pending_pool.hpp
/// Stable response entries lent for one dispatch or completion (AR-9, BR-12).
/// HTTPHeaders is too large for the callback stack. FlatMap keeps lent
/// entries stationary across unrelated releases.

namespace pico_http::detail {

template <std::size_t Capacity>
class PendingPool {
   public:
    using ConnectionId = tcp::ConnectionId;

    static constexpr std::size_t CAPACITY = Capacity;

    /// Return a reset entry for id, or nullptr when full (BR-12, 503). A
    /// repeated claim is a caller error and resets the existing entry rather
    /// than stranding it.
    [[nodiscard]] HTTPResponse* claim(ConnectionId id) {
        if (!id.valid()) {
            return nullptr;
        }
        // In-place construction avoids passing a large HTTPResponse by value.
        HTTPResponse* response = entries_.try_emplace(id);
        if (response == nullptr) {
            return nullptr;
        }
        response->reset();
        return response;
    }

    /// Returns the entry keyed by the complete connection id, or nullptr
    /// (AR-4).
    [[nodiscard]] HTTPResponse* find(ConnectionId id) {
        if (!id.valid()) {
            return nullptr;
        }
        return entries_.find(id);
    }

    /// Release the entry for id. An absent entry is inert.
    void release(ConnectionId id) {
        if (!id.valid()) {
            return;
        }
        // erase() releases any body storage, keeping copied-body allocation
        // within AR-1.
        entries_.erase(id);
    }

    [[nodiscard]] std::size_t in_use() const noexcept {
        return entries_.size();
    }

    [[nodiscard]] bool full() const noexcept { return entries_.full(); }

   private:
    pico_util::FlatMap<ConnectionId, HTTPResponse, Capacity> entries_;
};

}  // namespace pico_http::detail
