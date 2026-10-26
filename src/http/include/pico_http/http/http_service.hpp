// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "pico_http/config/limits.hpp"
#include "pico_http/config/profile.hpp"
#include "pico_http/config/sizing.hpp"
#include "pico_http/http/detail/pending_pool.hpp"
#include "pico_http/http/detail/response_writer.hpp"
#include "pico_http/http/headers.hpp"
#include "pico_http/http/http_parser.hpp"
#include "pico_http/http/message.hpp"
#include "pico_http/http/route_table.hpp"
#include "pico_http/tcp/tcp_transport.hpp"

/// \file http_service.hpp
/// HTTP/1.1 server on ITcpTransport. Per-connection state lives in a fixed
/// slot array indexed by ConnectionId; requests are parsed incrementally
/// (HttpParser) and responses stream out through credit-based sends that
/// resume on writable events — so a response is never silently truncated
/// regardless of size (AR-5). Request boundary violations map to
/// 400/413/414/417/431/501/505 with Connection: close (BR-1..BR-11), a
/// deferral past its cap to 503 (BR-12) and an oversized response head or
/// copied body to 500 (BR-13, BR-14). No lwIP
/// dependency: firmware injects the lwIP transport, host tests inject
/// FakeTcpTransport.

namespace pico_http {

/// The profile-derived sizes a translation unit was compiled against.
///
/// The sizing profile is chosen by macros, and those macros change class
/// layout — HttpService's slot array, HTTPHeaders' storage, the RX buffer. A
/// consumer that compiles its own translation units with different macro values
/// than the library archive was built with therefore disagrees with it about
/// object layout, silently. CMake propagates the definitions as usage
/// requirements so this cannot normally happen; this is the check for when it
/// does anyway (a hand-rolled build, a stale prebuilt archive), because the
/// failure mode is memory corruption with no other symptom.
struct ProfileBinding {
    /// Profile::MAX_CONNECTIONS, the slot-array extent.
    std::size_t max_connections;
    /// Profile::RX_CAPACITY, the RX assembly buffer size.
    std::size_t rx_capacity;
    /// Limits::MAX_BUFFERED_BODY (BR-5).
    std::size_t max_buffered_body;
    /// HTTPHeaders::MAX_HEADER_NAME (PICO_HTTP_MAX_HEADER_NAME), the name
    /// storage.
    std::size_t header_name_capacity;
    /// HTTPHeaders::MAX_HEADER_VALUE (PICO_HTTP_MAX_HEADER_VALUE), the value
    /// storage.
    std::size_t header_value_capacity;
    /// BR-12. Reported separately from conn_size below: the deferred-response
    /// pool is a service member, not per-slot storage, so a disagreement about
    /// it does not move the slot layout and the size comparison cannot see it.
    std::size_t max_pending_responses;
    /// Size and alignment of the per-connection slot. The named knobs above are
    /// the ones worth reporting individually, but they are not the only ones
    /// that move this layout — HEAD_CAPACITY and GENERATOR_CHUNK size the
    /// response writer, and the parser's scratch and fixed strings come from
    /// MAX_URI, MAX_HEADER_LINE, MAX_METHOD and MAX_VERSION. Comparing the slot
    /// itself covers every such knob, including any a downstream profile adds.
    std::size_t conn_size;
    /// Alignment of the per-connection slot, paired with conn_size.
    std::size_t conn_align;

    /// Member-wise equality. A mismatch is the layout disagreement this struct
    /// exists to detect.
    bool operator==(const ProfileBinding&) const = default;
};

// Implements ITransportEvents privately: the service is the transport's event
// sink, but those methods are not part of its own API.
/// The HTTP/1.1 server: routing, request parsing, response writing, keep-alive,
/// per-phase timeouts and observability, over an ITcpTransport.
///
/// Statically allocated. Every buffer, the connection slot array and the
/// deferred-response pool are sized at compile time from the active profile,
/// and nothing is allocated after start() returns (AR-1) — so a capacity that
/// cannot be met produces a status code (413, 431) rather than a truncated
/// request or a failed allocation.
///
/// Construct it with a transport, add routes, then start(). A handler either
/// fills the response and returns Disposition::COMPLETE, or returns
/// Disposition::PENDING and resolves the request later through respond()
/// (AR-9), which is what keeps handler CPU time off the transport's callback
/// path.
///
/// Implements ITransportEvents privately: the service is the transport's event
/// sink, but those methods are not part of its own API.
class HttpService : private ITransportEvents {
   private:
    /// The compile-time sizing policy, selected via PICO_HTTP_ACTIVE_PROFILE
    /// (config/profile.hpp). The concept states what a replacement must supply,
    /// so a mis-specified downstream profile fails here rather than inside the
    /// parser's instantiation.
    using Profile = PICO_HTTP_ACTIVE_PROFILE;
    static_assert(HttpProfile<Profile>,
                  "PICO_HTTP_ACTIVE_PROFILE must satisfy the HttpProfile "
                  "concept (see pico_http/config/profile.hpp)");

   public:
    /// The active profile's boundary limits (BR-1..BR-5), exposed so an app
    /// can report the real caps (e.g. /limits) rather than assuming the default
    /// profile's — the two differ under, e.g., -DPICO_HTTP_PROFILE=p8-rp2350.
    using Limits = typename Profile::Limits;

    /// Generation-tagged connection handle (AR-4). Public because respond()
    /// takes one: a deferring handler holds it across the wait.
    using ConnectionId = ITcpTransport::ConnectionId;

    /// A route handler. Fills `response` and returns Disposition::COMPLETE, or
    /// returns Disposition::PENDING having taken responsibility for calling
    /// respond() later (AR-9).
    ///
    /// `response` is library-owned and valid **only for the duration of the
    /// call**, in both cases. A handler must not keep the reference: the entry
    /// returns to the pool when the handler returns, and another connection may
    /// claim it immediately.
    ///
    /// A handler that returns PENDING therefore fills nothing here — anything
    /// written to `response` before returning PENDING is discarded. It keeps
    /// `request.request` (a RequestId) and later calls respond() with a filler,
    /// which the service invokes against a fresh entry at completion time. That
    /// is the whole reason the filler exists: with no reference to hold, a late
    /// completion cannot write into an entry that now belongs to someone else.
    ///
    /// The request views (method, URI, headers, body) are valid for longer than
    /// `response` is: to the handler's return for COMPLETE, and until the
    /// request resolves for PENDING, because the parser is not reset while a
    /// connection is parked.
    /// Aliases the namespace-scope pico_http::RequestHandler, which the route
    /// table also uses — one declaration, so the two cannot drift.
    using RequestHandler = ::pico_http::RequestHandler;

    /// Keep-alive policy (RR-5, NR-2). idle_timeout_seconds is the TR-3
    /// idle window between requests on a persistent connection; the request
    /// cap bounds how long one client can hold a slot.
    struct KeepAliveConfig {
        /// Default idle window between requests, in seconds (TR-3).
        static constexpr std::uint32_t DEFAULT_IDLE_TIMEOUT_SECONDS = 15;
        /// Default cap on requests served over one connection (RR-5).
        static constexpr std::uint32_t DEFAULT_MAX_REQUESTS = 16;
        /// Whether connections persist past one request. When false, every
        /// response carries Connection: close.
        bool enabled = true;
        /// Idle window between requests before the connection is torn down.
        std::uint32_t idle_timeout_seconds = DEFAULT_IDLE_TIMEOUT_SECONDS;
        /// Requests served on one connection before it is closed, so a single
        /// client cannot hold a slot indefinitely.
        std::uint32_t max_requests_per_connection = DEFAULT_MAX_REQUESTS;
    };

    /// Per-phase request timeouts (TR-1, TR-2, TR-4). TR-3 (idle) comes from
    /// KeepAliveConfig::idle_timeout_seconds; TR-5 (drain linger) is enforced
    /// by the transport.
    struct TimeoutConfig {
        /// Default header_ms.
        static constexpr std::uint32_t DEFAULT_HEADER_MS = 5000;
        /// Default body_ms.
        static constexpr std::uint32_t DEFAULT_BODY_MS = 5000;
        /// Default write_ms.
        static constexpr std::uint32_t DEFAULT_WRITE_MS = 10000;
        /// Default handler_ms.
        static constexpr std::uint32_t DEFAULT_HANDLER_MS = 10000;
        /// TR-1: accept to head complete. Bounds a client that opens a
        /// connection and sends a partial request line.
        std::uint32_t header_ms = DEFAULT_HEADER_MS;
        /// TR-2: time without body progress.
        std::uint32_t body_ms = DEFAULT_BODY_MS;
        /// TR-4: time without acknowledgement progress while sending.
        std::uint32_t write_ms = DEFAULT_WRITE_MS;
        // TR-6: handler returned PENDING and has not called respond(). Matches
        // the write-stall bound rather than the shorter header one: a deferring
        // handler is legitimately waiting on something slow, so this must catch
        // a hung handler without tearing down a merely slow one.
        /// TR-6: time a handler may stay PENDING before the connection is
        /// torn down.
        std::uint32_t handler_ms = DEFAULT_HANDLER_MS;
    };

    /// Observability counters (OR-1).
    struct Statistics {
        /// Connections accepted.
        std::uint32_t accepts = 0;
        /// Connections refused because no slot was free (RR-3).
        std::uint32_t refusals_slot_full = 0;
        /// Requests answered, whatever the status.
        std::uint32_t requests_completed = 0;
        /// Parse failures, the total across the per-status counters below.
        std::uint32_t parse_errors = 0;
        /// Parse failures answered 400 Bad Request.
        std::uint32_t parse_errors_400 = 0;
        /// Parse failures answered 413 Payload Too Large (BR-5).
        std::uint32_t parse_errors_413 = 0;
        /// Parse failures answered 414 URI Too Long (BR-1).
        std::uint32_t parse_errors_414 = 0;
        /// Parse failures answered 417 Expectation Failed (NR-5).
        std::uint32_t parse_errors_417 = 0;
        /// Parse failures answered 431 Header Fields Too Large (BR-2, BR-3).
        std::uint32_t parse_errors_431 = 0;
        /// Parse failures answered 501 Not Implemented.
        std::uint32_t parse_errors_501 = 0;
        /// Parse failures answered 505 Version Not Supported.
        std::uint32_t parse_errors_505 = 0;
        /// Connections ended by a peer FIN.
        std::uint32_t disconnect_peer_closed = 0;
        /// Connections ended by a peer RST.
        std::uint32_t disconnect_peer_reset = 0;
        /// Connections the local stack aborted: lwIP's error callback with an
        /// error other than a reset. A fault of this side, never folded into
        /// the peer's count (OR-1).
        std::uint32_t disconnect_aborted = 0;
        /// Connections ended by a timeout, the total across the kinds below.
        std::uint32_t disconnect_timeout = 0;
        /// Timeouts on the head phase (TR-1).
        std::uint32_t disconnect_timeout_header = 0;
        /// Timeouts on body progress (TR-2).
        std::uint32_t disconnect_timeout_body = 0;
        /// Timeouts on the keep-alive idle window (TR-3).
        std::uint32_t disconnect_timeout_idle = 0;
        /// Timeouts on write progress (TR-4).
        std::uint32_t disconnect_timeout_write = 0;
        /// Timeouts on the close-drain linger (TR-5).
        std::uint32_t disconnect_timeout_drain = 0;
        /// TR-6: a handler deferred and never resolved.
        std::uint32_t disconnect_timeout_handler = 0;
        /// Connections ended because the server itself stopped.
        std::uint32_t disconnect_server_closed = 0;
        /// Times a response could not be written in full and was parked to
        /// resume on a writable event. Expected under load, not a fault.
        std::uint32_t backpressure_events = 0;
        /// Times the receive window was held closed against the peer because
        /// bytes were still buffered. Routine under load — it is what AR-6
        /// flow control looks like from outside, not a fault.
        std::uint32_t rx_window_throttle_events = 0;
        /// Times delivered bytes did not fit the RX assembly buffer and were
        /// discarded. Structurally unreachable while RX_CAPACITY covers the
        /// advertised window (the static_assert below), and a desynced TCP
        /// stream if it ever happens. Counted apart from the throttle events
        /// above because a fault buried in a counter that increments in normal
        /// operation cannot be seen (CXX-07.6).
        std::uint32_t rx_overflow_drops = 0;
        /// Handlers that returned Disposition::PENDING (AR-9). Routine — this
        /// is the feature working, not a fault.
        std::uint32_t pending_dispatches = 0;
        /// Handlers that returned PENDING while the BR-12 deferral cap was
        /// already reached, and were answered 503. A capacity fault, counted
        /// apart from pending_dispatches so it is not buried in a number that
        /// grows in normal operation (CXX-07.6) — the same reason the two RX
        /// counters are separate. Only a *deferral* is ever counted here: a
        /// handler that answers inline is not subject to the cap.
        std::uint32_t deferrals_refused = 0;
        /// Responses answered with the fixed 500 because the handler's head
        /// overflowed HEAD_CAPACITY (BR-13) or its copied body exceeded
        /// MAX_BUFFERED_BODY (BR-14): a handler fault. Such a response also
        /// counts in requests_completed; this counter is what tells it apart
        /// (OR-1).
        std::uint32_t responses_refused = 0;
        /// Request bytes received across all connections.
        std::uint64_t bytes_in = 0;
        /// Response bytes written across all connections.
        std::uint64_t bytes_out = 0;
        /// Running peak of concurrently occupied slots, against
        /// MAX_CONNECTIONS. Compare with active_connections() for the live
        /// count.
        std::uint32_t slot_high_watermark = 0;
        /// Running peak of concurrently deferred requests, against MAX_PENDING.
        std::uint32_t pending_high_watermark = 0;
    };

    /// Connection slots this service holds, from the active profile. Fixed at
    /// compile time; a connection arriving with none free is refused (RR-3).
    static constexpr std::size_t MAX_CONNECTIONS = Profile::MAX_CONNECTIONS;
    static_assert(MAX_CONNECTIONS == ::pico_http::MAX_CONNECTIONS,
                  "profile slot count must match the transport's slot count");

    /// BR-12: concurrent deferred requests (AR-9). Also the response-pool size,
    /// which needs only one entry per live dispatch turn — so this is margin
    /// there and the binding limit on deferrals.
    static constexpr std::size_t MAX_PENDING = Profile::MAX_PENDING_RESPONSES;
    static_assert(MAX_PENDING >= 1,
                  "the deferral cap must be at least one: a zero cap answers "
                  "every deferring handler with 503");
    // A cap above the slot count can never be reached — a connection can park
    // at most one request — so the excess is heap no request can use.
    static_assert(MAX_PENDING <= MAX_CONNECTIONS,
                  "the deferral cap cannot exceed the slot count");

    // The line cap and the storage caps must agree about what a header can be.
    // A line cap below name + ':' + OWS + value rejects at the line before the
    // value cap is reachable, so /limits would advertise a value cap the
    // parser never enforces and the shortfall would be per-slot storage no
    // request could fill. Asserted here rather than in HttpParser, which tests
    // deliberately instantiate with tiny limits to drive the 431 boundary.
    static_assert(Profile::Limits::MAX_HEADER_LINE >=
                      HTTPHeaders::MAX_HEADER_NAME +
                          HTTPHeaders::MAX_HEADER_VALUE + HEADER_LINE_SYNTAX,
                  "the profile's MAX_HEADER_LINE must admit a full-length "
                  "header name and a full-length header value on one line");

    /// How this translation unit was sized. Evaluated at the *caller's* compile
    /// time, so it reports the caller's macro values, not the library's. A
    /// function rather than a constant because it measures Conn, which is
    /// declared further down: a member function body is compiled as if after
    /// the class is complete, an in-class initialiser is not.
    static constexpr ProfileBinding caller_binding() noexcept;

    /// `binding` defaults to the caller's view of the profile and is compared
    /// against the library's; a mismatch aborts rather than proceeding with two
    /// incompatible layouts. Do not pass it explicitly.
    explicit HttpService(std::unique_ptr<ITcpTransport> transport,
                         ProfileBinding binding = caller_binding());
    ~HttpService() override;

    HttpService(const HttpService&) = delete;
    HttpService& operator=(const HttpService&) = delete;
    HttpService(HttpService&&) = delete;
    HttpService& operator=(HttpService&&) = delete;

    /// Bind and start serving. Returns false if the port could not be bound.
    /// Everything before start() — construction, route registration, config —
    /// is the one-time init budget (AR-1) and may allocate; after start() the
    /// service is driven entirely by transport callbacks.
    bool start(std::uint16_t port);

    /// Close every live connection (clients see SERVER_CLOSED teardown) and
    /// stop listening. The destructor calls this.
    void stop();

    /// Register a handler. `uri_pattern` is an exact match, or a prefix match
    /// when it ends in `*` (e.g. "/files/*"). Returns false, registering
    /// nothing, if the method or pattern exceeds RouteTable's fixed capacity
    /// (15 / 127 chars) — a truncated pattern would never match. Registration
    /// is init-only: call before start() — it allocates (init budget) and is
    /// not synchronized with in-flight dispatch.
    [[nodiscard]] bool add_route(std::string_view method,
                                 std::string_view uri_pattern,
                                 RequestHandler handler);
    /// Handler for "/" and an empty URI. Init-only, like add_route().
    void set_default_handler(RequestHandler handler);
    /// Handler for anything no route matches. Init-only, like add_route().
    void set_404_handler(RequestHandler handler);
    /// Replace the keep-alive policy. Takes effect on connections accepted
    /// after the call.
    void set_keep_alive_config(const KeepAliveConfig& config);

    /// Replace the per-phase timeouts. Takes effect on timeouts armed after
    /// the call, not on those already running.
    void set_timeout_config(const TimeoutConfig& config) {
        timeout_config_ = config;
    }

    /// The per-phase timeouts currently in force.
    [[nodiscard]] const TimeoutConfig& timeout_config() const noexcept {
        return timeout_config_;
    }

    /// The keep-alive policy currently in force.
    [[nodiscard]] const KeepAliveConfig& keep_alive_config() const noexcept {
        return keep_alive_config_;
    }

    /// The observability counters (OR-1). A snapshot reference into the
    /// service, not a copy: read the fields, do not hold it across a callback.
    [[nodiscard]] const Statistics& statistics() const noexcept {
        return stats_;
    }

    /// Zero every counter, including the high-water marks. Does not touch live
    /// connections.
    void reset_statistics() noexcept { stats_ = Statistics{}; }

    /// Live count of occupied connection slots (as opposed to
    /// statistics().slot_high_watermark, which is the running peak).
    [[nodiscard]] std::size_t active_connections() const noexcept;

    /// Resolve a request whose handler returned Disposition::PENDING (AR-9).
    ///
    /// Records `fill`/`ctx` against the request and rings the transport's
    /// doorbell; the response is built and sent on the transport's next turn,
    /// when the service invokes `fill(ctx, response)` against a pool entry —
    /// and only if the request is still live. `fill` therefore either runs
    /// against a valid response for the right request, or does not run at all.
    ///
    /// **This call performs no send.** It allocates nothing, makes no lwIP
    /// call, and holds the transport's completion lock only for the stores that
    /// publish the record — so it is safe from any context, including core 1
    /// and an ISR. `ctx` must outlive the call to `fill`, which has not
    /// happened when this returns.
    ///
    /// Inert, returning false, when `id` does not name a request still waiting
    /// on a completion: the connection timed out (TR-6), was reset, was
    /// reclaimed by stop(), has moved on to a later request, or has already had
    /// a completion recorded. That is the normal race rather than an error
    /// (AR-4). A false return means `fill` will never run.
    ///
    /// A true return means the completion was *recorded*, not that it will be
    /// delivered: delivery needs the transport to signal its own context
    /// (ITcpTransport::signal_completion) or to drive on_poll() itself. On a
    /// transport that does neither, a PENDING handler's response is never
    /// written and the connection hangs until TR-6.
    ///
    /// Calling this from inside the handler, before returning PENDING, is
    /// supported: the request is completed as the handler returns rather than
    /// being parked.
    bool respond(RequestId id, ResponseFiller fill, void* ctx);

    /// Live count of connections parked awaiting respond() (BR-12), against
    /// MAX_PENDING. The running peak is statistics().pending_high_watermark.
    [[nodiscard]] std::size_t pending_responses() const noexcept {
        return deferred_count_;
    }

   private:
    // Serialized response head (status line + headers). Fixed so serialization
    // never touches the heap (AR-1). 1 KiB comfortably holds this server's
    // responses (status line + a handful of headers + framing); a handler
    // emitting a header block larger than this is rejected rather than sent
    // truncated (see begin_response). 1 KiB × 8 slots = 8 KiB static.
    static constexpr std::size_t HEAD_CAPACITY = Profile::HEAD_CAPACITY;
    // Scratch used to pull bytes from a generator body one chunk at a time.
    static constexpr std::size_t GENERATOR_CHUNK = Profile::GENERATOR_CHUNK;

    // RX assembly buffer capacity. This MUST be >= the transport's advertised
    // receive window (lwIP TCP_WND, currently 2×MSS): received bytes are only
    // acknowledged to the transport (consume() -> tcp_recved, reopening the
    // window) once the parser has taken them, so unconsumed bytes hold the
    // window closed rather than being dropped (AR-6). If the buffer were
    // smaller than a full window, lwIP could deliver more than fits in one
    // burst and the excess — already ACKed at the TCP layer and freed with the
    // pbuf — would be lost, desyncing the stream. Holding delivered bytes here
    // also lets the service process pipelined requests serially while an
    // earlier response is still draining under backpressure.
    static constexpr std::size_t RX_CAPACITY = Profile::RX_CAPACITY;
    static_assert(
        RX_CAPACITY >= ADVERTISED_RX_WINDOW,
        "RX assembly buffer must cover a full advertised window so no "
        "delivered byte is dropped (AR-6)");

    // Connection phase, selecting which timeout is armed (TR-1..TR-4, TR-6).
    enum class Phase : std::uint8_t {
        RECV_HEAD,
        RECV_BODY,
        PENDING,  // TR-6: handler deferred, awaiting respond()
        SENDING,
        IDLE
    };

    struct Conn {
        HttpParser<Profile::Limits> parser{};

        // Bytes received from the transport but not yet consumed by the parser.
        std::array<std::uint8_t, RX_CAPACITY> rx{};
        std::size_t rx_len = 0;

        Phase phase = Phase::RECV_HEAD;

        // The response in flight: serialized head, body source, cursors and
        // framing state (heap-free streaming, AR-5).
        detail::ResponseWriter<HEAD_CAPACITY, GENERATOR_CHUNK> writer{};

        std::uint32_t requests_served = 0;
        bool active = false;
        bool closing = false;  // finish draining the response, then close
        // A handler returned PENDING and has not resolved yet (AR-9). While
        // set, the parser is NOT reset — the request views handed to that
        // handler point into it — and no further pipelined request is consumed,
        // so buffered bytes hold the receive window closed (AR-6).
        bool pending = false;

        // --- Deferred-completion record (AR-9) ------------------------------
        //
        // Written by respond(), which may run on core 1 or in an ISR, and read
        // and cleared by poll_completions() on the transport's context. Every
        // access to the three fields below is bracketed by the transport's
        // completion lock (AR-3) so a half-published record is never observed.

        // Names the request currently dispatching or parked, so a completion
        // that arrives for an earlier request on this same connection is
        // rejected instead of resolving the current one.
        RequestId pending_request{};
        // The filler respond() recorded, invoked on the async-context turn.
        ResponseFiller pending_fill = nullptr;
        void* pending_ctx = nullptr;
        // respond() has published a completion that poll_completions() has not
        // yet taken. Separate from `pending`: a completion can be recorded
        // before the handler has even returned (a handler that resolves
        // inline), at which point the request is not parked at all.
        bool completion_ready = false;

        // Return the slot to its FREE state without materialising a ~12 KB
        // temporary on the stack (the parser alone is several KB). Only the
        // length/cursor fields are reset — the large rx and head arrays keep
        // their storage — and a copied body is freed, so an idle slot holds no
        // copied-body heap (AR-1). Called on connect,
        // disconnect, and server stop, all of which run inside a transport
        // callback.
        void reset() noexcept {
            parser.reset();
            rx_len = 0;
            phase = Phase::RECV_HEAD;
            writer.reset();
            requests_served = 0;
            active = false;
            closing = false;
            pending = false;
            pending_request = {};
            pending_fill = nullptr;
            pending_ctx = nullptr;
            completion_ready = false;
        }
    };

    // --- ITransportEvents ---------------------------------------------------
    void on_connect(ConnectionId id) override;
    void on_disconnect(ConnectionId id, DisconnectReason reason) override;
    void on_receive(ConnectionId id,
                    std::span<const std::uint8_t> data) override;
    void on_writable(ConnectionId id, std::size_t sendable) override;

    void on_refused() override { ++stats_.refusals_slot_full; }

    /// The transport reached a turn on which recorded completions can be sent
    /// (AR-9). This is where a deferred response is actually built and queued —
    /// respond() only published the record.
    void on_poll() override { poll_completions(); }

    void poll_completions();
    // Drop whatever completion handle a slot carries, so nothing can resolve a
    // request that is already answered or refused.
    void clear_completion_record(Conn& conn);
    // Build and send one recorded completion. Runs on the transport's context.
    void complete_deferred(Conn& conn, RequestId id, ResponseFiller fill,
                           void* ctx);

    Conn* conn_for(ConnectionId id);
    void process(Conn& conn, ConnectionId id);
    // The request as a handler sees it, over the connection's parser storage.
    // Rebuilt rather than stored across a deferral: the parser still holds the
    // request (a pending slot is never reset), so the view is reconstructible
    // and there is no second copy to keep in step.
    [[nodiscard]] static HTTPRequestView request_view(const Conn& conn,
                                                      RequestId id);
    // Route and run the handler, returning what it did with the response.
    [[nodiscard]] Disposition invoke_handler(const HTTPRequestView& request,
                                             HTTPResponse& response);
    void dispatch_request(Conn& conn, ConnectionId id);
    // Frame and queue a filled response, then release its pool entry. Shared by
    // the synchronous path and respond(), so the framing decisions (NR-2, NR-6)
    // cannot diverge between them.
    void complete_request(Conn& conn, RequestId id, HTTPResponse& response);
    // Refuse a deferral past the BR-12 cap. Separate from serialize_error: an
    // overload is not a malformed request, so it carries no parse-error class
    // and does not bump parse_errors.
    void serialize_service_unavailable(Conn& conn);
    void begin_response(Conn& conn, const HTTPRequestView& request,
                        HTTPResponse& response, bool close_connection);
    // Answer with the fixed 500 and close, freeing the handler's copied body:
    // the response that cannot be sent as built (BR-13, BR-14).
    void begin_internal_error(Conn& conn, HTTPResponse& response);
    // Serializes the status line and header section into `head`, applying the
    // framing already chosen for this response. Returns the byte count, or
    // nullopt when the head does not fit HEAD_CAPACITY.
    [[nodiscard]] std::optional<std::size_t> serialize_head(
        std::span<std::uint8_t> head, const Conn& conn,
        std::string_view version, const HTTPResponse& response,
        detail::Framing framing, bool close_connection) const;
    void serialize_error(Conn& conn, ParseResult error);
    void pump(Conn& conn, ConnectionId id);
    [[nodiscard]] bool should_keep_alive(const HTTPRequestView& request,
                                         const HTTPResponse& response,
                                         const Conn& conn) const;
    void apply_keep_alive_config();

    // Arm the transport timeout matching `phase` and record it on the conn.
    // Called only on genuine phase transitions so a fixed-window timeout (TR-1)
    // is not reset byte-by-byte, which would defeat slowloris defense.
    void set_phase(Conn& conn, ConnectionId id, Phase phase);
    // After parsing/pumping, arm the deadline for whatever comes next: the body
    // window while reading a body, a fresh header window when a new request's
    // head is arriving (armed only on entry so slowloris drip cannot keep
    // resetting it), or the keep-alive idle window when nothing is pending.
    void finalize_phase(Conn& conn, ConnectionId id);
    // Emit the interim "100 Continue" once, if the request asked for it (NR-5).
    // The writer owns delivering it, and any not-yet-accepted remainder, before
    // the response head so the two cannot interleave on the wire.
    void maybe_send_continue(Conn& conn, ConnectionId id);

    std::unique_ptr<ITcpTransport> transport_;
    // Deferred responses in flight (AR-9, BR-12). A service member rather than
    // per-slot storage: sized by how many handlers defer at once, not by how
    // many connections exist.
    // Response objects lent to a handler for the duration of one dispatch or
    // one deferred completion, never across a deferral (AR-9). Sized by how
    // many such turns can be live at once — one — with MAX_PENDING's margin so
    // a future re-entrant path cannot silently start refusing requests.
    detail::PendingPool<MAX_PENDING> pending_{};
    // Connections currently parked awaiting respond(). This, not the pool, is
    // what BR-12 caps: a pool entry is held only for the length of a turn, so
    // sizing the cap on it would let slow handlers refuse ordinary synchronous
    // traffic on other connections.
    std::size_t deferred_count_ = 0;
    RouteTable routes_{};
    RequestHandler default_handler_;
    RequestHandler not_found_handler_;
    KeepAliveConfig keep_alive_config_{};
    TimeoutConfig timeout_config_{};
    Statistics stats_{};
    std::array<Conn, MAX_CONNECTIONS> conns_{};
};

// Out of line: Conn must be complete before sizeof/alignof can be taken, and a
// default argument is only evaluated at the call site, so the constructor's
// `binding = caller_binding()` still resolves to the caller's values.
constexpr ProfileBinding HttpService::caller_binding() noexcept {
    return ProfileBinding{
        .max_connections = Profile::MAX_CONNECTIONS,
        .rx_capacity = Profile::RX_CAPACITY,
        .max_buffered_body = Profile::Limits::MAX_BUFFERED_BODY,
        .header_name_capacity = HTTPHeaders::MAX_HEADER_NAME,
        .header_value_capacity = HTTPHeaders::MAX_HEADER_VALUE,
        .max_pending_responses = Profile::MAX_PENDING_RESPONSES,
        .conn_size = sizeof(Conn),
        .conn_align = alignof(Conn),
    };
}

}  // namespace pico_http
