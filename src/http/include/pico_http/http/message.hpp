// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include "pico_http/config/profile.hpp"
#include "pico_http/http/headers.hpp"
#include "pico_http/tcp/connection_id.hpp"
#include "pico_util/bytes/as_bytes.hpp"
#include "pico_util/containers/fixed_string.hpp"

namespace pico_http {

/// What a handler did with the response it was given (AR-9).
///
/// The names are deliberately not `STATIC`-style bare words: a macro of the
/// same spelling in a firmware configport header would mangle them regardless
/// of the enum's scope (see BodyMode::STATIC_VIEW below for the one that bit).
enum class Disposition : std::uint8_t {
    /// The response is filled; the service may send it as the handler returns.
    COMPLETE,
    /// The handler will resolve the request later via HttpService::respond(),
    /// passing a filler rather than keeping the response it was handed — that
    /// object is gone when the handler returns. The request views stay valid
    /// until the request resolves, and the connection reads no further request
    /// in the meantime, so deferring applies backpressure rather than queueing
    /// work. Bounded by TR-6.
    PENDING,
};

/// Request-scoped handle used to resolve a deferred request (AR-9).
///
/// A ConnectionId alone is not enough. It names a *connection*, and on a
/// keep-alive connection it is identical across every request that connection
/// serves — so a late completion for request N would match request N+1 and send
/// that request's half-built response to the peer. The sequence number makes
/// the handle name one request, so the mismatch is detected instead.
///
/// Holding one is always safe: the connection's generation tag makes it inert
/// once the connection is gone (AR-4), and the sequence makes it inert once
/// that connection has moved on.
struct RequestId {
    /// The connection the request arrived on. Goes inert once that connection
    /// ends, via its generation tag (AR-4).
    tcp::ConnectionId connection;
    /// The connection's request counter at dispatch. Wraps at 2^32 requests on
    /// one connection, which the RR-5 per-connection request cap makes
    /// unreachable.
    std::uint32_t sequence = 0;

    /// Whether this names a connection at all. A default-constructed RequestId
    /// is invalid; validity here does not mean the request is still current.
    [[nodiscard]] bool valid() const noexcept { return connection.valid(); }

    /// Equality over both the connection and the sequence, which is what makes
    /// a late completion for an earlier request on the same connection
    /// distinguishable from the current one.
    [[nodiscard]] friend bool operator==(const RequestId& lhs,
                                         const RequestId& rhs) noexcept {
        return lhs.connection == rhs.connection && lhs.sequence == rhs.sequence;
    }
};

/// Non-owning view representing a parsed HTTP request.
///
/// **Lifetime:** every view (method/uri/version, header values, body) points
/// into per-connection parser storage that is recycled as soon as the handler
/// returns. Views are valid **only for the duration of the handler call** —
/// anything needed later must be copied out. In particular, never stash a
/// view in a response served via set_body_static().
struct HTTPRequestView {
    /// Request method token, e.g. "GET".
    std::string_view method;
    /// Request target as received, undecoded.
    std::string_view uri;
    /// HTTP-version token, e.g. "HTTP/1.1".
    std::string_view version;
    /// The request's parsed headers, owned by the connection's parser. Null
    /// only on the synthetic view used to serialize an error for a request
    /// that never parsed. A pointer, not a value: HTTPHeaders is several KB of
    /// fixed storage, and copying it into every dispatch cost more than the
    /// request handling itself.
    const HTTPHeaders* headers = nullptr;
    /// Buffered request body, empty when there is none. Same handler-scoped
    /// lifetime as the views above.
    std::span<const std::uint8_t> body;
    /// Identifies this request (AR-9). Unlike every other member here this is a
    /// value, not a view, and it stays meaningful after the handler returns —
    /// it is the whole of what a deferring handler keeps, and what it passes to
    /// HttpService::respond(). Holding one is always safe: it goes inert when
    /// the connection ends (AR-4) and when the connection moves to its next
    /// request.
    RequestId request;
};

/// A view over storage the caller declares outlives the response.
///
/// `set_body_static()` takes this rather than a bare span because a bare span
/// is the same type as `HTTPRequestView::body`, whose parser storage is
/// recycled the moment the handler returns — so `set_body_static(request.body)`
/// was in scope, compiled, and transmitted from recycled storage. Three doc
/// comments said not to, which is all that stood between a handler and the bug.
///
/// There is no conversion from a span or a string view: the named factory below
/// is the only way in, so the lifetime claim is written at the call site
/// instead of being made silently by a conversion. It does not make the claim
/// true — a handler can still assert it of the wrong storage — but it cannot be
/// made by accident, and a reader sees it being made.
///
/// A `const char (&)[N]` factory would prove the literal case rather than
/// asserting it, since a request view cannot bind to one. It is absent
/// deliberately: `modernize-avoid-c-arrays` and
/// `cppcoreguidelines-pro-bounds-array-to-pointer-decay` both reject that
/// signature, and a suppression to keep it would buy less than it costs
/// (`EN-00.3`).
class StaticBody {
   public:
    /// An empty body.
    StaticBody() = default;

    /// Storage the caller asserts outlives the response — a flash page, a
    /// `static` buffer, a table that is never rewritten while a response is in
    /// flight. The name is the assertion: use it only where that is true, and
    /// never for anything reached through `HTTPRequestView`.
    [[nodiscard]] static constexpr StaticBody outliving_response(
        std::span<const std::uint8_t> data) {
        return StaticBody(data);
    }

    /// As above, for text.
    [[nodiscard]] static StaticBody outliving_response(
        std::string_view content) {
        return StaticBody(pico_util::as_bytes(content));
    }

    /// The bytes to transmit.
    [[nodiscard]] constexpr std::span<const std::uint8_t> bytes()
        const noexcept {
        return bytes_;
    }

    /// How many bytes the body holds.
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return bytes_.size();
    }

    /// Whether the body holds nothing.
    [[nodiscard]] constexpr bool empty() const noexcept {
        return bytes_.empty();
    }

   private:
    constexpr explicit StaticBody(std::span<const std::uint8_t> bytes) noexcept
        : bytes_(bytes) {}

    std::span<const std::uint8_t> bytes_;
};

/// Mutable response object used by HTTP handlers.
///
/// The body can be supplied in one of three modes, letting the service stream
/// it out under backpressure without an unbounded staging buffer (AR-2, AR-5):
///   - BUFFERED (`set_body`): the handler's bytes are copied into a response
///     buffer. Suits small dynamic bodies: one over the profile's
///     MAX_BUFFERED_BODY is answered with a fixed 500 instead (BR-14).
///   - STATIC_VIEW (`set_body_static`): the body is a view over caller-owned
///     memory that outlives the response (flash pages, fixed strings), copied
///     only into the transmit ring.
///   - GENERATOR (`set_body_generator`): a resumable producer fills ring-sized
///     regions on demand — arbitrarily large responses in bounded RAM.
struct HTTPResponse {
    /// 200. The default status.
    static constexpr uint16_t STATUS_OK = 200U;
    /// 400, for a malformed request line, header or chunk framing.
    static constexpr uint16_t STATUS_BAD_REQUEST = 400U;
    /// 404, for a request target no route matches.
    static constexpr uint16_t STATUS_NOT_FOUND = 404U;
    /// 405, for a target that exists under a different method.
    static constexpr uint16_t STATUS_METHOD_NOT_ALLOWED = 405U;
    /// 413, for a body over the profile's MAX_BUFFERED_BODY (BR-5).
    static constexpr uint16_t STATUS_PAYLOAD_TOO_LARGE = 413U;
    /// 414, for a request line or URI over its cap (BR-1).
    static constexpr uint16_t STATUS_URI_TOO_LONG = 414U;
    /// 417, for an Expect the server cannot satisfy (NR-5).
    static constexpr uint16_t STATUS_EXPECTATION_FAILED = 417U;
    /// 431, for a header line, header count or head total over its cap (BR-2,
    /// BR-3). The capacity failure that AR-1 turns into a status rather than a
    /// truncation.
    static constexpr uint16_t STATUS_HEADER_FIELDS_TOO_LARGE = 431U;
    /// 501, for a transfer coding or method the server does not implement.
    static constexpr uint16_t STATUS_NOT_IMPLEMENTED = 501U;
    /// 505, for an HTTP version this server does not speak.
    static constexpr uint16_t STATUS_VERSION_NOT_SUPPORTED = 505U;
    /// Max reason-phrase length, in characters. As with the header caps this is
    /// the usable length, not the array extent — the storage below is declared
    /// one larger for pico_util::FixedString's terminator. The longest phrase
    /// this server emits, "Request Header Fields Too Large", is 31 characters,
    /// so the cap is not slack: serialize_error() static_asserts the table
    /// against it.
    static constexpr size_t STATUS_TEXT_CAPACITY = 32U;

    // Sentinel generator_length marking a length-less (streamed) generator body
    // whose total size is unknown up front: emitted as Transfer-Encoding:
    // chunked to HTTP/1.1 clients, or close-delimited to HTTP/1.0 ones (NR-6).
    /// Sentinel for generator_length: the body's total size is unknown, so it
    /// is emitted chunked to HTTP/1.1 clients and close-delimited to HTTP/1.0
    /// ones (NR-6).
    static constexpr std::size_t CHUNKED_LENGTH = SIZE_MAX;

    // STATIC_VIEW is deliberately not named STATIC: the cyw43 driver's
    // configport header does `#define STATIC static`, which would macro-mangle
    // the shorter name in firmware builds (host builds compile fine, making
    // the breakage easy to miss). Do not rename it back.
    /// Where the body's bytes come from. See the class note for the trade-off
    /// between the three.
    enum class BodyMode : std::uint8_t { BUFFERED, STATIC_VIEW, GENERATOR };

    /// Resumable body producer. Fills `out` with body bytes beginning at
    /// `offset` and returns how many it wrote (>0 unless offset == length).
    /// Called repeatedly by the writer as transmit-ring space frees, so it must
    /// be a pure function of `offset` (no internal cursor) — the writer owns
    /// the position, which makes partial sends and retransmits safe.
    using BodyGenerator = std::size_t (*)(void* ctx, std::size_t offset,
                                          std::span<std::uint8_t> out);

    /// Status code sent on the status line.
    uint16_t status_code = STATUS_OK;
    /// Reason phrase sent after the code. Truncated at STATUS_TEXT_CAPACITY.
    pico_util::FixedString<STATUS_TEXT_CAPACITY + 1> status_text =
        pico_util::FixedString<STATUS_TEXT_CAPACITY + 1>::truncated("OK");
    /// Response headers. Content-Length and Transfer-Encoding are set by the
    /// service from the body mode, not here.
    HTTPHeaders headers{};

    /// Which of the three body sources below is live.
    BodyMode body_mode = BodyMode::BUFFERED;
    /// What set_body did. A handler that needs to know whether its body will
    /// be sent reads it; the service needs nothing from it (body_refused).
    enum class BodyResult : std::uint8_t {
        /// The body was copied and its Content-Type set.
        SET,
        /// The body was copied; the Content-Type did not fit the header value
        /// cap and was not set, so a client may interpret the body differently.
        CONTENT_TYPE_DROPPED,
        /// The body exceeded MAX_COPIED_BODY: nothing was copied, and the
        /// service answers a fixed 500 instead (BR-14).
        REFUSED,
    };

    /// BUFFERED source: bytes copied from the handler.
    std::vector<std::uint8_t> body;
    /// Largest body set_body copies: the active profile's buffered-body cap
    /// (BR-14).
    static constexpr std::size_t MAX_COPIED_BODY =
        PICO_HTTP_ACTIVE_PROFILE::Limits::MAX_BUFFERED_BODY;
    /// Set when set_body was handed more than MAX_COPIED_BODY: nothing was
    /// copied, and the service answers a fixed 500 instead (BR-14). Cleared by
    /// any later set_body*() that is accepted, and by reset().
    bool body_refused = false;
    /// STATIC_VIEW source: caller-owned memory that must outlive the response.
    StaticBody static_body;
    /// GENERATOR source: the producer the writer pulls from.
    BodyGenerator generator = nullptr;
    /// Opaque context passed back to `generator` on every call.
    void* generator_ctx = nullptr;
    /// Total bytes `generator` will produce, or CHUNKED_LENGTH when unknown.
    std::size_t generator_length = 0;

    /// Number of body bytes this response will emit, across all modes.
    [[nodiscard]] std::size_t body_size() const {
        switch (body_mode) {
            case BodyMode::STATIC_VIEW:
                return static_body.size();
            case BodyMode::GENERATOR:
                return generator_length;
            case BodyMode::BUFFERED:
            default:
                return body.size();
        }
    }

    /// Reserve capacity for the response body to minimise reallocations.
    void reserve_body(size_t size) { body.reserve(size); }

    /// Return the response to its default state for reuse by the pending pool
    /// (AR-9).
    ///
    /// The body buffer is **released**, not cleared: an entry that kept
    /// capacity between turns would hold copied-body heap no connection owns,
    /// outside AR-1's bound.
    void reset() {
        status_code = STATUS_OK;
        // Two characters into STATUS_TEXT_CAPACITY: cannot truncate.
        static_cast<void>(status_text.assign("OK"));
        headers.clear();
        body_mode = BodyMode::BUFFERED;
        std::vector<std::uint8_t>().swap(body);
        body_refused = false;
        static_body = StaticBody();
        generator = nullptr;
        generator_ctx = nullptr;
        generator_length = 0;
    }

    /// Set the Content-Type header, replacing any existing one.
    ///
    /// \return false when the header did not fit — the type is longer than
    ///         PICO_HTTP_MAX_HEADER_VALUE, or the table is already at capacity.
    ///         The response is then sent without a Content-Type, which a client
    ///         is entitled to interpret differently, so a handler that builds
    ///         header content from request data should check.
    bool set_content_type(std::string_view type) {
        return headers.set("Content-Type", type);
    }

    /// Buffer `content` as the body, copying it. Suits small dynamic bodies;
    /// for a body already in memory that outlives the response, prefer
    /// set_body_static() and avoid the copy entirely. A body over
    /// MAX_COPIED_BODY is not copied: body_refused is set, nothing stays
    /// allocated, and the service answers a fixed 500 and closes (BR-14), so a
    /// larger body is static or generated.
    /// \return what was done; see BodyResult.
    BodyResult set_body(std::string_view content,
                        std::string_view content_type = "text/plain") {
        body_mode = BodyMode::BUFFERED;
        if (content.size() > MAX_COPIED_BODY) {
            refuse_body();
            return BodyResult::REFUSED;
        }
        body_refused = false;
        // Exact-sized and swapped in, so no capacity reserved earlier reaches
        // the slot that adopts it (AR-1).
        std::vector<std::uint8_t>(content.begin(), content.end()).swap(body);
        return set_content_type(content_type)
                   ? BodyResult::SET
                   : BodyResult::CONTENT_TYPE_DROPPED;
    }

    /// Buffer `data` as the body, copying it. Binary overload of the above.
    /// \return what was done; see BodyResult.
    BodyResult set_body(
        std::span<const std::uint8_t> data,
        std::string_view content_type = "application/octet-stream") {
        body_mode = BodyMode::BUFFERED;
        if (data.size() > MAX_COPIED_BODY) {
            refuse_body();
            return BodyResult::REFUSED;
        }
        body_refused = false;
        std::vector<std::uint8_t>(data.begin(), data.end()).swap(body);
        return set_content_type(content_type)
                   ? BodyResult::SET
                   : BodyResult::CONTENT_TYPE_DROPPED;
    }

    /// Body over storage that outlives the response, copied only into the
    /// transmit ring.
    ///
    /// Build the argument with `StaticBody::outliving_response()` or
    /// `StaticBody::outliving_response()`. A bare span does not convert, which
    /// is what stops a view into the request being served from recycled parser
    /// storage.
    /// \return whether the Content-Type header fit; the body is set either way.
    bool set_body_static(StaticBody data, std::string_view content_type =
                                              "application/octet-stream") {
        body_mode = BodyMode::STATIC_VIEW;
        body_refused = false;
        static_body = data;
        return set_content_type(content_type);
    }

    /// Streamed body of `length` bytes produced on demand by `fn(ctx, offset,
    /// out)`. Uses bounded RAM regardless of `length` (AR-5).
    /// \return whether the Content-Type header fit; the body is set either way.
    bool set_body_generator(
        BodyGenerator fn, void* ctx, std::size_t length,
        std::string_view content_type = "application/octet-stream") {
        body_mode = BodyMode::GENERATOR;
        body_refused = false;
        generator = fn;
        generator_ctx = ctx;
        generator_length = length;
        return set_content_type(content_type);
    }

    /// Streamed body of unknown length (NR-6). The generator fills ring-sized
    /// regions on demand exactly as the lengthed overload, but signals
    /// end-of-body by returning 0. Emitted as Transfer-Encoding: chunked to
    /// HTTP/1.1 clients, or close-delimited (Connection: close) to HTTP/1.0
    /// ones. Uses bounded RAM regardless of total size (AR-5).
    /// \return whether the Content-Type header fit; the body is set either way.
    bool set_body_generator(
        BodyGenerator fn, void* ctx,
        std::string_view content_type = "application/octet-stream") {
        return set_body_generator(fn, ctx, CHUNKED_LENGTH, content_type);
    }

    /// True for a length-less generator body (see the two-argument
    /// set_body_generator): the response is framed as chunked/close-delimited
    /// rather than with a Content-Length.
    [[nodiscard]] bool is_chunked_body() const {
        return body_mode == BodyMode::GENERATOR &&
               generator_length == CHUNKED_LENGTH;
    }

   private:
    // Release the buffer rather than clear it: a refused body leaves nothing
    // allocated, which is what lets AR-1's bound hold at peak (BR-14).
    void refuse_body() {
        std::vector<std::uint8_t>().swap(body);
        body_refused = true;
    }
};

/// Fills a deferred response at completion time (AR-9).
///
/// A deferring handler supplies one of these instead of keeping the
/// `HTTPResponse&` it was given: the service invokes it on the async-context
/// turn, and *only* if the request is still live. That is what makes a late
/// completion structurally unable to write into an entry another connection has
/// since claimed — the handler never holds a pointer to write through.
///
/// A function pointer plus context, not std::function, for the same reason
/// BodyGenerator is: a capturing closure would allocate and break AR-1.
using ResponseFiller = void (*)(void* ctx, HTTPResponse& response);

/// A route handler (AR-9). Declared here, beside the two types it takes, so the
/// route table and the service name one signature rather than each restating
/// it, and the two cannot disagree.
///
/// See HttpService::RequestHandler for the lifetime contract on `response`.
using RequestHandler =
    std::function<Disposition(const HTTPRequestView&, HTTPResponse&)>;

}  // namespace pico_http
