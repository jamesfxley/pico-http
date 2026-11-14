// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "pico_http/http/detail/keep_alive.hpp"
#include "pico_http/http/detail/parsing.hpp"

using pico_http::detail::ascii_to_lower;
using pico_http::detail::header_contains_token;
using pico_http::detail::iequals;
using pico_http::detail::keep_alive_allowed;
using pico_http::detail::trim_ows;

namespace {

TEST(ParsingTest, AsciiToLowerLeavesNonAlphabeticBytesAlone) {
    EXPECT_EQ(ascii_to_lower('A'), 'a');
    EXPECT_EQ(ascii_to_lower('Z'), 'z');
    EXPECT_EQ(ascii_to_lower('a'), 'a');
    EXPECT_EQ(ascii_to_lower('5'), '5');
    EXPECT_EQ(ascii_to_lower('-'), '-');
    // Bytes outside A-Z must not shift, including the adjacent '@' and '['.
    EXPECT_EQ(ascii_to_lower('@'), '@');
    EXPECT_EQ(ascii_to_lower('['), '[');
}

TEST(ParsingTest, IequalsComparesCaseInsensitivelyAndByLength) {
    EXPECT_TRUE(iequals("Content-Length", "content-length"));
    EXPECT_TRUE(iequals("", ""));
    EXPECT_FALSE(iequals("close", "closed"));
    EXPECT_FALSE(iequals("close", "clos"));
    EXPECT_FALSE(iequals("a", "b"));
}

TEST(ParsingTest, TrimOwsStripsSpacesAndTabsFromBothEnds) {
    EXPECT_EQ(trim_ows("  value  "), "value");
    EXPECT_EQ(trim_ows("\t\tvalue\t"), "value");
    EXPECT_EQ(trim_ows("value"), "value");
    EXPECT_EQ(trim_ows(""), "");
    // An all-whitespace input catches underflow.
    EXPECT_EQ(trim_ows(" \t \t "), "");
    EXPECT_EQ(trim_ows("  a b  "), "a b");
}

TEST(ParsingTest, HeaderContainsTokenMatchesWholeListElements) {
    EXPECT_TRUE(header_contains_token("close", "close"));
    EXPECT_TRUE(header_contains_token("keep-alive, close", "close"));
    EXPECT_TRUE(header_contains_token("  Close  ", "close"));
    EXPECT_TRUE(header_contains_token("a,\tclose\t,b", "close"));
    // A token must match a whole list element.
    EXPECT_FALSE(header_contains_token("closed", "close"));
    EXPECT_FALSE(header_contains_token("not-close", "close"));
    EXPECT_FALSE(header_contains_token("", "close"));
    EXPECT_FALSE(header_contains_token(",,,", "close"));
    EXPECT_TRUE(header_contains_token(",,close,,", "close"));
}

// Persistence defaults depend on version (NR-2).
TEST(KeepAliveTest, DisabledAlwaysCloses) {
    EXPECT_FALSE(keep_alive_allowed(false, "keep-alive", "HTTP/1.1", "", 0, 0));
}

TEST(KeepAliveTest, Http11PersistsUnlessTheRequestSaysClose) {
    EXPECT_TRUE(keep_alive_allowed(true, "", "HTTP/1.1", "", 0, 0));
    EXPECT_FALSE(keep_alive_allowed(true, "close", "HTTP/1.1", "", 0, 0));
    // Generated error responses use the HTTP/1.1 default for an absent
    // version.
    EXPECT_TRUE(keep_alive_allowed(true, "", "", "", 0, 0));
}

TEST(KeepAliveTest, Http10ClosesUnlessTheRequestAsksToPersist) {
    EXPECT_FALSE(keep_alive_allowed(true, "", "HTTP/1.0", "", 0, 0));
    EXPECT_TRUE(keep_alive_allowed(true, "keep-alive", "HTTP/1.0", "", 0, 0));
    EXPECT_FALSE(
        keep_alive_allowed(true, "keep-alive, close", "HTTP/1.0", "", 0, 0));
}

TEST(KeepAliveTest, RequestCapClosesOnceReached) {
    EXPECT_TRUE(keep_alive_allowed(true, "", "HTTP/1.1", "", 3, 4));
    EXPECT_FALSE(keep_alive_allowed(true, "", "HTTP/1.1", "", 4, 4));
    EXPECT_FALSE(keep_alive_allowed(true, "", "HTTP/1.1", "", 5, 4));
    // A zero cap means no cap.
    EXPECT_TRUE(keep_alive_allowed(true, "", "HTTP/1.1", "", 1000, 0));
}

TEST(KeepAliveTest, ResponseConnectionHeaderOverridesTheVersionDefault) {
    EXPECT_FALSE(keep_alive_allowed(true, "", "HTTP/1.1", "close", 0, 0));
    EXPECT_TRUE(keep_alive_allowed(true, "", "HTTP/1.0", "keep-alive", 0, 0));
    EXPECT_TRUE(keep_alive_allowed(true, "", "HTTP/1.1", "Upgrade", 0, 0));
}

TEST(KeepAliveTest, RequestCapOutranksTheResponseConnectionHeader) {
    // The request cap applies after the response header, so a handler cannot
    // override the bound.
    EXPECT_FALSE(keep_alive_allowed(true, "", "HTTP/1.1", "keep-alive", 9, 4));
    EXPECT_FALSE(keep_alive_allowed(true, "", "HTTP/1.1", "keep-alive", 4, 4));
    EXPECT_TRUE(keep_alive_allowed(true, "", "HTTP/1.1", "keep-alive", 3, 4));
    EXPECT_TRUE(keep_alive_allowed(true, "", "HTTP/1.1", "keep-alive", 99, 0));
}

}  // namespace
