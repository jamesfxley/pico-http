// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <string>

#include "pico_http/http/headers.hpp"

using pico_http::HTTPHeaders;

TEST(HttpHeadersTest, StoresAndOverridesCaseInsensitively) {
    HTTPHeaders headers;
    headers.set("Content-Type", "text/plain");
    headers.set("content-length", "42");

    EXPECT_TRUE(headers.has("CONTENT-TYPE"));
    EXPECT_EQ(headers.get("content-length"), "42");

    headers.set("Content-Length", "7");
    EXPECT_EQ(headers.get("CONTENT-LENGTH"), "7");

    headers.clear();
    EXPECT_FALSE(headers.has("content-type"));
    EXPECT_EQ(headers.size(), 0U);
}

TEST(HttpHeadersTest, AddCombinesRepeatedNamesAsList) {
    HTTPHeaders headers;
    EXPECT_TRUE(headers.add("Accept", "text/html"));
    EXPECT_TRUE(headers.add("accept", "text/plain"));
    EXPECT_EQ(headers.get("Accept"), "text/html, text/plain");
    EXPECT_EQ(headers.size(), 1U);

    EXPECT_TRUE(headers.add("X-Empty", ""));
    EXPECT_TRUE(headers.add("X-Empty", "v"));
    EXPECT_EQ(headers.get("X-Empty"), "v");
}

TEST(HttpHeadersTest, AddFailsWholeWhenCombinedValueOverflows) {
    HTTPHeaders headers;
    const std::string long_value(HTTPHeaders::MAX_HEADER_VALUE - 4, 'a');
    ASSERT_TRUE(headers.add("X", long_value));
    // Overflow leaves the combined value unchanged (BR-7).
    EXPECT_FALSE(headers.add("X", "bbbb"));
    EXPECT_EQ(headers.get("X"), long_value);
}

TEST(HttpHeadersTest, StoresNameAndValueOfExactlyTheAdvertisedCap) {
    HTTPHeaders headers;
    const std::string name(HTTPHeaders::MAX_HEADER_NAME, 'n');
    const std::string value(HTTPHeaders::MAX_HEADER_VALUE, 'v');

    EXPECT_TRUE(headers.set(name, value));
    EXPECT_EQ(headers.get(name), value);
    EXPECT_EQ(headers.size(), 1U);
}

TEST(HttpHeadersTest, RejectsOverlongNameOrValueWithoutStoringAnything) {
    // A rejected store must leave no truncated entry (BR-7).
    HTTPHeaders headers;
    const std::string long_name(HTTPHeaders::MAX_HEADER_NAME + 1, 'n');
    const std::string long_value(HTTPHeaders::MAX_HEADER_VALUE + 1, 'v');

    EXPECT_FALSE(headers.set(long_name, "v"));
    EXPECT_EQ(headers.size(), 0U);
    EXPECT_FALSE(headers.has(long_name));

    EXPECT_FALSE(headers.set("X", long_value));
    EXPECT_EQ(headers.size(), 0U);
    EXPECT_FALSE(headers.has("X"));

    EXPECT_FALSE(headers.add(long_name, "v"));
    EXPECT_EQ(headers.size(), 0U);
}

TEST(HttpHeadersTest, RejectedOverwriteLeavesTheStoredValueIntact) {
    // Truncation before failure would corrupt the existing value.
    HTTPHeaders headers;
    const std::string long_value(HTTPHeaders::MAX_HEADER_VALUE + 1, 'v');
    ASSERT_TRUE(headers.set("X", "original"));

    EXPECT_FALSE(headers.set("X", long_value));
    EXPECT_EQ(headers.get("X"), "original");
    EXPECT_EQ(headers.size(), 1U);
}

TEST(HttpHeadersTest, RejectedAddIntoAnEmptyValueLeavesItEmpty) {
    HTTPHeaders headers;
    const std::string long_value(HTTPHeaders::MAX_HEADER_VALUE + 1, 'v');
    ASSERT_TRUE(headers.add("X-Empty", ""));

    EXPECT_FALSE(headers.add("X-Empty", long_value));
    EXPECT_EQ(headers.get("X-Empty"), "");
    EXPECT_EQ(headers.size(), 1U);
}

TEST(HttpHeadersTest, HasSeesHeaderWithEmptyValue) {
    HTTPHeaders headers;
    headers.set("X-Flag", "");
    EXPECT_TRUE(headers.has("X-Flag"));
    EXPECT_FALSE(headers.has("X-Other"));
}
