// SPDX-License-Identifier: MIT
// Deferred completion (AR-9), the BR-12 deferral cap and the handler deadline
// (TR-6).
//
// The cases that matter here are the ones where the connection changes state
// while a handler still owes it a response: a timeout, a peer reset, a server
// stop, a duplicate completion, a *later request on the same connection*. Each
// is a misdirected response or a stranded deferral if the request-scoped handle
// or a release path is wrong, and none is reachable from the synchronous tests.
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

// A handler that parks the request and records what it was given. It keeps no
// pointer to the response — that is the contract (AR-9) — only the RequestId,
// and answers later through a filler.
struct Deferrer {
    std::vector<RequestId> parked;
    // Copied out of the request view at handler time, to compare against what
    // the same view says after the deferral.
    std::string uri_at_dispatch;
    // What the filler writes, and how many times it ran. A filler that runs for
    // a request the service should have rejected shows up here as a count, even
    // when nothing reaches the wire.
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

// Registers /defer and /now on a harness, returning the deferrer that owns the
// parked ids.
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

    // Record the completion, then run the async-context turn that respond()
    // asked for. Separated in the tests that care which half does what.
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

// --- the deferral itself ---------------------------------------------------

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

    // respond() is documented as safe from core 1 and from an ISR (AR-9), which
    // is only true while it performs no send and makes no lwIP call. It records
    // and rings; the turn does the rest.
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

    // The parser must still hold the request: the handler's views point into
    // it, and the framing decisions at completion read it again. Anything that
    // reset it would show up as a corrupt response line below.
    EXPECT_TRUE(fixture.resolve(fixture.last_parked(), "ok"));
    fixture.harness.flush(id);

    const std::string sent = fixture.harness.wire->sent_text(id);
    EXPECT_TRUE(sent.starts_with("HTTP/1.1 200 OK\r\n")) << sent;
}

TEST(DeferredTest, PipelinedRequestWaitsForTheDeferralThenIsServed) {
    DeferHarness fixture;
    const ConnectionId id = fixture.harness.connect();
    // Both requests arrive in one segment. The second must not be dispatched
    // while the first is parked -- doing so would reset the parser under the
    // first handler's feet (AR-9) and answer out of order.
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

    // Separate delivery, so this re-enters process() with the slot already
    // parked -- the case the loop condition guards, as opposed to the in-loop
    // break that covers a pipelined pair arriving together. Dispatching here
    // would reset the parser under the parked handler's request views (AR-9).
    fixture.harness.wire->deliver_text(id, GET_NOW);

    // The sharp signal is that the handler was not entered a second time. With
    // the slot parked the parser still reports the completed first request, so
    // a process() that does not check `pending` re-dispatches *that* request --
    // same URI, nothing sent, nothing completed. Only the invocation count
    // distinguishes it.
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

    // Bytes arriving behind a parked handler are held, unconsumed, with the
    // receive window closed against the peer for as long as TR-6 allows. That
    // is the same observable effect as backpressure behind a draining response,
    // and OR-1 names the effect -- so it must be counted. Counting only the
    // draining case leaves the one new source of sustained throttling in this
    // release invisible.
    fixture.harness.wire->deliver_text(id, GET_NOW);

    EXPECT_GE(fixture.harness.service->statistics().rx_window_throttle_events,
              1U)
        << "a deferral holding the window closed was not counted";
}

// --- stale, duplicate and abandoned completions ----------------------------

TEST(DeferredTest, RespondAfterPeerResetIsInert) {
    DeferHarness fixture;
    const ConnectionId id = fixture.open_and_defer();
    const RequestId parked = fixture.last_parked();

    fixture.harness.wire->reset_peer(id);
    // The deferral must go back to the BR-12 cap on the disconnect, not linger
    // until TR-6.
    EXPECT_EQ(fixture.harness.service->pending_responses(), 0U);

    // The handler answers anyway. This is the race the generation tag exists
    // for (AR-4): it must not write into a slot that now belongs to nobody, or
    // to the next peer.
    EXPECT_FALSE(fixture.resolve(parked, "too late"));
    EXPECT_EQ(fixture.deferrer.fills, 0)
        << "the filler ran for a request whose connection was gone";
    EXPECT_EQ(fixture.harness.service->pending_responses(), 0U);
}

TEST(DeferredTest, LateCompletionCannotReachAnotherConnectionsResponse) {
    DeferHarness fixture;
    const ConnectionId first = fixture.open_and_defer();
    const RequestId abandoned = fixture.last_parked();

    // The first connection dies while its handler is still working. Its
    // response object goes back to the pool here.
    fixture.harness.wire->reset_peer(first);

    // A second connection arrives and defers, so it now owns whatever the first
    // one released. This is the step that separates "released" from "safe": if
    // the abandoned handler still held a pointer into the pool, its late write
    // would land in this connection's live response and be sent to this peer.
    const ConnectionId second = fixture.open_and_defer();
    const RequestId live = fixture.last_parked();
    ASSERT_FALSE(abandoned == live);

    EXPECT_FALSE(
        fixture.resolve(abandoned, "leaked from the first connection"));
    EXPECT_EQ(fixture.deferrer.fills, 0);
    fixture.harness.flush(second);
    EXPECT_EQ(fixture.harness.wire->sent_text(second), "")
        << "an abandoned completion wrote into a live connection's response";

    // The live request still answers correctly and with its own body.
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

    // The same keep-alive connection defers again. Its ConnectionId is
    // unchanged -- generation only moves when the slot is recycled -- so a
    // handle that named only the connection would now match this second
    // request, and the first handler's retry would resolve it with the wrong
    // body. The sequence in RequestId is what rejects it.
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
    // Two live connections, so a handler that reported a fixed or stale handle
    // rather than the request's own would answer the wrong peer.
    const ConnectionId first = fixture.open_and_defer();
    const ConnectionId second = fixture.open_and_defer();

    ASSERT_EQ(fixture.deferrer.parked.size(), 2U);
    EXPECT_EQ(fixture.deferrer.parked[0].connection, first);
    EXPECT_EQ(fixture.deferrer.parked[1].connection, second);

    // Answering via the handle the handler was given reaches that connection
    // only.
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
    // Live connection, but no handler ever deferred on it.
    EXPECT_FALSE(fixture.harness.service->respond(
        RequestId{.connection = id, .sequence = 1}, &Deferrer::fill,
        &fixture.deferrer));
    // A null filler has nothing to run, so it is rejected rather than recorded
    // and dereferenced on the turn.
    const ConnectionId parked = fixture.open_and_defer();
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

// --- a handler that resolves inside its own body ---------------------------

TEST(DeferredTest, HandlerRespondingInlineIsCompletedRatherThanParked) {
    Harness harness;
    Deferrer deferrer;
    // A handler whose async work was already done: it calls respond() from its
    // own frame and then returns PENDING. Nothing in the API forbids this, and
    // a completion source that sometimes runs synchronously produces it without
    // the handler's author choosing to.
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
    // Completed as the handler returned, so nothing is parked and nothing is
    // waiting on TR-6.
    EXPECT_EQ(harness.service->pending_responses(), 0U);
    EXPECT_EQ(harness.service->statistics().requests_completed, 1U);
}

// --- the BR-12 deferral cap ------------------------------------------------

TEST(DeferredTest, DeferralCapAnswers503AndRecoversWhenOneResolves) {
    DeferHarness fixture;
    std::vector<RequestId> ids;
    ids.reserve(HttpService::MAX_PENDING);
    for (std::size_t i = 0; i < HttpService::MAX_PENDING; ++i) {
        // The connection id is not needed: the request-scoped handle is what
        // resolves a deferral, and last_parked() reports it.
        static_cast<void>(fixture.open_and_defer());
        ids.push_back(fixture.last_parked());
    }
    EXPECT_EQ(fixture.harness.service->pending_responses(),
              HttpService::MAX_PENDING);

    // One deferral past the cap. The refusal is built on the stack, so it needs
    // no response entry of its own -- which is what makes it reportable at the
    // moment it occurs (BR-12).
    const ConnectionId refused = fixture.open_and_defer();
    fixture.harness.flush(refused);

    const std::string sent = fixture.harness.wire->sent_text(refused);
    EXPECT_NE(sent.find("503 Service Unavailable"), std::string::npos) << sent;
    EXPECT_NE(sent.find("Connection: close"), std::string::npos) << sent;

    const auto& stats = fixture.harness.service->statistics();
    EXPECT_EQ(stats.deferrals_refused, 1U);
    // An overload is not a malformed request; it must not land in parse_errors.
    EXPECT_EQ(stats.parse_errors, 0U);
    EXPECT_EQ(stats.pending_high_watermark, HttpService::MAX_PENDING);

    // Resolving one deferral frees exactly one place, and deferring works
    // again -- the cap must be transient, not a permanent wedge.
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

    // A plain synchronous route on a fresh connection, with the deferral cap
    // fully drawn down. This must be a 200: a handler that answers inline holds
    // its response object only for the length of its own dispatch, so it is not
    // subject to BR-12 at all. Claiming against the cap on arrival instead
    // would let a few slow handlers black-hole every other connection's
    // ordinary traffic -- including the /stats route an operator would use to
    // see it.
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
    // Far more requests than the cap allows. A synchronous handler is never
    // parked, so if the release path on that dispatch were missing this wedges
    // at 503 well before the loop ends.
    for (int i = 0; i < 50; ++i) {
        const std::string sent = fixture.harness.round_trip(id, GET_NOW);
        ASSERT_NE(sent.find("200 OK"), std::string::npos) << sent;
    }
    EXPECT_EQ(fixture.harness.service->pending_responses(), 0U);
    const auto& stats = fixture.harness.service->statistics();
    EXPECT_EQ(stats.deferrals_refused, 0U);
    EXPECT_EQ(stats.pending_dispatches, 0U);
}

// --- the response pool's own contract --------------------------------------

TEST(PendingPoolTest, ReleaseDropsTheBodyBufferRatherThanHoldingIt) {
    pico_http::detail::PendingPool<2> pool;
    const ConnectionId id = ConnectionId::make(1, 1);

    HTTPResponse* entry = pool.claim(id);
    ASSERT_NE(entry, nullptr);
    entry->reserve_body(4096);
    ASSERT_GE(entry->body.capacity(), 4096U);

    pool.release(id);

    // Emptiness is not the property under test -- the capacity is. An entry is
    // the swap partner for a connection slot's body buffer (BodySource::adopt),
    // so after a buffered response it holds the slot's *previous* buffer.
    // Holding that until some later claim happens to land on this entry leaves
    // the heap live with no connection owning it, which puts peak copied-body
    // allocation above AR-1's MAX_CONNECTIONS x MAX_BUFFERED_BODY bound.
    EXPECT_EQ(entry->body.capacity(), 0U);
    EXPECT_EQ(pool.in_use(), 0U);
}
