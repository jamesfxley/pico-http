// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "pico_util/containers/ring_index.hpp"

namespace pico_http {

/// Fixed-capacity transmit storage. NOCOPY bytes must remain valid until ACK.
/// ack() reclaims bytes; mark_sent() retains them for retransmission. write()
/// copies bytes; other operations are constant-time.
template <std::size_t Capacity>
class TxRing {
    static_assert(Capacity > 0, "TxRing capacity must be non-zero");

   public:
    /// Capacity in bytes.
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

    /// True when no bytes are unsent or in flight.
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

    /// The contiguous unsent run, ending at the buffer wrap. mark_sent()
    /// exposes the next run.
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

    /// Mark up to n unsent bytes in flight, retaining them until ack().
    void mark_sent(std::size_t n) noexcept {
        n = std::min(n, unsent_);
        unsent_ -= n;
        in_flight_ += n;
    }

    /// Release up to n in-flight bytes. The adapter bounds credit as
    /// described in architecture.md.
    void ack(std::size_t n) noexcept {
        n = std::min(n, in_flight_);
        head_ = index_of(n);  // advance head by n (index_of already adds head_)
        in_flight_ -= n;
    }

    /// Clear storage during teardown only. A live transport can still
    /// reference in-flight bytes.
    void clear() noexcept {
        head_ = pico_util::RingPosition{};
        unsent_ = 0;
        in_flight_ = 0;
    }

   private:
    [[nodiscard]] pico_util::RingPosition index_of(
        std::size_t offset_from_head) const noexcept {
        // Ring occupancy keeps head_ + offset_from_head below 2 * Capacity,
        // as ring_index requires.
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
