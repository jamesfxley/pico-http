// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "pico_http/config/limits.hpp"
#include "pico_http/http/detail/parsing.hpp"
#include "pico_http/http/headers.hpp"
#include "pico_util/containers/fixed_string.hpp"

/// \file http_parser.hpp
/// Incremental, single-pass HTTP/1.x request parser (BR-1..BR-5 boundaries,
/// and the RFC conformance requirements). Fed
/// arbitrarily segmented byte spans, it consumes each byte exactly once (no
/// O(n^2) rescan-from-start), enforces the boundary limits from HttpLimits, and
/// stops at a complete request or the first violation. Pure C++, no lwIP, no
/// heap: request line, headers, and a bounded body live in fixed storage owned
/// by the parser. Chunked Transfer-Encoding is decoded into the body buffer;
/// any other (unsupported) coding is rejected with 501 (BR-9).

namespace pico_http {

/// Parser outcome. Non-zero error values are the HTTP status the transport
/// should return before closing the connection.
enum class ParseResult : std::uint16_t {
    INCOMPLETE = 0,
    COMPLETE = 1,
    BAD_REQUEST = 400,
    PAYLOAD_TOO_LARGE = 413,
    URI_TOO_LONG = 414,
    EXPECTATION_FAILED = 417,
    HEADER_FIELDS_TOO_LARGE = 431,
    NOT_IMPLEMENTED = 501,
    HTTP_VERSION_NOT_SUPPORTED = 505,
};

/// Lowest status code that denotes a failure; every ParseResult at or above it
/// is an error verdict carrying that status.
constexpr std::uint16_t FIRST_ERROR_STATUS = 400U;

/// Whether a ParseResult is a failure verdict rather than a state. True for
/// every value at or above FIRST_ERROR_STATUS, whose numeric value is the HTTP
/// status the service must send.
constexpr bool is_error(ParseResult result) noexcept {
    return static_cast<std::uint16_t>(result) >= FIRST_ERROR_STATUS;
}

/// Incremental HTTP/1.1 request parser over a caller-supplied byte stream.
///
/// Feed it whatever arrives, in whatever sizes it arrives: it holds its own
/// state between calls and never needs a whole request at once. Every capacity
/// is fixed by `Limits` and nothing is allocated (AR-1), so exceeding one is a
/// verdict — a ParseResult carrying the status to send — and never a truncated
/// field. `feed()` stops at a request boundary and reports how much it took,
/// leaving the rest for the next request, which is how pipelining works.
///
/// One parser handles one request at a time; call reset() before the next.
template <typename Limits = DefaultHttpLimits>
class HttpParser {
    // The header-count limit is enforced by HTTPHeaders' fixed storage, so the
    // policy's advertised MAX_HEADERS (reported via /limits) must match it —
    // otherwise the server would advertise a cap it does not actually enforce.
    static_assert(Limits::MAX_HEADERS == HTTPHeaders::MAX_HEADERS,
                  "Limits::MAX_HEADERS must equal HTTPHeaders::MAX_HEADERS");

   public:
    /// Feed a byte span. Returns the parser state after consuming as much as
    /// possible; `consumed` is set to the number of bytes taken from `data`
    /// (bytes past a Complete/error boundary are left for the caller — this is
    /// how request pipelining is handled). Call reset() before the next
    /// request.
    ParseResult feed(std::span<const std::uint8_t> data,
                     std::size_t& consumed) {
        consumed = 0;
        if (state_ == State::DONE) {
            return result_;
        }
        while (consumed < data.size() && result_ == ParseResult::INCOMPLETE) {
            const std::size_t before = consumed;
            const State state_before = state_;
            const std::uint8_t byte = data[consumed];
            step(byte, data, consumed);

            const std::size_t delta = consumed - before;
            const std::uint8_t charge = charge_class(state_before);
            if ((charge & CHARGE_REQUEST_LINE) == CHARGE_REQUEST_LINE) {
                rl_bytes_ += delta;
                if (rl_bytes_ > Limits::MAX_REQUEST_LINE) {
                    fail(ParseResult::URI_TOO_LONG);
                }
            }
            if ((charge & CHARGE_HEAD) != 0U) {
                head_bytes_ += delta;
                if (head_bytes_ > Limits::MAX_REQUEST_HEAD) {
                    fail(ParseResult::HEADER_FIELDS_TOO_LARGE);
                }
            }
        }
        return result_;
    }

    /// Return to the pre-request state, ready for the next request on the same
    /// connection. Every accessor below reverts to its empty value; the
    /// `Limits` the parser was instantiated with are unaffected.
    void reset() noexcept {
        state_ = State::METHOD;
        result_ = ParseResult::INCOMPLETE;
        crlf_skipped_ = false;
        scratch_len_ = 0;
        rl_bytes_ = 0;
        head_bytes_ = 0;
        body_len_ = 0;
        body_needed_ = 0;
        content_length_ = 0;
        chunk_remaining_ = 0;
        chunk_size_has_digit_ = false;
        overhead_bytes_ = 0;
        host_count_ = 0;
        cl_present_ = false;
        te_present_ = false;
        te_chunked_ = false;
        expect_continue_ = false;
        head_complete_ = false;
        method_.clear();
        uri_.clear();
        version_.clear();
        headers_.clear();
    }

    /// The verdict so far. INCOMPLETE means more bytes are needed; anything
    /// is_error() accepts is terminal and names the status to send.
    [[nodiscard]] ParseResult result() const noexcept { return result_; }

    /// Whether a whole request has been parsed. The accessors below are
    /// meaningful only once this is true, or on an error carrying the field.
    [[nodiscard]] bool complete() const noexcept {
        return result_ == ParseResult::COMPLETE;
    }

    /// Request method token, e.g. "GET". Views the parser's own storage and is
    /// invalidated by reset() or the next feed().
    [[nodiscard]] std::string_view method() const noexcept {
        return method_.view();
    }

    /// Request target as received, undecoded. Same lifetime as method().
    [[nodiscard]] std::string_view uri() const noexcept { return uri_.view(); }

    /// HTTP-version token, e.g. "HTTP/1.1". Same lifetime as method().
    [[nodiscard]] std::string_view version() const noexcept {
        return version_.view();
    }

    /// The parsed header collection. Same lifetime as method().
    [[nodiscard]] const HTTPHeaders& headers() const noexcept {
        return headers_;
    }

    /// Buffered request body. Empty for a request that has none, and for one
    /// whose body was streamed past the parser rather than buffered. Same
    /// lifetime as method().
    [[nodiscard]] std::span<const std::uint8_t> body() const noexcept {
        return std::span<const std::uint8_t>(body_.data(), body_len_);
    }

    /// Value of the Content-Length header, or 0 when absent. This is what the
    /// request declared, not what has arrived — compare against body().size()
    /// to tell a complete body from a partial one.
    [[nodiscard]] std::uint64_t content_length() const noexcept {
        return content_length_;
    }

    /// True when the request carried "Expect: 100-continue" (NR-5). The service
    /// may use this to decide whether to emit an interim 100 response.
    [[nodiscard]] bool expect_continue() const noexcept {
        return expect_continue_;
    }

    /// True once the request head (request line + headers) has been fully
    /// parsed and the parser has moved on to the body. Lets the service tell a
    /// header-completion timeout (TR-1) from a body-progress timeout (TR-2).
    [[nodiscard]] bool reading_body() const noexcept { return head_complete_; }

    /// True once any byte of the current request has been consumed (and it is
    /// not yet complete): the connection is mid-request, not idle. Lets the
    /// service keep a header/body deadline armed rather than falling back to
    /// the idle deadline while a partial request sits in the parser. Derived
    /// from the state machine: while INCOMPLETE, any progress out of the
    /// initial METHOD state, an accumulated method token, or a consumed
    /// leading CRLF (crlf_skipped_) means bytes were taken.
    [[nodiscard]] bool in_progress() const noexcept {
        return result_ == ParseResult::INCOMPLETE &&
               (state_ != State::METHOD || scratch_len_ > 0 || crlf_skipped_);
    }

   private:
    enum class State : std::uint8_t {
        METHOD,
        START_LF,  // saw CR before any method byte, expect LF (RFC 9112 §2.2)
        URI,
        VERSION,
        VERSION_LF,       // saw CR after version, expect LF
        HEADER_START,     // start of a header line: blank line or new header
        HEADER_LINE,      // accumulating a header line up to CR
        HEADER_LF,        // saw CR in a header line, expect LF
        HEADERS_END_LF,   // saw CR on a blank line, expect final LF
        BODY,             // identity body (Content-Length)
        CHUNK_SIZE,       // reading a chunk-size hex line
        CHUNK_EXT,        // skipping a chunk extension up to CR
        CHUNK_SIZE_LF,    // saw CR after chunk size, expect LF
        CHUNK_DATA,       // copying chunk-data bytes
        CHUNK_DATA_CR,    // expect CR after chunk data
        CHUNK_DATA_LF,    // expect LF after chunk data
        TRAILER_START,    // start of a trailer line or the final CRLF
        TRAILER_LINE,     // skipping a trailer line up to CR
        TRAILER_LINE_LF,  // expect LF after a trailer line
        TRAILER_END_LF,   // saw final CR, expect final LF
        DONE,
    };

    static constexpr std::uint8_t SP = static_cast<std::uint8_t>(' ');
    static constexpr std::uint8_t HT = static_cast<std::uint8_t>('\t');
    static constexpr std::uint8_t CR = static_cast<std::uint8_t>('\r');
    static constexpr std::uint8_t LF = static_cast<std::uint8_t>('\n');
    static constexpr std::uint8_t DEL = 0x7FU;

    // Radix of a Content-Length value and of a chunk-size field respectively,
    // plus the value the letters a-f/A-F carry as hex digits.
    static constexpr std::uint64_t DECIMAL_BASE = 10U;
    static constexpr std::uint64_t HEX_BASE = 16U;
    static constexpr int HEX_LETTER_VALUE = 10;

    static constexpr std::size_t SCRATCH_CAPACITY =
        (Limits::MAX_URI > Limits::MAX_HEADER_LINE ? Limits::MAX_URI
                                                   : Limits::MAX_HEADER_LINE) +
        2U;

    // Every token accumulated in scratch_ is bounded by its own limit before
    // push_scratch() is called, so scratch must cover the largest of them.
    // MAX_URI and MAX_HEADER_LINE are covered by construction above; the method
    // and version tokens are not, and a downstream Limits policy (limits.hpp
    // exists to be replaced) that raised either past the scratch would silently
    // truncate the token — routing a request to the wrong handler rather than
    // rejecting it. Bind the two so that cannot compile.
    static_assert(Limits::MAX_METHOD <= SCRATCH_CAPACITY,
                  "Limits::MAX_METHOD must fit the parser scratch buffer");
    static_assert(Limits::MAX_VERSION <= SCRATCH_CAPACITY,
                  "Limits::MAX_VERSION must fit the parser scratch buffer");

    /// Which byte budgets a state's consumption counts against (BR-1, BR-3).
    /// REQUEST_LINE implies HEAD: the request line is part of the head.
    static constexpr std::uint8_t CHARGE_NONE = 0U;
    static constexpr std::uint8_t CHARGE_HEAD = 1U;
    static constexpr std::uint8_t CHARGE_REQUEST_LINE = 2U | CHARGE_HEAD;

    /// The charge class of every state, in one place. Deliberately switches
    /// over State with no default: adding a state without deciding which
    /// budgets it is charged against is a compile error (-Wswitch), not a
    /// silently uncharged byte.
    static constexpr std::uint8_t charge_class(State state) noexcept {
        switch (state) {
            case State::METHOD:
            case State::START_LF:
            case State::URI:
            case State::VERSION:
            case State::VERSION_LF:
                return CHARGE_REQUEST_LINE;
            case State::HEADER_START:
            case State::HEADER_LINE:
            case State::HEADER_LF:
            case State::HEADERS_END_LF:
                return CHARGE_HEAD;
            // Body and chunk framing sit outside the head, so MAX_REQUEST_HEAD
            // bounds the head alone; their own limits are enforced elsewhere.
            case State::BODY:
            case State::CHUNK_SIZE:
            case State::CHUNK_EXT:
            case State::CHUNK_SIZE_LF:
            case State::CHUNK_DATA:
            case State::CHUNK_DATA_CR:
            case State::CHUNK_DATA_LF:
            case State::TRAILER_START:
            case State::TRAILER_LINE:
            case State::TRAILER_LINE_LF:
            case State::TRAILER_END_LF:
            case State::DONE:
                return CHARGE_NONE;
        }
        return CHARGE_NONE;
    }

    static constexpr int hex_value(std::uint8_t ch) noexcept {
        // No cast on the subtractions: integer promotion has already made each
        // one an int, so naming the type again is a no-op that
        // -Wuseless-cast rejects.
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + HEX_LETTER_VALUE;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + HEX_LETTER_VALUE;
        }
        return -1;
    }

    /// True iff the entire Transfer-Encoding value is a single "chunked" coding
    /// — the only framing we can decode. Any additional coding (e.g.
    /// "gzip, chunked" or "chunked, gzip") is unsupported and must be rejected
    /// with 501 (BR-9), never decoded as if the extra coding were absent. A
    /// comma anywhere means more than one coding is present, so it cannot be
    /// chunked-only.
    static constexpr bool te_is_chunked_only(std::string_view value) noexcept {
        if (value.find(',') != std::string_view::npos) {
            return false;
        }
        return detail::iequals(detail::trim_ows(value), "chunked");
    }

    static constexpr bool is_tchar(std::uint8_t ch) noexcept {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9')) {
            return true;
        }
        switch (static_cast<char>(ch)) {
            case '!':
            case '#':
            case '$':
            case '%':
            case '&':
            case '\'':
            case '*':
            case '+':
            case '-':
            case '.':
            case '^':
            case '_':
            case '`':
            case '|':
            case '~':
                return true;
            default:
                return false;
        }
    }

    static constexpr bool is_uri_char(std::uint8_t ch) noexcept {
        return ch > SP && ch < DEL;  // visible ASCII, no space/CTL/DEL
    }

    // A control octet that must not appear in a header field line. HTAB is
    // exempt (valid OWS and field-content); CR is handled separately as the
    // line terminator. Rejects C0 controls, a bare LF, and DEL (RFC 9110 §5.5).
    static constexpr bool is_field_ctl(std::uint8_t ch) noexcept {
        return (ch < SP && ch != HT) || ch == DEL;
    }

    static bool is_token(std::string_view text) noexcept {
        if (text.empty()) {
            return false;
        }
        return std::ranges::all_of(text, [](char ch) {
            return is_tchar(static_cast<std::uint8_t>(ch));
        });
    }

    void fail(ParseResult code) noexcept {
        result_ = code;
        state_ = State::DONE;
    }

    void succeed() noexcept {
        result_ = ParseResult::COMPLETE;
        state_ = State::DONE;
    }

    // Every caller bounds the token against its own limit before pushing, and
    // the static_asserts above prove each of those limits fits the scratch, so
    // the guard below is unreachable in a well-formed build. It stays as the
    // backstop for a limits policy that stops matching the buffer, and it
    // rejects rather than truncating (CXX-07.2) — this used to return a bool
    // that all three call sites discarded, which read as a check but performed
    // none.
    void push_scratch(std::uint8_t byte) noexcept {
        if (scratch_len_ >= SCRATCH_CAPACITY) {
            fail(ParseResult::BAD_REQUEST);
            return;
        }
        scratch_[scratch_len_++] = static_cast<char>(byte);
    }

    [[nodiscard]] std::string_view scratch_view() const noexcept {
        return std::string_view(scratch_.data(), scratch_len_);
    }

    // One `switch (state_)` over the whole request grammar. Splitting it per
    // state group would spread the transition table — which state each byte
    // moves to, and which boundary limit each arm charges — across functions
    // that only ever run in this order, making the machine harder to verify
    // against the spec rather than easier.
    // The complexity is the HTTP/1.1 grammar's, and splitting the switch
    // across functions would scatter the state transitions the spec is read
    // against, as the comment above says.
    // NOLINTNEXTLINE(readability-function-size,readability-function-cognitive-complexity)
    void step(std::uint8_t byte, std::span<const std::uint8_t> data,
              std::size_t& consumed) {
        switch (state_) {
            case State::METHOD:
                if (byte == SP) {
                    if (!method_.assign(scratch_view()) || scratch_len_ == 0) {
                        fail(ParseResult::BAD_REQUEST);
                    } else {
                        scratch_len_ = 0;
                        state_ = State::URI;
                    }
                } else if (byte == CR && scratch_len_ == 0 && !crlf_skipped_) {
                    // Ignore one empty line before the request-line
                    // (RFC 9112 §2.2): clients commonly emit a stray CRLF
                    // after a request body. Exactly one — a second is 400.
                    state_ = State::START_LF;
                } else if (!is_tchar(byte) ||
                           scratch_len_ >= Limits::MAX_METHOD) {
                    fail(ParseResult::BAD_REQUEST);
                } else {
                    push_scratch(byte);
                }
                ++consumed;
                break;

            case State::START_LF:
                if (byte != LF) {
                    fail(ParseResult::BAD_REQUEST);
                } else {
                    crlf_skipped_ = true;
                    state_ = State::METHOD;
                }
                ++consumed;
                break;

            case State::URI:
                if (byte == SP) {
                    if (scratch_len_ == 0) {
                        fail(ParseResult::BAD_REQUEST);
                    } else {
                        // Bounded by the MAX_URI arm above, which fails with
                        // URI_TOO_LONG before scratch can exceed uri_'s
                        // capacity; the assign cannot truncate.
                        static_cast<void>(uri_.assign(scratch_view()));
                        scratch_len_ = 0;
                        state_ = State::VERSION;
                    }
                } else if (!is_uri_char(byte)) {
                    fail(ParseResult::BAD_REQUEST);
                } else if (scratch_len_ >= Limits::MAX_URI) {
                    fail(ParseResult::URI_TOO_LONG);
                } else {
                    push_scratch(byte);
                }
                ++consumed;
                break;

            case State::VERSION:
                if (byte == CR) {
                    const ParseResult version_status =
                        classify_version(scratch_view());
                    if (version_status != ParseResult::COMPLETE) {
                        fail(version_status);
                    } else {
                        // Bounded by the MAX_VERSION arm above, as URI is.
                        static_cast<void>(version_.assign(scratch_view()));
                        scratch_len_ = 0;
                        state_ = State::VERSION_LF;
                    }
                } else if (byte == LF || scratch_len_ >= Limits::MAX_VERSION) {
                    fail(ParseResult::BAD_REQUEST);
                } else {
                    push_scratch(byte);
                }
                ++consumed;
                break;

            case State::VERSION_LF:
                if (byte != LF) {
                    fail(ParseResult::BAD_REQUEST);
                } else {
                    state_ = State::HEADER_START;
                }
                ++consumed;
                break;

            case State::HEADER_START:
                if (byte == CR) {
                    state_ = State::HEADERS_END_LF;
                    ++consumed;
                } else if (byte == SP || byte == HT) {
                    fail(ParseResult::BAD_REQUEST);  // obsolete line folding
                    ++consumed;
                } else {
                    state_ =
                        State::HEADER_LINE;  // reprocess byte as line content
                }
                break;

            case State::HEADER_LINE:
                if (byte == CR) {
                    state_ = State::HEADER_LF;
                } else if (is_field_ctl(byte)) {
                    fail(ParseResult::BAD_REQUEST);
                } else if (scratch_len_ >= Limits::MAX_HEADER_LINE) {
                    fail(ParseResult::HEADER_FIELDS_TOO_LARGE);
                } else {
                    push_scratch(byte);
                }
                ++consumed;
                break;

            case State::HEADER_LF:
                if (byte != LF) {
                    fail(ParseResult::BAD_REQUEST);
                } else {
                    process_header_line(scratch_view());
                    scratch_len_ = 0;
                    if (result_ == ParseResult::INCOMPLETE) {
                        state_ = State::HEADER_START;
                    }
                }
                ++consumed;
                break;

            case State::HEADERS_END_LF:
                if (byte != LF) {
                    fail(ParseResult::BAD_REQUEST);
                    ++consumed;
                } else {
                    ++consumed;
                    begin_body();
                }
                break;

            case State::BODY: {
                const std::size_t take =
                    std::min(data.size() - consumed, body_needed_ - body_len_);
                std::copy_n(
                    data.begin() + static_cast<std::ptrdiff_t>(consumed), take,
                    body_.begin() + static_cast<std::ptrdiff_t>(body_len_));
                body_len_ += take;
                consumed += take;
                if (body_len_ == body_needed_) {
                    succeed();
                }
                break;
            }

            case State::CHUNK_SIZE: {
                const int digit = hex_value(byte);
                if (digit >= 0) {
                    // Reject an overflowing chunk size rather than let it wrap
                    // (a wrap to 0 would be misread as the last chunk, letting
                    // the real payload be reparsed as a new request — a
                    // request-smuggling shape). Mirrors the Content-Length
                    // overflow guard. chunk_remaining_ doubles as the size
                    // accumulator here and the countdown once data starts.
                    if (chunk_remaining_ >
                        (UINT64_MAX - static_cast<std::uint64_t>(digit)) /
                            HEX_BASE) {
                        fail(ParseResult::PAYLOAD_TOO_LARGE);
                    } else {
                        chunk_remaining_ = chunk_remaining_ * HEX_BASE +
                                           static_cast<std::uint64_t>(digit);
                        chunk_size_has_digit_ = true;
                        count_overhead();
                    }
                    // The missing-digit and the not-a-chunk-size branches both
                    // reject, but for different RFC reasons that are stated
                    // separately; merging them would erase the distinction.
                    // NOLINTNEXTLINE(bugprone-branch-clone)
                } else if (!chunk_size_has_digit_) {
                    // RFC 9112 requires 1*HEXDIG: a bare ";ext" or CRLF where a
                    // size belongs must not be misread as the last chunk (an
                    // early terminator would let the remaining chunk data be
                    // reparsed as the next request — a request-smuggling
                    // shape).
                    fail(ParseResult::BAD_REQUEST);
                } else if (byte == ';') {
                    state_ = State::CHUNK_EXT;
                } else if (byte == CR) {
                    state_ = State::CHUNK_SIZE_LF;
                } else {
                    fail(ParseResult::BAD_REQUEST);
                }
                ++consumed;
                break;
            }

            case State::CHUNK_EXT:
                if (byte == CR) {
                    state_ = State::CHUNK_SIZE_LF;
                } else if (is_field_ctl(byte)) {
                    // Extensions are skipped, not parsed, but a control octet
                    // (e.g. a bare LF) must still be rejected — it could hide
                    // framing from an intermediary that parses more laxly.
                    fail(ParseResult::BAD_REQUEST);
                } else {
                    count_overhead();
                }
                ++consumed;
                break;

            case State::CHUNK_SIZE_LF:
                if (byte != LF) {
                    fail(ParseResult::BAD_REQUEST);
                } else if (chunk_remaining_ == 0) {
                    state_ = State::TRAILER_START;  // last chunk
                } else {
                    state_ = State::CHUNK_DATA;
                }
                ++consumed;
                break;

            case State::CHUNK_DATA: {
                // Bulk-copy the chunk-data run rather than dispatch per byte
                // (as the identity BODY state does). `want` is clamped in
                // 64-bit first so a huge chunk_remaining_ cannot truncate on a
                // 32-bit size_t before the min with the available bytes.
                const std::size_t avail = data.size() - consumed;
                auto want = static_cast<std::size_t>(
                    std::min<std::uint64_t>(avail, chunk_remaining_));
                const std::size_t room = Limits::MAX_BUFFERED_BODY - body_len_;
                const std::size_t take = std::min(want, room);
                std::copy_n(
                    data.begin() + static_cast<std::ptrdiff_t>(consumed), take,
                    body_.begin() + static_cast<std::ptrdiff_t>(body_len_));
                body_len_ += take;
                consumed += take;
                chunk_remaining_ -= take;
                if (take < want) {
                    fail(ParseResult::PAYLOAD_TOO_LARGE);  // over buffered cap
                } else if (chunk_remaining_ == 0) {
                    state_ = State::CHUNK_DATA_CR;
                }
                break;
            }

            case State::CHUNK_DATA_CR:
                if (byte != CR) {
                    fail(ParseResult::BAD_REQUEST);
                } else {
                    state_ = State::CHUNK_DATA_LF;
                }
                ++consumed;
                break;

            case State::CHUNK_DATA_LF:
                if (byte != LF) {
                    fail(ParseResult::BAD_REQUEST);
                } else {
                    // chunk_remaining_ is already 0 (the data run drained it);
                    // it now re-accumulates the next chunk's size.
                    chunk_size_has_digit_ = false;
                    state_ = State::CHUNK_SIZE;
                }
                ++consumed;
                break;

            case State::TRAILER_START:
                if (byte == CR) {
                    state_ = State::TRAILER_END_LF;
                } else if (is_field_ctl(byte)) {
                    fail(ParseResult::BAD_REQUEST);
                } else {
                    count_overhead();
                    state_ = State::TRAILER_LINE;
                }
                ++consumed;
                break;

            case State::TRAILER_LINE:
                if (byte == CR) {
                    state_ = State::TRAILER_LINE_LF;
                } else if (is_field_ctl(byte)) {
                    // Trailer fields are discarded, but keep the same field
                    // syntax rule as headers: control octets are rejected.
                    fail(ParseResult::BAD_REQUEST);
                } else {
                    count_overhead();
                }
                ++consumed;
                break;

            case State::TRAILER_LINE_LF:
                if (byte != LF) {
                    fail(ParseResult::BAD_REQUEST);
                } else {
                    state_ = State::TRAILER_START;
                }
                ++consumed;
                break;

            case State::TRAILER_END_LF:
                if (byte != LF) {
                    fail(ParseResult::BAD_REQUEST);
                } else {
                    succeed();
                }
                ++consumed;
                break;

            case State::DONE:
                break;
        }
    }

    // HTTP-version = "HTTP/" DIGIT "." DIGIT (RFC 9112 §2.6): exactly one digit
    // either side of the dot.
    static bool is_http_version_syntax(std::string_view version) noexcept {
        constexpr std::string_view PREFIX = "HTTP/";
        if (version.size() != PREFIX.size() + 3U ||
            !version.starts_with(PREFIX)) {
            return false;
        }
        auto is_digit = [](char ch) { return ch >= '0' && ch <= '9'; };
        return is_digit(version[PREFIX.size()]) &&
               version[PREFIX.size() + 1] == '.' &&
               is_digit(version[PREFIX.size() + 2]);
    }

    // Classify the request-line version token. A supported version yields
    // COMPLETE; a well-formed but unsupported one (e.g. HTTP/2.0, HTTP/1.9)
    // yields 505 (RFC 9110 §15.6.6); anything not matching HTTP-version syntax
    // is a malformed request line -> 400.
    static ParseResult classify_version(std::string_view version) noexcept {
        if (version == "HTTP/1.0" || version == "HTTP/1.1") {
            return ParseResult::COMPLETE;
        }
        return is_http_version_syntax(version)
                   ? ParseResult::HTTP_VERSION_NOT_SUPPORTED
                   : ParseResult::BAD_REQUEST;
    }

    void process_header_line(std::string_view line) {
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            fail(ParseResult::BAD_REQUEST);
            return;
        }
        const std::string_view name = line.substr(0, colon);
        if (!is_token(name)) {
            fail(ParseResult::BAD_REQUEST);  // e.g. space before colon
            return;
        }
        const std::string_view value = detail::trim_ows(line.substr(colon + 1));

        if (detail::iequals(name, "Content-Length")) {
            ingest_content_length(value);
        } else if (detail::iequals(name, "Transfer-Encoding")) {
            // A second Transfer-Encoding header means more than one coding is
            // stacked across lines (e.g. "gzip" then "chunked"); we can only
            // frame a lone "chunked", so anything else stays unsupported (501
            // via begin_body).
            te_chunked_ = !te_present_ && te_is_chunked_only(value);
            te_present_ = true;
        } else if (detail::iequals(name, "Expect")) {
            // The only expectation this server implements is 100-continue; any
            // other member is an expectation it cannot meet, and silently
            // ignoring it would be lying to the client (RFC 9110 §10.1.1 ->
            // 417).
            if (detail::iequals(value, "100-continue")) {
                expect_continue_ = true;
            } else {
                fail(ParseResult::EXPECTATION_FAILED);
                return;
            }
        } else if (detail::iequals(name, "Host")) {
            // HTTPHeaders collapses duplicates on set, so track the count here;
            // begin_body enforces exactly one for HTTP/1.1 (RFC 9112 §3.2).
            ++host_count_;
        }
        if (result_ != ParseResult::INCOMPLETE) {
            return;
        }

        // add() combines a repeated field name into a comma-separated list
        // (RFC 9110 §5.2) rather than letting the last occurrence silently
        // replace the earlier ones — replacement would let e.g. a second
        // "Connection: keep-alive" line erase an earlier "close". Overflow of
        // count or combined length is 431, never a silent drop (BR-7).
        if (!headers_.add(name, value)) {
            fail(ParseResult::HEADER_FIELDS_TOO_LARGE);
        }
    }

    void ingest_content_length(std::string_view value) {
        if (cl_present_) {
            fail(ParseResult::BAD_REQUEST);  // duplicate Content-Length
            return;
        }
        if (value.empty()) {
            fail(ParseResult::BAD_REQUEST);
            return;
        }
        std::uint64_t parsed = 0;
        for (const char ch : value) {
            if (ch < '0' || ch > '9') {
                fail(ParseResult::BAD_REQUEST);
                return;
            }
            const auto digit = static_cast<std::uint64_t>(ch - '0');
            if (parsed > (UINT64_MAX - digit) / DECIMAL_BASE) {
                fail(ParseResult::PAYLOAD_TOO_LARGE);  // overflow
                return;
            }
            parsed = parsed * DECIMAL_BASE + digit;
        }
        cl_present_ = true;
        content_length_ = parsed;
    }

    void begin_body() {
        // NR-5: RFC 9110 10.1.1 and 15.2 forbid 1xx for HTTP/1.0.
        if (version_.view() == "HTTP/1.0") {
            expect_continue_ = false;
        }
        head_complete_ = true;  // request line + headers fully parsed
        // RFC 9112 §3.2: an HTTP/1.1 request MUST carry exactly one Host
        // header; a missing or duplicated Host is malformed. HTTP/1.0 has no
        // such rule.
        if (version_.view() == "HTTP/1.1" && host_count_ != 1U) {
            fail(ParseResult::BAD_REQUEST);
            return;
        }
        // The 1.0-with-TE and the CL+TE branches both reject, but they are
        // distinct framing faults with distinct RFC citations; merging them
        // would erase the distinction.
        // NOLINTNEXTLINE(bugprone-branch-clone)
        if (te_present_ && version_.view() != "HTTP/1.1") {
            // Transfer-Encoding did not exist in HTTP/1.0: a 1.0 message
            // carrying it has faulty framing and must be rejected, not decoded
            // (RFC 9112 §6.1).
            fail(ParseResult::BAD_REQUEST);
        } else if (te_present_ && cl_present_) {
            fail(ParseResult::BAD_REQUEST);  // smuggling: CL + TE
        } else if (te_present_) {
            if (!te_chunked_) {
                fail(ParseResult::NOT_IMPLEMENTED);  // unsupported coding
            } else {
                chunk_remaining_ = 0;  // accumulates the first chunk size
                chunk_size_has_digit_ = false;
                state_ = State::CHUNK_SIZE;
            }
        } else if (!cl_present_ || content_length_ == 0) {
            succeed();
        } else if (content_length_ > Limits::MAX_BUFFERED_BODY) {
            fail(ParseResult::PAYLOAD_TOO_LARGE);
        } else {
            body_needed_ = static_cast<std::size_t>(content_length_);
            state_ = State::BODY;
        }
    }

    // Bound chunk framing / trailer overhead so a hostile stream of tiny chunks
    // or endless trailers cannot spin forever.
    void count_overhead() {
        if (++overhead_bytes_ > Limits::MAX_REQUEST_HEAD) {
            fail(ParseResult::HEADER_FIELDS_TOO_LARGE);
        }
    }

    State state_ = State::METHOD;
    ParseResult result_ = ParseResult::INCOMPLETE;
    // One empty line before the request-line has been consumed (RFC 9112
    // §2.2); a second is rejected. Also marks the connection as mid-request
    // for in_progress().
    bool crlf_skipped_ = false;

    std::array<char, SCRATCH_CAPACITY> scratch_{};
    std::size_t scratch_len_ = 0;

    std::size_t rl_bytes_ = 0;
    std::size_t head_bytes_ = 0;

    pico_util::FixedString<Limits::MAX_METHOD + 1> method_{};
    pico_util::FixedString<Limits::MAX_URI + 1> uri_{};
    pico_util::FixedString<Limits::MAX_VERSION + 1> version_{};
    HTTPHeaders headers_{};

    std::array<std::uint8_t, Limits::MAX_BUFFERED_BODY> body_{};
    std::size_t body_len_ = 0;
    std::size_t body_needed_ = 0;

    std::uint64_t content_length_ = 0;
    // Doubles as the chunk-size hex accumulator (in CHUNK_SIZE) and the
    // remaining-bytes countdown (in CHUNK_DATA); one field, one lifecycle.
    std::uint64_t chunk_remaining_ = 0;
    // A chunk-size line needs 1*HEXDIG before ';' or CR (RFC 9112 §7.1): a
    // zero-length size must not alias the "0" last-chunk. Cleared on entry to
    // CHUNK_SIZE.
    bool chunk_size_has_digit_ = false;
    std::size_t overhead_bytes_ = 0;  // chunk framing / trailer bytes
    std::size_t host_count_ = 0;      // Host header occurrences (RFC 9112 §3.2)
    bool cl_present_ = false;
    bool te_present_ = false;
    bool te_chunked_ = false;
    bool expect_continue_ = false;
    bool head_complete_ = false;
};

}  // namespace pico_http
