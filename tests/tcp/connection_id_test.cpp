// SPDX-License-Identifier: MIT
#include "pico_http/tcp/connection_id.hpp"

#include <gtest/gtest.h>

using pico_http::tcp::ConnectionId;

TEST(ConnectionIdTest, DefaultIsInvalidSentinel) {
    const ConnectionId id;
    EXPECT_FALSE(id.valid());
    EXPECT_EQ(id.raw(), 0U);
}

TEST(ConnectionIdTest, PacksAndUnpacksFields) {
    const ConnectionId id = ConnectionId::make(0xABU, 0x1234U, 0xCDU);
    EXPECT_EQ(id.slot(), 0xABU);
    EXPECT_EQ(id.generation(), 0x1234U);
    EXPECT_EQ(id.tag(), 0xCDU);
    EXPECT_TRUE(id.valid());
}

TEST(ConnectionIdTest, RawRoundTrips) {
    const ConnectionId id = ConnectionId::make(7U, 9U, 3U);
    EXPECT_EQ(ConnectionId::from_raw(id.raw()), id);
}

TEST(ConnectionIdTest, Slot0Generation1IsValid) {
    // The allocator never issues generation 0, so slot 0 is still usable.
    const ConnectionId id = ConnectionId::make(0U, 1U, 0U);
    EXPECT_TRUE(id.valid());
    EXPECT_NE(id.raw(), 0U);
}

TEST(ConnectionIdTest, DiffersByAnyField) {
    const ConnectionId base = ConnectionId::make(1U, 1U, 0U);
    EXPECT_NE(base, ConnectionId::make(2U, 1U, 0U));
    EXPECT_NE(base, ConnectionId::make(1U, 2U, 0U));
    EXPECT_NE(base, ConnectionId::make(1U, 1U, 1U));
}
