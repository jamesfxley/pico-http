// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>

#include "pico_http/http/message.hpp"
#include "pico_http/tcp/connection_id.hpp"
#include "pico_util/containers/flat_map.hpp"

/// \file pending_pool.hpp
/// Fixed pool of response objects lent to handlers for one turn (AR-9).
///
/// A handler fills a pooled HTTPResponse rather than a stack temporary, because
/// an HTTPResponse carries a full HTTPHeaders — several KB — and the dispatch
/// path runs inside a transport callback, on the async-context stack.
///
/// An entry is held for the duration of one dispatch or one deferred
/// completion, never across a deferral: a deferring handler keeps no reference
/// and supplies a filler instead (AR-9). The pool is therefore sized by how
/// many turns can be live at once, not by how many requests are deferred — the
/// BR-12 deferral cap is counted separately by the service. Keeping it per-turn
/// is what stops slow handlers from starving ordinary synchronous traffic, and
/// what makes a late completion unable to reach a recycled entry.
///
/// The map underneath is `pico_util::FlatMap`, keyed by the whole
/// generation-tagged connection id. Its guarantee that an entry never moves is
/// load-bearing here rather than incidental: `claim()` hands out a pointer the
/// service holds across the handler call, and a container that relocated
/// entries on an unrelated `release()` would leave that pointer naming a
/// different response — with an `HTTPResponse` being kilobytes wide, it would
/// also copy one on every completed request.

namespace pico_http::detail {

template <std::size_t Capacity>
class PendingPool {
   public:
    using ConnectionId = tcp::ConnectionId;

    static constexpr std::size_t CAPACITY = Capacity;

    /// Claim an entry for `id`, reset and ready to fill, or nullptr when every
    /// entry is held (BR-12 → 503).
    ///
    /// If `id` already holds an entry the same entry is returned, reset. That
    /// is a caller error — the service dispatches a connection only while it
    /// holds none — but reusing it keeps a double-claim bounded, where handing
    /// out a second entry would strand the first for the life of the process.
    [[nodiscard]] HTTPResponse* claim(ConnectionId id) {
        if (!id.valid()) {
            return nullptr;
        }
        // try_emplace rather than insert_or_assign: an HTTPResponse is too
        // large to pass by value, and a value-initialised slot is what reset()
        // would produce anyway. It returns the existing entry for a key already
        // present, which is the double-claim case below.
        HTTPResponse* response = entries_.try_emplace(id);
        if (response == nullptr) {
            return nullptr;
        }
        response->reset();
        return response;
    }

    /// The entry `id` holds, or nullptr if it holds none. A completion naming a
    /// connection whose slot has since been recycled finds nothing, because the
    /// key is the whole generation-tagged id and the recycled slot carries a
    /// new generation (AR-4).
    [[nodiscard]] HTTPResponse* find(ConnectionId id) {
        if (!id.valid()) {
            return nullptr;
        }
        return entries_.find(id);
    }

    /// Release whatever `id` holds. Inert when it holds none, so every teardown
    /// path can call it unconditionally — which is what stops an abandoned
    /// request from stranding an entry.
    void release(ConnectionId id) {
        if (!id.valid()) {
            return;
        }
        // FlatMap::erase resets the value as it releases the slot, which is
        // what this needs rather than merely tidy. BodySource::adopt swaps, so
        // a released entry holds the connection slot's *previous* body buffer;
        // keeping it until some later claim landed here would leave that heap
        // live with no connection owning it, putting peak copied-body
        // allocation above AR-1's MAX_CONNECTIONS x MAX_BUFFERED_BODY bound.
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
