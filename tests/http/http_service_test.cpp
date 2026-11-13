// SPDX-License-Identifier: MIT
#include "pico_http/http/http_service.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pico_http/http/message.hpp"
#include "pico_http/tcp/tcp_transport.hpp"
#include "pico_http/testing/fake_tcp_transport.hpp"
#include "pico_http/testing/service_harness.hpp"
#include "pico_util/heap/heap_watermark.hpp"

using pico_http::Disposition;
using pico_http::HTTPRequestView;
using pico_http::HTTPResponse;
using pico_http::HttpService;
using pico_http::StaticBody;
using pico_http::testing::FakeTcpTransport;
using Harness = pico_http::testing::ServiceHarness;
using ConnectionId = pico_http::ITcpTransport::ConnectionId;

namespace {

std::string round_trip(Harness& harness, ConnectionId id,
                       std::string_view request) {
    return harness.round_trip(id, request);
}

}  // namespace

TEST(HttpServiceTest, RoutesGetRequestToHandler) {
    Harness harness;
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/hello", [](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body("hi there");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string response =
        round_trip(harness, id, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n");

    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_NE(response.find("Content-Length: 8\r\n"), std::string::npos);
    EXPECT_TRUE(response.ends_with("hi there"));
}

TEST(HttpServiceTest, UnknownRouteReturns404) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    const std::string response =
        round_trip(harness, id, "GET /nope HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 404 Not Found\r\n"));
}

// A URI registered under another method returns 405 (NR-8).
TEST(HttpServiceTest, KnownUriWrongMethodReturns405WithAllow) {
    Harness harness;
    auto ok = [](const HTTPRequestView&, HTTPResponse& res) {
        res.set_body("ok");
        return Disposition::COMPLETE;
    };
    EXPECT_TRUE(harness.service->add_route("GET", "/thing", ok));
    EXPECT_TRUE(harness.service->add_route("PUT", "/thing", ok));
    const ConnectionId id = harness.wire->connect();
    // Allow lists GET and PUT for the unregistered DELETE method (RFC 9110
    // §15.5.6).
    const std::string response =
        round_trip(harness, id, "DELETE /thing HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
    const std::size_t allow = response.find("Allow: ");
    ASSERT_NE(allow, std::string::npos);
    const std::string allow_line =
        response.substr(allow, response.find("\r\n", allow) - allow);
    EXPECT_NE(allow_line.find("GET"), std::string::npos);
    EXPECT_NE(allow_line.find("PUT"), std::string::npos);
}

// Wildcard routes contribute to Allow (NR-8).
TEST(HttpServiceTest, WildcardRouteContributesToAllowOn405) {
    Harness harness;
    EXPECT_TRUE(harness.service->add_route(
        "POST", "/files/*", [](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body("ok");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string response =
        round_trip(harness, id, "GET /files/a.txt HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
    EXPECT_NE(response.find("Allow: POST\r\n"), std::string::npos);
}

TEST(HttpServiceTest, AllowListsEveryMethodWithoutTruncation) {
    Harness harness;
    // Every method token must fit in full at the maximum count and length.
    constexpr int METHOD_COUNT = 8;
    std::vector<std::string> methods;
    methods.reserve(METHOD_COUNT);
    for (int i = 0; i < METHOD_COUNT; ++i) {
        methods.push_back("CUSTOMMETHODM0" + std::to_string(i));
    }
    for (const std::string& method : methods) {
        EXPECT_TRUE(harness.service->add_route(
            method, "/x", [](const HTTPRequestView&, HTTPResponse& res) {
                res.set_body("ok");
                return Disposition::COMPLETE;
            }));
    }
    const ConnectionId id = harness.wire->connect();
    const std::string response =
        round_trip(harness, id, "GET /x HTTP/1.1\r\nHost: h\r\n\r\n");
    ASSERT_TRUE(response.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
    for (const std::string& method : methods) {
        EXPECT_NE(response.find(method), std::string::npos)
            << "missing " << method;
    }
}

// HEAD support follows GET support (RFC 9110 §9.1).

TEST(HttpServiceTest, HeadIsServedFromGetRouteWithoutBody) {
    Harness harness;
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/page", [](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body("page body");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string response =
        round_trip(harness, id, "HEAD /page HTTP/1.1\r\nHost: h\r\n\r\n");

    // The Content-Length describes GET's body despite HEAD sending no body
    // bytes.
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_NE(response.find("Content-Length: 9\r\n"), std::string::npos);
    EXPECT_TRUE(response.ends_with("\r\n\r\n"));
    ASSERT_TRUE(
        // Incorrect HEAD framing would corrupt the next response.
        harness.wire->is_open(id));

    const std::string all =
        round_trip(harness, id, "GET /page HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT_TRUE(all.ends_with("page body"));
}

TEST(HttpServiceTest, ExplicitHeadRouteWinsOverGetFallback) {
    Harness harness;
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/r", [](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body("get");
            return Disposition::COMPLETE;
        }));
    EXPECT_TRUE(harness.service->add_route(
        "HEAD", "/r", [](const HTTPRequestView&, HTTPResponse& res) {
            res.headers.set("X-Head", "1");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string response =
        round_trip(harness, id, "HEAD /r HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT_NE(response.find("X-Head: 1\r\n"), std::string::npos);
    EXPECT_TRUE(response.ends_with("\r\n\r\n"));
}

TEST(HttpServiceTest, HeadOnChunkedRouteEmitsHeadersOnlyNoLastChunk) {
    Harness harness;
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/stream", [](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body_generator(
                [](void*, std::size_t, std::span<std::uint8_t>) {
                    return std::size_t{0};
                },
                nullptr, "text/plain");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string response =
        round_trip(harness, id, "HEAD /stream HTTP/1.1\r\nHost: h\r\n\r\n");
    // HEAD sends neither chunks nor a last-chunk (RFC 9110 §9.3.2).
    EXPECT_NE(response.find("Transfer-Encoding: chunked\r\n"),
              std::string::npos);
    EXPECT_TRUE(response.ends_with("\r\n\r\n"));
    EXPECT_FALSE(response.ends_with("0\r\n\r\n"));
    EXPECT_TRUE(harness.wire->is_open(id));
}

TEST(HttpServiceTest, HeadOnUnroutedUriIs404WithoutBody) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    const std::string response =
        round_trip(harness, id, "HEAD /missing HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 404 Not Found\r\n"));
    EXPECT_TRUE(response.ends_with("\r\n\r\n"));
}

TEST(HttpServiceTest, PostBodyIsReadableByHandler) {
    Harness harness;
    std::string got_body;
    EXPECT_TRUE(harness.service->add_route(
        "POST", "/echo", [&](const HTTPRequestView& req, HTTPResponse& res) {
            got_body.assign(req.body.begin(), req.body.end());
            res.set_body("ok");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    round_trip(
        harness, id,
        "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello");
    EXPECT_EQ(got_body, "hello");
}

TEST(HttpServiceTest, KeepAliveByDefaultOnHttp11) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    const std::string response =
        round_trip(harness, id, "GET / HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT_NE(response.find("Connection: keep-alive\r\n"), std::string::npos);
    EXPECT_TRUE(harness.wire->is_open(id));
}

TEST(HttpServiceTest, ConnectionCloseHeaderClosesSocket) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    const std::string response = round_trip(
        harness, id, "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    EXPECT_NE(response.find("Connection: close\r\n"), std::string::npos);
    EXPECT_FALSE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().disconnect_server_closed, 1U);
}

TEST(HttpServiceTest, Http10DefaultsToClose) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    const std::string response =
        round_trip(harness, id, "GET / HTTP/1.0\r\n\r\n");
    EXPECT_NE(response.find("Connection: close\r\n"), std::string::npos);
    EXPECT_FALSE(harness.wire->is_open(id));
}

TEST(HttpServiceTest, KeepAliveRequestCapClosesOnLastAllowed) {
    Harness harness;
    HttpService::KeepAliveConfig cfg;
    cfg.max_requests_per_connection = 2;
    harness.service->set_keep_alive_config(cfg);
    const ConnectionId id = harness.wire->connect();

    harness.wire->deliver_text(id, "GET / HTTP/1.1\r\nHost: h\r\n\r\n");
    harness.flush(id);
    EXPECT_TRUE(harness.wire->is_open(id));

    harness.wire->deliver_text(id, "GET / HTTP/1.1\r\nHost: h\r\n\r\n");
    harness.flush(id);
    EXPECT_FALSE(harness.wire->is_open(id));
}

TEST(HttpServiceTest, PipelinedRequestsAnsweredInOrder) {
    Harness harness;
    int n = 0;
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/a", [&](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body(std::to_string(++n));
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string response = round_trip(
        harness, id,
        "GET /a HTTP/1.1\r\nHost: h\r\n\r\nGET /a HTTP/1.1\r\nHost: h\r\n\r\n");
    const std::size_t first = response.find("HTTP/1.1 200");
    ASSERT_NE(first, std::string::npos);
    const std::size_t second = response.find("HTTP/1.1 200", first + 1);
    ASSERT_NE(second, std::string::npos);
    EXPECT_EQ(response.find("HTTP/1.1 200", second + 1), std::string::npos)
        << "two pipelined requests must produce exactly two responses";
    EXPECT_EQ(n, 2) << "the handler must run once per pipelined request";
    EXPECT_TRUE(response.ends_with("2"));
}

// The outliving X body supports sizes larger than a copied body may hold
// (BR-14).
namespace {
std::string_view x_body(std::size_t size) {
    static const std::string BODY(200000, 'X');
    return std::string_view(BODY).substr(0, size);
}
}  // namespace

// Streaming across backpressure (AR-5).

TEST(HttpServiceTest, LargeResponseStreamsWithoutTruncationUnderBackpressure) {
    Harness harness(
        /*send_capacity=*/512);  // The small buffer forces resumption.
    constexpr std::size_t BODY = 200000;
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/big", [&](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body_static(StaticBody::outliving_response(x_body(BODY)));
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string response =
        round_trip(harness, id, "GET /big HTTP/1.1\r\nHost: h\r\n\r\n");

    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_NE(response.find("Content-Length: 200000\r\n"), std::string::npos);
    const std::size_t body_start = response.find("\r\n\r\n") + 4;
    EXPECT_EQ(response.size() - body_start, BODY);
    EXPECT_GT(harness.service->statistics().backpressure_events, 0U);
}

// Boundary cases (BR-1..BR-8).

TEST(HttpServiceTest, OversizedContentLengthReturns413) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    const std::string response = round_trip(
        harness, id,
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 100000\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 413"));
    EXPECT_NE(response.find("Connection: close\r\n"), std::string::npos);
    EXPECT_FALSE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().parse_errors, 1U);
}

TEST(HttpServiceTest, OverlongUriReturns414) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    const std::string uri(600, 'a');
    const std::string response =
        round_trip(harness, id, "GET /" + uri + " HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 414"));
}

TEST(HttpServiceTest, TooManyHeadersReturns431) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    std::string request = "GET / HTTP/1.1\r\nHost: h\r\n";
    for (int i = 0; i < 20; ++i) {
        request += "H" + std::to_string(i) + ": v\r\n";
    }
    request += "\r\n";
    const std::string response = round_trip(harness, id, request);
    EXPECT_TRUE(response.starts_with("HTTP/1.1 431"));
}

TEST(HttpServiceTest, ChunkedRequestBodyIsDecodedForHandler) {
    Harness harness;
    std::string got;
    EXPECT_TRUE(harness.service->add_route(
        "POST", "/upload", [&](const HTTPRequestView& req, HTTPResponse& res) {
            got.assign(req.body.begin(), req.body.end());
            res.set_body("ok");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string response = round_trip(
        harness, id,
        "POST /upload HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n2\r\n!!\r\n0\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200"));
    EXPECT_EQ(got, "hello!!");
}

TEST(HttpServiceTest, UnsupportedTransferEncodingReturns501) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    const std::string response = round_trip(
        harness, id,
        "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 501"));
}

TEST(HttpServiceTest, MalformedRequestLineReturns400) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    const std::string response = round_trip(harness, id, "GET /x\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 400"));
}

// An HTTP/1.1 request must carry exactly one Host field (NR-7).
TEST(HttpServiceTest, Http11WithoutHostReturns400) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    const std::string response =
        round_trip(harness, id, "GET / HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 400 Bad Request\r\n"));
}

TEST(HttpServiceTest, UnsupportedHttpVersionReturns505) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    // A syntactically valid unsupported version returns 505 (RFC 9110
    // §15.6.6).
    const std::string response =
        round_trip(harness, id, "GET /x HTTP/2.0\r\n\r\n");
    EXPECT_TRUE(
        response.starts_with("HTTP/1.1 505 HTTP Version Not Supported"));
}

// Service teardown accounting (AR-4/RR-1).

// Timeouts contribute to their reason count and total. FIN counts as
// peer_closed (OR-1).
TEST(HttpServiceTest, PeerCloseAndTheTimeoutTotalAreCounted) {
    Harness harness;
    const ConnectionId closed = harness.wire->connect();
    harness.wire->close_peer(closed);
    EXPECT_EQ(harness.service->statistics().disconnect_peer_closed, 1U);

    const ConnectionId stalled = harness.wire->connect();
    harness.wire->deliver_text(stalled, "G");
    harness.wire->advance_time(6000);
    ASSERT_FALSE(harness.wire->is_open(stalled));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_header, 1U);
    EXPECT_EQ(harness.service->statistics().disconnect_timeout, 1U);
    EXPECT_EQ(harness.service->statistics().disconnect_peer_closed, 1U);
}

// Local aborts have a separate count from peer resets (OR-1).
TEST(HttpServiceTest, LocalAbortIsCountedApartFromAPeerReset) {
    Harness harness;
    const ConnectionId aborted = harness.wire->connect();
    harness.wire->abort_local(aborted);
    EXPECT_FALSE(harness.wire->is_open(aborted));
    EXPECT_EQ(harness.service->statistics().disconnect_aborted, 1U);
    EXPECT_EQ(harness.service->statistics().disconnect_peer_reset, 0U);

    const ConnectionId reset = harness.wire->connect();
    harness.wire->reset_peer(reset);
    EXPECT_EQ(harness.service->statistics().disconnect_aborted, 1U);
    EXPECT_EQ(harness.service->statistics().disconnect_peer_reset, 1U);
}

TEST(HttpServiceTest, PeerResetFreesSlotAndCountsReason) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    harness.wire->reset_peer(id);
    EXPECT_EQ(harness.service->statistics().disconnect_peer_reset, 1U);

    for (std::size_t i = 0; i < FakeTcpTransport::MAX_CLIENTS; ++i) {
        EXPECT_TRUE(harness.wire->connect().valid());
    }
}

TEST(HttpServiceTest, ConcurrentConnectionsAreIsolated) {
    Harness harness;
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/id", [](const HTTPRequestView& req, HTTPResponse& res) {
            res.set_body(std::string(req.uri));
            return Disposition::COMPLETE;
        }));
    const ConnectionId first = harness.wire->connect();
    const ConnectionId second = harness.wire->connect();

    harness.wire->deliver_text(first, "GET /id HTTP/1.1\r\nHost: h\r\n\r\n");
    harness.wire->deliver_text(second, "GET /id HTTP/1.1\r\nHost: h\r\n\r\n");
    harness.flush(first);
    harness.flush(second);

    EXPECT_TRUE(harness.wire->sent_text(first).ends_with("/id"));
    EXPECT_TRUE(harness.wire->sent_text(second).ends_with("/id"));
    EXPECT_EQ(harness.service->statistics().requests_completed, 2U);
}

TEST(HttpServiceTest, StatisticsTrackAcceptsAndRequests) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    round_trip(harness, id, "GET / HTTP/1.1\r\nHost: h\r\n\r\n");
    const auto& stats = harness.service->statistics();
    EXPECT_EQ(stats.accepts, 1U);
    EXPECT_EQ(stats.requests_completed, 1U);
    EXPECT_EQ(stats.slot_high_watermark, 1U);
    EXPECT_GT(stats.bytes_out, 0U);
    EXPECT_GT(stats.bytes_in, 0U);
}

// Static and generated response bodies (AR-2, AR-5).

TEST(HttpServiceTest, StaticBodyIsServed) {
    Harness harness;
    static constexpr std::string_view PAGE = "static flash page";
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/static", [](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body_static(StaticBody::outliving_response(PAGE),
                                "text/plain");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string response =
        round_trip(harness, id, "GET /static HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_NE(response.find("Content-Length: 17\r\n"), std::string::npos);
    EXPECT_TRUE(response.ends_with(PAGE));
}

// The byte-offset pattern catches incorrect resumption.
namespace {
std::size_t pattern_generator(void* /*ctx*/, std::size_t offset,
                              std::span<std::uint8_t> out) {
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>((offset + i) % 251U);
    }
    return out.size();
}
}  // namespace

TEST(HttpServiceTest, GeneratorBodyStreamsLargeBodyBitExactUnderBackpressure) {
    Harness harness(/*send_capacity=*/512);
    // The body is 256 KiB (PR-8).
    constexpr std::size_t BODY = std::size_t{256} * 1024;
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/gen", [](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body_generator(pattern_generator, nullptr, BODY,
                                   "application/octet-stream");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    std::string response =
        round_trip(harness, id, "GET /gen HTTP/1.1\r\nHost: h\r\n\r\n");

    EXPECT_NE(response.find("Content-Length: 262144\r\n"), std::string::npos);
    const std::size_t body_start = response.find("\r\n\r\n") + 4;
    ASSERT_EQ(response.size() - body_start, BODY);
    for (std::size_t i = 0; i < BODY; ++i) {
        ASSERT_EQ(static_cast<std::uint8_t>(response[body_start + i]),
                  static_cast<std::uint8_t>(i % 251U))
            << "mismatch at body offset " << i;
    }
    EXPECT_GT(harness.service->statistics().backpressure_events, 0U);
}

// The produced-byte count detects regeneration of discarded chunks.
struct CountingGenerator {
    std::size_t bytes_produced = 0;
    std::size_t calls = 0;
};

namespace {
std::size_t counting_generator(void* ctx, std::size_t offset,
                               std::span<std::uint8_t> out) {
    auto* state = static_cast<CountingGenerator*>(ctx);
    ++state->calls;
    state->bytes_produced += out.size();
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>((offset + i) % 251U);
    }
    return out.size();
}
}  // namespace

TEST(HttpServiceTest, GeneratorIsNotReinvokedForBytesTheRingRejects) {
    Harness harness(
        /*send_capacity=*/512);  // The small ring forces repeated stalls.
    constexpr std::size_t BODY = std::size_t{16} * 1024;
    CountingGenerator gen;
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/gen", [&](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body_generator(counting_generator, &gen, BODY,
                                   "application/octet-stream");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string response =
        round_trip(harness, id, "GET /gen HTTP/1.1\r\nHost: h\r\n\r\n");

    const std::size_t body_start = response.find("\r\n\r\n") + 4;
    ASSERT_EQ(response.size() - body_start, BODY);
    EXPECT_EQ(gen.bytes_produced, BODY);
    EXPECT_GT(harness.service->statistics().backpressure_events, 0U);
}

// Unknown-length response bodies (NR-6).

// The produced-byte count detects duplicate generation across stalls.
struct BoundedGenerator {
    std::size_t total = 0;
    std::size_t produced = 0;
};

namespace {
std::size_t bounded_generator(void* ctx, std::size_t offset,
                              std::span<std::uint8_t> out) {
    auto* state = static_cast<BoundedGenerator*>(ctx);
    if (offset >= state->total) {
        return 0;
    }
    const std::size_t n = std::min(out.size(), state->total - offset);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::uint8_t>((offset + i) % 251U);
    }
    state->produced += n;
    return n;
}
}  // namespace

namespace {
std::string decode_chunked(std::string_view framed) {
    std::string out;
    std::size_t pos = 0;
    while (pos < framed.size()) {
        const std::size_t eol = framed.find("\r\n", pos);
        if (eol == std::string_view::npos) {
            break;
        }
        const std::size_t size =
            std::stoul(std::string(framed.substr(pos, eol - pos)), nullptr, 16);
        pos = eol + 2;
        if (size == 0) {
            break;
        }
        out.append(framed.substr(pos, size));
        pos += size + 2;
    }
    return out;
}
}  // namespace

namespace {
void expect_pattern(std::string_view body) {
    for (std::size_t i = 0; i < body.size(); ++i) {
        ASSERT_EQ(static_cast<std::uint8_t>(body[i]),
                  static_cast<std::uint8_t>(i % 251U))
            << "mismatch at body offset " << i;
    }
}
}  // namespace

TEST(HttpServiceTest, ChunkedGeneratorResponseRoundTrips) {
    Harness harness;
    BoundedGenerator gen{.total = 3000U};
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/stream", [&](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body_generator(bounded_generator, &gen, "text/plain");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string resp =
        round_trip(harness, id, "GET /stream HTTP/1.1\r\nHost: h\r\n\r\n");

    EXPECT_NE(resp.find("Transfer-Encoding: chunked\r\n"), std::string::npos);
    EXPECT_EQ(resp.find("Content-Length:"), std::string::npos);
    EXPECT_TRUE(resp.ends_with("0\r\n\r\n"));  // last-chunk terminator

    const std::size_t body_start = resp.find("\r\n\r\n") + 4;
    const std::string decoded =
        decode_chunked(std::string_view(resp).substr(body_start));
    ASSERT_EQ(decoded.size(), gen.total);
    expect_pattern(decoded);
}

TEST(HttpServiceTest, ChunkedResponseStreamsBitExactUnderBackpressure) {
    Harness harness(
        // The small ring splits framing across resumptions.
        /*send_capacity=*/64);
    BoundedGenerator gen{.total = 8192U};
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/stream", [&](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body_generator(bounded_generator, &gen, "text/plain");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string resp =
        round_trip(harness, id, "GET /stream HTTP/1.1\r\nHost: h\r\n\r\n");

    const std::size_t body_start = resp.find("\r\n\r\n") + 4;
    const std::string decoded =
        decode_chunked(std::string_view(resp).substr(body_start));
    ASSERT_EQ(decoded.size(), gen.total);
    expect_pattern(decoded);
    EXPECT_EQ(gen.produced, gen.total);
    EXPECT_GT(harness.service->statistics().backpressure_events, 0U);
}

TEST(HttpServiceTest, ChunkedDowngradesToCloseDelimitedForHttp10) {
    Harness harness;
    BoundedGenerator gen{.total = 1000U};
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/stream", [&](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body_generator(bounded_generator, &gen, "text/plain");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string resp =
        round_trip(harness, id, "GET /stream HTTP/1.0\r\n\r\n");

    EXPECT_EQ(resp.find("Transfer-Encoding"), std::string::npos);
    EXPECT_EQ(resp.find("Content-Length"), std::string::npos);
    EXPECT_NE(resp.find("Connection: close\r\n"), std::string::npos);
    const std::size_t body_start = resp.find("\r\n\r\n") + 4;
    ASSERT_EQ(resp.size() - body_start, gen.total);
    expect_pattern(std::string_view(resp).substr(body_start));
    EXPECT_FALSE(
        // EOF is the only body delimiter for this response.
        harness.wire->is_open(id));
}

TEST(HttpServiceTest, Http10DowngradeOverridesAHandlerSetKeepAliveHeader) {
    // A close-delimited body must not announce keep-alive, which would hide
    // its end from the client (NR-2, NR-6).
    Harness harness;
    BoundedGenerator gen{.total = 300U};
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/stream", [&](const HTTPRequestView&, HTTPResponse& res) {
            res.headers.set("Connection", "keep-alive");
            res.headers.set("Keep-Alive", "timeout=15");
            res.set_body_generator(bounded_generator, &gen, "text/plain");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string resp =
        round_trip(harness, id, "GET /stream HTTP/1.0\r\n\r\n");

    EXPECT_NE(resp.find("Connection: close\r\n"), std::string::npos);
    EXPECT_EQ(resp.find("keep-alive"), std::string::npos);
    EXPECT_EQ(resp.find("Keep-Alive"), std::string::npos);
    EXPECT_EQ(resp.find("Transfer-Encoding"), std::string::npos);
    EXPECT_EQ(resp.find("Content-Length"), std::string::npos);
    const std::size_t body_start = resp.find("\r\n\r\n") + 4;
    ASSERT_EQ(resp.size() - body_start, gen.total);
    EXPECT_FALSE(harness.wire->is_open(id));
}

TEST(HttpServiceTest, RequestCapClosesDespiteAHandlerSetKeepAliveHeader) {
    // A handler cannot override the request cap with Connection: keep-alive.
    Harness harness;
    HttpService::KeepAliveConfig config;
    config.max_requests_per_connection = 1;
    harness.service->set_keep_alive_config(config);
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/ping", [](const HTTPRequestView&, HTTPResponse& res) {
            res.headers.set("Connection", "keep-alive");
            res.set_body("pong");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string resp =
        round_trip(harness, id, "GET /ping HTTP/1.1\r\nHost: h\r\n\r\n");

    EXPECT_NE(resp.find("Connection: close\r\n"), std::string::npos);
    EXPECT_EQ(resp.find("keep-alive"), std::string::npos);
    EXPECT_FALSE(harness.wire->is_open(id));
}

TEST(HttpServiceTest, KeepAliveSurvivesChunkedResponse) {
    Harness harness;
    BoundedGenerator gen{.total = 500U};
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/stream", [&](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body_generator(bounded_generator, &gen, "text/plain");
            return Disposition::COMPLETE;
        }));
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/ping", [](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body("pong");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();

    const std::string r1 =
        round_trip(harness, id, "GET /stream HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT_NE(r1.find("Transfer-Encoding: chunked\r\n"), std::string::npos);
    ASSERT_TRUE(
        // The last-chunk delimits the body without closing TCP.
        harness.wire->is_open(id));

    const std::string all =
        round_trip(harness, id, "GET /ping HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT_TRUE(all.ends_with("pong"));
}

// Counter accounting (OR-1).

// The soak checks these same counter identities on the device (RR-4).
TEST(HttpServiceTest, TheCountersAccountForEveryConnection) {
    // A small send window, so a large response stalls for the write timeout.
    Harness harness(/*send_capacity=*/512);
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/big", [](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body_static(StaticBody::outliving_response(x_body(100000)));
            return Disposition::COMPLETE;
        }));
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/defer", [](const HTTPRequestView&, HTTPResponse&) {
            return Disposition::PENDING;
        }));
    const auto closed = [&] {
        const auto& stats = harness.service->statistics();
        return stats.disconnect_peer_closed + stats.disconnect_peer_reset +
               stats.disconnect_aborted + stats.disconnect_timeout +
               stats.disconnect_server_closed;
    };
    // Each close path and timeout kind has its own connection.
    harness.wire->close_peer(harness.wire->connect());
    harness.wire->reset_peer(harness.wire->connect());
    harness.wire->abort_local(harness.wire->connect());
    // The server closes this connection.
    round_trip(harness, harness.wire->connect(), "BAD\r\n\r\n");
    // This connection idles.
    round_trip(harness, harness.wire->connect(),
               "GET / HTTP/1.1\r\nHost: h\r\n\r\n");
    // This connection reaches the header deadline.
    harness.wire->deliver_text(harness.wire->connect(), "G");
    // This connection reaches the body deadline.
    harness.wire->deliver_text(
        harness.wire->connect(),
        "POST /u HTTP/1.1\r\nHost: h\r\nContent-Length: 10\r\n\r\nab");
    // This connection's unacknowledged response holds a write deadline.
    harness.wire->deliver_text(harness.wire->connect(),
                               "GET /big HTTP/1.1\r\nHost: h\r\n\r\n");
    // This connection reaches the drain linger deadline.
    harness.wire->deliver_text(
        harness.wire->connect(),
        "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    // The pending handler retains its own deadline.
    harness.wire->deliver_text(harness.wire->connect(),
                               "GET /defer HTTP/1.1\r\nHost: h\r\n\r\n");
    // Sixteen seconds exceeds every deadline, including idle.
    harness.wire->advance_time(16000);

    const auto& stats = harness.service->statistics();
    EXPECT_EQ(stats.accepts, 10U);
    EXPECT_EQ(harness.service->active_connections(), 0U);
    EXPECT_EQ(stats.accepts, closed() + harness.service->active_connections());
    for (const std::uint32_t kind :
         {stats.disconnect_peer_closed, stats.disconnect_peer_reset,
          stats.disconnect_aborted, stats.disconnect_timeout_header,
          stats.disconnect_timeout_body, stats.disconnect_timeout_idle,
          stats.disconnect_timeout_write, stats.disconnect_timeout_drain,
          stats.disconnect_timeout_handler}) {
        EXPECT_EQ(kind, 1U);
    }
    EXPECT_EQ(stats.disconnect_server_closed, 1U);
    EXPECT_EQ(
        stats.disconnect_timeout,
        stats.disconnect_timeout_header + stats.disconnect_timeout_body +
            stats.disconnect_timeout_idle + stats.disconnect_timeout_write +
            stats.disconnect_timeout_drain + stats.disconnect_timeout_handler);
    EXPECT_EQ(stats.parse_errors,
              stats.parse_errors_400 + stats.parse_errors_413 +
                  stats.parse_errors_414 + stats.parse_errors_417 +
                  stats.parse_errors_431 + stats.parse_errors_501 +
                  stats.parse_errors_505);
    EXPECT_EQ(stats.parse_errors, 1U);
}

TEST(HttpServiceTest, RefusalsCounterIncrementsWhenSlotsFull) {
    Harness harness;
    std::vector<ConnectionId> ids;
    for (std::size_t i = 0; i < FakeTcpTransport::MAX_CLIENTS; ++i) {
        const ConnectionId id = harness.wire->connect();
        EXPECT_TRUE(id.valid());
        ids.push_back(id);
    }
    // Slot exhaustion rejects the connection at TCP level (RR-3).
    EXPECT_FALSE(harness.wire->connect().valid());
    EXPECT_EQ(harness.service->statistics().refusals_slot_full, 1U);
}

TEST(HttpServiceTest, ParseErrorClassCountersAreTracked) {
    Harness harness;
    auto send = [&](std::string_view req) {
        const ConnectionId id = harness.wire->connect();
        round_trip(harness, id, req);
    };
    // One request each for the 413, 414, 431, 501, 400 and 505 classes;
    // UnknownExpectationReturns417 covers 417.
    send("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 100000\r\n\r\n");
    send("GET /" + std::string(600, 'a') + " HTTP/1.1\r\nHost: h\r\n\r\n");
    {
        std::string req = "GET / HTTP/1.1\r\nHost: h\r\n";
        for (int i = 0; i < 20; ++i) {
            req += "H" + std::to_string(i) + ": v\r\n";
        }
        req += "\r\n";
        send(req);
    }
    send("POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip\r\n\r\n");
    send("GET /x\r\n\r\n");
    send("GET /x HTTP/2.0\r\n\r\n");

    const auto& text = harness.service->statistics();
    EXPECT_EQ(text.parse_errors_413, 1U);
    EXPECT_EQ(text.parse_errors_414, 1U);
    EXPECT_EQ(text.parse_errors_431, 1U);
    EXPECT_EQ(text.parse_errors_501, 1U);
    EXPECT_EQ(text.parse_errors_400, 1U);
    EXPECT_EQ(text.parse_errors_505, 1U);
    EXPECT_EQ(text.parse_errors, 6U);
}

// Transport timeout scans (TR-1..TR-5).

TEST(HttpServiceTest, HeaderTimeoutClosesSlowlorisAndCountsIt) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    // Drip-fed bytes must not reset the header window from accept.
    for (int i = 0; i < 4; ++i) {
        harness.wire->deliver_text(id, "G");
        harness.wire->advance_time(2000);
    }
    EXPECT_FALSE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_header, 1U);
}

TEST(HttpServiceTest, BodyProgressTimeoutClosesStalledUpload) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    // The completed head switches timing to body progress (TR-2).
    harness.wire->deliver_text(
        id, "POST /u HTTP/1.1\r\nHost: h\r\nContent-Length: 10\r\n\r\nab");
    ASSERT_TRUE(harness.wire->is_open(id));
    harness.wire->advance_time(4000);
    EXPECT_TRUE(harness.wire->is_open(id));
    harness.wire->advance_time(2000);
    EXPECT_FALSE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_body, 1U);
}

// The fake clock checks both edges of the 200 ms tolerance (TR-2).
TEST(HttpServiceTest, BodyProgressTimeoutFiresWithinItsBand) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    harness.wire->deliver_text(
        id, "POST /u HTTP/1.1\r\nHost: h\r\nContent-Length: 10\r\n\r\nab");
    harness.wire->advance_time(4800);
    EXPECT_TRUE(harness.wire->is_open(id));
    harness.wire->advance_time(400);
    EXPECT_FALSE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_body, 1U);
}

TEST(HttpServiceTest, UnknownExpectationReturns417) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    const std::string response =
        round_trip(harness, id,
                   "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 2\r\n"
                   "Expect: 200-maybe\r\n\r\nhi");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 417 Expectation Failed\r\n"));
    EXPECT_NE(response.find("Connection: close\r\n"), std::string::npos);
    EXPECT_EQ(harness.service->statistics().parse_errors_417, 1U);
}

TEST(HttpServiceTest, IdleKeepAliveConnectionTimesOut) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    round_trip(
        // Only a completed response arms the idle deadline.
        harness, id, "GET / HTTP/1.1\r\nHost: h\r\n\r\n");
    ASSERT_TRUE(harness.wire->is_open(id));

    harness.wire->advance_time(14000);
    EXPECT_TRUE(harness.wire->is_open(id));
    harness.wire->advance_time(2000);
    EXPECT_FALSE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_idle, 1U);
}

TEST(HttpServiceTest, CompletedRequestClearsHeaderTimeout) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    round_trip(harness, id, "GET / HTTP/1.1\r\nHost: h\r\n\r\n");
    // A completed request must switch from the 5 s header deadline to
    // the 15 s idle deadline.
    harness.wire->advance_time(6000);
    EXPECT_TRUE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_header, 0U);
}

TEST(HttpServiceTest, WriteStallTimeoutClosesSlowReader) {
    Harness harness(
        /*send_capacity=*/512);
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/big", [](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body_static(StaticBody::outliving_response(x_body(100000)));
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    harness.wire->deliver_text(id, "GET /big HTTP/1.1\r\nHost: h\r\n\r\n");
    // The 10 s write deadline should fire without ACK progress.
    ASSERT_TRUE(harness.wire->is_open(id));
    harness.wire->advance_time(11000);
    EXPECT_FALSE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_write, 1U);
}

// The write deadline holds 200 ms before 10 s and fires 200 ms after it,
// with no ACK in between (TR-4).
TEST(HttpServiceTest, WriteStallTimeoutFiresWithinItsBand) {
    Harness harness(/*send_capacity=*/512);
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/big", [](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body_static(StaticBody::outliving_response(x_body(100000)));
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    harness.wire->deliver_text(id, "GET /big HTTP/1.1\r\nHost: h\r\n\r\n");
    harness.wire->advance_time(9800);
    EXPECT_TRUE(harness.wire->is_open(id));
    harness.wire->advance_time(400);
    EXPECT_FALSE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_write, 1U);
}

// A partial pipelined head must use the 5 s header window after the preceding
// response drains (TR-1).
TEST(HttpServiceTest,
     PartialPipelinedRequestAfterBackpressureUsesHeaderTimeout) {
    Harness harness(/*send_capacity=*/512);
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/big", [](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body_static(StaticBody::outliving_response(x_body(4000)));
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    harness.wire->deliver_text(id,
                               "GET /big HTTP/1.1\r\nHost: h\r\n\r\nGET /bi");
    harness.flush(id);
    ASSERT_TRUE(harness.wire->is_open(id));

    harness.wire->advance_time(
        // Six seconds separates the header and write deadlines.
        6000);
    EXPECT_FALSE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_header, 1U);
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_write, 0U);
}

// The interim response precedes the body and final response (NR-5).
TEST(HttpServiceTest, Expect100ContinueEmittedBeforeBody) {
    Harness harness;
    std::string got;
    EXPECT_TRUE(harness.service->add_route(
        "POST", "/echo", [&](const HTTPRequestView& req, HTTPResponse& res) {
            got.assign(req.body.begin(), req.body.end());
            res.set_body("ok");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    harness.wire->deliver_text(
        id,
        "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n"
        "Expect: 100-continue\r\n\r\n");
    harness.flush(id);
    const std::string interim = harness.wire->sent_text(id);
    EXPECT_NE(interim.find("HTTP/1.1 100 Continue\r\n\r\n"), std::string::npos);
    ASSERT_TRUE(harness.wire->is_open(id));

    harness.wire->deliver_text(id, "hello");
    harness.flush(id);
    EXPECT_EQ(got, "hello");
    const std::string full = harness.wire->sent_text(id);
    ASSERT_NE(full.find("100 Continue"), std::string::npos);
    ASSERT_NE(full.find("200 OK"), std::string::npos);
    EXPECT_LT(full.find("100 Continue"), full.find("200 OK"));
}

// HTTP/1.0 ignores the expectation (NR-5, RFC 9110 §10.1.1).
TEST(HttpServiceTest, Http10IgnoresContinueExpectation) {
    Harness harness;
    std::string got;
    EXPECT_TRUE(harness.service->add_route(
        "POST", "/echo", [&](const HTTPRequestView& req, HTTPResponse& res) {
            got.assign(req.body.begin(), req.body.end());
            res.set_body("ok");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    // The head alone must not produce an interim response.
    harness.wire->deliver_text(
        id,
        "POST /echo HTTP/1.0\r\nHost: h\r\nContent-Length: 5\r\n"
        "Expect: 100-continue\r\n\r\n");
    harness.flush(id);
    const std::string interim = harness.wire->sent_text(id);
    EXPECT_TRUE(interim.empty());
    ASSERT_TRUE(harness.wire->is_open(id));

    harness.wire->deliver_text(id, "hello");
    harness.flush(id);
    EXPECT_EQ(got, "hello");
    const std::string full = harness.wire->sent_text(id);
    EXPECT_EQ(full.find("100 Continue"), std::string::npos);
    ASSERT_NE(full.find("200 OK"), std::string::npos);
}

// The interim 100 must survive a partial write and must resume at its cursor.
// Restarting would duplicate its prefix (NR-5).
TEST(HttpServiceTest, PartiallyAcceptedContinueResumesWithoutCorruption) {
    constexpr std::string_view CONTINUE = "HTTP/1.1 100 Continue\r\n\r\n";
    auto add_routes = [](Harness& harness, std::string& got) {
        EXPECT_TRUE(harness.service->add_route(
            "GET", "/pad", [](const HTTPRequestView&, HTTPResponse& res) {
                res.set_body(std::string(256, 'p'));
                return Disposition::COMPLETE;
            }));
        EXPECT_TRUE(harness.service->add_route(
            "POST", "/echo",
            [&got](const HTTPRequestView& req, HTTPResponse& res) {
                got.assign(req.body.begin(), req.body.end());
                res.set_body("ok");
                return Disposition::COMPLETE;
            }));
    };

    std::size_t pad_size = 0;
    {
        Harness probe;
        std::string sink;
        add_routes(probe, sink);
        const ConnectionId id = probe.wire->connect();
        pad_size = round_trip(probe, id, "GET /pad HTTP/1.1\r\nHost: h\r\n\r\n")
                       .size();
    }

    // Ten bytes of credit split the interim 100 after the unacknowledged /pad
    // response.
    Harness harness(pad_size + 10);
    std::string got;
    add_routes(harness, got);
    const ConnectionId id = harness.wire->connect();
    harness.wire->deliver_text(id, "GET /pad HTTP/1.1\r\nHost: h\r\n\r\n");
    harness.wire->deliver_text(
        id,
        "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n"
        "Expect: 100-continue\r\n\r\n");
    EXPECT_EQ(harness.wire->send_inflight(id), pad_size + 10);

    harness.flush(id);
    harness.wire->deliver_text(id, "hello");
    harness.flush(id);

    const std::string full = harness.wire->sent_text(id);
    EXPECT_EQ(got, "hello");
    EXPECT_EQ(full.substr(pad_size, CONTINUE.size()), CONTINUE);
    const std::size_t first = full.find("HTTP/1.1 100");
    ASSERT_NE(first, std::string::npos);
    EXPECT_EQ(full.find("HTTP/1.1 100", first + 1), std::string::npos);
    const std::size_t final_pos = full.find("HTTP/1.1 200 OK", pad_size);
    ASSERT_NE(final_pos, std::string::npos);
    EXPECT_EQ(final_pos, pad_size + CONTINUE.size());
}

// An overflowing response head must produce 500 with Connection: close
// (BR-13).
TEST(HttpServiceTest, OversizedResponseHeadYields500NotSilentClose) {
    Harness harness;
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/big", [](const HTTPRequestView&, HTTPResponse& res) {
            for (int i = 0; i < 12; ++i) {
                res.headers.set("X-Pad-" + std::to_string(i),
                                std::string(120, 'v'));
            }
            res.set_body("x");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string response =
        round_trip(harness, id, "GET /big HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 500 Internal Server Error\r\n"));
    EXPECT_NE(response.find("Connection: close\r\n"), std::string::npos);
    EXPECT_FALSE(harness.wire->is_open(id));
}

// The fixed 500 replaces the handler's entire response (BR-13).
TEST(HttpServiceTest, OversizedResponseHeadSendsOnlyTheFixed500) {
    Harness harness;
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/big", [](const HTTPRequestView&, HTTPResponse& res) {
            for (int i = 0; i < 12; ++i) {
                res.headers.set("X-Pad-" + std::to_string(i),
                                std::string(120, 'v'));
            }
            res.set_body("handler-body");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string response =
        round_trip(harness, id, "GET /big HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT_EQ(response,
              "HTTP/1.1 500 Internal Server Error\r\n"
              "Content-Length: 0\r\nConnection: close\r\n\r\n");
    EXPECT_EQ(harness.service->statistics().responses_refused, 1U);
}

// Oversized copied bodies produce the fixed 500 without adoption (BR-14,
// AR-1).
TEST(HttpServiceTest, CopiedBodyOverTheCapIsAnswered500) {
    Harness harness;
    constexpr std::size_t CAP = HttpService::Limits::MAX_BUFFERED_BODY;
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/over", [](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body(std::string(CAP + 1, 'x'));
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string response =
        round_trip(harness, id, "GET /over HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT_EQ(response,
              "HTTP/1.1 500 Internal Server Error\r\n"
              "Content-Length: 0\r\nConnection: close\r\n\r\n");
    EXPECT_FALSE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().responses_refused, 1U);
}

// Direct body writes also enforce the copied-body cap (BR-14).
TEST(HttpServiceTest, ARefusedCopiedBodyIsFreedNotHeld) {
    if (!pico_util::HeapWatermark::measuring()) {
        GTEST_SKIP() << "this build cannot read the live heap";
    }
    Harness harness;
    // Below glibc's mmap threshold, uordblks counts this allocation.
    constexpr std::size_t BIG = std::size_t{64} * 1024;
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/over", [](const HTTPRequestView&, HTTPResponse& res) {
            res.body.assign(BIG, 'x');
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::size_t before = pico_util::HeapWatermark::in_use_bytes();
    const std::string response =
        round_trip(harness, id, "GET /over HTTP/1.1\r\nHost: h\r\n\r\n");
    ASSERT_TRUE(response.starts_with("HTTP/1.1 500 "));
    EXPECT_LT(pico_util::HeapWatermark::in_use_bytes(), before + (BIG / 2));
}

// A reserved capacity above the body cap detects untrimmed adoption. The 8 B
// window keeps the response in flight during measurement (AR-1).
TEST(HttpServiceTest, SpareBodyCapacityIsNotCarriedIntoTheSlot) {
    if (!pico_util::HeapWatermark::measuring()) {
        GTEST_SKIP() << "this build cannot read the live heap";
    }
    Harness harness(/*send_capacity=*/8);
    // Below glibc's mmap threshold, uordblks counts this allocation.
    constexpr std::size_t BIG = std::size_t{64} * 1024;
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/direct", [](const HTTPRequestView&, HTTPResponse& res) {
            res.reserve_body(BIG);
            res.body.assign({'o', 'k'});
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::size_t before = pico_util::HeapWatermark::in_use_bytes();
    harness.wire->deliver_text(id, "GET /direct HTTP/1.1\r\nHost: h\r\n\r\n");
    ASSERT_GT(harness.wire->send_inflight(id),
              0U);  // One copied body within the cap may remain in flight.
    EXPECT_LE(pico_util::HeapWatermark::in_use_bytes(),
              before + HttpService::Limits::MAX_BUFFERED_BODY);
}

// The static-body control isolates copied heap from fake-transport storage
// (AR-1).
TEST(HttpServiceTest, ACopiedBodyStalledMidSendIsFreedWhenThePeerResets) {
    if (!pico_util::HeapWatermark::measuring()) {
        GTEST_SKIP() << "this build cannot read the live heap";
    }
    constexpr std::size_t CAP = HttpService::Limits::MAX_BUFFERED_BODY;
    const auto growth = [](bool copied) {
        Harness harness(/*send_capacity=*/8);
        EXPECT_TRUE(harness.service->add_route(
            "GET", "/body",
            [copied](const HTTPRequestView&, HTTPResponse& res) {
                if (copied) {
                    res.set_body(x_body(CAP));
                } else {
                    res.set_body_static(
                        StaticBody::outliving_response(x_body(CAP)));
                }
                return Disposition::COMPLETE;
            }));
        const ConnectionId id = harness.wire->connect();
        const std::size_t before = pico_util::HeapWatermark::in_use_bytes();
        harness.wire->deliver_text(id, "GET /body HTTP/1.1\r\nHost: h\r\n\r\n");
        EXPECT_GT(harness.wire->send_inflight(id), 0U);
        harness.wire->reset_peer(id);
        return pico_util::HeapWatermark::in_use_bytes() - before;
    };
    const std::size_t control = growth(false);
    EXPECT_LT(growth(true), control + (CAP / 2));
}

// The fake retains sent bytes. A static-body control isolates heap held by
// the idle slot (AR-1).
TEST(HttpServiceTest, ASentCopiedBodyIsNotHeldByTheSlot) {
    if (!pico_util::HeapWatermark::measuring()) {
        GTEST_SKIP() << "this build cannot read the live heap";
    }
    constexpr std::size_t CAP = HttpService::Limits::MAX_BUFFERED_BODY;
    const auto growth = [](bool copied) {
        Harness harness;
        EXPECT_TRUE(harness.service->add_route(
            "GET", "/body",
            [copied](const HTTPRequestView&, HTTPResponse& res) {
                if (copied) {
                    res.set_body(x_body(CAP));
                } else {
                    res.set_body_static(
                        StaticBody::outliving_response(x_body(CAP)));
                }
                return Disposition::COMPLETE;
            }));
        const ConnectionId id = harness.wire->connect();
        const std::size_t before = pico_util::HeapWatermark::in_use_bytes();
        {
            const std::string response = round_trip(
                harness, id, "GET /body HTTP/1.1\r\nHost: h\r\n\r\n");
            EXPECT_TRUE(response.ends_with(x_body(CAP)));
        }
        EXPECT_TRUE(harness.wire->is_open(id));
        return pico_util::HeapWatermark::in_use_bytes() - before;
    };
    const std::size_t control = growth(false);
    EXPECT_LT(growth(true), control + (CAP / 2));
}

// The at-cap body crosses several ring fills (BR-14, AR-5).
TEST(HttpServiceTest, CopiedBodyAtTheCapIsServed) {
    Harness harness(/*send_capacity=*/512);
    constexpr std::size_t CAP = HttpService::Limits::MAX_BUFFERED_BODY;
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/at", [](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body(std::string(CAP, 'x'));
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string response =
        round_trip(harness, id, "GET /at HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(response.ends_with(std::string(CAP, 'x')));
    EXPECT_GT(harness.service->statistics().backpressure_events, 0U);
}

TEST(HttpServiceTest, DrainCloseLingerTimeoutFires) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    // Without ACKs, TR-5 must reclaim the connection waiting for its close to
    // drain.
    harness.wire->deliver_text(
        id, "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    ASSERT_TRUE(harness.wire->is_open(id));
    harness.wire->advance_time(1500);
    EXPECT_TRUE(harness.wire->is_open(id));
    harness.wire->advance_time(1000);
    EXPECT_FALSE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_drain, 1U);
    EXPECT_EQ(harness.service->statistics().disconnect_server_closed, 0U);
}

// Both edges of the 200 ms linger tolerance are checked (TR-5).
TEST(HttpServiceTest, DrainCloseLingerFiresWithinItsBand) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    harness.wire->deliver_text(
        id, "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    harness.wire->advance_time(1800);
    EXPECT_TRUE(harness.wire->is_open(id));
    harness.wire->advance_time(400);
    EXPECT_FALSE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_drain, 1U);
}

// One segmented arrival keeps a CallbackScope across teardown. The pipelined
// request after Connection: close must not be served (RFC 9112 §9.6). The
// closing flag guards it before conn_for() can observe a released slot.
TEST(HttpServiceTest, PipelinedRequestBehindCloseIsNotDispatched) {
    Harness harness;
    int dispatched = 0;
    ASSERT_TRUE(harness.service->add_route(
        "GET", "/x", [&](const HTTPRequestView&, HTTPResponse& response) {
            ++dispatched;
            response.set_body("ok");
            return Disposition::COMPLETE;
        }));

    const ConnectionId id = harness.connect();
    std::array<std::string_view, 2> segments{
        "GET /x HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n",
        "GET /x HTTP/1.1\r\nHost: h\r\n\r\n",
    };
    harness.wire->deliver_segmented(id, segments);
    harness.flush(id);

    EXPECT_EQ(dispatched, 1) << "the request behind the close was served";
    EXPECT_FALSE(harness.wire->is_open(id));
}

TEST(HttpServiceTest, MismatchedProfileBindingAborts) {
    pico_http::ProfileBinding wrong = HttpService::caller_binding();
    wrong.rx_capacity += 1;
    EXPECT_DEATH(
        {
            const HttpService service(std::make_unique<FakeTcpTransport>(),
                                      wrong);
        },
        "");
}

// conn_size and conn_align detect layout differences beyond the named profile
// fields, including fields a downstream profile might add.
TEST(HttpServiceTest, ProfileBindingDetectsSlotLayoutDrift) {
    pico_http::ProfileBinding wrong = HttpService::caller_binding();
    wrong.conn_size += 1;
    EXPECT_DEATH(
        {
            const HttpService service(std::make_unique<FakeTcpTransport>(),
                                      wrong);
        },
        "");

    pico_http::ProfileBinding misaligned = HttpService::caller_binding();
    misaligned.conn_align *= 2;
    EXPECT_DEATH(
        {
            const HttpService service(std::make_unique<FakeTcpTransport>(),
                                      misaligned);
        },
        "");
}

TEST(HttpServiceTest, MatchingProfileBindingConstructs) {
    const HttpService service(std::make_unique<FakeTcpTransport>(),
                              HttpService::caller_binding());
    EXPECT_EQ(service.active_connections(), 0U);
}

// Service-owned framing (NR-3, NR-6).

namespace {

std::size_t count_occurrences(std::string_view haystack,
                              std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string_view::npos;
         at = haystack.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

}  // namespace

TEST(HttpServiceTest, HandlerSetContentLengthDoesNotReachTheWire) {
    // Echoing the unchecked handler length would consume bytes from the next
    // response.
    Harness harness;
    static constexpr std::string_view BODY = "hello";
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/lie", [](const HTTPRequestView&, HTTPResponse& res) {
            EXPECT_TRUE(res.headers.set("Content-Length", "1024"));
            res.set_body_static(StaticBody::outliving_response(BODY),
                                "text/plain");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string resp =
        round_trip(harness, id, "GET /lie HTTP/1.1\r\nHost: x\r\n\r\n");

    EXPECT_EQ(count_occurrences(resp, "Content-Length:"), 1U);
    EXPECT_NE(resp.find("Content-Length: 5\r\n"), std::string::npos);
    EXPECT_EQ(resp.find("1024"), std::string::npos);
    const std::size_t body_start = resp.find("\r\n\r\n") + 4;
    EXPECT_EQ(resp.substr(body_start), BODY);
}

TEST(HttpServiceTest, HandlerSetContentLengthOnAGeneratorBodyIsDropped) {
    // Echoing the handler length would announce both chunked and length
    // framing.
    Harness harness;
    BoundedGenerator gen{.total = 300U};
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/stream", [&](const HTTPRequestView&, HTTPResponse& res) {
            EXPECT_TRUE(res.headers.set("Content-Length", "300"));
            res.set_body_generator(bounded_generator, &gen, "text/plain");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string resp =
        round_trip(harness, id, "GET /stream HTTP/1.1\r\nHost: x\r\n\r\n");

    EXPECT_EQ(resp.find("Content-Length"), std::string::npos);
    EXPECT_EQ(count_occurrences(resp, "Transfer-Encoding: chunked\r\n"), 1U);
}

TEST(HttpServiceTest, HandlerSetTransferEncodingDoesNotReachTheWire) {
    // Announcing chunked framing would make the client parse hello as a chunk
    // size.
    Harness harness;
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/te", [](const HTTPRequestView&, HTTPResponse& res) {
            EXPECT_TRUE(res.headers.set("Transfer-Encoding", "chunked"));
            res.set_body_static(StaticBody::outliving_response("hello"),
                                "text/plain");
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    const std::string resp =
        round_trip(harness, id, "GET /te HTTP/1.1\r\nHost: x\r\n\r\n");

    EXPECT_EQ(resp.find("Transfer-Encoding"), std::string::npos);
    EXPECT_EQ(count_occurrences(resp, "Content-Length: 5\r\n"), 1U);
}

// A transport must support completion locking after server close, even when
// listen failed (AR-3).

TEST(HttpServiceTest, StopTakesTheCompletionLockAfterClosingTheServer) {
    Harness harness;
    const FakeTcpTransport& wire = *harness.wire;

    harness.service->stop();

    EXPECT_TRUE(wire.server_closed());
    EXPECT_GT(wire.completion_locks_after_close_server(), 0U);
    EXPECT_TRUE(wire.completion_lock_balanced());
}

TEST(HttpServiceTest, StopTakesTheCompletionLockWhenListenNeverSucceeded) {
    auto transport = std::make_unique<FakeTcpTransport>();
    FakeTcpTransport& wire = *transport;
    wire.set_listen_result(false);

    HttpService service(std::move(transport));
    EXPECT_FALSE(service.start(Harness::PORT));

    service.stop();

    EXPECT_GT(wire.completion_locks_taken(), 0U);
    EXPECT_TRUE(wire.completion_lock_balanced());
}

// Byte-at-a-time delivery exercises RX state across on_receive calls.
TEST(HttpServiceTest, ARequestDeliveredOneByteAtATimeIsAnsweredIdentically) {
    const std::string request =
        "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello";

    const auto serve = [](Harness& harness) {
        EXPECT_TRUE(harness.service->add_route(
            "POST", "/echo", [](const HTTPRequestView& req, HTTPResponse& res) {
                res.set_body(std::string(req.body.begin(), req.body.end()));
                return Disposition::COMPLETE;
            }));
    };

    Harness whole;
    serve(whole);
    const ConnectionId whole_id = whole.wire->connect();
    const std::string expected = whole.round_trip(whole_id, request);
    ASSERT_NE(expected.find("HTTP/1.1 200"), std::string::npos);
    ASSERT_TRUE(expected.ends_with("hello"));

    Harness split;
    serve(split);
    const ConnectionId split_id = split.wire->connect();
    for (const char byte : request) {
        ASSERT_EQ(
            split.wire->deliver_text(split_id, std::string_view(&byte, 1)), 1U)
            << "the receive window closed mid-request";
    }
    split.flush(split_id);

    EXPECT_EQ(split.wire->sent_text(split_id), expected);
}

// The pipelined pair exercises incremental parsing across request boundaries.
TEST(HttpServiceTest, PipelinedRequestsDeliveredOneByteAtATimeBothAnswered) {
    int served = 0;
    Harness harness;
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/a", [&served](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body(std::to_string(++served));
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();

    const std::string two =
        "GET /a HTTP/1.1\r\nHost: h\r\n\r\nGET /a HTTP/1.1\r\nHost: h\r\n\r\n";
    for (const char byte : two) {
        ASSERT_EQ(harness.wire->deliver_text(id, std::string_view(&byte, 1)),
                  1U);
        harness.flush(id);  // Draining each byte keeps the receive window open.
    }

    const std::string response = harness.wire->sent_text(id);
    EXPECT_EQ(served, 2) << "both pipelined requests must reach the handler";
    const std::size_t first = response.find("HTTP/1.1 200");
    ASSERT_NE(first, std::string::npos);
    ASSERT_NE(response.find("HTTP/1.1 200", first + 1), std::string::npos);
    EXPECT_TRUE(response.ends_with("2"));
}

// The second request is partial in the first arrival. RX compaction with
// retained bytes is covered by
// DeferredTest.PipelinedRequestWaitsForTheDeferralThenIsServed.
TEST(HttpServiceTest, APipelinedRequestHeadMaySpanTwoArrivals) {
    std::vector<std::string> uris;
    Harness harness;
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/a", [&uris](const HTTPRequestView& req, HTTPResponse& res) {
            uris.emplace_back(req.uri);
            res.set_body(std::string(req.uri));
            return Disposition::COMPLETE;
        }));
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/bb", [&uris](const HTTPRequestView& req, HTTPResponse& res) {
            uris.emplace_back(req.uri);
            res.set_body(std::string(req.uri));
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();

    const std::string first = "GET /a HTTP/1.1\r\nHost: h\r\n\r\n";
    const std::string second = "GET /bb HTTP/1.1\r\nHost: h\r\n\r\n";

    // Different URI lengths expose a boundary slip as an incorrect URI.
    const std::size_t head = 12;
    ASSERT_EQ(harness.wire->deliver_text(id, first + second.substr(0, head)),
              first.size() + head);
    harness.flush(id);
    ASSERT_EQ(uris.size(), 1U);

    ASSERT_EQ(harness.wire->deliver_text(id, second.substr(head)),
              second.size() - head);
    harness.flush(id);

    ASSERT_EQ(uris.size(), 2U) << "the split second request was not parsed";
    EXPECT_EQ(uris.at(0), "/a");
    EXPECT_EQ(uris.at(1), "/bb");
    EXPECT_TRUE(harness.wire->sent_text(id).ends_with("/bb"));
}

// The fake records the configured idle window independently of armed
// deadlines (TR-3).
TEST(HttpServiceTest, TheIdleTimeoutTheTransportIsToldFollowsTheConfig) {
    Harness harness;
    EXPECT_EQ(harness.wire->idle_timeout_seconds(),
              HttpService::KeepAliveConfig::DEFAULT_IDLE_TIMEOUT_SECONDS);

    HttpService::KeepAliveConfig cfg;
    cfg.idle_timeout_seconds = 42;
    harness.service->set_keep_alive_config(cfg);
    EXPECT_EQ(harness.wire->idle_timeout_seconds(), 42U);

    cfg.enabled = false;
    harness.service->set_keep_alive_config(cfg);
    EXPECT_EQ(harness.wire->idle_timeout_seconds(), 0U);
}
