// SPDX-License-Identifier: MIT
#include "pico_http/tcp/detail/timeout_scheduler.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

using pico_http::tcp::detail::time_reached;
using pico_http::tcp::detail::TimeoutScheduler;

namespace {

constexpr std::uint8_t KIND_HEADER = 1;
constexpr std::uint8_t KIND_IDLE = 3;

template <std::size_t N>
std::vector<std::pair<std::size_t, std::uint8_t>> collect_expired(
    TimeoutScheduler<N>& sched, std::uint32_t now) {
    std::vector<std::pair<std::size_t, std::uint8_t>> fired;
    sched.for_each_expired(now, [&](std::size_t i, std::uint8_t kind) {
        fired.emplace_back(i, kind);
    });
    return fired;
}

}  // namespace

TEST(TimeReachedTest, BasicOrdering) {
    EXPECT_FALSE(time_reached(100U, 200U));
    EXPECT_TRUE(time_reached(200U, 200U));
    EXPECT_TRUE(time_reached(201U, 200U));
}

TEST(TimeReachedTest, CorrectAcrossThe32BitWrap) {
    constexpr std::uint32_t MAX = std::numeric_limits<std::uint32_t>::max();
    const std::uint32_t deadline = MAX - 5U;  // near the top of the range
    const std::uint32_t now = 10U;            // wrapped past zero

    // 10 is 16 ms after (MAX - 5), so the deadline has been reached.
    EXPECT_TRUE(time_reached(now, deadline));
    // Just before the deadline, still pending.
    EXPECT_FALSE(time_reached(MAX - 10U, deadline));
}

TEST(TimeoutSchedulerTest, FiresOnlyExpiredArmedSlots) {
    TimeoutScheduler<4> sched;
    sched.arm(0, 100U, KIND_HEADER);
    sched.arm(2, 300U, KIND_IDLE);

    auto fired = collect_expired(sched, 150U);
    ASSERT_EQ(fired.size(), 1U);
    EXPECT_EQ(fired[0].first, 0U);
    EXPECT_EQ(fired[0].second, KIND_HEADER);
}

TEST(TimeoutSchedulerTest, FiredSlotIsDisarmedAndDoesNotRefire) {
    TimeoutScheduler<2> sched;
    sched.arm(0, 100U, KIND_HEADER);
    EXPECT_TRUE(sched.armed(0));

    auto first = collect_expired(sched, 100U);
    EXPECT_EQ(first.size(), 1U);
    EXPECT_FALSE(sched.armed(0));

    auto second = collect_expired(sched, 500U);
    EXPECT_TRUE(second.empty());
}

TEST(TimeoutSchedulerTest, DisarmPreventsFiring) {
    TimeoutScheduler<2> sched;
    sched.arm(1, 100U, KIND_IDLE);
    sched.disarm(1);
    EXPECT_TRUE(collect_expired(sched, 1000U).empty());
}

TEST(TimeoutSchedulerTest, ReArmDuringExpiryScanReschedules) {
    TimeoutScheduler<2> sched;
    sched.arm(0, 100U, KIND_HEADER);

    // The callback re-arms slot 0 further out; it must not fire twice in this
    // pass and must be pending for a later now.
    int calls = 0;
    sched.for_each_expired(150U, [&](std::size_t i, std::uint8_t) {
        ++calls;
        sched.arm(i, 400U, KIND_IDLE);
    });
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(sched.armed(0));
    EXPECT_TRUE(collect_expired(sched, 400U).size() == 1U);
}
