// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "pico_http/tcp/connection_id.hpp"

/// \file tcp_transport.hpp
/// Host-safe transport abstraction. Connections are named by generation-tagged
/// ConnectionId, never raw pointers (AR-4); sends are credit-based and never
/// silently truncate (AR-5); received bytes are explicitly acknowledged via
/// consume() so the transport can gate the receive window (AR-6). No lwIP
/// dependency — the firmware LwipTcpTransport and the host FakeTcpTransport
/// both implement it. Sizing constants (slot count, MSS, advertised window)
/// belong to the config module, not to this interface.

namespace pico_http {

/// Why a connection ended (OR-1). Delivered to the disconnect
/// callback so the HTTP layer can tell graceful close from RST/timeout — and
/// which timeout — for its counters.
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

/// The per-connection timeout currently armed (TR-1..TR-6). Exactly one is
/// active at a time, reflecting the connection's phase; the HTTP layer re-arms
/// on each state transition. DRAIN is transport-internal: the HTTP layer never
/// arms it (arm_timeout rejects it), because the transport arms it itself
/// inside close_client(). The linger duration is a sizing tunable and lives in
/// the config module.
enum class TimeoutKind : std::uint8_t {
    HEADER,   // TR-1
    BODY,     // TR-2
    IDLE,     // TR-3
    WRITE,    // TR-4
    DRAIN,    // TR-5, transport-internal
    HANDLER,  // TR-6, deferred handler awaiting respond()
};

/// Map an armed timeout kind to the DisconnectReason reported when it expires.
/// Shared by both transports. Exhaustive with no default: a new kind without a
/// reason is a compile error rather than a silently mis-reported teardown.
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
    return DisconnectReason::TIMEOUT_DRAIN;  // unreachable: switch is
                                             // exhaustive
}

/// Everything a transport reports to its owner. Virtual dispatch rather than
/// stored std::functions: registration then costs no allocation at all, which
/// matters because a std::function assignment can allocate and the steady-state
/// paths must not (AR-1). Every method defaults to a no-op so an implementer
/// overrides only the events it cares about.
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

    /// A connection ended, for the given reason. Delivered after any callback
    /// on the stack has unwound, so the id stays usable for the whole of the
    /// callback that caused the close.
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

    /// An inbound connection was refused because no slot was free (RR-3 / OR-1
    /// refusals_slot_full). Refused at the TCP layer with no HTTP response, so
    /// there is no ConnectionId to report.
    virtual void on_refused() {}

    /// The transport reached a turn on which deferred completions may be
    /// serviced, having been asked to by signal_completion() (AR-9). Runs on
    /// the same context as every other event here, which is the point:
    /// respond() may be called from core 1 or an ISR, but the send it leads to
    /// happens here, on the async-context path, like every other send.
    virtual void on_poll() {}
};

/// The TCP seam the HTTP layer is written against.
///
/// Everything above this interface — parsing, routing, response assembly,
/// backpressure, timeouts — is stack-agnostic and host-testable, which is why
/// only the lwIP glue needs hardware to exercise. LwipTcpTransport implements
/// it on firmware; FakeTcpTransport implements it for the host suite.
///
/// The transport owns connection lifetime and issues the ConnectionId that
/// names one. It calls into an ITransportEvents sink rather than taking
/// callbacks, so registration allocates nothing (AR-1).
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

    /// Credit-based send. Returns the number of bytes accepted into the
    /// transmit ring (0..data.size()). A short or zero return means the ring is
    /// full; resume when on_writable fires. Never drops bytes silently.
    ///
    /// `data` is borrowed for the duration of the call: an implementation must
    /// copy the bytes it accepts, not retain the span. This is a requirement
    /// rather than a convention — the response writer sends from a loop-body
    /// scratch buffer that goes out of scope on the next iteration, so a
    /// transport holding the span for an asynchronous write would transmit
    /// whatever replaced it. Retaining the *copy* is the point:
    /// LwipTcpTransport keeps its ring's bytes valid until the peer acks them,
    /// which is what lets it hand lwIP a pointer without TCP_WRITE_FLAG_COPY.
    virtual std::size_t send(ConnectionId client,
                             std::span<const std::uint8_t> data) = 0;

    /// Bytes send() can accept right now — the free space in the transmit ring.
    /// Lets a producer size its next fill so every byte it generates is
    /// accepted, rather than generating optimistically and discarding what the
    /// ring rejects (bounds generator re-invocation under
    /// backpressure). This is transmit-ring free space, distinct from the
    /// on_writable `sendable` argument, which is the just-acked delta and does
    /// not describe ring capacity — clamping a producer by `sendable` would
    /// stall. Default returns SIZE_MAX (capacity unknown/unbounded) so a
    /// transport that does not model a ring stays correct without overriding.
    [[nodiscard]] virtual std::size_t send_credit(
        ConnectionId /*client*/) const {
        return SIZE_MAX;
    }

    /// Acknowledge `bytes` received bytes as consumed by the application. The
    /// transport reopens that much receive window (delayed tcp_recved); bytes
    /// left unconsumed hold the window closed — genuine RX flow control.
    virtual void consume(ConnectionId client, std::size_t bytes) = 0;

    /// Arm the single per-connection timeout to fire `ms_from_now` milliseconds
    /// from now with the given kind, replacing any previously armed one
    /// (TR-1..TR-4, TR-6). On expiry the connection is torn down and the
    /// disconnect
    /// callback
    /// fires with the matching TIMEOUT_* reason.
    virtual void arm_timeout(ConnectionId client, TimeoutKind kind,
                             std::uint32_t ms_from_now) = 0;

    /// Disarm the per-connection timeout (e.g. a request completed).
    virtual void clear_timeout(ConnectionId client) = 0;

    /// Route transport events to `sink`, which is borrowed, not owned, and
    /// must outlive the transport. Set it before listen(). Passing nullptr
    /// detaches, after which events are dropped.
    virtual void set_event_sink(ITransportEvents* sink) = 0;

    /// True while `client` names a live connection. Distinguishes a send() that
    /// returned 0 because the ring is full — retry on the next writable — from
    /// one that returned 0 because the connection is gone, which no amount of
    /// waiting will fix.
    [[nodiscard]] virtual bool is_open(ConnectionId client) const = 0;

    /// Optional hint: the HTTP layer reports its configured idle window here
    /// when keep-alive config changes. The authoritative TR-3 idle timeout is
    /// the one armed via arm_timeout(), so a transport may ignore this —
    /// hence the default no-op (the lwIP transport does; the fake records it
    /// so tests can assert the configured value).
    virtual void set_idle_timeout(std::uint32_t /*seconds*/) {}

    /// Ask for an on_poll() on a later turn of the transport's own context
    /// (AR-9). Called by respond(), so it must be safe from any context —
    /// core 1, an ISR — and must not block, allocate, or touch lwIP.
    ///
    /// **Deferred completion is a capability of the transport, not of the
    /// service.** The default is a no-op, so a transport that neither
    /// overrides this nor drives on_poll() itself cannot serve a handler that
    /// returns Disposition::PENDING: respond() records the completion and
    /// returns true, and the response is never written. It surfaces as a
    /// connection that hangs until the TR-6 timeout tears it down, not as a
    /// missing override. A transport that cannot signal must either drive
    /// on_poll() directly, as the host double does, or document that PENDING
    /// is unsupported on it.
    virtual void signal_completion() {}

    /// Bracket the deferred-completion record (AR-9, AR-3). respond() writes it
    /// from an arbitrary context while the transport's context reads and clears
    /// it, so the two stores that publish a completion must not tear. Held for
    /// pointer stores only: no lwIP call, no allocation, no send. The default
    /// pair is a no-op because a single-context transport has nothing to
    /// exclude — the host fake takes it, so host tests exercise the same code
    /// path the firmware locks.
    ///
    /// Whatever the bracket is, it must be valid for the transport's whole
    /// lifetime: HttpService::stop() closes the server and *then* clears every
    /// slot under this lock, and a service whose listen() failed still runs
    /// stop() from its destructor. A lock created in listen() or released in
    /// close_server() is taken outside its own lifetime on both paths.
    virtual void lock_completions() {}

    /// Release what lock_completions() took. Prefer detail::CompletionLock over
    /// calling this by hand — an early return between the two is what the RAII
    /// bracket exists to make impossible.
    virtual void unlock_completions() {}
};

namespace detail {

/// RAII bracket over ITcpTransport::lock_completions() (AR-3, AR-9).
///
/// Scoped rather than manual because every early return in the completion paths
/// would otherwise be a place to leave interrupts disabled. Non-copyable and
/// non-movable: the lock is tied to the scope that took it.
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
