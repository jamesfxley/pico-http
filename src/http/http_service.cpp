// SPDX-License-Identifier: MIT
#include "pico_http/http/http_service.hpp"

#include "pico_http/pico_http.hpp"

// The umbrella version is checked in a compiled unit, including firmware
// builds.
static_assert(pico_http::VERSION_MAJOR == PICO_HTTP_CMAKE_VERSION_MAJOR);
static_assert(pico_http::VERSION_MINOR == PICO_HTTP_CMAKE_VERSION_MINOR);
static_assert(pico_http::VERSION_PATCH == PICO_HTTP_CMAKE_VERSION_PATCH);

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "pico_http/http/detail/default_handlers.hpp"
#include "pico_http/http/detail/keep_alive.hpp"
#include "pico_http/http/detail/parsing.hpp"
#include "pico_http/http/detail/response_writer.hpp"
#include "pico_http/http/headers.hpp"
#include "pico_http/http/http_parser.hpp"
#include "pico_http/http/message.hpp"
#include "pico_http/http/route_table.hpp"
#include "pico_http/tcp/tcp_transport.hpp"
#include "pico_util/bytes/as_bytes.hpp"

/// \file http_service.cpp
/// Connection state, incremental parsing and response streaming.

namespace pico_http {
namespace {

constexpr std::uint32_t MS_PER_SECOND = 1000U;

constexpr std::string_view HTTP_VERSION_11 = "HTTP/1.1";

// Overflow is recorded so a response head cannot be sent truncated.
class HeadWriter {
   public:
    explicit HeadWriter(std::span<std::uint8_t> buffer) : buffer_(buffer) {}

    void put(std::string_view text) {
        const std::size_t room = buffer_.size() - len_;
        const std::size_t take = std::min(text.size(), room);
        std::copy_n(
            text.begin(), take,
            std::next(buffer_.begin(), static_cast<std::ptrdiff_t>(len_)));
        len_ += take;
        if (take < text.size()) {
            overflow_ = true;
        }
    }

    void put_char(char ch) { put(std::string_view(&ch, 1)); }

    void put_uint(std::uint64_t value) {
        std::array<char, UINT64_MAX_DIGITS> tmp{};
        std::size_t i = tmp.size();
        // The body must run once for zero to emit its digit.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-do-while)
        do {
            tmp[--i] = static_cast<char>('0' + (value % DECIMAL_BASE));
            value /= DECIMAL_BASE;
        } while (value != 0U);
        put(std::string_view(
            std::next(tmp.begin(), static_cast<std::ptrdiff_t>(i)), tmp.end()));
    }

    [[nodiscard]] std::size_t length() const { return len_; }

    [[nodiscard]] bool overflow() const { return overflow_; }

   private:
    // 2^64 - 1 is 20 decimal digits, the widest value put_uint can render.
    static constexpr std::size_t UINT64_MAX_DIGITS = 20;
    static constexpr std::uint64_t DECIMAL_BASE = 10U;

    std::span<std::uint8_t> buffer_;
    std::size_t len_ = 0;
    bool overflow_ = false;
};

struct ErrorInfo {
    std::uint16_t code;
    std::string_view text;
    // Which parse-error counter to bump, so serialize_error does not re-switch
    // over the same status classes error_info already discriminates.
    std::uint32_t HttpService::Statistics::* counter;
};

// The reason phrases this server emits, named so the assertion below covers
// exactly the set rather than whichever one someone remembered to check.
constexpr std::string_view REASON_400 = "Bad Request";
constexpr std::string_view REASON_405 = "Method Not Allowed";
constexpr std::string_view REASON_413 = "Payload Too Large";
constexpr std::string_view REASON_414 = "URI Too Long";
constexpr std::string_view REASON_417 = "Expectation Failed";
constexpr std::string_view REASON_431 = "Request Header Fields Too Large";
constexpr std::string_view REASON_501 = "Not Implemented";
constexpr std::string_view REASON_503 = "Service Unavailable";
constexpr std::string_view REASON_505 = "HTTP Version Not Supported";

// Service-unavailable response code (BR-12).
constexpr std::uint16_t STATUS_SERVICE_UNAVAILABLE = 503U;

constexpr std::size_t MAX_REASON_SIZE =
    std::ranges::max({REASON_400.size(), REASON_405.size(), REASON_413.size(),
                      REASON_414.size(), REASON_417.size(), REASON_431.size(),
                      REASON_501.size(), REASON_503.size(), REASON_505.size()});

// Reason phrases have to fit status_text because assign() cannot report
// failure to the client.
static_assert(MAX_REASON_SIZE <= HTTPResponse::STATUS_TEXT_CAPACITY,
              "every reason phrase must fit HTTPResponse::status_text");

// Upper bound on distinct methods listed in a 405 Allow header. Covers the
// standard method set; extra registrations past this are simply not listed.
constexpr std::size_t MAX_ALLOW_METHODS = 8;

// The Allow buffer covers every method and separator, and fits header
// storage.
constexpr std::size_t ALLOW_BUFFER =
    MAX_ALLOW_METHODS * (RouteTable::METHOD_CAPACITY + 2);
static_assert(ALLOW_BUFFER <= HTTPHeaders::MAX_HEADER_VALUE);

// Build a fixed-buffer 405 response with Allow (NR-8; RFC 9110 §15.5.6,
// §10.2.1).
void make_method_not_allowed(HTTPResponse& response,
                             std::span<const std::string_view> methods) {
    response.status_code = HTTPResponse::STATUS_METHOD_NOT_ALLOWED;
    // The static_assert at the top of this file bounds every reason phrase
    // by STATUS_TEXT_CAPACITY, so no assign of one can truncate.
    static_cast<void>(response.status_text.assign(REASON_405));

    std::array<char, ALLOW_BUFFER> allow{};
    std::size_t len = 0;
    auto append = [&](std::string_view text) {
        const std::size_t room = allow.size() - len;
        const std::size_t take = std::min(text.size(), room);
        std::copy_n(text.begin(), take, allow.begin() + len);
        len += take;
    };
    for (std::size_t i = 0; i < methods.size(); ++i) {
        if (i != 0) {
            append(", ");
        }
        append(methods[i]);
    }
    response.headers.set("Allow", std::string_view(allow.data(), len));
    response.set_body_static(StaticBody::outliving_response(REASON_405),
                             "text/plain");
}

// Framing comes from the body mode. Handler framing headers are discarded.
// Handler persistence headers are discarded when the service closes
// (NR-2, NR-6).
void put_handler_headers(HeadWriter& head, const HTTPResponse& response,
                         bool close_connection) {
    for (const auto& header : response.headers) {
        const std::string_view name = header.name.view();
        if (detail::iequals(name, "Content-Length") ||
            detail::iequals(name, "Transfer-Encoding")) {
            continue;
        }
        if (close_connection && (detail::iequals(name, "Connection") ||
                                 detail::iequals(name, "Keep-Alive"))) {
            continue;
        }
        head.put(name);
        head.put(": ");
        head.put(header.value.view());
        head.put("\r\n");
    }
}

ErrorInfo error_info(ParseResult error) {
    using S = HttpService::Statistics;
    using R = HTTPResponse;
    switch (error) {
        case ParseResult::PAYLOAD_TOO_LARGE:
            return {.code = R::STATUS_PAYLOAD_TOO_LARGE,
                    .text = REASON_413,
                    .counter = &S::parse_errors_413};
        case ParseResult::URI_TOO_LONG:
            return {.code = R::STATUS_URI_TOO_LONG,
                    .text = REASON_414,
                    .counter = &S::parse_errors_414};
        case ParseResult::EXPECTATION_FAILED:
            return {.code = R::STATUS_EXPECTATION_FAILED,
                    .text = REASON_417,
                    .counter = &S::parse_errors_417};
        case ParseResult::HEADER_FIELDS_TOO_LARGE:
            return {.code = R::STATUS_HEADER_FIELDS_TOO_LARGE,
                    .text = REASON_431,
                    .counter = &S::parse_errors_431};
        case ParseResult::NOT_IMPLEMENTED:
            return {.code = R::STATUS_NOT_IMPLEMENTED,
                    .text = REASON_501,
                    .counter = &S::parse_errors_501};
        case ParseResult::HTTP_VERSION_NOT_SUPPORTED:
            return {.code = R::STATUS_VERSION_NOT_SUPPORTED,
                    .text = REASON_505,
                    .counter = &S::parse_errors_505};
        default:
            return {.code = R::STATUS_BAD_REQUEST,
                    .text = REASON_400,
                    .counter = &S::parse_errors_400};
    }
}

}  // namespace

HttpService::HttpService(std::unique_ptr<ITcpTransport> transport,
                         ProfileBinding binding)
    : transport_(std::move(transport)) {
    if (!transport_) {
        std::abort();
    }
    // The caller and library bindings have to agree before any
    // layout-dependent access.
    if (binding != caller_binding()) {
        std::abort();
    }
    default_handler_ = detail::default_welcome_handler;
    not_found_handler_ = detail::default_not_found_handler;

    transport_->set_event_sink(this);
    apply_keep_alive_config();
}

HttpService::~HttpService() { stop(); }

bool HttpService::start(std::uint16_t port) {
    return transport_ != nullptr && transport_->listen(port);
}

void HttpService::stop() {
    transport_->close_server();
    for (Conn& conn : conns_) {
        // A respond() can race from another core.
        // It must not observe a half-cleared slot.
        const detail::CompletionLock lock(*transport_);
        conn.reset();
    }
    // The deferral count must return to zero. close_server() may omit
    // individual disconnect notifications (BR-12).
    deferred_count_ = 0;
}

bool HttpService::add_route(std::string_view method,
                            std::string_view uri_pattern,
                            RequestHandler handler) {
    return routes_.add_route(method, uri_pattern, std::move(handler));
}

void HttpService::set_default_handler(RequestHandler handler) {
    default_handler_ = std::move(handler);
}

void HttpService::set_404_handler(RequestHandler handler) {
    not_found_handler_ = std::move(handler);
}

void HttpService::set_keep_alive_config(const KeepAliveConfig& config) {
    keep_alive_config_ = config;
    if (keep_alive_config_.enabled &&
        keep_alive_config_.idle_timeout_seconds == 0U) {
        keep_alive_config_.idle_timeout_seconds =
            KeepAliveConfig::DEFAULT_IDLE_TIMEOUT_SECONDS;
    }
    apply_keep_alive_config();
}

std::size_t HttpService::active_connections() const noexcept {
    std::size_t count = 0;
    for (const Conn& conn : conns_) {
        count += conn.active ? 1U : 0U;
    }
    return count;
}

void HttpService::apply_keep_alive_config() {
    if (transport_ == nullptr) {
        return;
    }
    transport_->set_idle_timeout(keep_alive_config_.enabled
                                     ? keep_alive_config_.idle_timeout_seconds
                                     : 0U);
}

void HttpService::set_phase(Conn& conn, ConnectionId id, Phase phase) {
    conn.phase = phase;
    switch (phase) {
        case Phase::RECV_HEAD:
            transport_->arm_timeout(id, TimeoutKind::HEADER,
                                    timeout_config_.header_ms);
            break;
        case Phase::RECV_BODY:
            transport_->arm_timeout(id, TimeoutKind::BODY,
                                    timeout_config_.body_ms);
            break;
        case Phase::PENDING:
            transport_->arm_timeout(id, TimeoutKind::HANDLER,
                                    timeout_config_.handler_ms);
            break;
        case Phase::SENDING:
            transport_->arm_timeout(id, TimeoutKind::WRITE,
                                    timeout_config_.write_ms);
            break;
        case Phase::IDLE:
            if (keep_alive_config_.enabled &&
                keep_alive_config_.idle_timeout_seconds > 0U) {
                transport_->arm_timeout(
                    id, TimeoutKind::IDLE,
                    keep_alive_config_.idle_timeout_seconds * MS_PER_SECOND);
            } else {
                transport_->clear_timeout(id);
            }
            break;
    }
}

void HttpService::finalize_phase(Conn& conn, ConnectionId id) {
    if (conn.pending) {
        // Receive progress leaves the TR-6 deadline unchanged, so a peer that
        // keeps sending cannot hold a stalled handler open.
        return;
    }
    if (conn.closing || conn.writer.active() || !conn.active) {
        // Sending retains its write deadline; closing has drain linger (TR-5).
        return;
    }
    if (conn.parser.reading_body()) {
        // Reading a body: keep the body-progress deadline (TR-2) armed. Safe to
        // re-arm — feed() only reaches here having made progress.
        set_phase(conn, id, Phase::RECV_BODY);
    } else if (conn.parser.in_progress() || conn.rx_len > 0) {
        // The header window is armed only on entry to the head phase so a
        // slowloris drip cannot reset the window (TR-1).
        if (conn.phase != Phase::RECV_HEAD) {
            set_phase(conn, id, Phase::RECV_HEAD);
        }
    } else {
        set_phase(conn, id, Phase::IDLE);  // keep-alive idle (TR-3)
    }
}

void HttpService::maybe_send_continue(Conn& conn, ConnectionId id) {
    if (!conn.parser.expect_continue()) {
        return;
    }
    // The writer latches the interim response before sending. A partial write
    // can then resume without repeating bytes (NR-5).
    conn.writer.request_continue();
    pump(conn, id);
}

HttpService::Conn* HttpService::conn_for(ConnectionId id) {
    const std::size_t slot = id.slot();
    if (slot >= conns_.size() || !conns_[slot].active) {
        return nullptr;
    }
    // Disconnect notification is deferred. The transport can already have
    // released an active service slot.
    if (transport_ == nullptr || !transport_->is_open(id)) {
        return nullptr;
    }
    return &conns_[slot];
}

void HttpService::on_connect(ConnectionId id) {
    // The transport and conns_ share MAX_CONNECTIONS, so every issued slot
    // index fits.
    const std::size_t slot = id.slot();
    conns_[slot].reset();
    conns_[slot].active = true;
    ++stats_.accepts;

    stats_.slot_high_watermark =
        std::max(stats_.slot_high_watermark,
                 static_cast<std::uint32_t>(active_connections()));

    // Start the header-completion deadline (TR-1) — a fixed window from accept,
    // so slowloris drip cannot keep resetting it.
    set_phase(conns_[slot], id, Phase::RECV_HEAD);
}

void HttpService::on_disconnect(ConnectionId id, DisconnectReason reason) {
    // The service releases the pool entry before resetting the slot,
    // including a teardown during dispatch (AR-9, BR-12).
    pending_.release(id);
    const std::size_t slot = id.slot();
    if (slot < conns_.size()) {
        Conn& conn = conns_[slot];
        // Return the deferral credit on every teardown. reset() invalidates
        // the completion record (BR-12, TR-6, AR-4).
        if (conn.pending && deferred_count_ > 0) {
            --deferred_count_;
        }
        const detail::CompletionLock lock(*transport_);
        conn.reset();
    }
    switch (reason) {
        case DisconnectReason::PEER_CLOSED:
            ++stats_.disconnect_peer_closed;
            break;
        case DisconnectReason::PEER_RESET:
            ++stats_.disconnect_peer_reset;
            break;
        case DisconnectReason::ABORTED:
            ++stats_.disconnect_aborted;
            break;
        case DisconnectReason::TIMEOUT_HEADER:
            ++stats_.disconnect_timeout_header;
            ++stats_.disconnect_timeout;
            break;
        case DisconnectReason::TIMEOUT_BODY:
            ++stats_.disconnect_timeout_body;
            ++stats_.disconnect_timeout;
            break;
        case DisconnectReason::TIMEOUT_IDLE:
            ++stats_.disconnect_timeout_idle;
            ++stats_.disconnect_timeout;
            break;
        case DisconnectReason::TIMEOUT_WRITE:
            ++stats_.disconnect_timeout_write;
            ++stats_.disconnect_timeout;
            break;
        case DisconnectReason::TIMEOUT_DRAIN:
            ++stats_.disconnect_timeout_drain;
            ++stats_.disconnect_timeout;
            break;
        case DisconnectReason::TIMEOUT_HANDLER:
            ++stats_.disconnect_timeout_handler;
            ++stats_.disconnect_timeout;
            break;
        case DisconnectReason::SERVER_CLOSED:
            ++stats_.disconnect_server_closed;
            break;
    }
}

void HttpService::on_receive(ConnectionId id,
                             std::span<const std::uint8_t> data) {
    Conn* conn = conn_for(id);
    if (conn == nullptr) {
        return;
    }
    if (conn->closing) {
        transport_->consume(id, data.size());  // draining; ignore new requests
        return;
    }

    // Only parsed bytes reopen the receive window. RX_CAPACITY covers a full
    // window; count any unexpected overflow (AR-6).
    const std::size_t space = RX_CAPACITY - conn->rx_len;
    const std::size_t take = std::min(data.size(), space);
    std::copy_n(data.begin(), take,
                conn->rx.begin() + static_cast<std::ptrdiff_t>(conn->rx_len));
    conn->rx_len += take;
    stats_.bytes_in += take;
    if (take < data.size()) {
        ++stats_.rx_overflow_drops;
    }

    process(*conn, id);

    // Draining responses, and deferred handlers for up to TR-6, can hold the
    // receive window closed (RR-6, OR-1).
    Conn* still = conn_for(id);
    if (still != nullptr && still->rx_len > 0 &&
        (still->writer.active() || still->pending)) {
        ++stats_.rx_window_throttle_events;
    }
}

void HttpService::on_writable(ConnectionId id, std::size_t /*sendable*/) {
    Conn* conn = conn_for(id);
    if (conn == nullptr) {
        return;
    }
    pump(*conn, id);  // keep the in-flight response draining
    Conn* still = conn_for(id);
    if (still == nullptr) {
        return;  // the response drained and closed the connection
    }
    if (still->writer.active()) {
        // ACK progress on a still-pending response: reset the write-stall
        // deadline (TR-4).
        set_phase(*still, id, Phase::SENDING);
    } else if (!still->closing) {
        // Response fully drained; resume pipelined requests and finalise phase.
        process(*still, id);
    }
}

// conn stays valid for this call: teardown invalidates id at once, but
// CallbackScope defers on_disconnect() and its conn.reset() until the
// outermost transport callback unwinds.
void HttpService::process(Conn& conn, ConnectionId id) {
    std::size_t offset = 0;
    // A busy slot keeps pipelined bytes unacknowledged and preserves deferred
    // request views (AR-6, AR-9).
    while (offset < conn.rx_len && !conn.closing && !conn.writer.active() &&
           !conn.pending) {
        std::size_t consumed = 0;
        const std::span<const std::uint8_t> in(
            conn.rx.data() + static_cast<std::ptrdiff_t>(offset),
            conn.rx_len - offset);
        const ParseResult status = conn.parser.feed(in, consumed);
        offset += consumed;
        if (status == ParseResult::INCOMPLETE) {
            // Body progress re-arms TR-2. Header progress leaves the fixed
            // TR-1 window unchanged.
            if (conn.parser.reading_body()) {
                maybe_send_continue(conn, id);
                set_phase(conn, id, Phase::RECV_BODY);
            }
            break;
        }
        if (is_error(status)) {
            serialize_error(conn, status);
            break;
        }
        dispatch_request(conn, id);
        if (conn.pending) {
            // Preserve deferred request storage and the TR-6 deadline until
            // respond() completes the request (AR-9).
            break;
        }
        conn.parser.reset();
        set_phase(conn, id, Phase::SENDING);  // response now in flight
        pump(conn, id);  // start sending; stays active if it stalls
        if (!conn.writer.active() && !conn.closing) {
            // Response fully sent; open a header window for the next request in
            // case the pipeline continues (finalised to IDLE below if not).
            set_phase(conn, id, Phase::RECV_HEAD);
        }
    }

    // Return receive credit only for consumed bytes. A stale id after pump()
    // is inert (AR-6).
    if (offset > 0) {
        transport_->consume(id, offset);
    }

    // offset counts only bytes taken from rx, so it cannot exceed rx_len.
    if (offset > 0) {
        std::copy(conn.rx.begin() + static_cast<std::ptrdiff_t>(offset),
                  conn.rx.begin() + static_cast<std::ptrdiff_t>(conn.rx_len),
                  conn.rx.begin());
        conn.rx_len -= offset;
    }

    // A response queued at the break above (e.g. an error) needs TR-4 too.
    if (conn.writer.active() && conn.phase != Phase::SENDING) {
        set_phase(conn, id, Phase::SENDING);
    }
    // Drive any response that was queued but not yet pumped.
    pump(conn, id);

    // Replace the sending deadline after a response drains, including when a
    // pipelined request follows.
    finalize_phase(conn, id);
}

HTTPRequestView HttpService::request_view(const Conn& conn, RequestId id) {
    HTTPRequestView request;
    request.method = conn.parser.method();
    request.uri = conn.parser.uri();
    request.version = conn.parser.version();
    request.headers = &conn.parser.headers();
    request.body = conn.parser.body();
    request.request = id;
    return request;
}

Disposition HttpService::invoke_handler(const HTTPRequestView& request,
                                        HTTPResponse& response) {
    const auto* route = routes_.find_route(request.method, request.uri);
    if (route == nullptr && request.method == "HEAD") {
        // GET supplies a fallback handler for HEAD; begin_response suppresses
        // the body (NR-9; RFC 9110 §9.1).
        route = routes_.find_route("GET", request.uri);
    }
    if (route != nullptr) {
        return route->handler(request, response);
    }
    if (request.uri == "/" || request.uri.empty()) {
        return default_handler_(request, response);
    }
    // A known URI with another method receives 405 and Allow (RFC 9110
    // §15.5.6).
    std::array<std::string_view, MAX_ALLOW_METHODS> methods{};
    const std::size_t count = routes_.allowed_methods(request.uri, methods);
    if (count > 0) {
        make_method_not_allowed(
            response, std::span<const std::string_view>(methods).first(count));
        return Disposition::COMPLETE;
    }
    return not_found_handler_(request, response);
}

void HttpService::dispatch_request(Conn& conn, ConnectionId id) {
    ++conn.requests_served;
    const RequestId request_id{.connection = id,
                               .sequence = conn.requests_served};

    // The request is published before calling the handler so an inline
    // respond() finds it; otherwise the request would hang until TR-6.
    {
        const detail::CompletionLock lock(*transport_);
        conn.pending_request = request_id;
        conn.pending_fill = nullptr;
        conn.pending_ctx = nullptr;
        conn.completion_ready = false;
    }

    // The response pool entry lasts only for the handler call, including a
    // deferral (AR-9).
    HTTPResponse* response = pending_.claim(id);
    if (response == nullptr) {
        // A response-object shortage must still produce an answer. It shares
        // the deferral-exhaustion counter (OR-1).
        ++stats_.deferrals_refused;
        conn.pending = false;
        serialize_service_unavailable(conn);
        return;
    }

    const HTTPRequestView request = request_view(conn, request_id);
    const Disposition disposition = invoke_handler(request, *response);

    ResponseFiller inline_fill = nullptr;
    void* inline_ctx = nullptr;
    {
        const detail::CompletionLock lock(*transport_);
        if (conn.completion_ready) {
            inline_fill = conn.pending_fill;
            inline_ctx = conn.pending_ctx;
            conn.completion_ready = false;
        }
        // Invalidate the completion record when the handler answers COMPLETE.
        conn.pending_fill = nullptr;
        conn.pending_ctx = nullptr;
    }

    if (disposition == Disposition::PENDING) {
        if (inline_fill != nullptr) {
            // An inline completion needs no later transport turn.
            response->reset();
            inline_fill(inline_ctx, *response);
            ++stats_.pending_dispatches;
            conn.pending = false;
            complete_request(conn, request_id, *response);
            return;
        }
        if (deferred_count_ >= MAX_PENDING) {
            // The cap applies only to deferrals so synchronous handlers
            // remain available (BR-12).
            ++stats_.deferrals_refused;
            conn.pending = false;
            pending_.release(id);
            clear_completion_record(conn);
            serialize_service_unavailable(conn);
            return;
        }
        // Preserve parser storage for deferred views and hold pipelined bytes
        // unacknowledged (AR-6).
        conn.pending = true;
        ++deferred_count_;
        ++stats_.pending_dispatches;
        if (deferred_count_ > stats_.pending_high_watermark) {
            stats_.pending_high_watermark =
                static_cast<std::uint32_t>(deferred_count_);
        }
        pending_.release(id);
        set_phase(conn, id, Phase::PENDING);  // TR-6
        return;
    }

    conn.pending = false;
    complete_request(conn, request_id, *response);
}

void HttpService::complete_request(Conn& conn, RequestId id,
                                   HTTPResponse& response) {
    // The parser retains the request across a deferral, so framing reads the
    // original request.
    const HTTPRequestView request = request_view(conn, id);
    const bool close_connection = !should_keep_alive(request, response, conn);
    if (close_connection) {
        conn.closing = true;
    }
    begin_response(conn, request, response, close_connection);
    // The service releases the pool entry before sending so backpressure
    // cannot hold it.
    pending_.release(id.connection);
    // Nothing may resolve an answered request again. Clearing the handle
    // makes a duplicate respond() return false.
    clear_completion_record(conn);
    ++stats_.requests_completed;
}

void HttpService::clear_completion_record(Conn& conn) {
    const detail::CompletionLock lock(*transport_);
    conn.pending_request = {};
    conn.pending_fill = nullptr;
    conn.pending_ctx = nullptr;
    conn.completion_ready = false;
}

bool HttpService::respond(RequestId id, ResponseFiller fill, void* ctx) {
    if (!id.valid() || fill == nullptr) {
        return false;
    }
    // Publication holds the completion lock without allocation or lwIP calls.
    // poll_completions() sends on the transport context (AR-9).
    bool recorded = false;
    {
        const detail::CompletionLock lock(*transport_);
        Conn* conn = conn_for(id.connection);
        // Generation and sequence reject stale requests. completion_ready
        // rejects duplicate publication (TR-6, AR-4).
        if (conn != nullptr && conn->pending_request == id &&
            !conn->completion_ready) {
            conn->pending_fill = fill;
            conn->pending_ctx = ctx;
            conn->completion_ready = true;
            recorded = true;
        }
    }
    if (recorded) {
        // Signal outside the critical section to limit the time with
        // interrupts disabled (AR-3).
        transport_->signal_completion();
    }
    return recorded;
}

void HttpService::poll_completions() {
    // A completion can arrive from another core or an ISR. Take its record
    // under the lock; the send must not run with interrupts disabled (AR-3).
    for (Conn& conn : conns_) {
        RequestId id{};
        ResponseFiller fill = nullptr;
        void* ctx = nullptr;
        {
            const detail::CompletionLock lock(*transport_);
            if (!conn.active || !conn.pending || !conn.completion_ready) {
                continue;
            }
            id = conn.pending_request;
            fill = conn.pending_fill;
            ctx = conn.pending_ctx;
            conn.completion_ready = false;
            conn.pending_fill = nullptr;
            conn.pending_ctx = nullptr;
        }
        complete_deferred(conn, id, fill, ctx);
    }
}

void HttpService::complete_deferred(Conn& conn, RequestId id,
                                    ResponseFiller fill, void* ctx) {
    conn.pending = false;
    if (deferred_count_ > 0) {
        --deferred_count_;
    }

    HTTPResponse* response = pending_.claim(id.connection);
    if (response == nullptr) {
        // A response-object shortage receives an error instead of leaving the
        // peer waiting.
        ++stats_.deferrals_refused;
        clear_completion_record(conn);
        serialize_service_unavailable(conn);
        set_phase(conn, id.connection, Phase::SENDING);
        pump(conn, id.connection);
        return;
    }
    // The filler runs on the transport context against a fresh entry for its
    // live request (AR-9).
    fill(ctx, *response);
    complete_request(conn, id, *response);

    // Resume the pipelined bytes and receive-window credit held behind the
    // deferral (AR-6).
    const ConnectionId conn_id = id.connection;
    set_phase(conn, conn_id, Phase::SENDING);
    pump(conn, conn_id);
    Conn* still = conn_for(conn_id);
    if (still == nullptr) {
        return;  // the response drained and closed the connection
    }
    if (!still->writer.active() && !still->closing) {
        still->parser.reset();
        process(*still, conn_id);
    }
}

// Framing and persistence decisions share one head buffer and a single
// serialisation pass.
std::optional<std::size_t> HttpService::serialize_head(
    std::span<std::uint8_t> head_buffer, const Conn& conn,
    std::string_view version, const HTTPResponse& response,
    detail::Framing framing, bool close_connection) const {
    HeadWriter head{head_buffer};

    head.put(version);
    head.put_char(' ');
    head.put_uint(response.status_code);
    head.put_char(' ');
    head.put(response.status_text.view());
    head.put("\r\n");

    // Handler persistence headers apply only while the service keeps the
    // connection open. Framing always comes from the service.
    const bool has_connection =
        response.headers.has("Connection") && !close_connection;
    const bool has_keep_alive =
        response.headers.has("Keep-Alive") && !close_connection;

    put_handler_headers(head, response, close_connection);

    if (framing == detail::Framing::CHUNKED) {
        head.put("Transfer-Encoding: chunked\r\n");
    } else if (framing != detail::Framing::CLOSE_DELIMITED) {
        head.put("Content-Length: ");
        head.put_uint(response.body_size());
        head.put("\r\n");
    }
    if (!has_connection) {
        head.put("Connection: ");
        head.put(close_connection ? "close" : "keep-alive");
        head.put("\r\n");
    }
    if (!close_connection && keep_alive_config_.enabled && !has_keep_alive) {
        bool wrote_prefix = false;
        if (keep_alive_config_.idle_timeout_seconds > 0U) {
            head.put("Keep-Alive: timeout=");
            head.put_uint(keep_alive_config_.idle_timeout_seconds);
            wrote_prefix = true;
        }
        if (keep_alive_config_.max_requests_per_connection >
            conn.requests_served) {
            const std::uint32_t remaining =
                keep_alive_config_.max_requests_per_connection -
                conn.requests_served;
            head.put(wrote_prefix ? ", max=" : "Keep-Alive: max=");
            head.put_uint(remaining);
            wrote_prefix = true;
        }
        if (wrote_prefix) {
            head.put("\r\n");
        }
    }
    head.put("Server: pico\r\n");
    head.put("\r\n");

    if (head.overflow()) {
        return std::nullopt;
    }
    return head.length();
}

void HttpService::begin_internal_error(Conn& conn, HTTPResponse& response) {
    // The fixed 500 head reports response-capacity errors and closes the
    // connection (BR-13, BR-14).
    static constexpr std::string_view INTERNAL_ERROR =
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Content-Length: 0\r\nConnection: close\r\n\r\n";
    // HEAD_CAPACITY is configurable, so the unchecked fallback copy needs a
    // size assertion.
    static_assert(HEAD_CAPACITY >= INTERNAL_ERROR.size(),
                  "the profile's HEAD_CAPACITY must hold the fixed 500 "
                  "fallback head, which is emitted when nothing else fits");
    // Free the handler's copied body before the writer takes the response
    // over: adopting it would leave its allocation held by the slot.
    std::vector<std::uint8_t>().swap(response.body);
    std::copy_n(pico_util::as_bytes(INTERNAL_ERROR).begin(),
                INTERNAL_ERROR.size(), conn.writer.head_buffer().begin());
    conn.writer.begin(INTERNAL_ERROR.size(), response, detail::Framing::SIZED);
    conn.writer.drop_body();
    conn.closing = true;
    ++stats_.responses_refused;
}

void HttpService::begin_response(Conn& conn, const HTTPRequestView& request,
                                 HTTPResponse& response,
                                 bool close_connection) {
    const std::string_view version =
        request.version.empty() ? HTTP_VERSION_11 : request.version;

    // Unknown-length output is chunked for HTTP/1.1 and close-delimited for
    // HTTP/1.0 (NR-6).
    detail::Framing framing = detail::Framing::SIZED;
    if (response.is_chunked_body()) {
        framing = version == HTTP_VERSION_11 ? detail::Framing::CHUNKED
                                             : detail::Framing::CLOSE_DELIMITED;
    }
    if (framing == detail::Framing::CLOSE_DELIMITED) {
        close_connection = true;
        conn.closing = true;
    }

    // An oversized copied body is rejected before the slot adopts it (AR-1,
    // BR-14).
    if (response.body_refused ||
        (response.body_mode == HTTPResponse::BodyMode::BUFFERED &&
         response.body.size() > Limits::MAX_BUFFERED_BODY)) {
        begin_internal_error(conn, response);
        return;
    }
    // A body can reserve capacity beyond its size. Copy it exactly to keep
    // slot allocation bounded (AR-1).
    if (response.body.capacity() > Limits::MAX_BUFFERED_BODY) {
        std::vector<std::uint8_t>(response.body.begin(), response.body.end())
            .swap(response.body);
    }

    std::optional<std::size_t> head_len =
        serialize_head(conn.writer.head_buffer(), conn, version, response,
                       framing, close_connection);

    if (!head_len.has_value()) {
        // The handler's head did not fit HEAD_CAPACITY, so it cannot be framed
        // (BR-13).
        begin_internal_error(conn, response);
        return;
    }

    // Adopt the body without another copy. The slot releases copied storage
    // after sending (AR-2, AR-1).
    conn.writer.begin(*head_len, response, framing);

    if (request.method == "HEAD") {
        // HEAD sends neither body nor last-chunk (NR-9; RFC 9110 §9.3.2).
        conn.writer.drop_body();
    }
}

void HttpService::begin_boundary_response(Conn& conn, std::uint16_t code,
                                          std::string_view reason) {
    // The longest head is the widest reason phrase and a two-digit body length
    // in the fixed layout below.
    constexpr std::size_t TWO_DIGIT_LIMIT = 100U;
    static_assert(MAX_REASON_SIZE < TWO_DIGIT_LIMIT);
    static_assert(
        HEAD_CAPACITY >=
            std::string_view("HTTP/1.1 000 \r\nContent-Type: text/plain\r\n"
                             "Content-Length: 00\r\nConnection: close\r\n"
                             "Server: pico\r\n\r\n")
                    .size() +
                MAX_REASON_SIZE,
        "the profile's HEAD_CAPACITY must hold the longest boundary head");

    // The head goes straight into the writer's buffer and the reason phrase is
    // the body, so no HTTPResponse is built on the callback stack (AR-13).
    HeadWriter head{conn.writer.head_buffer()};
    head.put(HTTP_VERSION_11);
    head.put_char(' ');
    head.put_uint(code);
    head.put_char(' ');
    head.put(reason);
    head.put("\r\nContent-Type: text/plain\r\nContent-Length: ");
    head.put_uint(reason.size());
    head.put("\r\nConnection: close\r\nServer: pico\r\n\r\n");
    conn.closing = true;
    // The head fits by the static_assert above. Every reason phrase is a
    // string literal.
    conn.writer.begin(head.length(), pico_util::as_bytes(reason));
}

void HttpService::serialize_error(Conn& conn, ParseResult error) {
    const ErrorInfo info = error_info(error);
    begin_boundary_response(conn, info.code, info.text);

    ++(stats_.*info.counter);
    ++stats_.parse_errors;
}

void HttpService::serialize_service_unavailable(Conn& conn) {
    // The rejection must not need an entry in the exhausted pool. No handler
    // receives this response (BR-12).
    begin_boundary_response(conn, STATUS_SERVICE_UNAVAILABLE, REASON_503);
    // No parse_errors bump: the request was well formed and the server is out
    // of capacity, which is a different fact and gets its own counter (OR-1).
}

void HttpService::pump(Conn& conn, ConnectionId id) {
    if (!conn.writer.active()) {
        if (conn.closing) {
            transport_->close_client(id);
        }
        return;
    }

    const detail::WriteCounters counters{
        .bytes_out = stats_.bytes_out,
        .backpressure_events = stats_.backpressure_events};
    const detail::PumpResult result =
        conn.writer.pump(*transport_, id, counters);

    if (result == detail::PumpResult::STALLED) {
        return;  // resume on the next writable event
    }
    if (result == detail::PumpResult::DEAD) {
        // The disconnect notification resets this closed connection slot.
        conn.writer.reset();
        return;
    }
    if (result == detail::PumpResult::ABORT) {
        // An early generator end or a partial chunk leaves a detectably
        // truncated body; resumption would corrupt it.
        conn.closing = true;
    }
    if (conn.closing) {
        transport_->close_client(id);
    }
}

bool HttpService::should_keep_alive(const HTTPRequestView& request,
                                    const HTTPResponse& response,
                                    const Conn& conn) const {
    return detail::keep_alive_allowed(
        keep_alive_config_.enabled, request.headers->get("Connection"),
        request.version, response.headers.get("Connection"),
        conn.requests_served, keep_alive_config_.max_requests_per_connection);
}

}  // namespace pico_http
