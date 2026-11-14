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
/// HTTP service over a host-safe transport, with resumable output and
/// explicit boundary errors (AR-5, BR-1..BR-11, BR-12, BR-13, BR-14).

namespace pico_http {

/// Compiled profile sizes used to detect incompatible caller and library
/// layouts.
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
    /// Service-wide pool capacity, which a per-slot size comparison cannot
    /// detect (BR-12).
    std::size_t max_pending_responses;
    /// Slot size in bytes, covering profile sizes not compared individually.
    std::size_t conn_size;
    /// Alignment of the per-connection slot, paired with conn_size.
    std::size_t conn_align;

    /// Member-wise equality. A mismatch is the layout disagreement this struct
    /// exists to detect.
    bool operator==(const ProfileBinding&) const = default;
};

/// HTTP service with fixed slot buffers. After start() only copied bodies
/// allocate, within a bound (AR-1).
/// Handlers complete inline or resolve later through respond() (AR-9).
class HttpService : private ITransportEvents {
   private:
    /// The selected sizing policy. A replacement must satisfy HttpProfile.
    using Profile = PICO_HTTP_ACTIVE_PROFILE;
    static_assert(HttpProfile<Profile>,
                  "PICO_HTTP_ACTIVE_PROFILE must satisfy the HttpProfile "
                  "concept (see pico_http/config/profile.hpp)");

   public:
    /// Active parser limits for reporting the configured capacities
    /// (BR-1..BR-5).
    using Limits = typename Profile::Limits;

    /// Generation-tagged transport connection handle (AR-4).
    using ConnectionId = ITcpTransport::ConnectionId;

    /// A handler completes inline or defers through respond() (AR-9).
    /// A handler must not retain response beyond the call. Another connection
    /// can claim it immediately. Request views last until return for COMPLETE
    /// or resolution for PENDING.
    using RequestHandler = ::pico_http::RequestHandler;

    /// Persistence policy and the idle window between requests (RR-5, NR-2,
    /// TR-3).
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

    /// Request-phase deadlines. Idle timing comes from KeepAliveConfig; the
    /// transport enforces drain linger (TR-1, TR-2, TR-4, TR-3, TR-5).
    struct TimeoutConfig {
        /// Default header_ms.
        static constexpr std::uint32_t DEFAULT_HEADER_MS = 5000;
        /// Default body_ms.
        static constexpr std::uint32_t DEFAULT_BODY_MS = 5000;
        /// Default write_ms.
        static constexpr std::uint32_t DEFAULT_WRITE_MS = 10000;
        /// Default handler_ms.
        static constexpr std::uint32_t DEFAULT_HANDLER_MS = 10000;
        /// Accept-to-head deadline in milliseconds, bounding partial requests
        /// (TR-1).
        std::uint32_t header_ms = DEFAULT_HEADER_MS;
        /// Time without body progress, in milliseconds (TR-2).
        std::uint32_t body_ms = DEFAULT_BODY_MS;
        /// Time without ACK progress while sending, in milliseconds (TR-4).
        std::uint32_t write_ms = DEFAULT_WRITE_MS;
        /// Time in milliseconds a handler may stay PENDING before teardown.
        /// The deadline must catch a hung handler without closing a merely
        /// slow one (TR-6).
        std::uint32_t handler_ms = DEFAULT_HANDLER_MS;
    };

    /// Observability counters (OR-1).
    struct Statistics {
        /// Connections accepted.
        std::uint32_t accepts = 0;
        /// TCP connections rejected because no slot was free (RR-3).
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
        /// Local stack aborts other than peer resets (OR-1).
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
        /// Unresolved deferred handlers (TR-6).
        std::uint32_t disconnect_timeout_handler = 0;
        /// Connections ended because the server itself stopped.
        std::uint32_t disconnect_server_closed = 0;
        /// Times a response could not be written in full and was parked to
        /// resume on a writable event. Expected under load, not a fault.
        std::uint32_t backpressure_events = 0;
        /// Times buffered bytes held the receive window closed (AR-6).
        std::uint32_t rx_window_throttle_events = 0;
        /// Delivered bytes discarded because the RX buffer was full. Any
        /// increment indicates a fault.
        std::uint32_t rx_overflow_drops = 0;
        /// Handlers that returned Disposition::PENDING (AR-9). Routine — this
        /// is the feature working, not a fault.
        std::uint32_t pending_dispatches = 0;
        /// Deferrals answered 503 because the cap was reached. Inline
        /// handlers are exempt (BR-12).
        std::uint32_t deferrals_refused = 0;
        /// Fixed 500 responses for oversized heads or copied bodies, also
        /// included in requests_completed (BR-13, BR-14, OR-1).
        std::uint32_t responses_refused = 0;
        /// Request bytes received across all connections.
        std::uint64_t bytes_in = 0;
        /// Response bytes written across all connections.
        std::uint64_t bytes_out = 0;
        /// Peak occupied slots. active_connections() reports the current
        /// count.
        std::uint32_t slot_high_watermark = 0;
        /// Running peak of concurrently deferred requests, against MAX_PENDING.
        std::uint32_t pending_high_watermark = 0;
    };

    /// Connection slots this service holds, from the active profile. Fixed at
    /// compile time. Excess connections receive TCP RST (RR-3).
    static constexpr std::size_t MAX_CONNECTIONS = Profile::MAX_CONNECTIONS;
    static_assert(MAX_CONNECTIONS == ::pico_http::MAX_CONNECTIONS,
                  "profile slot count must match the transport's slot count");

    /// Deferred-request cap and response-pool capacity (BR-12, AR-9).
    static constexpr std::size_t MAX_PENDING = Profile::MAX_PENDING_RESPONSES;
    static_assert(MAX_PENDING >= 1,
                  "the deferral cap must be at least one: a zero cap answers "
                  "every deferring handler with 503");
    // A cap above the slot count can never be reached — a connection can park
    // at most one request — so the excess is heap no request can use.
    static_assert(MAX_PENDING <= MAX_CONNECTIONS,
                  "the deferral cap cannot exceed the slot count");

    // The line and storage caps must agree so every advertised header value
    // fits. Parser tests use smaller line limits to exercise 431.
    static_assert(Profile::Limits::MAX_HEADER_LINE >=
                      HTTPHeaders::MAX_HEADER_NAME +
                          HTTPHeaders::MAX_HEADER_VALUE + HEADER_LINE_SYNTAX,
                  "the profile's MAX_HEADER_LINE must admit a full-length "
                  "header name and a full-length header value on one line");

    /// Reports sizes from the caller translation unit. A function body can
    /// measure Conn after the class is complete.
    static constexpr ProfileBinding caller_binding() noexcept;

    /// Abort on incompatible caller and library layouts. Do not pass binding
    /// explicitly.
    explicit HttpService(std::unique_ptr<ITcpTransport> transport,
                         ProfileBinding binding = caller_binding());
    ~HttpService() override;

    HttpService(const HttpService&) = delete;
    HttpService& operator=(const HttpService&) = delete;
    HttpService(HttpService&&) = delete;
    HttpService& operator=(HttpService&&) = delete;

    /// Start serving, or return false if binding fails (AR-1).
    /// Construction, registration and configuration before start() may
    /// allocate. Transport callbacks drive the service afterwards.
    bool start(std::uint16_t port);

    /// Close every live connection (clients see SERVER_CLOSED teardown) and
    /// stop listening. The destructor calls this.
    void stop();

    /// Register an exact or trailing-* prefix route. Excess method or pattern
    /// length returns false without registration.
    /// Call before start(): registration allocates and is not synchronised
    /// with dispatch.
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

    /// Record a completion without allocation or sending, from any context
    /// (AR-9).
    /// ctx must outlive fill(). A stale or duplicate id returns false without
    /// running fill (TR-6, AR-4).
    /// A true return records the completion; delivery needs
    /// signal_completion() or on_poll(). Inline completion before returning
    /// PENDING is supported.
    bool respond(RequestId id, ResponseFiller fill, void* ctx);

    /// Live count of connections parked awaiting respond() (BR-12), against
    /// MAX_PENDING. The running peak is statistics().pending_high_watermark.
    [[nodiscard]] std::size_t pending_responses() const noexcept {
        return deferred_count_;
    }

   private:
    // Response-head capacity in bytes. An oversized head receives a fixed 500
    // (AR-1).
    static constexpr std::size_t HEAD_CAPACITY = Profile::HEAD_CAPACITY;
    // Scratch used to pull bytes from a generator body one chunk at a time.
    static constexpr std::size_t GENERATOR_CHUNK = Profile::GENERATOR_CHUNK;

    // RX_CAPACITY MUST cover the advertised receive window: lwIP has already
    // ACKed delivered bytes, so an overflow would be lost and desynchronise
    // the stream. Only parsed bytes return credit (AR-6).
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

        // The response in flight: serialised head, body source, cursors and
        // framing state (heap-free streaming, AR-5).
        detail::ResponseWriter<HEAD_CAPACITY, GENERATOR_CHUNK> writer{};

        std::uint32_t requests_served = 0;
        bool active = false;
        bool closing = false;  // finish draining the response, then close
        // While deferred, preserve parser storage for request views and stop
        // consuming pipelined requests (AR-9, AR-6).
        bool pending = false;

        // respond() can publish from another core or an ISR. The completion
        // lock brackets every access to the record (AR-9, AR-3).

        // The dispatching or parked request. A sequence mismatch rejects an
        // earlier completion.
        RequestId pending_request{};
        // The filler respond() recorded, invoked on the async-context turn.
        ResponseFiller pending_fill = nullptr;
        void* pending_ctx = nullptr;
        // Published completion not yet taken. Publication can precede the
        // handler returning PENDING.
        bool completion_ready = false;

        // Reset cursors and release copied storage without a large stack
        // temporary. Called on connect, disconnect and stop (AR-1).
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

    /// Builds and queues recorded completions on the transport context
    /// (AR-9).
    void on_poll() override { poll_completions(); }

    void poll_completions();
    // Drop whatever completion handle a slot carries, so nothing can resolve a
    // request that is already answered or rejected with an error status.
    void clear_completion_record(Conn& conn);
    // Build and send one recorded completion. Runs on the transport's context.
    void complete_deferred(Conn& conn, RequestId id, ResponseFiller fill,
                           void* ctx);

    Conn* conn_for(ConnectionId id);
    void process(Conn& conn, ConnectionId id);
    // Request views are rebuilt over parser storage retained during a
    // deferral.
    [[nodiscard]] static HTTPRequestView request_view(const Conn& conn,
                                                      RequestId id);
    // Route and run the handler, returning what it did with the response.
    [[nodiscard]] Disposition invoke_handler(const HTTPRequestView& request,
                                             HTTPResponse& response);
    void dispatch_request(Conn& conn, ConnectionId id);
    // Share framing and pool release between inline and deferred responses
    // (NR-2, NR-6).
    void complete_request(Conn& conn, RequestId id, HTTPResponse& response);
    // Answer an excess deferral without incrementing parse_errors (BR-12).
    static void serialize_service_unavailable(Conn& conn);
    void begin_response(Conn& conn, const HTTPRequestView& request,
                        HTTPResponse& response, bool close_connection);
    // Answer with the fixed 500 and close, freeing the handler's copied body:
    // the response that cannot be sent as built (BR-13, BR-14).
    void begin_internal_error(Conn& conn, HTTPResponse& response);
    // Serialise the head. Return its byte count, or nullopt if it exceeds
    // HEAD_CAPACITY.
    [[nodiscard]] std::optional<std::size_t> serialize_head(
        std::span<std::uint8_t> head, const Conn& conn,
        std::string_view version, const HTTPResponse& response,
        detail::Framing framing, bool close_connection) const;
    // Write a boundary response head and static reason-phrase body into the
    // connection's writer and close, building no HTTPResponse (AR-13).
    static void begin_boundary_response(Conn& conn, std::uint16_t code,
                                        std::string_view reason);
    void serialize_error(Conn& conn, ParseResult error);
    void pump(Conn& conn, ConnectionId id);
    [[nodiscard]] bool should_keep_alive(const HTTPRequestView& request,
                                         const HTTPResponse& response,
                                         const Conn& conn) const;
    void apply_keep_alive_config();

    // Arm only on a phase transition so header drips cannot reset the fixed
    // window (TR-1).
    void set_phase(Conn& conn, ConnectionId id, Phase phase);
    // Choose the body, new-head or idle deadline after parsing and sending.
    void finalize_phase(Conn& conn, ConnectionId id);
    // Queue the interim 100 before the final head, including across partial
    // sends (NR-5).
    void maybe_send_continue(Conn& conn, ConnectionId id);

    std::unique_ptr<ITcpTransport> transport_;
    // Response entries are held only during dispatch or completion, with
    // capacity beyond one live turn (AR-9, BR-12).
    detail::PendingPool<MAX_PENDING> pending_{};
    // Parked requests, counted independently of response entries held during
    // a call (BR-12).
    std::size_t deferred_count_ = 0;
    RouteTable routes_{};
    RequestHandler default_handler_;
    RequestHandler not_found_handler_;
    KeepAliveConfig keep_alive_config_{};
    TimeoutConfig timeout_config_{};
    Statistics stats_{};
    std::array<Conn, MAX_CONNECTIONS> conns_{};
};

// Conn must be complete for sizeof and alignof. The default argument still
// evaluates in the caller translation unit.
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
