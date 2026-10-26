// SPDX-License-Identifier: MIT
#include "pico_http/tcp/detail/slot_table.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "pico_http/tcp/connection_id.hpp"

using pico_http::tcp::ConnectionId;
using pico_http::tcp::detail::SlotTable;

namespace {

struct Payload {
    int value = 0;
};

}  // namespace

TEST(SlotTableTest, AcquireReturnsDistinctLiveSlots) {
    SlotTable<Payload, 3> table;
    const ConnectionId first = table.acquire();
    const ConnectionId second = table.acquire();
    ASSERT_TRUE(first.valid());
    ASSERT_TRUE(second.valid());
    EXPECT_NE(first, second);
    EXPECT_EQ(table.active_count(), 2U);

    Payload* first_slot = table.get(first);
    Payload* second_slot = table.get(second);
    ASSERT_NE(first_slot, nullptr);
    ASSERT_NE(second_slot, nullptr);
    first_slot->value = 10;
    second_slot->value = 20;
    EXPECT_EQ(first_slot->value, 10);
    EXPECT_EQ(second_slot->value, 20);
}

TEST(SlotTableTest, FullTableReturnsInvalidId) {
    SlotTable<Payload, 2> table;
    EXPECT_TRUE(table.acquire().valid());
    EXPECT_TRUE(table.acquire().valid());
    EXPECT_TRUE(table.full());
    const ConnectionId overflow = table.acquire();
    EXPECT_FALSE(overflow.valid());
    EXPECT_EQ(table.get(overflow), nullptr);
}

TEST(SlotTableTest, ReleaseFreesCapacityAndIsIdempotent) {
    SlotTable<Payload, 2> table;
    const ConnectionId first = table.acquire();
    EXPECT_TRUE(table.release(first));
    EXPECT_EQ(table.active_count(), 0U);
    // Second release of the same (now stale) id is a no-op, not a fault.
    EXPECT_FALSE(table.release(first));
}

TEST(SlotTableTest, StaleHandleAfterReuseDoesNotResolve) {
    // The core ABA guarantee (AR-4): reusing a slot invalidates old handles.
    SlotTable<Payload, 1> table;
    const ConnectionId first = table.acquire();
    Payload* first_slot = table.get(first);
    ASSERT_NE(first_slot, nullptr);
    first_slot->value = 111;
    table.release(first);

    const ConnectionId second =
        table.acquire();  // same physical slot, new generation
    ASSERT_TRUE(second.valid());
    EXPECT_EQ(second.slot(), first.slot());
    EXPECT_NE(second, first);

    EXPECT_EQ(table.get(first), nullptr);   // stale handle inert
    ASSERT_NE(table.get(second), nullptr);  // fresh handle live
    // Resolving through the fresh id reaches the (reused) slot; the payload is
    // deliberately not cleared on reuse — the caller reinitialises it.
    Payload* reused = table.get(second);
    ASSERT_NE(reused, nullptr);
    reused->value = 222;
    EXPECT_EQ(table.get(first), nullptr);  // still inert after the write
}

TEST(SlotTableTest, GenerationNeverIssuesZeroAcrossTheWrap) {
    // 70000 > 65535 forces the 16-bit generation to wrap. It must skip 0 so no
    // live id ever encodes to the invalid sentinel. (Within one 65535-reuse
    // window an old id stays inert; aliasing after a full wrap is the
    // documented ABA bound, not tested here.)
    SlotTable<Payload, 1> table;
    for (int i = 0; i < 70000; ++i) {
        const ConnectionId id = table.acquire();
        ASSERT_TRUE(id.valid());
        ASSERT_NE(id.raw(), 0U);
        ASSERT_NE(id.generation(), 0U);
        table.release(id);
    }
}

TEST(SlotTableTest, HighWatermarkTracksPeakOccupancy) {
    SlotTable<Payload, 4> table;
    const ConnectionId first = table.acquire();
    const ConnectionId second = table.acquire();
    const ConnectionId third = table.acquire();
    table.release(second);
    EXPECT_EQ(table.active_count(), 2U);
    EXPECT_EQ(table.high_watermark(), 3U);  // peak, not current
    (void)first;
    (void)third;
}

TEST(SlotTableTest, ForEachActiveVisitsOnlyLiveSlots) {
    SlotTable<Payload, 4> table;
    const ConnectionId first = table.acquire();
    const ConnectionId second = table.acquire();
    const ConnectionId third = table.acquire();
    Payload* first_slot = table.get(first);
    Payload* second_slot = table.get(second);
    Payload* third_slot = table.get(third);
    ASSERT_NE(first_slot, nullptr);
    ASSERT_NE(second_slot, nullptr);
    ASSERT_NE(third_slot, nullptr);
    first_slot->value = 1;
    second_slot->value = 2;
    third_slot->value = 3;
    table.release(second);

    std::vector<int> seen;
    table.for_each_active([&](ConnectionId id, Payload& payload) {
        EXPECT_TRUE(id.valid());
        seen.push_back(payload.value);
    });
    // b was released; only a and c remain.
    EXPECT_EQ(seen.size(), 2U);
}

TEST(SlotTableTest, IdAtMatchesAcquireAndClearsOnRelease) {
    SlotTable<Payload, 2> table;
    const ConnectionId first = table.acquire();
    EXPECT_EQ(table.id_at(first.slot()), first);
    table.release(first);
    EXPECT_FALSE(table.id_at(first.slot()).valid());
}
