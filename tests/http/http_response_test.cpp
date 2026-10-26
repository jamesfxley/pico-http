// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "pico_http/config/profile.hpp"
#include "pico_http/http/headers.hpp"
#include "pico_http/http/message.hpp"
#include "pico_util/testing/allocation_counter.hpp"

// Exactly one translation unit per test executable may include this, and this
// is that unit: the replaceable allocation functions cannot be inline, so a
// second include is a duplicate-symbol link error.
#include "pico_util/testing/allocation_operators.ipp"  // IWYU pragma: keep, it defines the replaced operators

using pico_http::HTTPResponse;

using pico_http::HTTPHeaders;
using pico_http::StaticBody;

TEST(HttpResponseTest, SetsBodyAndContentTypeFromString) {
    HTTPResponse response;
    response.set_body("payload", "text/plain");

    ASSERT_EQ(response.body.size(), 7U);
    const std::string body(response.body.begin(), response.body.end());
    EXPECT_EQ(body, "payload");
    EXPECT_EQ(response.headers.get("Content-Type"), "text/plain");
}

TEST(HttpResponseTest, SetsBinaryBodyAndContentType) {
    HTTPResponse response;
    std::vector<std::uint8_t> bytes{1, 2, 3, 4};
    response.set_body(bytes, "application/octet-stream");

    EXPECT_EQ(response.body, bytes);
    EXPECT_EQ(response.headers.get("content-type"), "application/octet-stream");
}

// BR-14 at the source: a copied body over the active profile's cap is refused
// before anything is allocated, so AR-1's bound holds at peak as well as
// between requests. The flag is what the service answers with its fixed 500.
TEST(HttpResponseTest, SetBodyOverTheCapAllocatesNothing) {
    constexpr std::size_t CAP =
        PICO_HTTP_ACTIVE_PROFILE::Limits::MAX_BUFFERED_BODY;
    HTTPResponse res;
    res.set_body(std::string(16, 'a'));
    const std::string over(CAP + 1, 'x');
    HTTPResponse::BodyResult result{};
    {
        // Nothing allocated, not only nothing held afterwards.
        const pico_util::testing::AllocationGuard guard;
        result = res.set_body(over);
        EXPECT_EQ(pico_util::testing::AllocationGuard::count(), 0U);
    }
    EXPECT_EQ(result, HTTPResponse::BodyResult::REFUSED);
    EXPECT_TRUE(res.body_refused);
    EXPECT_EQ(res.body.capacity(), 0U);

    const std::vector<std::uint8_t> bytes(CAP + 1, 0x55);
    HTTPResponse binary;
    EXPECT_EQ(binary.set_body(std::span<const std::uint8_t>(bytes)),
              HTTPResponse::BodyResult::REFUSED);
    EXPECT_TRUE(binary.body_refused);
    EXPECT_EQ(binary.body.capacity(), 0U);
}

// A body at the cap is copied, and a later body within it clears a refusal.
TEST(HttpResponseTest, SetBodyAtTheCapIsCopiedAndClearsARefusal) {
    constexpr std::size_t CAP =
        PICO_HTTP_ACTIVE_PROFILE::Limits::MAX_BUFFERED_BODY;
    HTTPResponse res;
    res.set_body(std::string(CAP + 1, 'x'));
    ASSERT_TRUE(res.body_refused);
    res.set_body(std::string(CAP, 'x'));
    EXPECT_FALSE(res.body_refused);
    EXPECT_EQ(res.body.size(), CAP);
}

// set_body names what it did: the body set, the body set without its
// Content-Type (the header value over its cap), or the body refused (BR-14).
TEST(HttpResponseTest, SetBodyReportsWhatItDid) {
    HTTPResponse res;
    EXPECT_EQ(res.set_body("ok"), HTTPResponse::BodyResult::SET);
    const std::string long_type(HTTPHeaders::MAX_HEADER_VALUE + 1, 't');
    EXPECT_EQ(res.set_body("ok", long_type),
              HTTPResponse::BodyResult::CONTENT_TYPE_DROPPED);
    EXPECT_EQ(res.body.size(), 2U);
}

TEST(HttpResponseTest, ResetReleasesTheBodyBufferInsteadOfKeepingCapacity) {
    HTTPResponse response;
    response.reserve_body(4096);
    ASSERT_GE(response.body.capacity(), 4096U);

    response.reset();

    // Emptiness is not enough — the capacity has to be gone. A pooled response
    // that only cleared would hold its buffer for the life of the pool, heap
    // no connection owns and outside AR-1's bound.
    EXPECT_EQ(response.body.capacity(), 0U);
    EXPECT_TRUE(response.body.empty());
}

// AR-11. The reason set_body_static takes a type rather than a span: a span is
// what HTTPRequestView::body is, so the trap compiled and transmitted from
// parser storage the connection had already recycled. These assertions fail if
// StaticBody ever gains a conversion, which is the only way the trap returns.
TEST(HttpResponseTest, ARequestViewCannotBecomeAStaticBody) {
    static_assert(
        !std::convertible_to<std::span<const std::uint8_t>, StaticBody>);
    static_assert(!std::convertible_to<std::string_view, StaticBody>);
    static_assert(
        !std::is_constructible_v<StaticBody, std::span<const std::uint8_t>>);
    // The named factory is the way in, and naming it is the caller's
    // assertion. Not constexpr: the char-to-byte reinterpretation is not
    // allowed in constant evaluation.
    EXPECT_EQ(StaticBody::outliving_response(std::string_view("abc")).size(),
              3U);
    EXPECT_EQ(StaticBody::outliving_response(std::string_view("xy")).size(),
              2U);
}

TEST(HttpResponseTest, ResetRestoresEveryFieldToItsDefault) {
    HTTPResponse response;
    response.status_code = HTTPResponse::STATUS_NOT_FOUND;
    ASSERT_TRUE(response.status_text.assign("Not Found"));
    response.headers.set("X-Trace", "abc");
    response.set_body_static(StaticBody::outliving_response("cached"),
                             "text/plain");

    response.reset();

    EXPECT_EQ(response.status_code, HTTPResponse::STATUS_OK);
    EXPECT_EQ(response.status_text.view(), "OK");
    EXPECT_EQ(response.headers.size(), 0U);
    EXPECT_EQ(response.body_mode, HTTPResponse::BodyMode::BUFFERED);
    EXPECT_TRUE(response.static_body.empty());
    EXPECT_EQ(response.generator, nullptr);
    EXPECT_EQ(response.generator_ctx, nullptr);
    EXPECT_EQ(response.generator_length, 0U);
    // A stale static_body or generator surviving a reset would serve the
    // previous request's body to the next one out of the same pool entry.
    EXPECT_EQ(response.body_size(), 0U);
}
