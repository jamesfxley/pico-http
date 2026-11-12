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
/// Shared slots, deadlines and drain-close lifecycle (AR-4, AR-7, TR-5).
/// Teardown invalidates the id immediately. Disconnect notification waits
/// until the outermost callback returns, preserving service state while
/// callbacks hold references.

namespace pico_http::tcp {

/// Fixed-capacity transport state and deferred disconnect delivery (AR-1).
/// Derived transports supply a slot payload and four stack-specific hooks.
template <typename Slot, std::size_t Capacity>
class TransportBase : public ITcpTransport {
   public:
    /// Connection slots this transport can hold open at once.
    static constexpr std::size_t CAPACITY = Capacity;

    // A transport can override an entry point to acquire its stack lock
    // before delegating to the shared implementation.

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

    /// Disconnect notifications wait until the outermost callback scope
    /// returns. Every entry point reaching application code opens a scope.
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

    // portability-template-virtual-member-function: the hooks are pure virtual.
    // NOLINTBEGIN(portability-template-virtual-member-function)

    /// Milliseconds from the transport's clock (real or simulated).
    [[nodiscard]] virtual std::uint32_t clock_ms() const = 0;

    /// True once every byte handed to this slot has been acknowledged, so a
    /// pending close may complete (TR-5).
    [[nodiscard]] virtual bool slot_drained(const Slot& slot) const = 0;

    /// Mark the slot as closing; the close completes once slot_drained().
    virtual void set_close_pending(Slot& slot) = 0;

    /// Release stack resources. Must not invoke application callbacks; the
    /// base reports the disconnect.
    virtual void release_slot_resources(ConnectionId client, Slot& slot,
                                        DisconnectReason reason) = 0;

    // portability-template-virtual-member-function: scope ends here.
    // NOLINTEND(portability-template-virtual-member-function)

    // --- Shared bodies for the ITcpTransport methods above -------------------

    /// Drain before closing, bounded by the linger deadline. A stale id is
    /// inert (TR-5, AR-4).
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

    /// Replaces the deadline for a live id. Only close_client_impl() may arm
    /// DRAIN.
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

    /// Release resources and invalidate the id before reporting disconnect.
    /// Notification waits for callback return; stale teardown is inert.
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

    // misc-non-private-member-variables-in-classes: derived classes use it.
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    /// Live connections, one slot each, fixed at Capacity.
    SlotTableType clients_{};
    /// Deadlines armed against those slots, indexed the same way.
    detail::TimeoutScheduler<Capacity> timeouts_{};
    // misc-non-private-member-variables-in-classes: scope ends here.
    // NOLINTEND(misc-non-private-member-variables-in-classes)

   private:
    /// Report disconnect now or queue it until callback return.
    /// The queue holds two teardowns per slot within a scope. Overflow aborts
    /// to prevent re-entry into the sink.
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

    /// Drain notifications at callback depth zero. A guard leaves re-entrant
    /// arrivals for the outer loop, preventing duplicate delivery.
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
