// SPDX-License-Identifier: MIT
#include "pico_http/http/http_service.hpp"

#include "pico_http/pico_http.hpp"

// The advertised version and project(pico_http VERSION ...) are two records of
// one fact. Bumping either alone stops the build here.
//
// The constants live in the umbrella header, whose target is INTERFACE and has
// no translation unit of its own; this is a compiled one that already links the
// public surface. The CMake values arrive PRIVATE, so they reach no installed
// consumer. A host test asserts the same pair, but that test is skipped on
// every configure with tests off -- which is every firmware and every consumer
// build.
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
/// Runtime for the HTTP server: slot-based connection state, incremental
/// parsing, streaming (backpressured) response writes, and boundary handling.

namespace pico_http {
namespace {

constexpr std::uint32_t MS_PER_SECOND = 1000U;

constexpr std::string_view HTTP_VERSION_11 = "HTTP/1.1";

// Appends a response head into a fixed byte buffer with no heap use. Tracks
// overflow so the caller can reject a response whose head does not fit rather
// than emit a truncated one.
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
        // Emits digits least-significant first, so the body must run once even
        // for value 0 — which renders as the single digit "0".
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

// BR-12. Not in HTTPResponse's status constants because it is not a status a
// handler sets: the service emits it when it has no capacity to run one.
constexpr std::uint16_t STATUS_SERVICE_UNAVAILABLE = 503U;

// status_text's cap is tight rather than generous: the longest phrase above is
// 31 characters against a 32-character cap. The assign() calls that store these
// cannot report truncation anywhere a client would see it, so a reworded or
// added phrase that does not fit has to fail the build instead (CXX-07.2).
static_assert(std::ranges::max({REASON_400.size(), REASON_405.size(),
                                REASON_413.size(), REASON_414.size(),
                                REASON_417.size(), REASON_431.size(),
                                REASON_501.size(), REASON_503.size(),
                                REASON_505.size()}) <=
                  HTTPResponse::STATUS_TEXT_CAPACITY,
              "every reason phrase must fit HTTPResponse::status_text");

// Upper bound on distinct methods listed in a 405 Allow header. Covers the
// standard method set; extra registrations past this are simply not listed.
constexpr std::size_t MAX_ALLOW_METHODS = 8;

// Worst-case Allow value: MAX_ALLOW_METHODS methods, each up to METHOD_CAPACITY
// chars, joined by ", ". The buffer covers that bound so the list never
// truncates mid-token, and the assert keeps it within the header value storage
// so set() emits it intact.
constexpr std::size_t ALLOW_BUFFER =
    MAX_ALLOW_METHODS * (RouteTable::METHOD_CAPACITY + 2);
static_assert(ALLOW_BUFFER <= HTTPHeaders::MAX_HEADER_VALUE);

// Fill `response` as a 405 Method Not Allowed carrying an Allow header that
// lists `methods` (NR-8; RFC 9110 §15.5.6, §10.2.1). The joined list is built
// in a fixed no-heap buffer.
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

// Emit the handler's own header section, less the four the service owns.
//
// Content-Length and Transfer-Encoding are dropped unconditionally: framing
// belongs to the service, which derives it from the body mode the handler
// actually set. A handler's value is never checked against that body, so
// echoing it lets a wrong length reach the wire ahead of a body of a different
// size, and the client reads the next pipelined response as the remainder --
// the same desync NR-3 rejects on the request side, produced by the server. It
// can also emit Content-Length and Transfer-Encoding in one head.
//
// Once the service has decided to close, the handler's persistence headers go
// the same way: they were set before the framing decision was taken, so
// `Connection: keep-alive` on a response the server is about to close would
// announce persistence it will not honour -- and on a close-delimited body,
// which carries neither Content-Length nor Transfer-Encoding, it would leave
// the client no way to find the end (NR-2, NR-6).
//
// serialize_head writes the authoritative headers afterwards.
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
    // caller_binding() here is the library's own view; `binding` is the
    // caller's, evaluated in the caller's translation unit. They differ only if
    // the two were compiled with different profile macros, in which case they
    // disagree about the layout of every type crossing this boundary. Nothing
    // after this point would be well defined.
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
        // Under the completion lock: a respond() racing this from another core
        // must not observe a slot half-cleared. Unconditional, as in
        // on_disconnect() -- the constructor aborts on a null transport, so
        // there is never one to guard against here.
        const detail::CompletionLock lock(*transport_);
        conn.reset();
    }
    // Every parked request is gone with its slot, so the BR-12 cap must go back
    // to zero: close_server() may tear connections down without an
    // on_disconnect for each, which would otherwise leave the cap permanently
    // drawn down and eventually refuse every deferral.
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
        return;  // TR-6 is armed and owns the deadline until respond() lands;
                 // re-arming here on each receive would let a peer that keeps
                 // sending hold a wedged handler open indefinitely.
    }
    if (conn.closing || conn.writer.active() || !conn.active) {
        return;  // a pending response keeps the write deadline (handled by
                 // caller); closing arms the drain-close linger (TR-5).
    }
    if (conn.parser.reading_body()) {
        // Reading a body: keep the body-progress deadline (TR-2) armed. Safe to
        // re-arm — feed() only reaches here having made progress.
        set_phase(conn, id, Phase::RECV_BODY);
    } else if (conn.parser.in_progress() || conn.rx_len > 0) {
        // A new request's head is arriving (possibly right after a response
        // finished draining). Arm a fresh header window (TR-1), but only on
        // entry to the head phase so slowloris drip cannot keep resetting it.
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
    // The declared body already passed the parser's limit checks (it is in
    // RECV_BODY, not rejected), so honour the expectation with an interim 100
    // (NR-5). The writer latches this before the first byte goes out: a
    // pipelined request can reach head-completion while the previous response
    // still occupies the transmit ring, so the write may be accepted only
    // partially, and the remainder has to resume from the cursor rather than
    // restart at offset 0.
    conn.writer.request_continue();
    pump(conn, id);
}

HttpService::Conn* HttpService::conn_for(ConnectionId id) {
    const std::size_t slot = id.slot();
    if (slot >= conns_.size() || !conns_[slot].active) {
        return nullptr;
    }
    // `active` alone is not liveness: disconnect notification is deferred, so a
    // connection torn down earlier in this same callback scope still has its
    // slot marked active until the scope unwinds. Ask the transport, which
    // released the id at teardown, so an event arriving after that teardown —
    // the next segment of a multi-pbuf delivery, say — is not dispatched
    // against a connection that no longer exists.
    if (transport_ == nullptr || !transport_->is_open(id)) {
        return nullptr;
    }
    return &conns_[slot];
}

void HttpService::on_connect(ConnectionId id) {
    // The transport's SlotTable mints ids only for slots < MAX_CONNECTIONS, and
    // conns_ is sized from the same shared owner (tcp_transport.hpp), so
    // id.slot() always indexes a valid slot — no capacity reject is possible
    // here.
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
    // Unconditional, and before the slot is reset: a connection torn down mid
    // dispatch still holds a pool entry, and nothing else will ever name this
    // id again. Inert when it holds none, so this covers every teardown reason
    // without asking which ones can defer (AR-9, BR-12).
    pending_.release(id);
    const std::size_t slot = id.slot();
    if (slot < conns_.size()) {
        Conn& conn = conns_[slot];
        // Give the deferral back to the BR-12 cap. Every teardown reaches here,
        // so a handler that never resolves costs its place for TR-6 and no
        // longer. Conn::reset() clears the completion record, which is what
        // makes the handler's eventual respond() inert: the handle it would
        // match is gone, on top of the generation tag rejecting the id (AR-4).
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

    // Buffer into the assembly window. Bytes are acknowledged to the transport
    // (consume() -> tcp_recved, which reopens the receive window) only once the
    // parser has taken them, in process(); until then they hold the window
    // closed. Because RX_CAPACITY >= the advertised window, a full window
    // always fits, so `take` == data.size() in steady state and no delivered
    // byte is dropped (AR-6). The short-copy branch is a belt-and-braces guard:
    // if it ever fired the excess would be lost, so surface it.
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

    // If bytes are still buffered because a response is draining under
    // backpressure, the window is being held closed against the peer — record
    // the throttle (RR-6 / OR-1).
    // A parked deferral counts as well as a draining response: both leave bytes
    // buffered with the window held closed against the peer, which is what the
    // counter names (OR-1). Omitting the deferral would leave the one new
    // source of sustained throttling in this release invisible, and a deferral
    // can hold the window for the whole TR-6 window (CXX-07.6).
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

// `conn` stays valid for this whole call even if the work below closes the
// connection. The transport releases the slot immediately — so `id` goes stale,
// conn_for() stops resolving it and every transport call naming it becomes a
// no-op — but holds the disconnect notification until the outermost transport
// callback unwinds (TransportBase::CallbackScope). on_disconnect, and the
// conn.reset() it performs, therefore cannot run underneath this frame, which
// is why the reference parameter stays safe to use after a teardown.
void HttpService::process(Conn& conn, ConnectionId id) {
    std::size_t offset = 0;
    // !conn.pending stops the loop for the same reason !conn.writer.active()
    // does: the slot is busy with one request, so the bytes behind it stay in
    // the assembly buffer, unacknowledged, holding the receive window closed
    // (AR-6). It also protects the parser storage the deferred handler's
    // request views point into (AR-9).
    while (offset < conn.rx_len && !conn.closing && !conn.writer.active() &&
           !conn.pending) {
        std::size_t consumed = 0;
        const std::span<const std::uint8_t> in(
            conn.rx.data() + static_cast<std::ptrdiff_t>(offset),
            conn.rx_len - offset);
        const ParseResult status = conn.parser.feed(in, consumed);
        offset += consumed;
        if (status == ParseResult::INCOMPLETE) {
            // Still receiving. Once the head is done, switch to the body
            // deadline (TR-2), re-armed on each progressing feed; while still
            // in the head, leave the fixed TR-1 window untouched so slowloris
            // drip cannot keep resetting it.
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
            // The handler deferred (AR-9). Leave the parser holding the request
            // its views point into, leave TR-6 armed by dispatch_request, and
            // stop consuming — respond() resumes from here.
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

    // Acknowledge the bytes the parser consumed so the transport reopens that
    // much receive window (delayed tcp_recved, AR-6). Bytes still buffered (a
    // partial request, or pipelined requests held behind a backpressured
    // response) stay unacknowledged, holding the window closed. If pump() tore
    // the connection down, the transport already released the slot, so this
    // call names a stale id and is a safe no-op.
    if (offset > 0) {
        transport_->consume(id, offset);
    }

    // Drop the consumed bytes from the front of the assembly buffer. offset
    // only ever accumulates bytes the parser took out of rx, so it cannot
    // exceed rx_len.
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

    // Arm the deadline for whatever comes next (idle, next request head/body).
    // This is also the path that recovers from a stale SENDING deadline after a
    // backpressured response finished draining with a pipelined request behind
    // it (via on_writable -> process).
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
        // NR-9: HEAD is supported wherever GET is (RFC 9110 §9.1). Run the GET
        // handler for its status and headers; begin_response suppresses the
        // body for every HEAD response.
        route = routes_.find_route("GET", request.uri);
    }
    if (route != nullptr) {
        return route->handler(request, response);
    }
    if (request.uri == "/" || request.uri.empty()) {
        return default_handler_(request, response);
    }
    // No handler for this method+URI. If the URI is registered under other
    // methods, the correct answer is 405 with an Allow header, not 404
    // (RFC 9110 §15.5.6).
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

    // Published before the handler runs, so a handler that resolves inline —
    // its async work was already done, or its callback ran synchronously — has
    // a live record to write to. Without this respond() would see an unparked
    // connection, record nothing, and the request would hang until TR-6.
    {
        const detail::CompletionLock lock(*transport_);
        conn.pending_request = request_id;
        conn.pending_fill = nullptr;
        conn.pending_ctx = nullptr;
        conn.completion_ready = false;
    }

    // The handler's response lives only for this call (AR-9). A deferring
    // handler does not keep it — it supplies a filler later — so the entry goes
    // back to the pool on every path out of here, and the pool is sized by
    // dispatch turns rather than by how many requests are deferred.
    HTTPResponse* response = pending_.claim(id);
    if (response == nullptr) {
        // Unreachable while the pool has margin over live turns, but a service
        // that cannot produce a response object must still answer rather than
        // leave the peer waiting. Counted with the deferral refusals: both mean
        // "no response object was available", and inventing a second counter
        // for an unreachable branch would not earn its place in OR-1.
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
        // A handler that answers COMPLETE owns the response it just filled;
        // drop any record so a stray respond() cannot resolve this request
        // twice.
        conn.pending_fill = nullptr;
        conn.pending_ctx = nullptr;
    }

    if (disposition == Disposition::PENDING) {
        if (inline_fill != nullptr) {
            // Resolved from inside the handler. Complete now rather than
            // parking: the answer is already available, and parking would cost
            // a doorbell and a turn to reach the same place.
            response->reset();
            inline_fill(inline_ctx, *response);
            ++stats_.pending_dispatches;
            conn.pending = false;
            complete_request(conn, request_id, *response);
            return;
        }
        if (deferred_count_ >= MAX_PENDING) {
            // BR-12. Checked here, not on arrival: a handler that answers
            // inline is not subject to the cap, so a saturated deferral cap
            // cannot refuse ordinary synchronous traffic on other connections.
            ++stats_.deferrals_refused;
            conn.pending = false;
            pending_.release(id);
            clear_completion_record(conn);
            serialize_service_unavailable(conn);
            return;
        }
        // Park the connection. The parser is deliberately NOT reset — the views
        // the handler is holding point into it — and process() stops consuming
        // pipelined requests, so buffered bytes hold the receive window closed
        // (AR-6) instead of queueing work behind a handler that has not
        // answered.
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
    // The request is still in the parser (a deferred slot is never reset before
    // this point), so the framing decisions read the same request the handler
    // saw whether it answered inline or ten seconds later.
    const HTTPRequestView request = request_view(conn, id);
    const bool close_connection = !should_keep_alive(request, response, conn);
    if (close_connection) {
        conn.closing = true;
    }
    begin_response(conn, request, response, close_connection);
    // begin_response has serialized the head and taken the body source, so the
    // pool entry is free from here. Released before the send so a response that
    // stalls under backpressure does not hold an entry another turn needs.
    pending_.release(id.connection);
    // This request is answered, so nothing may resolve it again. Clearing the
    // handle is what makes a second respond() report false instead of recording
    // a completion against a request that has already been sent.
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
    // Everything below is a lookup and three stores under the transport's
    // completion lock. No allocation, no lwIP call, no send — which is what
    // makes this callable from core 1 or an ISR (AR-9). The send happens in
    // poll_completions(), on the transport's own context.
    bool recorded = false;
    {
        const detail::CompletionLock lock(*transport_);
        Conn* conn = conn_for(id.connection);
        // Inert when the request is not the one this connection is on: the
        // connection timed out (TR-6), was reset, was reclaimed by stop() — the
        // generation tag catches those (AR-4) — or has already moved to a later
        // request, which the sequence catches. A second respond() for the same
        // request finds completion_ready already set, or the handle cleared by
        // the completion that already went out.
        if (conn != nullptr && conn->pending_request == id &&
            !conn->completion_ready) {
            conn->pending_fill = fill;
            conn->pending_ctx = ctx;
            conn->completion_ready = true;
            recorded = true;
        }
    }
    if (recorded) {
        // Ring the doorbell outside the lock: it is safe from any context, but
        // holding a critical section across it would extend the window with
        // interrupts disabled for no reason (AR-3).
        transport_->signal_completion();
    }
    return recorded;
}

void HttpService::poll_completions() {
    // A recorded completion may be published from another core or an ISR, so
    // the record is taken under the lock and acted on outside it — the send
    // below must not run with interrupts disabled (AR-3).
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
        // As in dispatch_request: unreachable while the pool has margin, and
        // answering beats leaving the peer waiting on a response that will
        // never come.
        ++stats_.deferrals_refused;
        clear_completion_record(conn);
        serialize_service_unavailable(conn);
        set_phase(conn, id.connection, Phase::SENDING);
        pump(conn, id.connection);
        return;
    }
    // The filler runs here and only here: against a fresh entry, for the
    // request it was recorded against, on the transport's context. A handler
    // that kept no reference has no way to write anywhere else (AR-9).
    fill(ctx, *response);
    complete_request(conn, id, *response);

    // Drive the response out and pick up whatever the peer sent while the
    // handler was thinking — a pipelined request buffered behind the deferral,
    // and the receive window that has been held closed since (AR-6).
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

// Serializes one response head in a single linear pass. The framing decisions
// (keep-alive, Content-Length vs chunked, HTTP/1.0 downgrade) are
// interdependent and each is written into the same HeadWriter as it is taken,
// so splitting the pass further would mean handing the half-built head and the
// accumulated decisions back and forth.
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

    // A handler's persistence headers count only while the service has not
    // decided to close; put_handler_headers drops them when it has, and the
    // authoritative pair is written below. Framing headers have no equivalent
    // read: put_handler_headers drops those unconditionally, so what follows is
    // the only Content-Length or Transfer-Encoding in the head.
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
    // Rather than close silently (which a client sees as an opaque network
    // error), emit a minimal 500 -- its fixed head trivially fits -- and close.
    // This keeps "every boundary condition yields a real status + Connection:
    // close" true (BR-13, BR-14).
    static constexpr std::string_view INTERNAL_ERROR =
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Content-Length: 0\r\nConnection: close\r\n\r\n";
    // HEAD_CAPACITY is a profile knob a downstream build can lower, and this
    // copy is unchecked, so bind "trivially fits" to the build rather than to
    // the comment above.
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

    // Framing for a length-less generator body (NR-6): chunked for HTTP/1.1,
    // close-delimited for HTTP/1.0 (which cannot parse Transfer-Encoding). The
    // downgrade forces the connection closed so the body's end is the socket's.
    detail::Framing framing = detail::Framing::SIZED;
    if (response.is_chunked_body()) {
        framing = version == HTTP_VERSION_11 ? detail::Framing::CHUNKED
                                             : detail::Framing::CLOSE_DELIMITED;
    }
    if (framing == detail::Framing::CLOSE_DELIMITED) {
        close_connection = true;
        conn.closing = true;
    }

    // A copied body is held in the slot until it is sent, so one over the
    // buffered-body cap is refused rather than adopted: that is what bounds
    // live copied-body allocation by MAX_CONNECTIONS x MAX_BUFFERED_BODY
    // (AR-1, BR-14).
    if (response.body_refused ||
        (response.body_mode == HTTPResponse::BodyMode::BUFFERED &&
         response.body.size() > Limits::MAX_BUFFERED_BODY)) {
        begin_internal_error(conn, response);
        return;
    }
    // A body within the cap may still carry capacity reserved past it, by
    // reserve_body or a direct write; the slot would hold that until the
    // response is sent, past MAX_BUFFERED_BODY, so it takes an exact-sized copy
    // instead (AR-1).
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

    // Take the body over from the handler's response (AR-2). A copied body was
    // allocated once by set_body(); the slot frees it once it is sent (AR-1).
    // Static and generator bodies allocate nothing.
    conn.writer.begin(*head_len, response, framing);

    if (request.method == "HEAD") {
        // NR-9: a HEAD response carries the header section the equivalent GET
        // would (including Content-Length / Transfer-Encoding, already
        // serialized above) but no body — and for a chunked framing, no
        // last-chunk (RFC 9110 §9.3.2).
        conn.writer.drop_body();
    }
}

void HttpService::serialize_error(Conn& conn, ParseResult error) {
    const ErrorInfo info = error_info(error);
    HTTPResponse response;
    response.status_code = info.code;
    // info.text is one of the reason phrases the static_assert bounds.
    static_cast<void>(response.status_text.assign(info.text));
    // info.text is a string literal (static storage) — serve it without a copy.
    response.set_body_static(StaticBody::outliving_response(info.text),
                             "text/plain");

    HTTPRequestView dummy;
    dummy.version = HTTP_VERSION_11;
    conn.closing = true;
    begin_response(conn, dummy, response, /*close_connection=*/true);

    ++(stats_.*info.counter);
    ++stats_.parse_errors;
}

void HttpService::serialize_service_unavailable(Conn& conn) {
    // A stack response, not a pooled one — which is the point: the pool being
    // full is the condition being reported, so the rejection must not need an
    // entry. Nothing here is handed to a handler, so it never outlives this
    // frame (BR-12).
    HTTPResponse response;
    response.status_code = STATUS_SERVICE_UNAVAILABLE;
    // As above.
    static_cast<void>(response.status_text.assign(REASON_503));
    response.set_body_static(StaticBody::outliving_response(REASON_503),
                             "text/plain");

    HTTPRequestView dummy;
    dummy.version = HTTP_VERSION_11;
    conn.closing = true;
    begin_response(conn, dummy, response, /*close_connection=*/true);
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
        // The connection is already gone, so there is nothing to send, close,
        // or time out. Drop the response; the transport's disconnect
        // notification resets the slot.
        conn.writer.reset();
        return;
    }
    if (result == detail::PumpResult::ABORT) {
        // The response cannot be continued correctly (a generator quit early,
        // or a transport accepted less than the credit it advertised mid-frame,
        // leaving a partial chunk committed). The client sees a truncated body,
        // which is detectable, rather than a corrupted one.
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
