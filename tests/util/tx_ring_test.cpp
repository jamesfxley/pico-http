// SPDX-License-Identifier: MIT
#include "pico_http/util/tx_ring.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <numeric>
#include <span>
#include <vector>

using pico_http::TxRing;

namespace {

template <std::size_t Capacity>
std::vector<std::uint8_t> drain_unsent(TxRing<Capacity>& ring) {
    std::vector<std::uint8_t> out;
    while (ring.unsent() > 0) {
        auto run = ring.peek();
        out.insert(out.end(), run.begin(), run.end());
        ring.mark_sent(run.size());
    }
    return out;
}

std::vector<std::uint8_t> iota_bytes(std::size_t n, std::uint8_t start = 0) {
    std::vector<std::uint8_t> bytes_out(n);
    std::iota(bytes_out.begin(), bytes_out.end(), start);
    return bytes_out;
}

}  // namespace

TEST(TxRingTest, WriteReportsCreditAndCapsAtCapacity) {
    TxRing<8> ring;
    EXPECT_EQ(ring.free_space(), 8U);

    auto data = iota_bytes(12);
    EXPECT_EQ(ring.write(data), 8U);  // only 8 fit
    EXPECT_EQ(ring.size(), 8U);
    EXPECT_EQ(ring.unsent(), 8U);
    EXPECT_EQ(ring.free_space(), 0U);
    EXPECT_EQ(ring.write(data), 0U);  // full: no credit
}

TEST(TxRingTest, PeekReturnsWrittenBytesInOrder) {
    TxRing<8> ring;
    auto data = iota_bytes(5, 10);
    ring.write(data);

    auto peeked = ring.peek();
    ASSERT_EQ(peeked.size(), 5U);
    const std::vector<std::uint8_t> got(peeked.begin(), peeked.end());
    EXPECT_EQ(got, data);
}

TEST(TxRingTest, BytesStayResidentUntilAckedNotWhenSent) {
    TxRing<8> ring;
    ring.write(iota_bytes(6));

    ring.mark_sent(6);
    EXPECT_EQ(ring.in_flight(), 6U);
    EXPECT_EQ(ring.unsent(), 0U);
    EXPECT_EQ(ring.free_space(), 2U);  // still occupied until acked

    ring.ack(4);
    EXPECT_EQ(ring.in_flight(), 2U);
    EXPECT_EQ(ring.free_space(), 6U);

    ring.ack(2);
    EXPECT_TRUE(ring.empty());
    EXPECT_EQ(ring.free_space(), 8U);
}

TEST(TxRingTest, WriteWrapsAroundPhysicalBuffer) {
    TxRing<8> ring;
    // Fill, send, ack the first 6 to advance head near the end.
    ring.write(iota_bytes(6, 0));
    ring.mark_sent(6);
    ring.ack(6);
    EXPECT_EQ(ring.free_space(), 8U);

    // This write wraps: 2 bytes at [6,8) and 4 bytes at [0,4).
    auto data = iota_bytes(6, 100);
    EXPECT_EQ(ring.write(data), 6U);

    // peek() only returns the contiguous run to the wrap point.
    auto first = ring.peek();
    EXPECT_EQ(first.size(), 2U);

    auto drained = drain_unsent(ring);
    EXPECT_EQ(drained, data);
}

TEST(TxRingTest, PartialAckThenRefillUsesFreedSpace) {
    TxRing<4> ring;
    ring.write(iota_bytes(4, 0));
    ring.mark_sent(4);
    ring.ack(2);
    EXPECT_EQ(ring.free_space(), 2U);

    std::array<std::uint8_t, 3> more{50, 51, 52};
    EXPECT_EQ(ring.write(more), 2U);  // only 2 free
    EXPECT_EQ(ring.unsent(), 2U);

    auto peeked = ring.peek();
    const std::vector<std::uint8_t> got(peeked.begin(), peeked.end());
    EXPECT_EQ(got, (std::vector<std::uint8_t>{50, 51}));
}

// The ring's core contract (and top item in the design risk register): bytes
// handed to the transport with mark_sent() must stay valid *at their physical
// location* until ack(), because lwIP NOCOPY retransmissions re-read those
// pointers. This drives the ring through many partial acks and refills — the
// pattern of a large streamed download — capturing each in-flight byte's
// address+value at send time and asserting nothing overwrites it before it is
// acked. A regression here is invisible on a clean LAN (no retransmits) but
// corrupts every retransmitted segment. Guards against the ack()-head bug.
TEST(TxRingTest, InFlightBytesStayStableUntilAcked) {
    TxRing<8> ring;

    struct Pending {
        const std::uint8_t* addr;
        std::uint8_t value;
    };

    std::deque<Pending> inflight;
    std::uint8_t next = 0;

    for (int iter = 0; iter < 50; ++iter) {
        while (ring.free_space() > 0) {
            std::uint8_t byte = next++;
            ring.write(std::span<const std::uint8_t>(&byte, 1));
        }
        while (ring.unsent() > 0) {
            auto run = ring.peek();
            for (const std::uint8_t& byte : run) {
                inflight.push_back({&byte, byte});
            }
            ring.mark_sent(run.size());
        }
        // Every still-unacked byte must read back exactly what was sent.
        for (const Pending& pending : inflight) {
            ASSERT_EQ(*pending.addr, pending.value)
                << "in-flight byte clobbered before ack (iter " << iter << ")";
        }
        const std::size_t to_ack = std::min<std::size_t>(3, ring.in_flight());
        ring.ack(to_ack);
        for (std::size_t k = 0; k < to_ack; ++k) {
            inflight.pop_front();
        }
    }
}

TEST(TxRingTest, OverAckIsClampedToInFlight) {
    TxRing<4> ring;
    ring.write(iota_bytes(2, 0));
    ring.mark_sent(2);
    ring.ack(10);  // clamp, no underflow
    EXPECT_TRUE(ring.empty());
    EXPECT_EQ(ring.free_space(), 4U);
}

TEST(TxRingTest, ClearResetsAllRegions) {
    TxRing<8> ring;
    ring.write(iota_bytes(5));
    ring.mark_sent(3);
    ring.clear();
    EXPECT_TRUE(ring.empty());
    EXPECT_EQ(ring.unsent(), 0U);
    EXPECT_EQ(ring.in_flight(), 0U);
    EXPECT_EQ(ring.free_space(), 8U);
}
