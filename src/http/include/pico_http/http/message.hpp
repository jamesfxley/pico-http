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

/// The handler completion decision (AR-9).
enum class Disposition : std::uint8_t {
    /// The response is filled; the service may send it as the handler returns.
    COMPLETE,
    /// The handler resolves later through HttpService::respond(). Request
    /// views survive until resolution; further requests wait (TR-6).
    PENDING,
};

/// Identifies one request by connection generation and request sequence (AR-9,
/// AR-4).
struct RequestId {
    /// The connection the request arrived on. Goes inert once that connection
    /// ends, via its generation tag (AR-4).
    tcp::ConnectionId connection;
    /// Request counter at dispatch. A nonzero connection request cap prevents
    /// 2^32-request wrap (RR-5).
    std::uint32_t sequence = 0;

    /// Whether this names a connection at all. A default-constructed RequestId
    /// is invalid; validity here does not mean the request is still current.
    [[nodiscard]] bool valid() const noexcept { return connection.valid(); }

    /// Compares connection and sequence to distinguish requests on a
    /// persistent connection.
    [[nodiscard]] friend bool operator==(const RequestId& lhs,
                                         const RequestId& rhs) noexcept {
        return lhs.connection == rhs.connection && lhs.sequence == rhs.sequence;
    }
};

/// Views into parser storage last until handler return for COMPLETE or
/// request resolution for PENDING.
/// Anything needed beyond that lifetime must be copied. Never use a request
/// view as a static response body.
struct HTTPRequestView {
    /// Request method token, e.g. "GET".
    std::string_view method;
    /// Request target as received, undecoded.
    std::string_view uri;
    /// HTTP-version token, e.g. "HTTP/1.1".
    std::string_view version;
    /// Parser-owned headers, or nullptr for a synthetic error request.
    const HTTPHeaders* headers = nullptr;
    /// Buffered body, valid until handler return or deferred resolution.
    std::span<const std::uint8_t> body;
    /// A value retained for deferred completion, inert after disconnect or
    /// the next request (AR-9, AR-4).
    RequestId request;
};

/// Caller-declared storage that outlives the response. Spans and string views
/// do not convert implicitly.
class StaticBody {
   public:
    /// An empty body.
    StaticBody() = default;

    /// Declares storage that remains valid throughout the response. Never use
    /// request storage.
    [[nodiscard]] static constexpr StaticBody outliving_response(
        std::span<const std::uint8_t> data) {
        return StaticBody(data);
    }

    /// Declare text storage that remains valid throughout the response.
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

/// A response with copied, static or generated body storage (AR-2, AR-5).
/// A copied body over MAX_BUFFERED_BODY receives a fixed 500 (BR-14).
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
    /// 431 for a header line, count or total exceeding capacity (BR-2, BR-3,
    /// AR-1).
    static constexpr uint16_t STATUS_HEADER_FIELDS_TOO_LARGE = 431U;
    /// 501, for a transfer coding or method the server does not implement.
    static constexpr uint16_t STATUS_NOT_IMPLEMENTED = 501U;
    /// 505, for an HTTP version this server does not speak.
    static constexpr uint16_t STATUS_VERSION_NOT_SUPPORTED = 505U;
    /// Usable reason-phrase capacity in characters, excluding the terminator.
    static constexpr size_t STATUS_TEXT_CAPACITY = 32U;

    /// Unknown body length, framed as chunked for HTTP/1.1 or close-delimited
    /// for HTTP/1.0 (NR-6).
    static constexpr std::size_t CHUNKED_LENGTH = SIZE_MAX;

    /// Body source. STATIC_VIEW avoids the STATIC macro that the cyw43
    /// configport header defines in firmware builds only, so host builds would
    /// not catch the clash. Do not rename it STATIC.
    enum class BodyMode : std::uint8_t { BUFFERED, STATIC_VIEW, GENERATOR };

    /// Fills out at offset and returns bytes written, positive before the
    /// known end. The producer must be a pure function of offset so partial
    /// writes can resume.
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
    /// set_body() sets this flag when a body exceeds MAX_COPIED_BODY.
    /// The service then answers a fixed 500. An accepted body setter or
    /// reset() clears the flag (BR-14).
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

    /// Reset the response and release copied storage (AR-9, AR-1).
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

    /// Set Content-Type.
    /// \return false if the type exceeds capacity or the header table is
    /// full. The response then lacks Content-Type, so a handler using request
    /// data should check.
    bool set_content_type(std::string_view type) {
        return headers.set("Content-Type", type);
    }

    /// Copy content into the body. An oversized body leaves no allocation and
    /// produces a fixed 500 with close (BR-14).
    /// \return The outcome recorded in BodyResult.
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

    /// Copy binary data into the body, subject to MAX_COPIED_BODY.
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

    /// Uses storage that outlives the response, copying only into the TX
    /// ring. \return Whether Content-Type fit; the body is set either way.
    bool set_body_static(StaticBody data, std::string_view content_type =
                                              "application/octet-stream") {
        body_mode = BodyMode::STATIC_VIEW;
        body_refused = false;
        static_body = data;
        return set_content_type(content_type);
    }

    /// Generates length bytes using bounded memory (AR-5). \return Whether
    /// Content-Type fit; the body is set either way.
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

    /// Generates until fn returns zero, using bounded memory (NR-6, AR-5).
    /// HTTP/1.1 uses chunked framing; HTTP/1.0 uses close-delimited framing.
    /// \return Whether Content-Type fit; the body is set either way.
    bool set_body_generator(
        BodyGenerator fn, void* ctx,
        std::string_view content_type = "application/octet-stream") {
        return set_body_generator(fn, ctx, CHUNKED_LENGTH, content_type);
    }

    /// True for unknown-length output requiring chunked or close-delimited
    /// framing.
    [[nodiscard]] bool is_chunked_body() const {
        return body_mode == BodyMode::GENERATOR &&
               generator_length == CHUNKED_LENGTH;
    }

   private:
    // Release body storage when an oversized copy is rejected, leaving nothing
    // allocated, which is what lets AR-1's bound hold at peak (BR-14).
    void refuse_body() {
        std::vector<std::uint8_t>().swap(body);
        body_refused = true;
    }
};

/// Fills a live deferred response on the async context without storing an
/// allocating closure (AR-9, AR-1).
using ResponseFiller = void (*)(void* ctx, HTTPResponse& response);

/// A route handler shared by the service and route table (AR-9).
/// See HttpService::RequestHandler for argument lifetimes.
using RequestHandler =
    std::function<Disposition(const HTTPRequestView&, HTTPResponse&)>;

}  // namespace pico_http
