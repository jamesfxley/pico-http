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

// NR-8: a URI registered under other methods answers 405 + Allow, not 404.
TEST(HttpServiceTest, KnownUriWrongMethodReturns405WithAllow) {
    Harness harness;
    auto ok = [](const HTTPRequestView&, HTTPResponse& res) {
        res.set_body("ok");
        return Disposition::COMPLETE;
    };
    EXPECT_TRUE(harness.service->add_route("GET", "/thing", ok));
    EXPECT_TRUE(harness.service->add_route("PUT", "/thing", ok));
    const ConnectionId id = harness.wire->connect();
    // DELETE is not registered for /thing, but GET and PUT are: 405 with an
    // Allow header listing the methods that are (RFC 9110 §15.5.6).
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

// NR-8: wildcard routes count towards the Allow list.
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
    // The worst case the Allow buffer is sized for: the maximum method count,
    // each token at the 15-char storage limit. Every token must appear in full.
    constexpr int METHOD_COUNT = 8;
    std::vector<std::string> methods;
    methods.reserve(METHOD_COUNT);
    for (int i = 0; i < METHOD_COUNT; ++i) {
        methods.push_back("CUSTOMMETHODM0" + std::to_string(i));  // 15 chars
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

// --- HEAD (RFC 9110 §9.1: supported wherever GET is) -------------------------

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

    // Header section exactly as GET would produce — including the GET body's
    // Content-Length — but no body bytes follow.
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_NE(response.find("Content-Length: 9\r\n"), std::string::npos);
    EXPECT_TRUE(response.ends_with("\r\n\r\n"));
    ASSERT_TRUE(
        harness.wire->is_open(id));  // framing intact: keep-alive survives

    // The connection is reusable: a GET on the same socket parses cleanly.
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
    EXPECT_TRUE(response.ends_with("\r\n\r\n"));  // still no body
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
    // Framing header as GET would send, but neither chunks nor the last-chunk
    // terminator (RFC 9110 §9.3.2).
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
    EXPECT_TRUE(response.ends_with("\r\n\r\n"));  // status + headers only
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
    EXPECT_TRUE(harness.wire->is_open(id));  // not closed
}

TEST(HttpServiceTest, ConnectionCloseHeaderClosesSocket) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    const std::string response = round_trip(
        harness, id, "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    EXPECT_NE(response.find("Connection: close\r\n"), std::string::npos);
    EXPECT_FALSE(harness.wire->is_open(id));  // closed after send
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
    EXPECT_TRUE(harness.wire->is_open(id));  // first request: stays open

    harness.wire->deliver_text(id, "GET / HTTP/1.1\r\nHost: h\r\n\r\n");
    harness.flush(id);
    EXPECT_FALSE(harness.wire->is_open(id));  // hit the cap: closed
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
    // Two 200 responses, bodies "1" then "2" -- exactly two, in that order.
    const std::size_t first = response.find("HTTP/1.1 200");
    ASSERT_NE(first, std::string::npos);
    const std::size_t second = response.find("HTTP/1.1 200", first + 1);
    ASSERT_NE(second, std::string::npos);
    EXPECT_EQ(response.find("HTTP/1.1 200", second + 1), std::string::npos)
        << "two pipelined requests must produce exactly two responses";
    EXPECT_EQ(n, 2) << "the handler must run once per pipelined request";
    EXPECT_TRUE(response.ends_with("2"));
}

// A body of `size` bytes of 'X' that outlives every response, for the cases
// that need one larger than a copied body may be (BR-14).
namespace {
std::string_view x_body(std::size_t size) {
    static const std::string BODY(200000, 'X');
    return std::string_view(BODY).substr(0, size);
}
}  // namespace

// --- Large responses stream out without truncation (AR-5) -------------------

TEST(HttpServiceTest, LargeResponseStreamsWithoutTruncationUnderBackpressure) {
    Harness harness(
        /*send_capacity=*/512);  // tiny transmit buffer forces resumes
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
    EXPECT_EQ(response.size() - body_start, BODY);  // whole body delivered
    EXPECT_GT(harness.service->statistics().backpressure_events, 0U);
}

// --- Boundary matrix (BR-1..BR-8): violation -> status + Connection: close ---

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
    // Only two fields in the request line — no version token at all.
    const std::string response = round_trip(harness, id, "GET /x\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 400"));
}

// NR-7: an HTTP/1.1 request must carry exactly one Host header.
TEST(HttpServiceTest, Http11WithoutHostReturns400) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    // No Host header on an HTTP/1.1 request (RFC 9112 §3.2).
    const std::string response =
        round_trip(harness, id, "GET / HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 400 Bad Request\r\n"));
}

TEST(HttpServiceTest, UnsupportedHttpVersionReturns505) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    // Well-formed HTTP-version syntax the server does not speak (RFC 9110
    // §15.6.6): 505, not 400.
    const std::string response =
        round_trip(harness, id, "GET /x HTTP/2.0\r\n\r\n");
    EXPECT_TRUE(
        response.starts_with("HTTP/1.1 505 HTTP Version Not Supported"));
}

// --- Teardown accounting (AR-4/RR-1 at the service layer) --------------------

// OR-1: a peer FIN counts as peer_closed, and every timeout reason also adds
// to the timeout total.
TEST(HttpServiceTest, PeerCloseAndTheTimeoutTotalAreCounted) {
    Harness harness;
    const ConnectionId closed = harness.wire->connect();
    harness.wire->close_peer(closed);
    EXPECT_EQ(harness.service->statistics().disconnect_peer_closed, 1U);

    const ConnectionId stalled = harness.wire->connect();
    harness.wire->deliver_text(stalled, "G");
    harness.wire->advance_time(6000);  // past the 5 s header window
    ASSERT_FALSE(harness.wire->is_open(stalled));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_header, 1U);
    EXPECT_EQ(harness.service->statistics().disconnect_timeout, 1U);
    EXPECT_EQ(harness.service->statistics().disconnect_peer_closed, 1U);
}

// OR-1: a local abort is a fault of this side, so it has its own counter and
// is never folded into the peer's reset count.
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

    // The slot is reusable immediately: fill all connections afterwards.
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
    EXPECT_GT(stats.bytes_in, 0U);  // request head counted inbound
}

// --- Static and generator body modes (AR-2, AR-5) ---------------------------

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

// A resumable generator: byte i of the body is (i % 251), a nontrivial pattern
// that catches offset/resume bugs. The writer owns the position, so the same
// function serves the whole body across many backpressured resumes.
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
    Harness harness(/*send_capacity=*/512);  // tiny buffer: forces many resumes
    constexpr std::size_t BODY = std::size_t{256} * 1024;  // PR-8: 256 KB
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

// Counts how many body bytes the generator is asked to produce across all
// resumes. With send_credit clamping, every produced byte is one the transmit
// ring accepted, so the total equals the body size exactly — no stalled chunk
// is ever generated and then discarded and regenerated.
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
        /*send_capacity=*/512);  // tiny ring: many stalls and resumes
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
    // The clamp to send_credit means the generator produced exactly the body
    // once (retires the README "generator regeneration under backpressure"
    // limitation); pre-fix this would exceed BODY as stalled chunks regenerate.
    EXPECT_EQ(gen.bytes_produced, BODY);
    EXPECT_GT(harness.service->statistics().backpressure_events, 0U);
}

// --- Chunked / length-less generator responses (NR-6) ------------------------

// A length-less generator: produces a bounded pattern body and returns 0 once
// `offset` reaches `total`, the end-of-body signal for the two-argument
// set_body_generator. `produced` accumulates every byte the writer asked for,
// so a test can assert the generator ran exactly once over the body.
struct BoundedGenerator {
    std::size_t total = 0;
    std::size_t produced = 0;
};

namespace {
std::size_t bounded_generator(void* ctx, std::size_t offset,
                              std::span<std::uint8_t> out) {
    auto* state = static_cast<BoundedGenerator*>(ctx);
    if (offset >= state->total) {
        return 0;  // end-of-body
    }
    const std::size_t n = std::min(out.size(), state->total - offset);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::uint8_t>((offset + i) % 251U);
    }
    state->produced += n;
    return n;
}
}  // namespace

// Decode Transfer-Encoding: chunked framing back to the payload, so a test can
// compare against what the generator produced.
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
            break;  // last-chunk
        }
        out.append(framed.substr(pos, size));
        pos += size + 2;  // chunk-data + trailing CRLF
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
        /*send_capacity=*/64);  // tiny ring: framing spans many resumes
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
    // send_credit bounds each frame, so the generator produced the body exactly
    // once despite stalling on nearly every chunk.
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

    // No chunked framing an HTTP/1.0 client cannot parse: raw body, closed to
    // delimit it.
    EXPECT_EQ(resp.find("Transfer-Encoding"), std::string::npos);
    EXPECT_EQ(resp.find("Content-Length"), std::string::npos);
    EXPECT_NE(resp.find("Connection: close\r\n"), std::string::npos);
    const std::size_t body_start = resp.find("\r\n\r\n") + 4;
    ASSERT_EQ(resp.size() - body_start, gen.total);
    expect_pattern(std::string_view(resp).substr(body_start));
    EXPECT_FALSE(
        harness.wire->is_open(id));  // connection closed after the body
}

TEST(HttpServiceTest, Http10DowngradeOverridesAHandlerSetKeepAliveHeader) {
    // The close-delimited downgrade is decided after should_keep_alive() has
    // already let the handler's Connection header win, so the head must not
    // echo it: a close-delimited body carries no Content-Length and no
    // Transfer-Encoding, and announcing keep-alive on top of that leaves the
    // client with no way to know where the body ends (NR-2, NR-6).
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
    // The cap bounds slot occupancy, so a handler cannot hold the slot open
    // past it by setting Connection: keep-alive (CXX-12.7).
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
        harness.wire->is_open(id));  // chunked is self-delimiting: stays open

    // A second request on the same connection is served normally.
    const std::string all =
        round_trip(harness, id, "GET /ping HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT_TRUE(all.ends_with("pong"));
}

// --- OR-1 counters -----------------------------------------------------------

// RR-4: the counter set accounts for every connection. Each accepted
// connection is either still open or ended with exactly one disconnect reason,
// the timeout total is the sum of its kinds, and parse_errors the sum of its
// classes -- the identities a soak checks the device's /stats against.
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
            return Disposition::PENDING;  // never resolved: the handler timeout
        }));
    const auto closed = [&] {
        const auto& stats = harness.service->statistics();
        return stats.disconnect_peer_closed + stats.disconnect_peer_reset +
               stats.disconnect_aborted + stats.disconnect_timeout +
               stats.disconnect_server_closed;
    };
    // Every close path, one connection each.
    harness.wire->close_peer(harness.wire->connect());
    harness.wire->reset_peer(harness.wire->connect());
    harness.wire->abort_local(harness.wire->connect());
    round_trip(harness, harness.wire->connect(),
               "BAD\r\n\r\n");  // server close
    round_trip(harness, harness.wire->connect(),
               "GET / HTTP/1.1\r\nHost: h\r\n\r\n");           // then idles
    harness.wire->deliver_text(harness.wire->connect(), "G");  // header
    harness.wire->deliver_text(
        harness.wire->connect(),
        "POST /u HTTP/1.1\r\nHost: h\r\nContent-Length: 10\r\n\r\nab");  // body
    harness.wire->deliver_text(
        harness.wire->connect(),
        "GET /big HTTP/1.1\r\nHost: h\r\n\r\n");  // write
    harness.wire->deliver_text(
        harness.wire->connect(),
        "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");  // drain
    harness.wire->deliver_text(
        harness.wire->connect(),
        "GET /defer HTTP/1.1\r\nHost: h\r\n\r\n");  // handler
    harness.wire->advance_time(
        16000);  // past every timeout, idle's 15 s included

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
    // The next connect finds no free slot: refused at the TCP layer (RR-3).
    EXPECT_FALSE(harness.wire->connect().valid());
    EXPECT_EQ(harness.service->statistics().refusals_slot_full, 1U);
}

TEST(HttpServiceTest, ParseErrorClassCountersAreTracked) {
    Harness harness;
    auto send = [&](std::string_view req) {
        const ConnectionId id = harness.wire->connect();
        round_trip(harness, id, req);
    };
    send(
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 100000\r\n\r\n");  // 413
    send("GET /" + std::string(600, 'a') +
         " HTTP/1.1\r\nHost: h\r\n\r\n");  // 414
    {
        std::string req = "GET / HTTP/1.1\r\nHost: h\r\n";
        for (int i = 0; i < 20; ++i) {
            req += "H" + std::to_string(i) + ": v\r\n";
        }
        req += "\r\n";
        send(req);  // 431
    }
    send(
        "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip\r\n\r\n");  // 501
    send("GET /x\r\n\r\n");           // 400: malformed request line
    send("GET /x HTTP/2.0\r\n\r\n");  // 505: unsupported version

    const auto& text = harness.service->statistics();
    EXPECT_EQ(text.parse_errors_413, 1U);
    EXPECT_EQ(text.parse_errors_414, 1U);
    EXPECT_EQ(text.parse_errors_431, 1U);
    EXPECT_EQ(text.parse_errors_501, 1U);
    EXPECT_EQ(text.parse_errors_400, 1U);
    EXPECT_EQ(text.parse_errors_505, 1U);
    EXPECT_EQ(text.parse_errors, 6U);
}

// --- Timeouts (TR-1..TR-5) enforced via the transport's timeout scan --------

TEST(HttpServiceTest, HeaderTimeoutClosesSlowlorisAndCountsIt) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    // Drip a partial request head, byte by byte, past the 5 s header window.
    // The window is fixed from accept, so drip must not keep resetting it.
    for (int i = 0; i < 4; ++i) {
        harness.wire->deliver_text(id, "G");
        harness.wire->advance_time(2000);  // 4 × 2 s = 8 s > 5 s header timeout
    }
    EXPECT_FALSE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_header, 1U);
}

TEST(HttpServiceTest, BodyProgressTimeoutClosesStalledUpload) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    // Head complete, body declared but stalled: the body-progress deadline
    // (TR-2, 5 s) governs — not the header window and not idle.
    harness.wire->deliver_text(
        id, "POST /u HTTP/1.1\r\nHost: h\r\nContent-Length: 10\r\n\r\nab");
    ASSERT_TRUE(harness.wire->is_open(id));
    harness.wire->advance_time(4000);
    EXPECT_TRUE(harness.wire->is_open(id));  // under the 5 s body window
    harness.wire->advance_time(2000);        // 6 s with no body progress
    EXPECT_FALSE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_body, 1U);
}

// TR-2's tolerance, at both edges: open 200 ms before the 5,000 ms deadline and
// closed 200 ms after it. The fake clock is exact, so the band is what the
// deadline arithmetic gives, not what a scheduler happens to deliver.
TEST(HttpServiceTest, BodyProgressTimeoutFiresWithinItsBand) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    harness.wire->deliver_text(
        id, "POST /u HTTP/1.1\r\nHost: h\r\nContent-Length: 10\r\n\r\nab");
    harness.wire->advance_time(4800);
    EXPECT_TRUE(harness.wire->is_open(id));
    harness.wire->advance_time(400);  // 5,200 ms
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
        harness, id,
        "GET / HTTP/1.1\r\nHost: h\r\n\r\n");  // completes, stays open (idle)
    ASSERT_TRUE(harness.wire->is_open(id));

    harness.wire->advance_time(14000);  // under the 15 s idle timeout
    EXPECT_TRUE(harness.wire->is_open(id));
    harness.wire->advance_time(2000);  // now past it
    EXPECT_FALSE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_idle, 1U);
}

TEST(HttpServiceTest, CompletedRequestClearsHeaderTimeout) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    round_trip(harness, id, "GET / HTTP/1.1\r\nHost: h\r\n\r\n");
    // A completed request must have moved off the 5 s header deadline onto the
    // 15 s idle one, so advancing just past 5 s does not close it.
    harness.wire->advance_time(6000);
    EXPECT_TRUE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_header, 0U);
}

TEST(HttpServiceTest, WriteStallTimeoutClosesSlowReader) {
    Harness harness(
        /*send_capacity=*/512);  // tiny buffer: response can't fully send
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/big", [](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body_static(StaticBody::outliving_response(x_body(100000)));
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    harness.wire->deliver_text(id, "GET /big HTTP/1.1\r\nHost: h\r\n\r\n");
    // Response is now stalled on backpressure (peer never acks). The write
    // deadline (10 s) should fire without any ACK progress.
    ASSERT_TRUE(harness.wire->is_open(id));
    harness.wire->advance_time(11000);
    EXPECT_FALSE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_write, 1U);
}

// TR-4's tolerance, at both edges: open 200 ms before the 10,000 ms write
// deadline and closed 200 ms after it, with no ACK progress throughout.
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
    harness.wire->advance_time(400);  // 10,200 ms
    EXPECT_FALSE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_write, 1U);
}

// A partial next request that surfaces only after a backpressured response
// finished draining must be governed by the 5 s header window (TR-1), not the
// stale 10 s write-stall deadline left armed from the previous response.
TEST(HttpServiceTest,
     PartialPipelinedRequestAfterBackpressureUsesHeaderTimeout) {
    Harness harness(/*send_capacity=*/512);  // force response A to backpressure
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/big", [](const HTTPRequestView&, HTTPResponse& res) {
            res.set_body_static(StaticBody::outliving_response(x_body(4000)));
            return Disposition::COMPLETE;
        }));
    const ConnectionId id = harness.wire->connect();
    // Request A (head complete) immediately followed by the first bytes of
    // request B's head. A's large response stalls under backpressure.
    harness.wire->deliver_text(id,
                               "GET /big HTTP/1.1\r\nHost: h\r\n\r\nGET /bi");
    harness.flush(id);  // drain A; B's partial head is now pending
    ASSERT_TRUE(harness.wire->is_open(id));

    harness.wire->advance_time(
        6000);  // past the 5 s header window, under 10 s write
    EXPECT_FALSE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_header, 1U);
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_write, 0U);
}

// NR-5: a request carrying "Expect: 100-continue" gets an interim 100 response
// before its body, and the final response follows it in order.
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
    // Client sends only the head and waits for 100 before transmitting the
    // body.
    harness.wire->deliver_text(
        id,
        "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n"
        "Expect: 100-continue\r\n\r\n");
    harness.flush(id);
    const std::string interim = harness.wire->sent_text(id);
    EXPECT_NE(interim.find("HTTP/1.1 100 Continue\r\n\r\n"), std::string::npos);
    ASSERT_TRUE(harness.wire->is_open(id));

    // Body arrives; the handler runs and the 200 follows the 100 in order.
    harness.wire->deliver_text(id, "hello");
    harness.flush(id);
    EXPECT_EQ(got, "hello");
    const std::string full = harness.wire->sent_text(id);
    ASSERT_NE(full.find("100 Continue"), std::string::npos);
    ASSERT_NE(full.find("200 OK"), std::string::npos);
    EXPECT_LT(full.find("100 Continue"), full.find("200 OK"));
}

// NR-5: RFC 9110 10.1.1 requires HTTP/1.0 to ignore this expectation.
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
    // Sending the head alone must not produce an interim response.
    harness.wire->deliver_text(
        id,
        "POST /echo HTTP/1.0\r\nHost: h\r\nContent-Length: 5\r\n"
        "Expect: 100-continue\r\n\r\n");
    harness.flush(id);
    const std::string interim = harness.wire->sent_text(id);
    EXPECT_TRUE(interim.empty());
    ASSERT_TRUE(harness.wire->is_open(id));

    // The body is read normally and the handler produces the final 200.
    harness.wire->deliver_text(id, "hello");
    harness.flush(id);
    EXPECT_EQ(got, "hello");
    const std::string full = harness.wire->sent_text(id);
    EXPECT_EQ(full.find("100 Continue"), std::string::npos);
    ASSERT_NE(full.find("200 OK"), std::string::npos);
}

// NR-5 regression: the interim 100 must survive a partially-accepted write. A
// pipelined "Expect: 100-continue" head can complete while the previous
// response still occupies the transmit ring, so the small write is not
// all-or-nothing; the service must resume the interim response from its byte
// cursor, never restart it (a restart puts "HTTP/1.1 1HTTP/1.1 100 Continue"
// on the wire).
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

    // Measure the exact /pad response size on a wide-open ring.
    std::size_t pad_size = 0;
    {
        Harness probe;
        std::string sink;
        add_routes(probe, sink);
        const ConnectionId id = probe.wire->connect();
        pad_size = round_trip(probe, id, "GET /pad HTTP/1.1\r\nHost: h\r\n\r\n")
                       .size();
    }

    // Ring sized so that with the whole /pad response unacked in flight, only
    // 10 bytes of credit remain: the interim 100 is accepted only partially.
    Harness harness(pad_size + 10);
    std::string got;
    add_routes(harness, got);
    const ConnectionId id = harness.wire->connect();
    harness.wire->deliver_text(id, "GET /pad HTTP/1.1\r\nHost: h\r\n\r\n");
    harness.wire->deliver_text(
        id,
        "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n"
        "Expect: 100-continue\r\n\r\n");
    // Nothing acked yet: exactly 10 bytes of the interim 100 went out.
    EXPECT_EQ(harness.wire->send_inflight(id), pad_size + 10);

    harness.flush(id);  // peer reads; the remainder resumes mid-message
    harness.wire->deliver_text(id, "hello");
    harness.flush(id);

    const std::string full = harness.wire->sent_text(id);
    EXPECT_EQ(got, "hello");
    // Exactly one interim response, contiguous, directly after /pad's bytes.
    EXPECT_EQ(full.substr(pad_size, CONTINUE.size()), CONTINUE);
    const std::size_t first = full.find("HTTP/1.1 100");
    ASSERT_NE(first, std::string::npos);
    EXPECT_EQ(full.find("HTTP/1.1 100", first + 1), std::string::npos);
    // ...and the final response follows it in order.
    const std::size_t final_pos = full.find("HTTP/1.1 200 OK", pad_size);
    ASSERT_NE(final_pos, std::string::npos);
    EXPECT_EQ(final_pos, pad_size + CONTINUE.size());
}

// A handler whose header block overflows the fixed head buffer must still get a
// real status line (a 500) + Connection: close, never an opaque silent close
// (BR-13).
TEST(HttpServiceTest, OversizedResponseHeadYields500NotSilentClose) {
    Harness harness;
    EXPECT_TRUE(harness.service->add_route(
        "GET", "/big", [](const HTTPRequestView&, HTTPResponse& res) {
            // Many headers whose total far exceeds
            // HEAD_CAPACITY (1024 B); each value stays under
            // the per-value cap.
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

// BR-13 in full: the fixed 500 head is the whole response. Nothing of the
// handler's own head or body follows it.
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

// BR-14: a copied body is held in the slot until the response is sent, so one
// over MAX_BUFFERED_BODY is refused with the fixed 500 rather than adopted.
// That is what makes AR-1's MAX_CONNECTIONS x MAX_BUFFERED_BODY bound hold.
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

// BR-14: a copied body the handler wrote directly, bypassing set_body, is
// refused and not held once the fixed 500 is answered.
TEST(HttpServiceTest, ARefusedCopiedBodyIsFreedNotHeld) {
    if (!pico_util::HeapWatermark::measuring()) {
        GTEST_SKIP() << "this build cannot read the live heap";
    }
    Harness harness;
    // Below glibc's mmap threshold, so the allocation is one uordblks counts.
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

// AR-1: a body within the cap does not carry spare capacity into the slot. The
// slot holds its body until the response is sent, so a body whose capacity was
// reserved past the cap would hold more than MAX_BUFFERED_BODY while in flight.
// An 8 B send window keeps the response from completing while the heap is read.
TEST(HttpServiceTest, SpareBodyCapacityIsNotCarriedIntoTheSlot) {
    if (!pico_util::HeapWatermark::measuring()) {
        GTEST_SKIP() << "this build cannot read the live heap";
    }
    Harness harness(/*send_capacity=*/8);
    // Below glibc's mmap threshold, so the allocation is one uordblks counts.
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
              0U);  // the response is in flight
    // What the slot may hold in flight is one body within the cap.
    EXPECT_LE(pico_util::HeapWatermark::in_use_bytes(),
              before + HttpService::Limits::MAX_BUFFERED_BODY);
}

// AR-1: a connection torn down mid-send frees the copied body it held, so the
// slot is idle with nothing allocated. The same steps with a static body are
// the control, as in ASentCopiedBodyIsNotHeldByTheSlot below.
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

// AR-1: once a copied body is on the wire the slot frees it, so an idle
// keep-alive connection holds no copied-body heap and the peak stays within
// MAX_CONNECTIONS x MAX_BUFFERED_BODY. The fake transport keeps every byte it
// sends, so the same bytes served as a static body are the control: the
// difference between the two is what the slot holds.
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
        EXPECT_TRUE(harness.wire->is_open(id));  // idle on keep-alive
        return pico_util::HeapWatermark::in_use_bytes() - before;
    };
    const std::size_t control = growth(false);
    EXPECT_LT(growth(true), control + (CAP / 2));
}

// BR-14's limit is inclusive: a copied body of exactly MAX_BUFFERED_BODY is
// served whole, and through backpressure, since a 512 B send window needs
// several resumes to carry it (AR-5).
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
    Harness
        harness;  // ample send buffer: the whole response is handed to the wire
    const ConnectionId id = harness.wire->connect();
    // Connection: close: the service hands off the full response then calls
    // close_client, which arms the 2 s drain linger. We never ack, so the
    // transport is stuck waiting for the peer to drain — TR-5 must fire.
    harness.wire->deliver_text(
        id, "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    ASSERT_TRUE(harness.wire->is_open(id));  // draining, awaiting ack
    harness.wire->advance_time(1500);
    EXPECT_TRUE(harness.wire->is_open(id));  // under the 2 s linger
    harness.wire->advance_time(1000);        // total 2.5 s: past it
    EXPECT_FALSE(harness.wire->is_open(id));
    // Drain-linger expiry is its own reason, distinct from a graceful close, so
    // an undrained peer is not miscounted as server_closed.
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_drain, 1U);
    EXPECT_EQ(harness.service->statistics().disconnect_server_closed, 0U);
}

// TR-5's tolerance, at both edges: open 200 ms before the 2,000 ms linger ends
// and closed 200 ms after it.
TEST(HttpServiceTest, DrainCloseLingerFiresWithinItsBand) {
    Harness harness;
    const ConnectionId id = harness.wire->connect();
    harness.wire->deliver_text(
        id, "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    harness.wire->advance_time(1800);
    EXPECT_TRUE(harness.wire->is_open(id));
    harness.wire->advance_time(400);  // 2,200 ms
    EXPECT_FALSE(harness.wire->is_open(id));
    EXPECT_EQ(harness.service->statistics().disconnect_timeout_drain, 1U);
}

// One arrival split across segments, the way lwIP delivers a chained pbuf: the
// only shape that raises a teardown *between* segments of a single arrival,
// which deliver() cannot reach because it opens a scope per call. A request
// pipelined behind a "Connection: close" must not be served (RFC 9112 §9.6).
//
// Two independent guards hold this: the connection's `closing` flag, and
// conn_for() refusing to resolve an id the transport has already released. The
// first is what this exercises today — every service-initiated teardown sets
// `closing` before releasing the slot — so the test pins the behaviour rather
// than discriminating between the two mechanisms.
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
    // "Connection: close" on the first request tears the slot down as soon as
    // its response drains; the second segment arrives inside the same scope.
    std::array<std::string_view, 2> segments{
        "GET /x HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n",
        "GET /x HTTP/1.1\r\nHost: h\r\n\r\n",
    };
    harness.wire->deliver_segmented(id, segments);
    harness.flush(id);

    EXPECT_EQ(dispatched, 1) << "the request behind the close was served";
    EXPECT_FALSE(harness.wire->is_open(id));
}

// A translation unit compiled with different profile macros than the library
// disagrees with it about the layout of every type crossing the constructor.
// The check exists because that failure is otherwise silent.
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

// The named knobs are not the only ones that move the slot layout:
// HEAD_CAPACITY, GENERATOR_CHUNK and the parser's limits do too, and a
// downstream profile may add more. conn_size/conn_align stand in for all of
// them, so a disagreement that none of the named fields would show still
// aborts.
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

// --- Framing is the service's alone (NR-3, NR-6) -----------------------------

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
    // A handler-set length is never compared against the body, so echoing one
    // would frame a 5-byte body as 1024 bytes and leave the client reading the
    // next pipelined response as the remainder.
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
    // The dangerous variant: a length-less generator frames as chunked, so
    // echoing the handler's Content-Length would emit both framings at once.
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
    // Chunked framing announced over a buffered body would have the client
    // read "hello" as a chunk-size line.
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

// --- Completion-lock lifetime (AR-3) ----------------------------------------
//
// stop() closes the server and then clears every slot under the completion
// lock, so a transport whose lock does not outlive its server is broken. These
// pin the two orderings a transport must survive; the firmware transport
// satisfies them by owning the critical section for the object's lifetime.

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

// EN-04.5 for the service's own incremental entry point.
// HttpService::on_receive assembles bytes in rx_/rx_len_ and compacts the front
// of that buffer when a request is consumed but bytes remain -- state carried
// across calls exactly as the parser's is. Segmentation here is chosen by the
// lwIP pbuf chain, not by the caller, and the finest split anywhere else in
// this suite was two deliver_text() calls.
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

// The same, for two pipelined requests: the second arrives while the first is
// still in the buffer, so the front-compaction runs with a non-empty remainder
// -- the path an off-by-one in that std::copy would corrupt.
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
        harness.flush(id);  // drain as we go, so the window never closes
    }

    const std::string response = harness.wire->sent_text(id);
    EXPECT_EQ(served, 2) << "both pipelined requests must reach the handler";
    const std::size_t first = response.find("HTTP/1.1 200");
    ASSERT_NE(first, std::string::npos);
    ASSERT_NE(response.find("HTTP/1.1 200", first + 1), std::string::npos);
    EXPECT_TRUE(response.ends_with("2"));
}

// A pipelined request whose head spans two arrivals, behind a request that
// already completed in the first of them. Nothing else in the suite delivers a
// *partial* second request alongside a whole first one.
//
// Deliberately not claimed as cover for the rx front-compaction. That copy only
// moves bytes when the receive loop stops with some still buffered, and the one
// break that does so is the deferral (`conn.pending`) -- on INCOMPLETE the
// parser has consumed everything, and on an error the connection closes and the
// remainder is never read again. DeferredTest.PipelinedRequestWaitsForThe-
// DeferralThenIsServed is what covers that copy; an off-by-one in it fails
// there and nowhere else, including here.
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

    // One arrival: all of the first request and the first 12 bytes of the
    // second. The two URIs differ in length so a boundary slip between them
    // shows up as a wrong URI rather than a wrong byte count.
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

// TR-3's configured value, as distinct from its armed deadline. The transport
// is told the idle window through set_idle_timeout, and the double records it
// so a test can assert it -- but nothing read it, so the accessor and the
// configuration path it reports on were both unexercised.
TEST(HttpServiceTest, TheIdleTimeoutTheTransportIsToldFollowsTheConfig) {
    Harness harness;
    EXPECT_EQ(harness.wire->idle_timeout_seconds(),
              HttpService::KeepAliveConfig::DEFAULT_IDLE_TIMEOUT_SECONDS);

    HttpService::KeepAliveConfig cfg;
    cfg.idle_timeout_seconds = 42;
    harness.service->set_keep_alive_config(cfg);
    EXPECT_EQ(harness.wire->idle_timeout_seconds(), 42U);

    // Keep-alive off means no idle window at all, not the default one.
    cfg.enabled = false;
    harness.service->set_keep_alive_config(cfg);
    EXPECT_EQ(harness.wire->idle_timeout_seconds(), 0U);
}
