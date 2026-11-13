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

// Exactly one translation unit per executable may include
// allocation_operators.ipp. Its non-inline replacement operators would
// otherwise cause duplicate symbols.
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

// No allocation occurs before an oversized copied body is rejected (BR-14,
// AR-1).
TEST(HttpResponseTest, SetBodyOverTheCapAllocatesNothing) {
    constexpr std::size_t CAP =
        PICO_HTTP_ACTIVE_PROFILE::Limits::MAX_BUFFERED_BODY;
    HTTPResponse res;
    res.set_body(std::string(16, 'a'));
    const std::string over(CAP + 1, 'x');
    HTTPResponse::BodyResult result{};
    {
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

// The result distinguishes body rejection from a missing Content-Type
// (BR-14).
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

    // The capacity has to be freed. Clearing alone retains heap outside the
    // copied-body bound (AR-1).
    EXPECT_EQ(response.body.capacity(), 0U);
    EXPECT_TRUE(response.body.empty());
}

// An implicit span conversion would let recycled request storage back a
// response (AR-11).
TEST(HttpResponseTest, ARequestViewCannotBecomeAStaticBody) {
    static_assert(
        !std::convertible_to<std::span<const std::uint8_t>, StaticBody>);
    static_assert(!std::convertible_to<std::string_view, StaticBody>);
    static_assert(
        !std::is_constructible_v<StaticBody, std::span<const std::uint8_t>>);
    // This runs at run time: the char-to-byte reinterpretation is not allowed
    // in constant evaluation.
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
    // A stale body source would serve the previous request's body.
    EXPECT_EQ(response.body_size(), 0U);
}
