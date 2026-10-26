// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "pico_util/containers/ring_index.hpp"

namespace pico_http {

/// Fixed-capacity byte ring for zero-copy TCP transmission.
///
/// Bytes move through three regions in FIFO order:
///   written (unsent) -> sent (in-flight, awaiting ACK) -> released on ack().
/// Data handed to the transport with a no-copy write must stay valid until the
/// peer acknowledges it, so bytes are only reclaimed in ack() — never when they
/// are merely handed off in mark_sent(). This lets the caller pass spans from
/// the ring straight to tcp_write() without TCP_WRITE_FLAG_COPY.
///
/// No heap: storage is a std::array<uint8_t, Capacity>. All operations are O(1)
/// except write()/the wrap-splitting peek, which are O(bytes moved).
template <std::size_t Capacity>
class TxRing {
    static_assert(Capacity > 0, "TxRing capacity must be non-zero");

   public:
    /// The template's Capacity, exposed so a consumer can size a buffer or an
    /// assertion against the ring without repeating the number.
    static constexpr std::size_t CAPACITY = Capacity;

    /// Total live bytes (unsent + in-flight).
    [[nodiscard]] std::size_t size() const noexcept {
        return unsent_ + in_flight_;
    }

    /// Bytes write() would accept right now. Only ack() increases this.
    [[nodiscard]] std::size_t free_space() const noexcept {
        return Capacity - size();
    }

    /// Bytes written but not yet handed to the transport — what peek() exposes.
    [[nodiscard]] std::size_t unsent() const noexcept { return unsent_; }

    /// Bytes handed to the transport and not yet acknowledged. Still resident,
    /// so a retransmission can re-read them.
    [[nodiscard]] std::size_t in_flight() const noexcept { return in_flight_; }

    /// Whether nothing is unsent or in flight. Not the same as full capacity
    /// being available only if nothing was ever written.
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /// Copy up to free_space() bytes from data into the ring. Returns the
    /// number of bytes accepted (credit semantics): a short return means the
    /// ring is full and the caller should retry after an ack() frees space.
    std::size_t write(std::span<const std::uint8_t> data) {
        const std::size_t to_copy = std::min(data.size(), free_space());
        pico_util::RingPosition tail = index_of(unsent_ + in_flight_);
        std::size_t copied = 0;
        while (copied < to_copy) {
            const std::size_t run =
                pico_util::ring_run<Capacity>(
                    tail, pico_util::RingCount{to_copy - copied})
                    .elements;
            std::copy_n(
                data.begin() + static_cast<std::ptrdiff_t>(copied), run,
                storage_.begin() + static_cast<std::ptrdiff_t>(tail.index));
            copied += run;
            tail = pico_util::ring_index<Capacity>(tail,
                                                   pico_util::RingCount{run});
        }
        unsent_ += to_copy;
        return to_copy;
    }

    /// Contiguous span of not-yet-sent bytes starting at the send cursor. When
    /// the unsent region wraps the physical buffer this returns only the run up
    /// to the wrap point; call mark_sent() then peek() again for the remainder.
    [[nodiscard]] std::span<const std::uint8_t> peek() const noexcept {
        if (unsent_ == 0) {
            return {};
        }
        const pico_util::RingPosition start = index_of(in_flight_);
        const std::size_t run =
            pico_util::ring_run<Capacity>(start, pico_util::RingCount{unsent_})
                .elements;
        return std::span<const std::uint8_t>(storage_.data() + start.index,
                                             run);
    }

    /// Reclassify n unsent bytes as in-flight (handed to the transport). The
    /// bytes remain resident until ack() so retransmission can re-read them.
    /// Saturating credit: the adapter bounds n by peek().size().
    void mark_sent(std::size_t n) noexcept {
        n = std::min(n, unsent_);
        unsent_ -= n;
        in_flight_ += n;
    }

    /// Release acknowledged credit, saturating at the in-flight count.
    /// The lwIP adapter passes only bytes acknowledged on this ring's PCB;
    /// architecture.md explains the credit bounds and callback lifetime.
    void ack(std::size_t n) noexcept {
        n = std::min(n, in_flight_);
        head_ = index_of(n);  // advance head by n (index_of already adds head_)
        in_flight_ -= n;
    }

    /// Drop everything, including bytes still in flight. For connection
    /// teardown only: the transport may still hold references to in-flight
    /// bytes, so calling this on a live connection abandons data the peer will
    /// never see.
    void clear() noexcept {
        head_ = pico_util::RingPosition{};
        unsent_ = 0;
        in_flight_ = 0;
    }

   private:
    [[nodiscard]] pico_util::RingPosition index_of(
        std::size_t offset_from_head) const noexcept {
        // pico_util::ring_index compares and resets rather than dividing,
        // which is what a target with no division instruction wants. Valid
        // here because the ring's occupancy invariant keeps
        // head_ + offset_from_head below 2 * Capacity: head_ is an index and
        // unsent_ + in_flight_ never exceeds Capacity.
        return pico_util::ring_index<Capacity>(
            head_, pico_util::RingCount{offset_from_head});
    }

    std::array<std::uint8_t, Capacity> storage_{};
    // index of oldest in-flight (unacked) byte
    pico_util::RingPosition head_;
    std::size_t unsent_ = 0;     // bytes written but not yet sent
    std::size_t in_flight_ = 0;  // bytes sent but not yet acked
};

}  // namespace pico_http
