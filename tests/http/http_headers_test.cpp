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
    EXPECT_EQ(headers.size(), 1U);  // one entry, combined value

    // Combining into an empty value takes the new value without a leading
    // separator.
    EXPECT_TRUE(headers.add("X-Empty", ""));
    EXPECT_TRUE(headers.add("X-Empty", "v"));
    EXPECT_EQ(headers.get("X-Empty"), "v");
}

TEST(HttpHeadersTest, AddFailsWholeWhenCombinedValueOverflows) {
    HTTPHeaders headers;
    const std::string long_value(HTTPHeaders::MAX_HEADER_VALUE - 4, 'a');
    ASSERT_TRUE(headers.add("X", long_value));
    // The combined value would overflow the fixed storage: the add fails and
    // the stored value is unchanged — never partially updated (BR-7).
    EXPECT_FALSE(headers.add("X", "bbbb"));
    EXPECT_EQ(headers.get("X"), long_value);
}

TEST(HttpHeadersTest, StoresNameAndValueOfExactlyTheAdvertisedCap) {
    // MAX_HEADER_* is the usable length, not the raw array extent —
    // pico_util::FixedString spends one byte of that on its terminator — so a
    // caller sizing a buffer against it gets the whole value back.
    HTTPHeaders headers;
    const std::string name(HTTPHeaders::MAX_HEADER_NAME, 'n');
    const std::string value(HTTPHeaders::MAX_HEADER_VALUE, 'v');

    EXPECT_TRUE(headers.set(name, value));
    EXPECT_EQ(headers.get(name), value);
    EXPECT_EQ(headers.size(), 1U);
}

TEST(HttpHeadersTest, RejectsOverlongNameOrValueWithoutStoringAnything) {
    // A rejected store must leave no trace: committing a truncated entry and
    // reporting failure would put storage that silently disagrees with the
    // request underneath the caller's 431 (BR-7).
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
    // set() on an existing name used to assign straight into the fixed storage,
    // truncating in place before returning false.
    HTTPHeaders headers;
    const std::string long_value(HTTPHeaders::MAX_HEADER_VALUE + 1, 'v');
    ASSERT_TRUE(headers.set("X", "original"));

    EXPECT_FALSE(headers.set("X", long_value));
    EXPECT_EQ(headers.get("X"), "original");
    EXPECT_EQ(headers.size(), 1U);
}

TEST(HttpHeadersTest, RejectedAddIntoAnEmptyValueLeavesItEmpty) {
    // add()'s empty-value branch takes the same in-place assign() path.
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
    EXPECT_TRUE(headers.has("X-Flag"));  // present, value empty
    EXPECT_FALSE(headers.has("X-Other"));
}
