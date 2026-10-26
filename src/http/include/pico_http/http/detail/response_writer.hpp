// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string_view>

#include "pico_http/http/detail/body_source.hpp"
#include "pico_http/http/message.hpp"
#include "pico_http/tcp/tcp_transport.hpp"
#include "pico_util/bytes/as_bytes.hpp"

/// \file response_writer.hpp
/// Drives one response onto the wire under backpressure. The writer owns the
/// serialized head, the body source and the cursors into both, and advances
/// through an explicit state rather than through a set of independent flags:
/// every send path has to leave the writer resumable from the next writable
/// event, which is only checkable if "where am I" is one value.
///
/// Framing for a length-less body (NR-6) is chosen by the caller and handed
/// over in begin(): chunked for HTTP/1.1, raw close-delimited for the HTTP/1.0
/// downgrade.

namespace pico_http::detail {

/// The two service counters the writer moves. Bundled by reference so the
/// writer stays independent of HttpService::Statistics. This is a call-scoped
/// parameter aggregate — constructed at the call site, never stored — so the
/// usual objection to reference members (surprising copy and assignment
/// semantics on a long-lived object) does not apply.
// NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
struct WriteCounters {
    std::uint64_t& bytes_out;
    std::uint32_t& backpressure_events;
};
// Closes the region opened above; the call-scoped-aggregate reason is there.
// NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)

/// How a body of unknown length is delimited.
enum class Framing : std::uint8_t {
    /// Body length is known and was written as Content-Length.
    SIZED,
    /// Transfer-Encoding: chunked (HTTP/1.1).
    CHUNKED,
    /// Raw stream delimited by the connection close (HTTP/1.0 downgrade).
    CLOSE_DELIMITED,
};

/// Outcome of one pump.
enum class PumpResult : std::uint8_t {
    /// Everything queued so far is on the wire; the response is complete.
    COMPLETE,
    /// The transmit ring is full. Resume on the next writable event.
    STALLED,
    /// The response cannot be continued correctly; the caller must close.
    /// Raised when a generator quits early, or when a transport accepts less
    /// than the credit it advertised mid-frame.
    ABORT,
    /// The connection is gone. A send accepting nothing is ambiguous on its
    /// own — a full ring and a dead connection both accept zero — so the
    /// writer asks the transport which it was. Without this the response would
    /// park waiting for a writable event that can never arrive, and only the
    /// TR-4 write timeout would eventually reclaim the slot.
    ///
    /// A slot already draining towards close is deliberately not DEAD: both
    /// transports refuse sends on it while is_open() still reports it live, so
    /// it reads as STALLED and is bounded by the shorter TR-5 drain linger
    /// rather than by TR-4.
    DEAD,
};

template <std::size_t HeadCapacity, std::size_t GeneratorChunk>
class ResponseWriter {
   public:
    /// Interim 100 Continue (NR-5), sent before the response head when the
    /// request asked for it.
    static constexpr std::string_view CONTINUE_RESPONSE =
        "HTTP/1.1 100 Continue\r\n\r\n";

    enum class State : std::uint8_t {
        /// No response in flight.
        IDLE,
        /// Delivering the interim 100 Continue.
        CONTINUE,
        /// Delivering the status line and headers.
        HEAD,
        /// Delivering the body.
        BODY,
        /// Body content complete; the terminating zero-length chunk is owed.
        LAST_CHUNK,
    };

    /// Writable view of the head buffer, for the caller to serialize into
    /// before begin(). Serialization is the caller's job because the framing
    /// decisions it makes depend on request context and keep-alive policy.
    std::span<std::uint8_t> head_buffer() noexcept { return head_; }

    /// Start emitting a response whose head occupies the first `head_len` bytes
    /// of head_buffer(). Takes the body over from the handler's response.
    void begin(std::size_t head_len, HTTPResponse& response,
               Framing framing) noexcept {
        head_len_ = head_len;
        head_sent_ = 0;
        body_.adopt(response);
        body_sent_ = 0;
        framing_ = framing;
        generator_drained_ = false;
        // The interim 100, if any, has already been delivered or is still
        // draining; begin() must not disturb its cursor.
        state_ = continue_pending() ? State::CONTINUE : State::HEAD;
    }

    /// Replace the response wholesale with a head that is already serialized
    /// into head_buffer(), dropping any body. Used for the fixed 500 fallback
    /// (BR-13, BR-14), and for HEAD, which carries the head with no body (RFC
    /// 9110 §9.3.2) and, for a chunked framing, no last-chunk either.
    void drop_body() noexcept {
        body_.reset();
        body_sent_ = 0;
        framing_ = Framing::SIZED;
        generator_drained_ = false;
    }

    void set_head_length(std::size_t head_len) noexcept {
        head_len_ = head_len;
        head_sent_ = 0;
    }

    /// Return to the idle state, freeing a copied body, so a slot that is reset
    /// mid-send holds no copied-body heap (AR-1).
    void reset() noexcept {
        state_ = State::IDLE;
        head_len_ = 0;
        head_sent_ = 0;
        body_.reset();
        body_sent_ = 0;
        framing_ = Framing::SIZED;
        generator_drained_ = false;
        continue_sent_ = false;
        continue_sent_bytes_ = 0;
    }

    /// True while a response is in flight.
    [[nodiscard]] bool active() const noexcept { return state_ != State::IDLE; }

    [[nodiscard]] State state() const noexcept { return state_; }

    /// Queue the interim 100 Continue. Idempotent: a request only gets one.
    void request_continue() noexcept {
        if (!continue_sent_) {
            continue_sent_ = true;
            continue_sent_bytes_ = 0;
            if (state_ == State::IDLE) {
                state_ = State::CONTINUE;
            }
        }
    }

    /// True when an interim 100 was queued but is not yet fully accepted.
    [[nodiscard]] bool continue_pending() const noexcept {
        return continue_sent_ &&
               continue_sent_bytes_ < CONTINUE_RESPONSE.size();
    }

    /// Why a send accepted nothing: backpressure (retry on writable) or a
    /// connection that no longer exists (stop).
    static PumpResult stalled_or_dead(const ITcpTransport& transport,
                                      ITcpTransport::ConnectionId id) {
        return transport.is_open(id) ? PumpResult::STALLED : PumpResult::DEAD;
    }

    /// Advance the response as far as the transmit ring allows.
    PumpResult pump(ITcpTransport& transport, ITcpTransport::ConnectionId id,
                    WriteCounters counters) {
        // The interim 100 must be fully delivered before any response bytes:
        // interleaving the two heads would corrupt the stream.
        if (continue_pending()) {
            if (!flush_continue(transport, id, counters)) {
                return stalled_or_dead(transport, id);
            }
            if (state_ == State::CONTINUE) {
                state_ = head_len_ > 0 ? State::HEAD : State::IDLE;
            }
        }
        if (state_ == State::IDLE) {
            return PumpResult::COMPLETE;
        }
        if (state_ == State::HEAD) {
            if (!send_head(transport, id, counters)) {
                return stalled_or_dead(transport, id);
            }
            state_ = State::BODY;
        }
        const PumpResult result =
            state_ == State::BODY || state_ == State::LAST_CHUNK
                ? send_body(transport, id, counters)
                : PumpResult::COMPLETE;
        if (result == PumpResult::COMPLETE) {
            state_ = State::IDLE;
            body_.release_buffer();
        }
        return result;
    }

   private:
    // Fixed width of the hex chunk-size field for a chunked response (NR-6).
    // Zero-padded (RFC 9112 §7.1 chunk-size = 1*HEXDIG permits leading zeros)
    // so the size line is a constant length: the writer reserves it at the
    // front of the scratch buffer, the generator fills the payload after it,
    // and the size is back-filled without shifting the payload. 4 digits cover
    // any single chunk up to 0xFFFF bytes, well above GeneratorChunk.
    static constexpr std::size_t CHUNK_SIZE_HEX_WIDTH = 4;
    // Framing bytes around one chunk: "XXXX\r\n" prefix + trailing "\r\n".
    static constexpr std::size_t CHUNK_FRAMING_OVERHEAD =
        CHUNK_SIZE_HEX_WIDTH + 2 + 2;
    static constexpr std::size_t MAX_ENCODABLE_CHUNK =
        (std::size_t{1} << (4U * CHUNK_SIZE_HEX_WIDTH)) - 1U;
    static_assert(GeneratorChunk <= MAX_ENCODABLE_CHUNK,
                  "CHUNK_SIZE_HEX_WIDTH (4 hex digits) must encode any single "
                  "chunk, whose payload never exceeds GeneratorChunk");
    // The floor the ceiling above implies but does not state. A chunked
    // generator computes its fill budget as GeneratorChunk - overhead in
    // std::size_t, so a profile with a smaller chunk wraps that subtraction to
    // near SIZE_MAX and takes an out-of-range subspan of the scratch buffer.
    // Nothing else between a downstream profile and that arithmetic bounds the
    // value: the HttpProfile concept requires only that it exists.
    static_assert(GeneratorChunk > CHUNK_FRAMING_OVERHEAD,
                  "Profile::GENERATOR_CHUNK must leave room for a chunk's own "
                  "framing plus at least one payload byte");

    static constexpr std::array<std::uint8_t, 5> LAST_CHUNK = {'0', '\r', '\n',
                                                               '\r', '\n'};

    // Writes the fixed-width, zero-padded hex chunk-size + CRLF into dst.
    static void write_chunk_size(std::span<std::uint8_t> dst,
                                 std::size_t size) noexcept {
        static constexpr std::string_view HEX = "0123456789abcdef";
        static constexpr std::size_t NIBBLE = 0xFU;
        for (std::size_t i = 0; i < CHUNK_SIZE_HEX_WIDTH; ++i) {
            const std::size_t shift = (CHUNK_SIZE_HEX_WIDTH - 1 - i) * 4U;
            dst[i] = static_cast<std::uint8_t>(HEX[(size >> shift) & NIBBLE]);
        }
        dst[CHUNK_SIZE_HEX_WIDTH] = static_cast<std::uint8_t>('\r');
        dst[CHUNK_SIZE_HEX_WIDTH + 1] = static_cast<std::uint8_t>('\n');
    }

    // Deliver the not-yet-accepted remainder of the interim 100. The ring may
    // take the small write only partially (a pipelined request can complete its
    // head while the previous response still drains), so the remainder resumes
    // from the cursor, never from offset 0.
    bool flush_continue(ITcpTransport& transport,
                        ITcpTransport::ConnectionId id,
                        WriteCounters counters) {
        while (continue_sent_bytes_ < CONTINUE_RESPONSE.size()) {
            const std::span<const std::uint8_t> remaining =
                pico_util::as_bytes(CONTINUE_RESPONSE)
                    .subspan(continue_sent_bytes_);
            const std::size_t accepted = transport.send(id, remaining);
            if (accepted == 0) {
                ++counters.backpressure_events;
                return false;
            }
            continue_sent_bytes_ += accepted;
            counters.bytes_out += accepted;
        }
        return true;
    }

    bool send_head(ITcpTransport& transport, ITcpTransport::ConnectionId id,
                   WriteCounters counters) {
        while (head_sent_ < head_len_) {
            const std::span<const std::uint8_t> remaining(
                std::next(head_.data(),
                          static_cast<std::ptrdiff_t>(head_sent_)),
                head_len_ - head_sent_);
            const std::size_t accepted = transport.send(id, remaining);
            if (accepted == 0) {
                ++counters.backpressure_events;
                return false;
            }
            head_sent_ += accepted;
            counters.bytes_out += accepted;
        }
        return true;
    }

    PumpResult send_body(ITcpTransport& transport,
                         ITcpTransport::ConnectionId id,
                         WriteCounters counters) {
        return body_.streaming() ? send_streamed_body(transport, id, counters)
                                 : send_sized_body(transport, id, counters);
    }

    // A body whose length is known up front. The two cases are separate
    // functions rather than a branch inside one loop because only one of them
    // needs the generator scratch: sharing the loop put a GeneratorChunk-sized
    // array in the frame of every buffered and static pump too, on the
    // async-context stack, for a branch that cannot reach it.
    PumpResult send_sized_body(ITcpTransport& transport,
                               ITcpTransport::ConnectionId id,
                               WriteCounters counters) {
        return body_.is_generator()
                   ? send_sized_generator_body(transport, id, counters)
                   : send_sized_direct_body(transport, id, counters);
    }

    // Buffered and static bodies are addressable in place: no scratch, no copy
    // beyond the transport's own.
    PumpResult send_sized_direct_body(ITcpTransport& transport,
                                      ITcpTransport::ConnectionId id,
                                      WriteCounters counters) {
        while (body_sent_ < body_.total()) {
            const std::span<const std::uint8_t> chunk =
                body_.bytes().subspan(body_sent_, body_.total() - body_sent_);
            const std::size_t accepted = transport.send(id, chunk);
            if (accepted == 0) {
                ++counters.backpressure_events;
                return stalled_or_dead(transport, id);
            }
            body_sent_ += accepted;
            counters.bytes_out += accepted;
        }
        return PumpResult::COMPLETE;
    }

    // A lengthed generator produces into scratch, a chunk per pump.
    PumpResult send_sized_generator_body(ITcpTransport& transport,
                                         ITcpTransport::ConnectionId id,
                                         WriteCounters counters) {
        while (body_sent_ < body_.total()) {
            // Clamp the fill to transmit-ring free space so every generated
            // byte is accepted by the send() below — never generate bytes the
            // ring will reject and then re-generate them on the next writable
            // event. A transport that does not model a ring reports SIZE_MAX,
            // leaving the fill effectively unclamped.
            const std::size_t credit = transport.send_credit(id);
            if (credit == 0) {
                ++counters.backpressure_events;
                return stalled_or_dead(transport, id);
            }
            // Deliberately left uninitialised (no `{}`): the generator
            // overwrites the region it fills and only the `produced` prefix is
            // read back, so zeroing it on every pump would be pure waste. It
            // must outlive the send() below, hence loop-body scope.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
            std::array<std::uint8_t, GeneratorChunk> scratch;
            const std::size_t want =
                std::min({scratch.size(), body_.total() - body_sent_, credit});
            const std::size_t produced = body_.generate(
                body_sent_, std::span<std::uint8_t>(scratch.data(), want));
            if (produced == 0U) {
                return PumpResult::ABORT;  // generator quit early
            }
            const std::size_t accepted = transport.send(
                id, std::span<const std::uint8_t>(scratch.data(), produced));
            if (accepted == 0) {
                ++counters.backpressure_events;
                return stalled_or_dead(transport, id);
            }
            body_sent_ += accepted;
            counters.bytes_out += accepted;
        }
        return PumpResult::COMPLETE;
    }

    // A length-less generator body (NR-6): pull ring-sized chunks until the
    // generator returns 0. send_credit bounds each write so a framed chunk is
    // accepted whole — chunk framing is not offset-addressable, so a
    // partial-frame resumption must never arise.
    // One resumable state machine over the chunked-framing rules described
    // above; the branches are the framing cases, and hoisting them out would
    // separate each from the resumption invariant it maintains.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    PumpResult send_streamed_body(ITcpTransport& transport,
                                  ITcpTransport::ConnectionId id,
                                  WriteCounters counters) {
        const bool chunked = framing_ == Framing::CHUNKED;
        while (true) {
            if (generator_drained_) {
                // Content is complete. A raw close-delimited stream is done —
                // the socket close delimits it. A chunked response still owes
                // the terminating zero-length chunk.
                if (!chunked) {
                    return PumpResult::COMPLETE;
                }
                state_ = State::LAST_CHUNK;
                if (transport.send_credit(id) < LAST_CHUNK.size()) {
                    ++counters.backpressure_events;
                    return stalled_or_dead(transport, id);
                }
                const std::size_t accepted = transport.send(id, LAST_CHUNK);
                counters.bytes_out += accepted;
                // A short accept here commits part of the terminator, so a
                // retry would duplicate bytes: close rather than corrupt.
                return accepted < LAST_CHUNK.size() ? PumpResult::ABORT
                                                    : PumpResult::COMPLETE;
            }

            const std::size_t overhead = chunked ? CHUNK_FRAMING_OVERHEAD : 0U;
            const std::size_t credit = transport.send_credit(id);
            if (credit <= overhead) {
                // No room for the framing plus at least one payload byte.
                ++counters.backpressure_events;
                return stalled_or_dead(transport, id);
            }

            // Chunked layout in scratch: [XXXX\r\n][payload...][\r\n]. The
            // generator fills the payload region after the reserved size field;
            // raw framing fills from the front. Left uninitialised for the
            // reason given in send_sized_body.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
            std::array<std::uint8_t, GeneratorChunk> scratch;
            const std::size_t prefix = chunked ? CHUNK_SIZE_HEX_WIDTH + 2 : 0;
            const std::size_t budget =
                std::min(scratch.size() - overhead, credit - overhead);
            const std::size_t produced = body_.generate(
                body_sent_,
                std::span<std::uint8_t>(scratch).subspan(prefix, budget));
            if (produced == 0U) {
                generator_drained_ = true;
                continue;  // emit the last-chunk (chunked) or finish (raw)
            }

            std::size_t framed_len = produced;
            if (chunked) {
                write_chunk_size(scratch, produced);
                scratch[prefix + produced] = static_cast<std::uint8_t>('\r');
                scratch[prefix + produced + 1] =
                    static_cast<std::uint8_t>('\n');
                framed_len = prefix + produced + 2;
            }

            const std::size_t accepted = transport.send(
                id, std::span<const std::uint8_t>(scratch.data(), framed_len));
            counters.bytes_out += accepted;
            if (chunked) {
                // send_credit() guaranteed the whole frame fits, so a short
                // accept means the transport over-reported its credit. The
                // partial frame is already committed and chunk framing is not
                // offset-addressable, so the stream cannot resume without
                // duplicating bytes. The client sees a truncated chunked body
                // (a detectable error), never a corrupted one.
                if (accepted < framed_len) {
                    return PumpResult::ABORT;
                }
                body_sent_ += produced;
            } else {
                // The raw stream is a pure function of offset, so commit
                // exactly what the transport took and resume from there.
                body_sent_ += accepted;
                if (accepted < framed_len) {
                    ++counters.backpressure_events;
                    return stalled_or_dead(transport, id);
                }
            }
        }
    }

    std::array<std::uint8_t, HeadCapacity> head_{};
    std::size_t head_len_ = 0;
    std::size_t head_sent_ = 0;

    BodySource body_{};
    std::size_t body_sent_ = 0;

    State state_ = State::IDLE;
    Framing framing_ = Framing::SIZED;
    bool generator_drained_ = false;

    bool continue_sent_ = false;
    std::size_t continue_sent_bytes_ = 0;
};

}  // namespace pico_http::detail
