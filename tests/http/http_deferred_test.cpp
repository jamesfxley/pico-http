// SPDX-License-Identifier: MIT
// Deferred completion and its limits (AR-9, BR-12, TR-6).
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "pico_http/http/detail/pending_pool.hpp"
#include "pico_http/http/http_service.hpp"
#include "pico_http/http/message.hpp"
#include "pico_http/testing/service_harness.hpp"

using pico_http::Disposition;
using pico_http::HTTPRequestView;
using pico_http::HTTPResponse;
using pico_http::HttpService;
using pico_http::RequestId;
using Harness = pico_http::testing::ServiceHarness;
using ConnectionId = HttpService::ConnectionId;

namespace {

constexpr std::string_view GET_DEFER = "GET /defer HTTP/1.1\r\nHost: x\r\n\r\n";
constexpr std::string_view GET_NOW = "GET /now HTTP/1.1\r\nHost: x\r\n\r\n";

// The handler retains only the RequestId (AR-9).
struct Deferrer {
    std::vector<RequestId> parked;
    // The saved URI detects changes to the request view during deferral.
    std::string uri_at_dispatch;
    // The count detects a filler run for a request the service should have
    // rejected, even without wire output.
    std::string body = "late";
    int fills = 0;

    static void fill(void* ctx, HTTPResponse& response) {
        auto* self = static_cast<Deferrer*>(ctx);
        ++self->fills;
        response.set_body(self->body, "text/plain");
    }

    HttpService::RequestHandler handler() {
        return [this](const HTTPRequestView& request, HTTPResponse&) {
            uri_at_dispatch = std::string(request.uri);
            parked.push_back(request.request);
            return Disposition::PENDING;
        };
    }
};

struct DeferHarness {
    Harness harness;
    Deferrer deferrer;

    explicit DeferHarness(std::size_t capacity = Harness::DEFAULT_SEND_CAPACITY)
        : harness(capacity) {
        EXPECT_TRUE(
            harness.service->add_route("GET", "/defer", deferrer.handler()));
        EXPECT_TRUE(harness.service->add_route(
            "GET", "/now", [](const HTTPRequestView&, HTTPResponse& res) {
                res.set_body("now", "text/plain");
                return Disposition::COMPLETE;
            }));
    }

    [[nodiscard]] ConnectionId open_and_defer(
        std::string_view request = GET_DEFER) const {
        const ConnectionId id = harness.connect();
        harness.wire->deliver_text(id, request);
        return id;
    }

    bool resolve(RequestId id, std::string_view body = "late") {
        deferrer.body = std::string(body);
        const bool recorded =
            harness.service->respond(id, &Deferrer::fill, &deferrer);
        harness.wire->run_pending_completions();
        return recorded;
    }

    [[nodiscard]] RequestId last_parked() const {
        return deferrer.parked.back();
    }
};

}  // namespace

TEST(DeferredTest, PendingHandlerSendsNothingUntilRespond) {
    DeferHarness fixture;
    const ConnectionId id = fixture.open_and_defer();

    EXPECT_EQ(fixture.harness.wire->sent_text(id), "");
    EXPECT_EQ(fixture.harness.service->statistics().pending_dispatches, 1U);
    EXPECT_EQ(fixture.harness.service->pending_responses(), 1U);

    EXPECT_TRUE(fixture.resolve(fixture.last_parked()));
    fixture.harness.flush(id);

    const std::string sent = fixture.harness.wire->sent_text(id);
    EXPECT_NE(sent.find("200 OK"), std::string::npos);
    EXPECT_NE(sent.find("late"), std::string::npos);
    EXPECT_EQ(fixture.harness.service->pending_responses(), 0U);
}

TEST(DeferredTest, RespondPerformsNoSendItselfAndRingsTheDoorbell) {
    DeferHarness fixture;
    const ConnectionId id = fixture.open_and_defer();

    // respond() makes no send or lwIP call before the async turn (AR-9).
    EXPECT_TRUE(fixture.harness.service->respond(
        fixture.last_parked(), &Deferrer::fill, &fixture.deferrer));
    EXPECT_EQ(fixture.harness.wire->sent_text(id), "")
        << "respond() sent the response inline instead of deferring it to the "
           "transport's context";
    EXPECT_EQ(fixture.deferrer.fills, 0)
        << "respond() ran the filler inline; it must run on the turn, where "
           "the "
           "service can still check the request is live";
    EXPECT_EQ(fixture.harness.wire->completion_signals(), 1U);

    EXPECT_TRUE(fixture.harness.wire->run_pending_completions());
    fixture.harness.flush(id);
    EXPECT_EQ(fixture.deferrer.fills, 1);
    EXPECT_NE(fixture.harness.wire->sent_text(id).find("late"),
              std::string::npos);
}

TEST(DeferredTest, RequestViewSurvivesTheDeferralWindow) {
    DeferHarness fixture;
    const ConnectionId id = fixture.open_and_defer();
    EXPECT_EQ(fixture.deferrer.uri_at_dispatch, "/defer");

    // The parser must retain the request for the handler's views and
    // completion framing.
    EXPECT_TRUE(fixture.resolve(fixture.last_parked(), "ok"));
    fixture.harness.flush(id);

    const std::string sent = fixture.harness.wire->sent_text(id);
    EXPECT_TRUE(sent.starts_with("HTTP/1.1 200 OK\r\n")) << sent;
}

TEST(DeferredTest, PipelinedRequestWaitsForTheDeferralThenIsServed) {
    DeferHarness fixture;
    const ConnectionId id = fixture.harness.connect();
    // The second request must not dispatch while the first is parked.
    // Resetting the parser would invalidate the first request and reorder
    // replies (AR-9).
    fixture.harness.wire->deliver_text(
        id, std::string(GET_DEFER) + std::string(GET_NOW));

    EXPECT_EQ(fixture.harness.service->statistics().pending_dispatches, 1U);
    EXPECT_EQ(fixture.harness.wire->sent_text(id), "");

    EXPECT_TRUE(fixture.resolve(fixture.last_parked(), "first"));
    fixture.harness.flush(id);

    const std::string sent = fixture.harness.wire->sent_text(id);
    const std::size_t first = sent.find("first");
    const std::size_t second = sent.find("now");
    EXPECT_NE(first, std::string::npos) << sent;
    EXPECT_NE(second, std::string::npos) << sent;
    EXPECT_LT(first, second) << "pipelined response overtook the deferred one";
}

TEST(DeferredTest, RequestArrivingDuringTheDeferralIsNotDispatched) {
    DeferHarness fixture;
    const ConnectionId id = fixture.open_and_defer();
    ASSERT_EQ(fixture.harness.service->statistics().pending_dispatches, 1U);
    const std::string parked_uri = fixture.deferrer.uri_at_dispatch;

    // Separate delivery re-enters process() with a parked request (AR-9).
    fixture.harness.wire->deliver_text(id, GET_NOW);

    // Without the pending guard, the parser presents the first request again.
    // Only the invocation count detects that redispatch.
    EXPECT_EQ(fixture.deferrer.parked.size(), 1U)
        << "the parked request was dispatched to its handler again";
    EXPECT_EQ(fixture.harness.service->statistics().pending_dispatches, 1U);
    EXPECT_EQ(fixture.harness.service->statistics().requests_completed, 0U);
    EXPECT_EQ(fixture.harness.wire->sent_text(id), "");
    EXPECT_EQ(fixture.deferrer.uri_at_dispatch, parked_uri);

    EXPECT_TRUE(fixture.resolve(fixture.last_parked(), "first"));
    fixture.harness.flush(id);

    const std::string sent = fixture.harness.wire->sent_text(id);
    const std::size_t first = sent.find("first");
    const std::size_t second = sent.find("now");
    EXPECT_NE(first, std::string::npos) << sent;
    EXPECT_NE(second, std::string::npos)
        << "the request held during the deferral was never served: " << sent;
    EXPECT_LT(first, second);
}

TEST(DeferredTest, ParkedDeferralWithBufferedBytesCountsAsWindowThrottle) {
    DeferHarness fixture;
    const ConnectionId id = fixture.open_and_defer();
    ASSERT_EQ(fixture.harness.service->statistics().rx_window_throttle_events,
              0U);

    // Bytes held behind a parked handler must count as receive-window
    // throttling until completion or timeout (TR-6, OR-1).
    fixture.harness.wire->deliver_text(id, GET_NOW);

    EXPECT_GE(fixture.harness.service->statistics().rx_window_throttle_events,
              1U)
        << "a deferral holding the window closed was not counted";
}

TEST(DeferredTest, RespondAfterPeerResetIsInert) {
    DeferHarness fixture;
    const ConnectionId id = fixture.open_and_defer();
    const RequestId parked = fixture.last_parked();

    fixture.harness.wire->reset_peer(id);
    // The disconnect must release the deferral against the cap before the
    // handler deadline (BR-12, TR-6).
    EXPECT_EQ(fixture.harness.service->pending_responses(), 0U);

    // A late completion must not write into an unowned or reassigned slot
    // (AR-4).
    EXPECT_FALSE(fixture.resolve(parked, "too late"));
    EXPECT_EQ(fixture.deferrer.fills, 0)
        << "the filler ran for a request whose connection was gone";
    EXPECT_EQ(fixture.harness.service->pending_responses(), 0U);
}

TEST(DeferredTest, LateCompletionCannotReachAnotherConnectionsResponse) {
    DeferHarness fixture;
    const ConnectionId first = fixture.open_and_defer();
    const RequestId abandoned = fixture.last_parked();

    fixture.harness.wire->reset_peer(first);

    // A late write through a retained response pointer would corrupt the
    // second connection's response.
    const ConnectionId second = fixture.open_and_defer();
    const RequestId live = fixture.last_parked();
    ASSERT_FALSE(abandoned == live);

    EXPECT_FALSE(
        fixture.resolve(abandoned, "leaked from the first connection"));
    EXPECT_EQ(fixture.deferrer.fills, 0);
    fixture.harness.flush(second);
    EXPECT_EQ(fixture.harness.wire->sent_text(second), "")
        << "an abandoned completion wrote into a live connection's response";

    EXPECT_TRUE(fixture.resolve(live, "mine"));
    fixture.harness.flush(second);
    const std::string sent = fixture.harness.wire->sent_text(second);
    EXPECT_NE(sent.find("mine"), std::string::npos) << sent;
    EXPECT_EQ(sent.find("leaked"), std::string::npos) << sent;
}

TEST(DeferredTest, CompletionForAnEarlierRequestOnTheSameConnectionIsInert) {
    DeferHarness fixture;
    const ConnectionId id = fixture.open_and_defer();
    const RequestId first = fixture.last_parked();

    EXPECT_TRUE(fixture.resolve(first, "first"));
    fixture.harness.flush(id);
    ASSERT_NE(fixture.harness.wire->sent_text(id).find("first"),
              std::string::npos);

    // The ConnectionId is unchanged on keep-alive. Only the RequestId
    // sequence rejects the earlier request's late completion.
    fixture.harness.wire->deliver_text(id, GET_DEFER);
    const RequestId second = fixture.last_parked();
    ASSERT_EQ(first.connection, second.connection);
    ASSERT_NE(first.sequence, second.sequence);

    EXPECT_FALSE(fixture.resolve(first, "stale"))
        << "a completion for request 1 resolved request 2";
    EXPECT_EQ(fixture.deferrer.fills, 1) << "the stale filler ran";

    EXPECT_TRUE(fixture.resolve(second, "second"));
    fixture.harness.flush(id);
    const std::string sent = fixture.harness.wire->sent_text(id);
    EXPECT_NE(sent.find("second"), std::string::npos) << sent;
    EXPECT_EQ(sent.find("stale"), std::string::npos) << sent;
}

TEST(DeferredTest, RespondAfterHandlerTimeoutIsInert) {
    DeferHarness fixture;
    const ConnectionId id = fixture.open_and_defer();
    const RequestId parked = fixture.last_parked();

    const auto& timeouts = fixture.harness.service->timeout_config();
    fixture.harness.wire->advance_time(timeouts.handler_ms + 1);

    const auto& stats = fixture.harness.service->statistics();
    EXPECT_EQ(stats.disconnect_timeout_handler, 1U);
    EXPECT_EQ(fixture.harness.service->pending_responses(), 0U);
    EXPECT_FALSE(fixture.harness.wire->is_open(id));

    EXPECT_FALSE(fixture.resolve(parked, "way too late"));
    EXPECT_EQ(fixture.deferrer.fills, 0);
}

TEST(DeferredTest, HandlerDeadlineDoesNotFireBeforeItsWindow) {
    DeferHarness fixture;
    const ConnectionId id = fixture.open_and_defer();

    const auto& timeouts = fixture.harness.service->timeout_config();
    fixture.harness.wire->advance_time(timeouts.handler_ms - 1);

    EXPECT_EQ(fixture.harness.service->statistics().disconnect_timeout_handler,
              0U);
    EXPECT_TRUE(fixture.harness.wire->is_open(id));
    EXPECT_TRUE(fixture.resolve(fixture.last_parked()));
}

TEST(DeferredTest, SecondRespondForTheSameRequestIsInert) {
    DeferHarness fixture;
    const ConnectionId id = fixture.open_and_defer();
    const RequestId parked = fixture.last_parked();

    EXPECT_TRUE(fixture.resolve(parked, "once"));
    fixture.harness.flush(id);
    const std::string after_first = fixture.harness.wire->sent_text(id);

    EXPECT_FALSE(fixture.resolve(parked, "twice"));
    fixture.harness.flush(id);
    EXPECT_EQ(fixture.harness.wire->sent_text(id), after_first)
        << "a duplicate completion wrote a second response";
    EXPECT_EQ(fixture.deferrer.fills, 1);
}

TEST(DeferredTest, HandlerReceivesTheRequestItMustRespondOn) {
    DeferHarness fixture;
    // Two live connections expose a handle that names the wrong peer.
    const ConnectionId first = fixture.open_and_defer();
    const ConnectionId second = fixture.open_and_defer();

    ASSERT_EQ(fixture.deferrer.parked.size(), 2U);
    EXPECT_EQ(fixture.deferrer.parked[0].connection, first);
    EXPECT_EQ(fixture.deferrer.parked[1].connection, second);

    EXPECT_TRUE(fixture.resolve(fixture.deferrer.parked[1], "second"));
    fixture.harness.flush(second);
    EXPECT_NE(fixture.harness.wire->sent_text(second).find("second"),
              std::string::npos);
    EXPECT_EQ(fixture.harness.wire->sent_text(first), "");
}

TEST(DeferredTest, RespondOnAnUnknownRequestIsInert) {
    DeferHarness fixture;
    EXPECT_FALSE(fixture.harness.service->respond(RequestId{}, &Deferrer::fill,
                                                  &fixture.deferrer));
    const ConnectionId id = fixture.harness.connect();
    EXPECT_FALSE(fixture.harness.service->respond(
        RequestId{.connection = id, .sequence = 1}, &Deferrer::fill,
        &fixture.deferrer));
    const ConnectionId parked = fixture.open_and_defer();
    // A null filler is rejected rather than recorded.
    EXPECT_FALSE(fixture.harness.service->respond(fixture.last_parked(),
                                                  nullptr, &fixture.deferrer));
    EXPECT_EQ(fixture.harness.wire->sent_text(parked), "");
}

TEST(DeferredTest, ServerStopReleasesEveryDeferral) {
    DeferHarness fixture;
    const ConnectionId id = fixture.open_and_defer();
    EXPECT_TRUE(fixture.harness.wire->is_open(id));
    EXPECT_EQ(fixture.harness.service->pending_responses(), 1U);

    fixture.harness.service->stop();
    EXPECT_EQ(fixture.harness.service->pending_responses(), 0U);
}

TEST(DeferredTest, HandlerRespondingInlineIsCompletedRatherThanParked) {
    Harness harness;
    Deferrer deferrer;
    // An async completion source can finish synchronously inside the handler.
    ASSERT_TRUE(harness.service->add_route(
        "GET", "/inline",
        [&harness, &deferrer](const HTTPRequestView& request, HTTPResponse&) {
            deferrer.body = "inline";
            harness.service->respond(request.request, &Deferrer::fill,
                                     &deferrer);
            return Disposition::PENDING;
        }));

    const ConnectionId id = harness.connect();
    harness.wire->deliver_text(id, "GET /inline HTTP/1.1\r\nHost: x\r\n\r\n");
    harness.flush(id);

    const std::string sent = harness.wire->sent_text(id);
    EXPECT_NE(sent.find("200 OK"), std::string::npos) << sent;
    EXPECT_NE(sent.find("inline"), std::string::npos) << sent;
    EXPECT_EQ(deferrer.fills, 1);
    // Completion before the handler returns leaves no parked request (TR-6).
    EXPECT_EQ(harness.service->pending_responses(), 0U);
    EXPECT_EQ(harness.service->statistics().requests_completed, 1U);
}

// Deferral capacity (BR-12).

TEST(DeferredTest, DeferralCapAnswers503AndRecoversWhenOneResolves) {
    DeferHarness fixture;
    std::vector<RequestId> ids;
    ids.reserve(HttpService::MAX_PENDING);
    for (std::size_t i = 0; i < HttpService::MAX_PENDING; ++i) {
        static_cast<void>(fixture.open_and_defer());
        ids.push_back(fixture.last_parked());
    }
    EXPECT_EQ(fixture.harness.service->pending_responses(),
              HttpService::MAX_PENDING);

    // The overload response needs no pool entry (BR-12).
    const ConnectionId refused = fixture.open_and_defer();
    fixture.harness.flush(refused);

    const std::string sent = fixture.harness.wire->sent_text(refused);
    // The exact bytes pin the boundary response's head and body (AR-13).
    EXPECT_EQ(sent,
              "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\n"
              "Content-Length: 19\r\nConnection: close\r\nServer: pico\r\n"
              "\r\nService Unavailable");

    const auto& stats = fixture.harness.service->statistics();
    EXPECT_EQ(stats.deferrals_refused, 1U);
    // An overload must not increment parse_errors.
    EXPECT_EQ(stats.parse_errors, 0U);
    EXPECT_EQ(stats.pending_high_watermark, HttpService::MAX_PENDING);

    // The cap must allow another deferral once one completes.
    EXPECT_TRUE(fixture.resolve(ids.back(), "x"));
    EXPECT_EQ(fixture.harness.service->pending_responses(),
              HttpService::MAX_PENDING - 1);
}

TEST(DeferredTest, SynchronousRequestIsServedWhileEveryDeferralIsHeld) {
    DeferHarness fixture;
    for (std::size_t i = 0; i < HttpService::MAX_PENDING; ++i) {
        static_cast<void>(fixture.open_and_defer());
    }
    ASSERT_EQ(fixture.harness.service->pending_responses(),
              HttpService::MAX_PENDING);

    // A synchronous request on a fresh connection must return 200 even when
    // all deferrals are occupied (BR-12).
    const ConnectionId id = fixture.harness.connect();
    const std::string sent = fixture.harness.round_trip(id, GET_NOW);

    EXPECT_NE(sent.find("200 OK"), std::string::npos) << sent;
    EXPECT_NE(sent.find("now"), std::string::npos) << sent;
    EXPECT_EQ(fixture.harness.service->statistics().deferrals_refused, 0U)
        << "a synchronous request was counted against the deferral cap";
}

TEST(DeferredTest, SynchronousHandlersDoNotAccumulateDeferrals) {
    DeferHarness fixture;
    const ConnectionId id = fixture.harness.connect();
    // A missing synchronous release exhausts the pool before this loop ends.
    for (int i = 0; i < 50; ++i) {
        const std::string sent = fixture.harness.round_trip(id, GET_NOW);
        ASSERT_NE(sent.find("200 OK"), std::string::npos) << sent;
    }
    EXPECT_EQ(fixture.harness.service->pending_responses(), 0U);
    const auto& stats = fixture.harness.service->statistics();
    EXPECT_EQ(stats.deferrals_refused, 0U);
    EXPECT_EQ(stats.pending_dispatches, 0U);
}

TEST(PendingPoolTest, ReleaseDropsTheBodyBufferRatherThanHoldingIt) {
    pico_http::detail::PendingPool<2> pool;
    const ConnectionId id = ConnectionId::make(1, 1);

    HTTPResponse* entry = pool.claim(id);
    ASSERT_NE(entry, nullptr);
    entry->reserve_body(4096);
    ASSERT_GE(entry->body.capacity(), 4096U);

    pool.release(id);

    // adopt() swaps body buffers. Retaining the slot's previous buffer after
    // release would exceed the copied-body heap bound (AR-1).
    EXPECT_EQ(entry->body.capacity(), 0U);
    EXPECT_EQ(pool.in_use(), 0U);
}
