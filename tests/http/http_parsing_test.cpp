// SPDX-License-Identifier: MIT
// Unit tests for the byte-scanning helpers and the keep-alive policy. Both were
// previously reached only through the service tests, which exercise the common
// paths but not the boundaries each helper is responsible for.
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
    // Bytes either side of the A-Z range must not be shifted: '@' precedes 'A'
    // and '[' follows 'Z'.
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
    // All-whitespace collapses to empty rather than underflowing.
    EXPECT_EQ(trim_ows(" \t \t "), "");
    // Interior whitespace is content, not OWS.
    EXPECT_EQ(trim_ows("  a b  "), "a b");
}

TEST(ParsingTest, HeaderContainsTokenMatchesWholeListElements) {
    EXPECT_TRUE(header_contains_token("close", "close"));
    EXPECT_TRUE(header_contains_token("keep-alive, close", "close"));
    EXPECT_TRUE(header_contains_token("  Close  ", "close"));
    EXPECT_TRUE(header_contains_token("a,\tclose\t,b", "close"));
    // A token must be a whole element: a substring of one does not count.
    EXPECT_FALSE(header_contains_token("closed", "close"));
    EXPECT_FALSE(header_contains_token("not-close", "close"));
    EXPECT_FALSE(header_contains_token("", "close"));
    // Empty elements from repeated separators are skipped, not matched.
    EXPECT_FALSE(header_contains_token(",,,", "close"));
    EXPECT_TRUE(header_contains_token(",,close,,", "close"));
}

// NR-2: version-dependent persistence defaults.
TEST(KeepAliveTest, DisabledAlwaysCloses) {
    EXPECT_FALSE(keep_alive_allowed(false, "keep-alive", "HTTP/1.1", "", 0, 0));
}

TEST(KeepAliveTest, Http11PersistsUnlessTheRequestSaysClose) {
    EXPECT_TRUE(keep_alive_allowed(true, "", "HTTP/1.1", "", 0, 0));
    EXPECT_FALSE(keep_alive_allowed(true, "close", "HTTP/1.1", "", 0, 0));
    // An absent version is treated as 1.1 (the service supplies 1.1 for its
    // own generated error responses).
    EXPECT_TRUE(keep_alive_allowed(true, "", "", "", 0, 0));
}

TEST(KeepAliveTest, Http10ClosesUnlessTheRequestAsksToPersist) {
    EXPECT_FALSE(keep_alive_allowed(true, "", "HTTP/1.0", "", 0, 0));
    EXPECT_TRUE(keep_alive_allowed(true, "keep-alive", "HTTP/1.0", "", 0, 0));
    // "close" wins when a client sends both.
    EXPECT_FALSE(
        keep_alive_allowed(true, "keep-alive, close", "HTTP/1.0", "", 0, 0));
}

TEST(KeepAliveTest, RequestCapClosesOnceReached) {
    EXPECT_TRUE(keep_alive_allowed(true, "", "HTTP/1.1", "", 3, 4));
    EXPECT_FALSE(keep_alive_allowed(true, "", "HTTP/1.1", "", 4, 4));
    EXPECT_FALSE(keep_alive_allowed(true, "", "HTTP/1.1", "", 5, 4));
    // A cap of zero means no cap.
    EXPECT_TRUE(keep_alive_allowed(true, "", "HTTP/1.1", "", 1000, 0));
}

TEST(KeepAliveTest, ResponseConnectionHeaderOverridesTheVersionDefault) {
    // Overrides the 1.1 default...
    EXPECT_FALSE(keep_alive_allowed(true, "", "HTTP/1.1", "close", 0, 0));
    // ...and the 1.0 default.
    EXPECT_TRUE(keep_alive_allowed(true, "", "HTTP/1.0", "keep-alive", 0, 0));
    // A response header naming neither token leaves the decision unchanged.
    EXPECT_TRUE(keep_alive_allowed(true, "", "HTTP/1.1", "Upgrade", 0, 0));
}

TEST(KeepAliveTest, RequestCapOutranksTheResponseConnectionHeader) {
    // The cap bounds how long one peer holds a slot, so it is applied after the
    // response header rather than before: a handler setting keep-alive cannot
    // switch off a resource bound it may not know exists (CXX-12.7).
    EXPECT_FALSE(keep_alive_allowed(true, "", "HTTP/1.1", "keep-alive", 9, 4));
    // At the cap exactly, not merely past it.
    EXPECT_FALSE(keep_alive_allowed(true, "", "HTTP/1.1", "keep-alive", 4, 4));
    // One below the cap the handler's keep-alive still stands.
    EXPECT_TRUE(keep_alive_allowed(true, "", "HTTP/1.1", "keep-alive", 3, 4));
    // A zero cap means uncapped, so the handler's keep-alive stands.
    EXPECT_TRUE(keep_alive_allowed(true, "", "HTTP/1.1", "keep-alive", 99, 0));
}

}  // namespace
