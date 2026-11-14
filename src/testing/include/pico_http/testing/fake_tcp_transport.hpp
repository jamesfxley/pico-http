// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pico_http/config/sizing.hpp"
#include "pico_http/tcp/tcp_transport.hpp"
#include "pico_http/tcp/transport_base.hpp"

namespace pico_http::testing {

namespace detail {

/// Default simulated TX capacity and RX window in bytes. TX is small enough
/// to exercise backpressure.
inline constexpr std::size_t DEFAULT_SIM_CAPACITY =
    2 * ::pico_http::TCP_MSS_BYTES;

/// Simulated peer state for one connection.
struct ClientSim {
    /// Bytes accepted by send() but not yet acknowledged by the peer.
    std::size_t inflight = 0;
    /// Bytes delivered to the service but not yet consume()d, which is what
    /// closes the simulated receive window.
    std::size_t rx_unacked = 0;
    /// Simulated transmit buffer size; inflight cannot exceed it.
    std::size_t send_capacity = DEFAULT_SIM_CAPACITY;
    /// Simulated receive window; rx_unacked cannot exceed it.
    std::size_t rx_window = DEFAULT_SIM_CAPACITY;
    /// close_client() was called and the slot is awaiting drain.
    bool close_pending = false;
};

}  // namespace detail

/// Host transport with bounded send and receive credit. Shared slots and
/// teardown come from TransportBase (AR-4).
class FakeTcpTransport
    : public tcp::TransportBase<detail::ClientSim,
                                ::pico_http::MAX_CONNECTIONS> {
   public:
    /// Connections the fake can hold open at once, matching the real transport.
    static constexpr std::size_t MAX_CLIENTS = ::pico_http::MAX_CONNECTIONS;
    /// Transmit buffer a new connection starts with, unless a test overrides
    /// it.
    static constexpr std::size_t DEFAULT_SEND_CAPACITY =
        detail::DEFAULT_SIM_CAPACITY;
    /// Receive window a new connection starts with, unless a test overrides it.
    static constexpr std::size_t DEFAULT_RX_WINDOW =
        detail::DEFAULT_SIM_CAPACITY;

    // --- ITcpTransport ------------------------------------------------------
    bool listen(std::uint16_t port) override;
    void close_server() override;
    std::size_t send(ConnectionId client,
                     std::span<const std::uint8_t> data) override;
    [[nodiscard]] std::size_t send_credit(ConnectionId client) const override;
    void consume(ConnectionId client, std::size_t bytes) override;
    void set_idle_timeout(std::uint32_t seconds) override;

    /// Records a completion signal for a later run_pending_completions() call
    /// (AR-9).
    void signal_completion() override { ++completion_signals_; }

    /// Deliver one recorded on_poll(), or return false if none is pending.
    bool run_pending_completions() {
        if (completion_signals_ == 0) {
            return false;
        }
        completion_signals_ = 0;
        notify_poll();
        return true;
    }

    /// Doorbells rung since the last run_pending_completions().
    [[nodiscard]] std::uint32_t completion_signals() const {
        return completion_signals_;
    }

    /// Records completion-lock brackets without locking on the single-context
    /// host (AR-3).
    void lock_completions() override {
        ++completion_lock_depth_;
        ++completion_locks_taken_;
        if (server_closed_) {
            ++completion_locks_after_close_server_;
        }
    }

    /// Release what lock_completions() took, counting an unbalanced release
    /// rather than going negative.
    void unlock_completions() override {
        if (completion_lock_depth_ == 0) {
            ++completion_lock_underflows_;
            return;
        }
        --completion_lock_depth_;
    }

    /// Brackets taken over this transport's lifetime.
    [[nodiscard]] std::uint32_t completion_locks_taken() const {
        return completion_locks_taken_;
    }

    /// Counts lock brackets taken after close_server(), as required by
    /// HttpService::stop().
    [[nodiscard]] std::uint32_t completion_locks_after_close_server() const {
        return completion_locks_after_close_server_;
    }

    /// True when every bracket taken has been released exactly once.
    [[nodiscard]] bool completion_lock_balanced() const {
        return completion_lock_depth_ == 0 && completion_lock_underflows_ == 0;
    }

    /// Register an on_connect lambda in the host-only owned event sink
    /// (AR-1).
    void set_on_connect(std::function<void(ConnectionId)> cb);
    /// Register a lambda for on_disconnect.
    void set_on_disconnect(
        std::function<void(ConnectionId, DisconnectReason)> cb);
    /// Register a lambda for on_receive.
    void set_on_receive(
        std::function<void(ConnectionId, std::span<const std::uint8_t>)> cb);
    /// Register a lambda for on_writable.
    void set_on_writable(std::function<void(ConnectionId, std::size_t)> cb);
    /// Register a lambda for on_refused.
    void set_on_refused(std::function<void()> cb);

    // --- Test control (simulate the peer / wire) ----------------------------

    /// Establish a connection; fires on_connect. Returns an invalid id when the
    /// client table is full.
    ConnectionId connect();

    /// Peer sends RST: fires on_disconnect(PEER_RESET) and frees the slot.
    void reset_peer(ConnectionId client);

    /// Inject a local stack abort and report ABORTED after releasing the
    /// slot.
    void abort_local(ConnectionId client);

    /// Peer sends FIN: fires on_disconnect(PEER_CLOSED) and frees the slot.
    void close_peer(ConnectionId client);

    /// Deliver bytes up to receive-window credit and return the count
    /// delivered.
    std::size_t deliver(ConnectionId client,
                        std::span<const std::uint8_t> data);
    /// deliver() over the bytes of `text`, for the common case of a test
    /// feeding a request written as a string literal.
    std::size_t deliver_text(ConnectionId client, std::string_view text);

    /// Deliver segments inside one callback scope, stopping at teardown or a
    /// closed receive window.
    /// Return bytes delivered. Unlike repeated deliver(), disconnect
    /// notification waits until all segments finish.
    std::size_t deliver_segmented(ConnectionId client,
                                  std::span<const std::string_view> segments);

    /// Peer acknowledges `bytes` of transmitted data, freeing transmit-buffer
    /// space and firing on_writable with the amount freed.
    void deliver_ack(ConnectionId client, std::size_t bytes);

    /// Advance virtual time in milliseconds and tear down expired connections
    /// with their matching reason.
    void advance_time(std::uint32_t ms);

    /// The simulated clock, in milliseconds since construction. Only
    /// advance_time() moves it.
    [[nodiscard]] std::uint32_t now_ms() const { return now_ms_; }

    /// Sets TX capacity for connections opened later. Small values exercise
    /// backpressure.
    void set_send_capacity(std::size_t bytes) { send_capacity_ = bytes; }

    /// Receive window for connections opened after this call. A small value is
    /// how a test forces deliver() to return short.
    void set_rx_window(std::size_t bytes) { rx_window_ = bytes; }

    /// Make the next listen() succeed or fail, so a test can cover the
    /// bind-failure path without a real socket.
    void set_listen_result(bool value) { listen_result_ = value; }

    // --- Observation --------------------------------------------------------
    /// Everything written to this connection so far, acknowledged or not.
    [[nodiscard]] std::vector<std::uint8_t> sent_bytes(
        ConnectionId client) const;
    /// sent_bytes() as a string, for asserting on a response as text.
    [[nodiscard]] std::string sent_text(ConnectionId client) const;
    /// Bytes sent but not yet acknowledged. Zero means the writer has drained.
    [[nodiscard]] std::size_t send_inflight(ConnectionId client) const;
    /// Bytes delivered but not yet consume()d — how far the window has closed.
    [[nodiscard]] std::size_t rx_unacked(ConnectionId client) const;

    /// Connections currently open, for asserting that a teardown freed a slot.
    [[nodiscard]] std::size_t active_clients() const {
        return clients_.active_count();
    }

    /// Every disconnect raised so far, in order, with its reason. Accumulates
    /// across the fake's lifetime and is never cleared.
    [[nodiscard]] const std::vector<std::pair<ConnectionId, DisconnectReason>>&
    disconnects() const {
        return disconnects_;
    }

    /// Every on_writable raised so far, with the credit each reported. The
    /// count matters: a streamed response should resume once per ack, not spin.
    [[nodiscard]] const std::vector<std::pair<ConnectionId, std::size_t>>&
    writable_events() const {
        return writable_events_;
    }

    /// Port passed to the most recent listen(). No socket is ever opened.
    [[nodiscard]] std::uint16_t last_port() const { return last_port_; }

    /// True after close_server() until a successful listen().
    /// While closed, connect() returns an invalid id without callbacks and
    /// advance_time() skips deadlines.
    [[nodiscard]] bool server_closed() const { return server_closed_; }

    /// The idle timeout the service last configured, in seconds.
    [[nodiscard]] std::uint32_t idle_timeout_seconds() const {
        return idle_timeout_seconds_;
    }

   protected:
    // --- TransportBase hooks ------------------------------------------------
    [[nodiscard]] std::uint32_t clock_ms() const override { return now_ms_; }

    /// TransportBase hook: a simulated peer has drained once it has
    /// acknowledged everything sent to it.
    [[nodiscard]] bool slot_drained(
        const detail::ClientSim& sim) const override {
        return sim.inflight == 0;
    }

    /// TransportBase hook: mark the slot as closing, pending drain.
    void set_close_pending(detail::ClientSim& sim) override {
        sim.close_pending = true;
    }

    /// Reset slot simulation state and record the disconnect. The base
    /// reports the event after callback return.
    void release_slot_resources(ConnectionId client, detail::ClientSim& sim,
                                DisconnectReason reason) override;

   private:
    /// Forwards each transport event to whichever lambda a test registered.
    class LambdaEvents : public ITransportEvents {
       public:
        void on_connect(ConnectionId client) override {
            if (connect) {
                connect(client);
            }
        }

        void on_disconnect(ConnectionId client,
                           DisconnectReason reason) override {
            if (disconnect) {
                disconnect(client, reason);
            }
        }

        void on_receive(ConnectionId client,
                        std::span<const std::uint8_t> data) override {
            if (receive) {
                receive(client, data);
            }
        }

        void on_writable(ConnectionId client, std::size_t sendable) override {
            if (writable) {
                writable(client, sendable);
            }
        }

        void on_refused() override {
            if (refused) {
                refused();
            }
        }

        std::function<void(ConnectionId)> connect;
        std::function<void(ConnectionId, DisconnectReason)> disconnect;
        std::function<void(ConnectionId, std::span<const std::uint8_t>)>
            receive;
        std::function<void(ConnectionId, std::size_t)> writable;
        std::function<void()> refused;
    };

    LambdaEvents lambda_events_{};
    std::uint32_t now_ms_ = 0;

    // Sent bytes keyed by raw ConnectionId so the log survives teardown (and
    // never aliases a reused slot, whose id carries a fresh generation).
    std::map<std::uint32_t, std::vector<std::uint8_t>> sent_by_id_;

    std::vector<std::pair<ConnectionId, DisconnectReason>> disconnects_;
    std::vector<std::pair<ConnectionId, std::size_t>> writable_events_;

    std::size_t send_capacity_ = DEFAULT_SEND_CAPACITY;
    std::size_t rx_window_ = DEFAULT_RX_WINDOW;
    std::uint16_t last_port_ = 0;
    bool listen_result_ = true;
    bool server_closed_ = false;
    std::uint32_t idle_timeout_seconds_ = 0;
    /// Pending AR-9 doorbells, delivered by run_pending_completions().
    std::uint32_t completion_signals_ = 0;
    std::uint32_t completion_lock_depth_ = 0;
    std::uint32_t completion_locks_taken_ = 0;
    std::uint32_t completion_locks_after_close_server_ = 0;
    std::uint32_t completion_lock_underflows_ = 0;
};

}  // namespace pico_http::testing
