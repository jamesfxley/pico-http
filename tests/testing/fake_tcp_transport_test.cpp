// SPDX-License-Identifier: MIT
#include "pico_http/testing/fake_tcp_transport.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pico_http/tcp/tcp_transport.hpp"

using pico_http::DisconnectReason;
using pico_http::testing::FakeTcpTransport;
using ConnectionId = pico_http::ITcpTransport::ConnectionId;

namespace {

std::vector<std::uint8_t> iota_bytes(std::size_t n, std::uint8_t start = 0) {
    std::vector<std::uint8_t> bytes_out(n);
    std::iota(bytes_out.begin(), bytes_out.end(), start);
    return bytes_out;
}

std::span<const std::uint8_t> bytes(
    const std::vector<std::uint8_t>& bytes_out) {
    return {bytes_out.data(), bytes_out.size()};
}

}  // namespace

TEST(FakeTcpTransportTest, ConnectFiresCallbackAndYieldsLiveId) {
    FakeTcpTransport transport;
    ConnectionId seen;
    transport.set_on_connect([&](ConnectionId id) { seen = id; });

    const ConnectionId id = transport.connect();
    EXPECT_TRUE(id.valid());
    EXPECT_EQ(seen, id);
    EXPECT_TRUE(transport.is_open(id));
    EXPECT_EQ(transport.active_clients(), 1U);
}

TEST(FakeTcpTransportTest, TableFullYieldsInvalidId) {
    FakeTcpTransport transport;
    for (std::size_t i = 0; i < FakeTcpTransport::MAX_CLIENTS; ++i) {
        EXPECT_TRUE(transport.connect().valid());
    }
    EXPECT_FALSE(transport.connect().valid());
}

TEST(FakeTcpTransportTest, SendIsCreditBasedAndCapturesBytes) {
    FakeTcpTransport transport;
    transport.set_send_capacity(8);
    const ConnectionId id = transport.connect();

    auto data = iota_bytes(12);
    EXPECT_EQ(transport.send(id, bytes(data)), 8U);  // only 8 fit
    EXPECT_EQ(transport.send_inflight(id), 8U);
    EXPECT_EQ(transport.send(id, bytes(data)), 0U);  // full: no credit

    auto captured = transport.sent_bytes(id);
    EXPECT_EQ(captured,
              std::vector<std::uint8_t>(data.begin(), data.begin() + 8));
}

TEST(FakeTcpTransportTest, SendCreditReportsRingFreeSpace) {
    FakeTcpTransport transport;
    transport.set_send_capacity(8);
    const ConnectionId id = transport.connect();

    EXPECT_EQ(transport.send_credit(id), 8U);  // empty ring: full capacity
    transport.send(id, bytes(iota_bytes(5)));
    EXPECT_EQ(transport.send_credit(id), 3U);  // 5 accepted, 3 free
    transport.send(id, bytes(iota_bytes(3)));
    EXPECT_EQ(transport.send_credit(id), 0U);  // full ring: no credit
    transport.deliver_ack(id, 6);
    EXPECT_EQ(transport.send_credit(id), 6U);  // ack frees space
}

TEST(FakeTcpTransportTest, SendCreditOnStaleIdIsZero) {
    FakeTcpTransport transport;
    const ConnectionId id = transport.connect();
    transport.reset_peer(id);  // slot freed; id now stale
    EXPECT_EQ(transport.send_credit(id), 0U);
}

TEST(FakeTcpTransportTest, AckFreesCreditAndFiresWritable) {
    FakeTcpTransport transport;
    transport.set_send_capacity(8);
    const ConnectionId id = transport.connect();

    transport.send(id, bytes(iota_bytes(8)));
    EXPECT_EQ(transport.send_inflight(id), 8U);

    transport.deliver_ack(id, 5);
    EXPECT_EQ(transport.send_inflight(id), 3U);
    ASSERT_EQ(transport.writable_events().size(), 1U);
    EXPECT_EQ(transport.writable_events()[0].first, id);
    EXPECT_EQ(transport.writable_events()[0].second, 5U);

    // Freed space is now usable.
    EXPECT_EQ(transport.send(id, bytes(iota_bytes(5, 100))), 5U);
}

TEST(FakeTcpTransportTest, DeliverThrottlesByReceiveWindow) {
    FakeTcpTransport transport;
    transport.set_rx_window(4);
    std::string received;
    transport.set_on_receive(
        [&](ConnectionId, std::span<const std::uint8_t> data) {
            received.append(data.begin(), data.end());
        });
    const ConnectionId id = transport.connect();

    auto data = iota_bytes(10);
    EXPECT_EQ(transport.deliver(id, bytes(data)), 4U);  // window is 4
    EXPECT_EQ(transport.rx_unacked(id), 4U);
    EXPECT_EQ(transport.deliver(id, bytes(data)), 0U);  // window closed

    // Consuming reopens the window (AR-6 flow control).
    transport.consume(id, 4);
    EXPECT_EQ(transport.rx_unacked(id), 0U);
    EXPECT_EQ(transport.deliver(id, bytes(data)), 4U);
    EXPECT_EQ(received.size(), 8U);
}

TEST(FakeTcpTransportTest, PeerResetReportsReasonAndClosesSlot) {
    FakeTcpTransport transport;
    const ConnectionId id = transport.connect();

    transport.reset_peer(id);
    ASSERT_EQ(transport.disconnects().size(), 1U);
    EXPECT_EQ(transport.disconnects()[0].first, id);
    EXPECT_EQ(transport.disconnects()[0].second, DisconnectReason::PEER_RESET);
    EXPECT_FALSE(transport.is_open(id));
    EXPECT_EQ(transport.active_clients(), 0U);
}

TEST(FakeTcpTransportTest, PeerCloseReportsGracefulReason) {
    FakeTcpTransport transport;
    const ConnectionId id = transport.connect();
    transport.close_peer(id);
    ASSERT_EQ(transport.disconnects().size(), 1U);
    EXPECT_EQ(transport.disconnects()[0].second, DisconnectReason::PEER_CLOSED);
}

TEST(FakeTcpTransportTest, OperationsOnStaleIdAreSafeNoOps) {
    FakeTcpTransport transport;
    const ConnectionId id = transport.connect();
    transport.reset_peer(id);  // id is now stale

    EXPECT_EQ(transport.send(id, bytes(iota_bytes(4))), 0U);
    transport.consume(id, 4);      // no crash
    transport.deliver_ack(id, 4);  // no crash
    EXPECT_EQ(transport.deliver(id, bytes(iota_bytes(4))), 0U);
    EXPECT_FALSE(transport.is_open(id));
}

TEST(FakeTcpTransportTest, ReusedSlotDoesNotResolveOldId) {
    FakeTcpTransport transport;
    const ConnectionId first = transport.connect();
    transport.reset_peer(first);
    const ConnectionId second =
        transport.connect();  // reuses the physical slot

    EXPECT_NE(first, second);
    EXPECT_FALSE(transport.is_open(first));
    EXPECT_TRUE(transport.is_open(second));
    transport.send(second, bytes(iota_bytes(3)));
    EXPECT_EQ(transport.sent_bytes(first).size(), 0U);  // old id inert
    EXPECT_EQ(transport.sent_bytes(second).size(), 3U);
}

TEST(FakeTcpTransportTest, CloseClientDrainsThenReportsServerClosed) {
    FakeTcpTransport transport;
    transport.set_send_capacity(16);
    const ConnectionId id = transport.connect();
    transport.send(id, bytes(iota_bytes(10)));

    transport.close_client(id);
    // Still draining: not closed until the queued bytes are acked.
    EXPECT_TRUE(transport.is_open(id));
    EXPECT_TRUE(transport.disconnects().empty());

    transport.deliver_ack(id, 10);
    ASSERT_EQ(transport.disconnects().size(), 1U);
    EXPECT_EQ(transport.disconnects()[0].second,
              DisconnectReason::SERVER_CLOSED);
    EXPECT_FALSE(transport.is_open(id));
}

TEST(FakeTcpTransportTest, CloseServerTearsDownAllClients) {
    FakeTcpTransport transport;
    std::vector<ConnectionId> ids;
    ids.reserve(3);
    for (int i = 0; i < 3; ++i) {
        ids.push_back(transport.connect());
    }
    transport.close_server();
    EXPECT_TRUE(transport.server_closed());
    EXPECT_EQ(transport.disconnects().size(), 3U);
    for (const ConnectionId id : ids) {
        EXPECT_FALSE(transport.is_open(id));
    }
}

TEST(FakeTcpTransportTest, ListenRecordsPortAndHonoursOverride) {
    FakeTcpTransport transport;
    EXPECT_TRUE(transport.listen(80));
    EXPECT_EQ(transport.last_port(), 80U);
    transport.set_listen_result(false);
    EXPECT_FALSE(transport.listen(8080));
    EXPECT_EQ(transport.last_port(), 8080U);
}

// --- Deferred disconnect delivery (TransportBase) ---------------------------
//
// A close issued from inside a transport callback must not re-enter the
// application through on_disconnect while that callback is still on the stack.
// The slot goes stale immediately; only the notification waits.

TEST(FakeTcpTransportTest,
     CloseFromInsideCallbackDefersDisconnectNotification) {
    FakeTcpTransport transport;
    const ConnectionId id = transport.connect();

    bool inside_receive = false;
    bool disconnect_seen_during_receive = false;
    bool disconnect_seen = false;

    transport.set_on_disconnect([&](ConnectionId, DisconnectReason) {
        disconnect_seen = true;
        if (inside_receive) {
            disconnect_seen_during_receive = true;
        }
    });
    transport.set_on_receive([&](ConnectionId client,
                                 std::span<const std::uint8_t>) {
        inside_receive = true;
        transport.close_client(client);  // nothing queued: drains immediately
        // The id is stale the moment the slot is released...
        EXPECT_FALSE(transport.is_open(client));
        // ...but the notification has not been delivered yet.
        EXPECT_FALSE(disconnect_seen);
        inside_receive = false;
    });

    transport.deliver_text(id, "GET / HTTP/1.1\r\n\r\n");

    EXPECT_FALSE(disconnect_seen_during_receive);
    EXPECT_TRUE(disconnect_seen);
    EXPECT_EQ(transport.disconnects().size(), 1U);
    EXPECT_EQ(transport.disconnects().front().second,
              DisconnectReason::SERVER_CLOSED);
}

TEST(FakeTcpTransportTest, DeferredDisconnectStillReportsPeerReason) {
    FakeTcpTransport transport;
    const ConnectionId id = transport.connect();

    std::vector<DisconnectReason> reasons;
    transport.set_on_disconnect([&](ConnectionId, DisconnectReason reason) {
        reasons.push_back(reason);
    });

    transport.reset_peer(id);

    ASSERT_EQ(reasons.size(), 1U);
    EXPECT_EQ(reasons.front(), DisconnectReason::PEER_RESET);
}

TEST(FakeTcpTransportTest, TimeoutSweepDeliversEveryDisconnectOnce) {
    FakeTcpTransport transport;
    const ConnectionId first = transport.connect();
    const ConnectionId second = transport.connect();

    std::vector<ConnectionId> torn_down;
    transport.set_on_disconnect(
        [&](ConnectionId id, DisconnectReason) { torn_down.push_back(id); });

    transport.arm_timeout(first, pico_http::TimeoutKind::HEADER, 100);
    transport.arm_timeout(second, pico_http::TimeoutKind::IDLE, 100);
    transport.advance_time(150);

    EXPECT_EQ(torn_down.size(), 2U);
    EXPECT_FALSE(transport.is_open(first));
    EXPECT_FALSE(transport.is_open(second));
}

TEST(FakeTcpTransportTest,
     CloseServerFromInsideCallbackDefersAllNotifications) {
    FakeTcpTransport transport;
    const ConnectionId first = transport.connect();
    transport.connect();

    bool inside_receive = false;
    std::size_t delivered_during_receive = 0;
    std::size_t delivered = 0;

    transport.set_on_disconnect([&](ConnectionId, DisconnectReason) {
        ++delivered;
        if (inside_receive) {
            ++delivered_during_receive;
        }
    });
    transport.set_on_receive([&](ConnectionId, std::span<const std::uint8_t>) {
        inside_receive = true;
        transport.close_server();
        inside_receive = false;
    });

    transport.deliver_text(first, "x");

    EXPECT_EQ(delivered_during_receive, 0U);
    EXPECT_EQ(delivered, 2U);
}

// Delivering a deferred disconnect runs at callback depth 0, so a sink that
// re-enters the transport from on_disconnect opens a fresh CallbackScope whose
// unwind flushes again. That nested flush must not replay the entries the
// outermost pass has already delivered.
TEST(FakeTcpTransportTest, ReentrantFlushDeliversEachDisconnectExactlyOnce) {
    FakeTcpTransport transport;
    const ConnectionId first = transport.connect();
    transport.connect();

    std::size_t delivered = 0;
    transport.set_on_disconnect([&](ConnectionId, DisconnectReason) {
        ++delivered;
        ASSERT_LE(delivered, 8U) << "disconnects are being redelivered";
        // A sink that touches the transport from its disconnect handler.
        transport.close_server();
    });

    transport.reset_peer(first);

    EXPECT_EQ(delivered, 2U);  // one per live connection, no repeats
}

TEST(FakeTcpTransportTest, IsOpenDistinguishesFullRingFromDeadConnection) {
    FakeTcpTransport transport;
    transport.set_send_capacity(4);
    const ConnectionId id = transport.connect();

    // A full ring accepts nothing, but the connection is still open.
    const std::vector<std::uint8_t> data = iota_bytes(8);
    EXPECT_EQ(transport.send(id, bytes(data)), 4U);
    EXPECT_EQ(transport.send(id, bytes(data)), 0U);
    EXPECT_TRUE(transport.is_open(id));

    // A dead connection also accepts nothing — is_open() is what separates
    // "retry on the next writable" from "stop".
    transport.reset_peer(id);
    EXPECT_EQ(transport.send(id, bytes(data)), 0U);
    EXPECT_FALSE(transport.is_open(id));
}

// deliver_segmented()'s own three clauses. The helper is the only way to raise
// a teardown *between* segments of one arrival, so it stands in for the lwIP
// recv callback's mid-chain `if (!self->is_open(id)) break;`. A service-level
// test that drives it cannot tell which of two mechanisms held; these can.

TEST(FakeTcpTransportTest, DeliverSegmentedReportsWhatItDelivered) {
    FakeTcpTransport transport;
    const ConnectionId id = transport.connect();

    std::string seen;
    transport.set_on_receive(
        [&seen](ConnectionId, std::span<const std::uint8_t> data) {
            seen.append(data.begin(), data.end());
        });

    const std::array<std::string_view, 3> segments{"GET ", "/a ", "HTTP/1.1"};
    EXPECT_EQ(transport.deliver_segmented(id, segments), 15U);
    EXPECT_EQ(seen, "GET /a HTTP/1.1");
    EXPECT_EQ(transport.rx_unacked(id), 15U);
}

TEST(FakeTcpTransportTest, DeliverSegmentedStopsWhenTheWindowCloses) {
    FakeTcpTransport transport;
    transport.set_rx_window(6);
    const ConnectionId id = transport.connect();

    std::string seen;
    int calls = 0;
    transport.set_on_receive(
        [&](ConnectionId, std::span<const std::uint8_t> data) {
            ++calls;
            seen.append(data.begin(), data.end());
        });

    // Four bytes fit whole; the next segment is truncated to the two bytes of
    // window that remain; the third never arrives at all.
    const std::array<std::string_view, 3> segments{"abcd", "efgh", "ijkl"};
    EXPECT_EQ(transport.deliver_segmented(id, segments), 6U);
    EXPECT_EQ(seen, "abcdef");
    EXPECT_EQ(calls, 2);

    // The connection is still open: a closed window is back-pressure, not a
    // teardown, and the rest arrives once the peer consumes.
    EXPECT_TRUE(transport.is_open(id));
    transport.consume(id, 6);
    EXPECT_EQ(transport.deliver_text(id, "ijkl"), 4U);
}

TEST(FakeTcpTransportTest, DeliverSegmentedStopsWhenTheConnectionGoesAway) {
    FakeTcpTransport transport;
    const ConnectionId id = transport.connect();

    int calls = 0;
    transport.set_on_receive(
        [&](ConnectionId client, std::span<const std::uint8_t>) {
            if (++calls == 2) {
                transport.close_client(client);
            }
        });

    // The handler tears the connection down on the second segment, so the third
    // is never delivered and the return counts only what arrived.
    const std::array<std::string_view, 3> segments{"ab", "cd", "ef"};
    EXPECT_EQ(transport.deliver_segmented(id, segments), 4U);
    EXPECT_EQ(calls, 2);
    EXPECT_FALSE(transport.is_open(id));
}

// close_server()'s second clause -- "and stop listening". The double modelled
// only the teardown half, so a connection offered after stop() was admitted
// here and RST by the firmware: the fake reported the opposite of the transport
// it stands for, on the one clause the method exists to provide.

TEST(FakeTcpTransportTest, ConnectAfterCloseServerIsRefusedWithoutACallback) {
    FakeTcpTransport transport;
    int connects = 0;
    int refusals = 0;
    transport.set_on_connect([&connects](ConnectionId) { ++connects; });
    transport.set_on_refused([&refusals] { ++refusals; });

    EXPECT_TRUE(transport.listen(80));
    EXPECT_TRUE(transport.connect().valid());
    EXPECT_EQ(connects, 1);

    transport.close_server();
    EXPECT_TRUE(transport.server_closed());

    // No listening pcb: the peer gets a RST and nothing reaches the service.
    // Not on_refused, which models a slot-table refusal (RR-3) the service does
    // see -- so neither callback fires.
    EXPECT_FALSE(transport.connect().valid());
    EXPECT_EQ(connects, 1);
    EXPECT_EQ(refusals, 0);
}

TEST(FakeTcpTransportTest, ListenAfterCloseServerAcceptsAgain) {
    FakeTcpTransport transport;
    EXPECT_TRUE(transport.listen(80));
    transport.close_server();
    ASSERT_FALSE(transport.connect().valid());

    // A stop/restart cycle: the real transport binds a fresh listening pcb, so
    // the door reopens. Without this the flag latched for the object's life and
    // a restart could not be tested at all.
    EXPECT_TRUE(transport.listen(8080));
    EXPECT_FALSE(transport.server_closed());
    EXPECT_TRUE(transport.connect().valid());

    // A failed listen leaves it closed rather than pretending otherwise.
    transport.close_server();
    transport.set_listen_result(false);
    EXPECT_FALSE(transport.listen(80));
    EXPECT_TRUE(transport.server_closed());
    EXPECT_FALSE(transport.connect().valid());
}

TEST(FakeTcpTransportTest, NoTimeoutSweepRunsAfterCloseServer) {
    FakeTcpTransport transport;
    EXPECT_TRUE(transport.listen(80));
    const ConnectionId id = transport.connect();

    int timeouts = 0;
    transport.set_on_disconnect(
        [&timeouts](ConnectionId, DisconnectReason reason) {
            if (reason == DisconnectReason::TIMEOUT_IDLE) {
                ++timeouts;
            }
        });
    transport.arm_timeout(id, pico_http::TimeoutKind::IDLE, 1000);
    transport.close_server();

    // close_server() removes the timeout worker from the async context, so no
    // sweep runs afterwards however far the clock is advanced. The disconnect
    // these connections got was SERVER_CLOSED, delivered by
    // close_all_clients().
    transport.advance_time(10000);
    EXPECT_EQ(timeouts, 0);
}
