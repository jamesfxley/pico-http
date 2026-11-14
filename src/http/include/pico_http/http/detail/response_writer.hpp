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
/// Resumable response output. The caller selects chunked or close-delimited
/// framing for unknown-length bodies (NR-6).

namespace pico_http::detail {

/// Call-scoped references to service counters. The aggregate is not stored.
// cppcoreguidelines-avoid-const-or-ref-data-members: calls bound lifetime.
// NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
struct WriteCounters {
    std::uint64_t& bytes_out;
    std::uint32_t& backpressure_events;
};
// cppcoreguidelines-avoid-const-or-ref-data-members: scope ends here.
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
    /// The caller must close after an early generator end or a partial frame.
    ABORT,
    /// The connection is gone, so no writable event will follow. A draining
    /// connection reads as STALLED, bounded by the TR-5 linger rather than the
    /// TR-4 write timeout.
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

    /// Head storage for the caller to serialise before begin().
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

    /// Use the serialised head without a body or last-chunk (BR-13, BR-14;
    /// RFC 9110 §9.3.2).
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
    // Hexadecimal chunk-size width in digits. Leading zeros are valid (NR-6;
    // RFC 9112 §7.1).
    static constexpr std::size_t CHUNK_SIZE_HEX_WIDTH = 4;
    // Framing bytes around one chunk: "XXXX\r\n" prefix + trailing "\r\n".
    static constexpr std::size_t CHUNK_FRAMING_OVERHEAD =
        CHUNK_SIZE_HEX_WIDTH + 2 + 2;
    static constexpr std::size_t MAX_ENCODABLE_CHUNK =
        (std::size_t{1} << (4U * CHUNK_SIZE_HEX_WIDTH)) - 1U;
    static_assert(GeneratorChunk <= MAX_ENCODABLE_CHUNK,
                  "CHUNK_SIZE_HEX_WIDTH (4 hex digits) must encode any single "
                  "chunk, whose payload never exceeds GeneratorChunk");
    // The payload budget subtracts framing overhead in size_t. A smaller
    // chunk would underflow.
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

    // Resume a partially accepted interim 100 from its cursor.
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

    // Separate generator output keeps scratch storage off the static and
    // buffered paths.
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
            // Bound the fill by ring credit to avoid generating bytes the ring
            // cannot accept. A transport without a ring reports SIZE_MAX,
            // which leaves the fill unbounded.
            const std::size_t credit = transport.send_credit(id);
            if (credit == 0) {
                ++counters.backpressure_events;
                return stalled_or_dead(transport, id);
            }
            // The scratch buffer must outlive send().
            // cppcoreguidelines-pro-type-member-init: all reads follow writes.
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

    // Credit bounds each frame so partial-frame resumption must never arise
    // (NR-6).
    // readability-function-cognitive-complexity: framing shares state.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    PumpResult send_streamed_body(ITcpTransport& transport,
                                  ITcpTransport::ConnectionId id,
                                  WriteCounters counters) {
        const bool chunked = framing_ == Framing::CHUNKED;
        while (true) {
            if (generator_drained_) {
                // Only chunked framing needs a terminating zero-length chunk.
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

            // Chunk scratch reserves [XXXX\r\n] before the payload and [\r\n]
            // after it. Raw output fills from the front.
            // cppcoreguidelines-pro-type-member-init: all reads follow writes.
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
                // A short write contradicts send_credit(). The committed
                // partial frame cannot resume without duplicating bytes.
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
