// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "pico_http/config/sizing.hpp"
#include "pico_http/tcp/detail/slot_table.hpp"
#include "pico_http/tcp/detail/timeout_scheduler.hpp"
#include "pico_http/tcp/tcp_transport.hpp"

/// \file transport_base.hpp
/// Slot, timeout and teardown machinery shared by every ITcpTransport
/// implementation. Owns the generation-tagged slot table (AR-4), the per-slot
/// timeout scheduler (AR-7), the event callbacks, and the drain-close lifecycle
/// (TR-5), so the firmware and host transports cannot diverge on any of them.
///
/// Disconnect notifications are deferred: a teardown that happens while a
/// transport callback is on the stack releases its slot immediately — the id
/// goes stale at once, so every later operation naming it is inert — but the
/// disconnect callback does not run until the outermost callback scope unwinds.
/// Without this, a service that calls close_client() mid-callback is re-entered
/// through on_disconnect while still holding a reference to the per-connection
/// state that the notification resets. Deferring makes that reference stable
/// for the duration of a callback by construction rather than by convention.
///
/// A derived transport supplies the slot payload and three hooks; everything
/// else here is shared. Pure C++ (no lwIP): host-testable.

namespace pico_http::tcp {

/// Everything an ITcpTransport needs that is not stack-specific: a fixed slot
/// table, connection-id generation, the timeout scheduler, and the deferred
/// disconnect delivery that makes an id stay valid for the duration of a
/// callback.
///
/// A derived transport supplies `Slot` — its own per-connection payload — and
/// the three hooks the shared code cannot provide. `Capacity` fixes the slot
/// count at compile time; there is no growth path, by design (AR-1).
///
/// Pure C++ with no lwIP dependency, which is what lets the whole HTTP layer be
/// host-tested against FakeTcpTransport.
template <typename Slot, std::size_t Capacity>
class TransportBase : public ITcpTransport {
   public:
    /// Connection slots this transport can hold open at once.
    static constexpr std::size_t CAPACITY = Capacity;

    // --- ITcpTransport: the parts that are identical in every transport -----
    //
    // These forward to the *_impl bodies below. A transport whose entry points
    // need external synchronisation (the lwIP one takes the stack lock)
    // overrides the public method, acquires what it needs, and delegates.

    /// Close a connection and release its slot. Delegates to close_client_impl.
    void close_client(ConnectionId client) override {
        close_client_impl(client);
    }

    /// Schedule `kind` to fire `ms_from_now` milliseconds ahead, replacing any
    /// timeout already armed on this connection. Delegates to arm_timeout_impl.
    void arm_timeout(ConnectionId client, TimeoutKind kind,
                     std::uint32_t ms_from_now) override {
        arm_timeout_impl(client, kind, ms_from_now);
    }

    /// Cancel this connection's armed timeout, if any. Delegates to
    /// clear_timeout_impl.
    void clear_timeout(ConnectionId client) override {
        clear_timeout_impl(client);
    }

    /// Install the event sink. Not owned; it must outlive the transport.
    void set_event_sink(ITransportEvents* sink) final { events_ = sink; }

    /// Whether `client` names a live connection. False for an id whose slot has
    /// been reused, which is what the generation counter exists to detect.
    [[nodiscard]] bool is_open(ConnectionId client) const final {
        return clients_.get(client) != nullptr;
    }

   protected:
    /// The fixed slot table this transport keeps connections in.
    using SlotTableType = detail::SlotTable<Slot, Capacity>;

    /// RAII marker for "a transport callback is on the stack". Disconnect
    /// notifications raised while any scope is live are queued and delivered
    /// when the outermost one unwinds. Every entry point that can reach
    /// application code — an lwIP raw callback, a timeout sweep, a test-side
    /// wire event — opens one.
    class CallbackScope {
       public:
        /// Open a scope on `owner`. Nesting is expected and counted; only the
        /// outermost scope's destructor delivers the queued disconnects.
        explicit CallbackScope(TransportBase& owner) noexcept : owner_(owner) {
            ++owner_.callback_depth_;
        }

        ~CallbackScope() {
            if (--owner_.callback_depth_ == 0) {
                owner_.flush_disconnects();
            }
        }

        CallbackScope(const CallbackScope&) = delete;
        CallbackScope& operator=(const CallbackScope&) = delete;
        CallbackScope(CallbackScope&&) = delete;
        CallbackScope& operator=(CallbackScope&&) = delete;

       private:
        TransportBase& owner_;
    };

    // --- Hooks a derived transport must supply ------------------------------

    // The four hooks below are pure virtual, so there is nothing for this
    // template to instantiate and no divergence between compilers to guard
    // against; the check cannot tell a pure virtual from a defaulted one.
    // Both concrete transports -- LwipTcpTransport and FakeTcpTransport --
    // are compiled and exercised, on firmware and host respectively.
    // NOLINTBEGIN(portability-template-virtual-member-function)

    /// Milliseconds from the transport's clock (real or simulated).
    [[nodiscard]] virtual std::uint32_t clock_ms() const = 0;

    /// True once every byte handed to this slot has been acknowledged, so a
    /// pending close may complete (TR-5).
    [[nodiscard]] virtual bool slot_drained(const Slot& slot) const = 0;

    /// Mark the slot as closing; the close completes once slot_drained().
    virtual void set_close_pending(Slot& slot) = 0;

    /// Release transport-owned resources for a slot being torn down (pcbs,
    /// buffers, simulation state). Must not invoke application callbacks — the
    /// base delivers the disconnect notification itself.
    virtual void release_slot_resources(ConnectionId client, Slot& slot,
                                        DisconnectReason reason) = 0;

    // Closes the region opened above; the pure-virtual reason is there.
    // NOLINTEND(portability-template-virtual-member-function)

    // --- Shared bodies for the ITcpTransport methods above -------------------

    /// Shared body for close_client(). Marks the slot closing, bounds the
    /// linger with a DRAIN timeout so an unacknowledged queue cannot hold the
    /// slot forever (TR-5), and completes at once if nothing is outstanding. A
    /// stale id is a no-op, not an error (AR-4).
    void close_client_impl(ConnectionId client) {
        Slot* slot = clients_.get(client);
        if (slot == nullptr) {
            return;  // stale id: safe no-op (AR-4)
        }
        set_close_pending(*slot);
        // Bound the linger: if the peer never acks the queued data, TR-5 fires.
        timeouts_.arm(client.slot(), clock_ms() + DRAIN_LINGER_MS,
                      static_cast<std::uint8_t>(TimeoutKind::DRAIN));
        finish_close_if_drained(client, *slot);
    }

    /// Shared body for arm_timeout(). A stale id is a no-op, and TimeoutKind::
    /// DRAIN is refused: the linger is transport-internal and only
    /// close_client_impl() may arm it.
    void arm_timeout_impl(ConnectionId client, TimeoutKind kind,
                          std::uint32_t ms_from_now) {
        if (clients_.get(client) == nullptr) {
            return;  // stale id: safe no-op
        }
        if (kind == TimeoutKind::DRAIN) {
            return;  // transport-internal: only close_client() arms the linger
        }
        timeouts_.arm(client.slot(), clock_ms() + ms_from_now,
                      static_cast<std::uint8_t>(kind));
    }

    /// Shared body for clear_timeout(). A stale id is a no-op.
    void clear_timeout_impl(ConnectionId client) {
        if (clients_.get(client) == nullptr) {
            return;
        }
        timeouts_.disarm(client.slot());
    }

    // --- Shared teardown ----------------------------------------------------

    /// Tear a connection down: release its resources and its slot (making the
    /// id stale immediately), then report the disconnect — deferred if a
    /// callback is on the stack. Idempotent: tearing down a stale id is a
    /// no-op.
    void teardown(ConnectionId client, DisconnectReason reason) {
        Slot* slot = clients_.get(client);
        if (slot == nullptr) {
            return;  // already gone / stale
        }
        timeouts_.disarm(client.slot());
        release_slot_resources(client, *slot, reason);
        // Release before notifying so anything that inspects the transport
        // during the notification sees the connection as already closed.
        clients_.release(client);
        notify_disconnect(client, reason);
    }

    /// Complete a pending close once the peer has acked everything queued.
    void finish_close_if_drained(ConnectionId client, Slot& slot) {
        if (slot.close_pending && slot_drained(slot)) {
            teardown(client, DisconnectReason::SERVER_CLOSED);
        }
    }

    /// Tear down every live connection (used by close_server()).
    void close_all_clients() {
        const CallbackScope scope(*this);
        std::array<ConnectionId, Capacity> active{};
        std::size_t count = 0;
        clients_.for_each_active(
            [&](ConnectionId id, Slot&) { active.at(count++) = id; });
        for (std::size_t i = 0; i < count; ++i) {
            teardown(active[i], DisconnectReason::SERVER_CLOSED);
        }
    }

    /// Fire every timeout whose deadline has passed, tearing down each
    /// connection with the reason matching the armed kind.
    void fire_expired_timeouts() {
        const CallbackScope scope(*this);
        timeouts_.for_each_expired(clock_ms(), [&](std::size_t index,
                                                   std::uint8_t kind) {
            const ConnectionId id = clients_.id_at(index);
            if (id.valid()) {
                teardown(id, timeout_reason(static_cast<TimeoutKind>(kind)));
            }
        });
    }

    // --- Notification helpers (respect the callback scope) ------------------

    /// Raise on_connect for a newly accepted connection. A no-op when no sink
    /// is installed, as are all the notify_* helpers.
    void notify_connect(ConnectionId client) {
        if (events_ != nullptr) {
            events_->on_connect(client);
        }
    }

    /// Raise on_receive with bytes from the wire. `data` is borrowed for the
    /// duration of the call only.
    void notify_receive(ConnectionId client,
                        std::span<const std::uint8_t> data) {
        if (events_ != nullptr) {
            events_->on_receive(client, data);
        }
    }

    /// Raise on_writable with the send credit that just became available.
    void notify_writable(ConnectionId client, std::size_t sendable) {
        if (events_ != nullptr) {
            events_->on_writable(client, sendable);
        }
    }

    /// Raise on_refused for a connection turned away with no slot free.
    void notify_refused() {
        if (events_ != nullptr) {
            events_->on_refused();
        }
    }

    /// Raise on_poll for the periodic tick the service does deferred work on.
    void notify_poll() {
        if (events_ != nullptr) {
            events_->on_poll();
        }
    }

    // Protected rather than private: the slot table and the timeout wheel are
    // the state a derived transport drives directly (the lwIP transport binds
    // pcbs to slots, the fake drives time), which is what this base exists to
    // share. An accessor pair per member would restate the same access with
    // more indirection.
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    /// Live connections, one slot each, fixed at Capacity.
    SlotTableType clients_{};
    /// Deadlines armed against those slots, indexed the same way.
    detail::TimeoutScheduler<Capacity> timeouts_{};
    // Closes the region opened above; the derived-access reason is there.
    // NOLINTEND(misc-non-private-member-variables-in-classes)

   private:
    /// Queue a disconnect for delivery after the outermost callback unwinds, or
    /// deliver it now when nothing is on the stack. The queue holds two entries
    /// per slot: one teardown each for a slot torn down, re-accepted and torn
    /// down again within a single scope — more teardowns than that cannot be
    /// raised before the scope unwinds, so overflow means the invariant this
    /// class enforces is already broken. Delivering anyway would re-enter the
    /// sink from inside a callback, which is exactly what deferral exists to
    /// prevent, so abort instead of proceeding into that state.
    void notify_disconnect(ConnectionId client, DisconnectReason reason) {
        if (callback_depth_ == 0) {
            deliver_disconnect(client, reason);
            return;
        }
        if (pending_count_ >= pending_.size()) {
            std::abort();
        }
        pending_[pending_count_++] = {client, reason};
    }

    /// Deliver the queued disconnects.
    ///
    /// This runs at callback depth 0, so a sink that re-enters the transport
    /// here opens a fresh CallbackScope whose unwind calls this again. Without
    /// the guard that nested pass would restart at index 0 — the entries this
    /// one already delivered are still queued — and redeliver them, recursing.
    /// The guard makes the outermost pass the only one that delivers; a nested
    /// call returns and leaves its entries for the loop below, which re-reads
    /// pending_count_ each iteration and so drains late arrivals in this pass.
    void flush_disconnects() {
        if (flushing_) {
            return;
        }
        flushing_ = true;
        for (std::size_t i = 0; i < pending_count_; ++i) {
            deliver_disconnect(pending_[i].id, pending_[i].reason);
        }
        pending_count_ = 0;
        flushing_ = false;
    }

    void deliver_disconnect(ConnectionId client, DisconnectReason reason) {
        if (events_ != nullptr) {
            events_->on_disconnect(client, reason);
        }
    }

    struct PendingDisconnect {
        ConnectionId id;
        DisconnectReason reason = DisconnectReason::SERVER_CLOSED;
    };

    ITransportEvents* events_ = nullptr;

    std::array<PendingDisconnect, Capacity * 2> pending_{};
    std::size_t pending_count_ = 0;
    std::size_t callback_depth_ = 0;
    bool flushing_ = false;
};

}  // namespace pico_http::tcp
