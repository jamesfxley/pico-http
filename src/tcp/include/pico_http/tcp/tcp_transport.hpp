// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "pico_http/tcp/connection_id.hpp"

/// \file tcp_transport.hpp
/// Host-safe connection handles, send credit and receive-window control
/// (AR-4, AR-5, AR-6).

namespace pico_http {

/// Disconnect reasons reported to the HTTP counters (OR-1).
enum class DisconnectReason : std::uint8_t {
    PEER_CLOSED,      // peer sent FIN
    PEER_RESET,       // peer sent RST
    ABORTED,          // local abort (error path / tcp_abort)
    TIMEOUT_HEADER,   // TR-1: request head not completed in time
    TIMEOUT_BODY,     // TR-2: no body progress
    TIMEOUT_IDLE,     // TR-3: keep-alive idle expired
    TIMEOUT_WRITE,    // TR-4: response stalled with no ACK progress
    TIMEOUT_DRAIN,    // TR-5: drain-close linger expired (peer never acked)
    TIMEOUT_HANDLER,  // TR-6: handler deferred and never resolved
    SERVER_CLOSED,    // close_server()/close_client() initiated locally
};

/// The single armed connection deadline. Only the transport arms DRAIN,
/// inside close_client() (TR-1..TR-6).
enum class TimeoutKind : std::uint8_t {
    HEADER,   // TR-1
    BODY,     // TR-2
    IDLE,     // TR-3
    WRITE,    // TR-4
    DRAIN,    // TR-5, transport-internal
    HANDLER,  // TR-6, deferred handler awaiting respond()
};

/// Map timeout kinds to disconnect reasons. A missing kind fails compilation.
inline DisconnectReason timeout_reason(TimeoutKind kind) noexcept {
    switch (kind) {
        case TimeoutKind::HEADER:
            return DisconnectReason::TIMEOUT_HEADER;
        case TimeoutKind::BODY:
            return DisconnectReason::TIMEOUT_BODY;
        case TimeoutKind::IDLE:
            return DisconnectReason::TIMEOUT_IDLE;
        case TimeoutKind::WRITE:
            return DisconnectReason::TIMEOUT_WRITE;
        case TimeoutKind::DRAIN:
            return DisconnectReason::TIMEOUT_DRAIN;
        case TimeoutKind::HANDLER:
            return DisconnectReason::TIMEOUT_HANDLER;
    }
    // The switch covers every TimeoutKind.
    return DisconnectReason::TIMEOUT_DRAIN;
}

/// Transport event sink. Registration allocates nothing; steady-state paths
/// must not allocate (AR-1).
/// Unimplemented events default to no-ops.
class ITransportEvents {
   public:
    /// Connection handle these callbacks are addressed with.
    using ConnectionId = tcp::ConnectionId;

    ITransportEvents() = default;
    virtual ~ITransportEvents() = default;
    ITransportEvents(const ITransportEvents&) = delete;
    ITransportEvents& operator=(const ITransportEvents&) = delete;
    ITransportEvents(ITransportEvents&&) = delete;
    ITransportEvents& operator=(ITransportEvents&&) = delete;

    /// A connection was accepted and its id issued.
    virtual void on_connect(ConnectionId /*client*/) {}

    /// Reports a disconnect after the outermost callback unwinds. The id is
    /// already stale, but service state remains stable until notification.
    virtual void on_disconnect(ConnectionId /*client*/,
                               DisconnectReason /*reason*/) {}

    /// Bytes arrived. The span is borrowed for the duration of the call: copy
    /// anything that must outlive it.
    virtual void on_receive(ConnectionId /*client*/,
                            std::span<const std::uint8_t> /*data*/) {}

    /// Send credit became available, `sendable` bytes of it. This is what a
    /// response stalled on backpressure resumes on.
    virtual void on_writable(ConnectionId /*client*/,
                             std::size_t /*sendable*/) {}

    /// No slot was free. The TCP connection is rejected without an HTTP
    /// response or a connection id (RR-3, OR-1).
    virtual void on_refused() {}

    /// A turn on the transport context when deferred completions may be
    /// serviced (AR-9).
    /// respond() may be called from another core or an ISR. Sending happens
    /// here.
    virtual void on_poll() {}
};

/// The network interface used by the portable HTTP layer. The transport owns
/// connection lifetime and allocates nothing to register its event sink
/// (AR-1).
class ITcpTransport {
   public:
    /// Connection handle this transport issues and accepts.
    using ConnectionId = tcp::ConnectionId;

    ITcpTransport() = default;
    virtual ~ITcpTransport() = default;
    ITcpTransport(const ITcpTransport&) = delete;
    ITcpTransport& operator=(const ITcpTransport&) = delete;
    ITcpTransport(ITcpTransport&&) = delete;
    ITcpTransport& operator=(ITcpTransport&&) = delete;

    /// Bind and start accepting connections. Returns false if the port could
    /// not be bound. Set every callback before calling this (see below).
    virtual bool listen(std::uint16_t port) = 0;

    /// Tear down every live connection (disconnect callbacks fire with
    /// SERVER_CLOSED) and stop listening.
    virtual void close_server() = 0;

    /// Request an orderly close of one connection; already-queued send data is
    /// still flushed before the socket goes away (DRAIN_CLOSE).
    virtual void close_client(ConnectionId client) = 0;

    /// Returns bytes accepted, from zero to data.size(), never dropping bytes
    /// silently; resume on on_writable. An implementation must copy
    /// accepted bytes during the call: the writer reuses its scratch buffer
    /// after send() returns.
    virtual std::size_t send(ConnectionId client,
                             std::span<const std::uint8_t> data) = 0;

    /// Current ring free space, distinct from the just-acknowledged
    /// on_writable delta.
    /// The default SIZE_MAX means unknown or unbounded capacity.
    [[nodiscard]] virtual std::size_t send_credit(
        ConnectionId /*client*/) const {
        return SIZE_MAX;
    }

    /// Return receive-window credit for bytes consumed. Unconsumed bytes keep
    /// the window closed.
    virtual void consume(ConnectionId client, std::size_t bytes) = 0;

    /// Replace the connection deadline with ms_from_now milliseconds. Expiry
    /// tears down with the matching reason (TR-1..TR-4, TR-6).
    virtual void arm_timeout(ConnectionId client, TimeoutKind kind,
                             std::uint32_t ms_from_now) = 0;

    /// Disarm the per-connection timeout (e.g. a request completed).
    virtual void clear_timeout(ConnectionId client) = 0;

    /// Borrow the event sink before listen(). The sink must outlive the
    /// transport; nullptr detaches it.
    virtual void set_event_sink(ITransportEvents* sink) = 0;

    /// True for a live connection. A zero-byte send on a dead connection
    /// cannot resume.
    [[nodiscard]] virtual bool is_open(ConnectionId client) const = 0;

    /// Reports configured idle timing. A transport may ignore the hint;
    /// arm_timeout() owns the deadline (TR-3).
    virtual void set_idle_timeout(std::uint32_t /*seconds*/) {}

    /// Request a later on_poll() on the transport context (AR-9).
    /// The call must be safe from any context, including an ISR, and must not
    /// block, allocate or touch lwIP.
    /// The default does nothing. A transport unable to signal must drive
    /// on_poll() itself or document PENDING as unsupported; otherwise
    /// completions wait until TR-6 teardown.
    virtual void signal_completion() {}

    /// Lock completion records across contexts. Publication stores must not
    /// tear (AR-9, AR-3).
    /// The default is inert. The lock must remain valid for the transport
    /// lifetime, including after close_server() and failed listen(). No send,
    /// allocation or lwIP call occurs under it.
    virtual void lock_completions() {}

    /// Release the completion lock. CompletionLock pairs acquisition and
    /// release across early returns.
    virtual void unlock_completions() {}
};

namespace detail {

/// Scope-bound completion lock, released even on an early return (AR-3,
/// AR-9).
class CompletionLock {
   public:
    /// Take `transport`'s completion lock for the lifetime of this object. The
    /// transport must outlive the lock.
    explicit CompletionLock(ITcpTransport& transport) noexcept
        : transport_(&transport) {
        transport_->lock_completions();
    }

    ~CompletionLock() { transport_->unlock_completions(); }

    CompletionLock(const CompletionLock&) = delete;
    CompletionLock& operator=(const CompletionLock&) = delete;
    CompletionLock(CompletionLock&&) = delete;
    CompletionLock& operator=(CompletionLock&&) = delete;

   private:
    ITcpTransport* transport_;
};

}  // namespace detail

}  // namespace pico_http
