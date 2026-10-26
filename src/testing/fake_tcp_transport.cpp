// SPDX-License-Identifier: MIT
#include "pico_http/testing/fake_tcp_transport.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pico_http/tcp/tcp_transport.hpp"
#include "pico_util/bytes/as_bytes.hpp"

namespace pico_http::testing {

void FakeTcpTransport::set_on_connect(std::function<void(ConnectionId)> cb) {
    lambda_events_.connect = std::move(cb);
    set_event_sink(&lambda_events_);
}

void FakeTcpTransport::set_on_disconnect(
    std::function<void(ConnectionId, DisconnectReason)> cb) {
    lambda_events_.disconnect = std::move(cb);
    set_event_sink(&lambda_events_);
}

void FakeTcpTransport::set_on_receive(
    std::function<void(ConnectionId, std::span<const std::uint8_t>)> cb) {
    lambda_events_.receive = std::move(cb);
    set_event_sink(&lambda_events_);
}

void FakeTcpTransport::set_on_writable(
    std::function<void(ConnectionId, std::size_t)> cb) {
    lambda_events_.writable = std::move(cb);
    set_event_sink(&lambda_events_);
}

void FakeTcpTransport::set_on_refused(std::function<void()> cb) {
    lambda_events_.refused = std::move(cb);
    set_event_sink(&lambda_events_);
}

bool FakeTcpTransport::listen(std::uint16_t port) {
    last_port_ = port;
    if (listen_result_) {
        // A successful listen re-opens the door a previous close_server() shut,
        // so a stop/restart cycle is testable. The real transport binds a fresh
        // listening pcb here; nothing of the old one survives.
        server_closed_ = false;
    }
    return listen_result_;
}

void FakeTcpTransport::close_server() {
    server_closed_ = true;
    close_all_clients();
}

std::size_t FakeTcpTransport::send(ConnectionId client,
                                   std::span<const std::uint8_t> data) {
    detail::ClientSim* sim = clients_.get(client);
    if (sim == nullptr || sim->close_pending) {
        return 0;
    }
    const std::size_t free_space = sim->send_capacity - sim->inflight;
    const std::size_t accepted = std::min(data.size(), free_space);
    std::vector<std::uint8_t>& log = sent_by_id_[client.raw()];
    log.insert(log.end(), data.begin(),
               data.begin() + static_cast<std::ptrdiff_t>(accepted));
    sim->inflight += accepted;
    return accepted;
}

std::size_t FakeTcpTransport::send_credit(ConnectionId client) const {
    const detail::ClientSim* sim = clients_.get(client);
    if (sim == nullptr || sim->close_pending) {
        return 0;  // matches send(): a closed/invalid connection accepts none
    }
    return sim->send_capacity - sim->inflight;
}

void FakeTcpTransport::consume(ConnectionId client, std::size_t bytes) {
    detail::ClientSim* sim = clients_.get(client);
    if (sim == nullptr) {
        return;
    }
    // Clamped, where LwipTcpTransport hands the value straight to tcp_recved.
    // A service acknowledging more bytes than were delivered would be absorbed
    // here and would over-advertise the receive window on device. Left as a
    // clamp rather than counted: both call sites in HttpService pass a value
    // the parser derived from bytes it was given -- data.size() on the closing
    // path, the parser's own offset on the receive path -- and no input has
    // been found that makes the total exceed the total delivered. Counting an
    // unreachable case would assert a bound this double cannot currently reach.
    sim->rx_unacked -= std::min(bytes, sim->rx_unacked);
}

void FakeTcpTransport::advance_time(std::uint32_t ms) {
    now_ms_ += ms;
    if (server_closed_) {
        // close_server() removes the timeout worker from the async context
        // (LwipTcpTransport::close_server), so no sweep runs after it. The
        // clock still advances: a test may close, wait, and listen again.
        return;
    }
    fire_expired_timeouts();
}

void FakeTcpTransport::set_idle_timeout(std::uint32_t seconds) {
    idle_timeout_seconds_ = seconds;
}

FakeTcpTransport::ConnectionId FakeTcpTransport::connect() {
    const CallbackScope scope(*this);
    if (server_closed_) {
        // Not listening: the real transport has closed its listening pcb and
        // cleared tcp_accept, so the peer gets a RST and no callback reaches
        // the service. Deliberately *not* notify_refused() -- that models a
        // slot-table refusal (RR-3), which the service does see. An id is
        // returned invalid so a caller that ignores the distinction cannot
        // drive a connection that the firmware would never have admitted.
        return ConnectionId{};
    }
    ConnectionId id = clients_.acquire();
    if (!id.valid()) {
        notify_refused();  // no free slot: refused at the TCP layer (RR-3)
        return id;
    }
    detail::ClientSim* sim = clients_.get(id);
    if (sim == nullptr) {
        // Unreachable: acquire() just returned this id, so it resolves. The
        // generation check hides that from the optimiser, which flags the
        // dereference at -O2 and above. Fail closed rather than suppress.
        clients_.release(id);
        return ConnectionId{};
    }
    *sim = detail::ClientSim{};
    sim->send_capacity = send_capacity_;
    sim->rx_window = rx_window_;
    notify_connect(id);
    return id;
}

void FakeTcpTransport::reset_peer(ConnectionId client) {
    const CallbackScope scope(*this);
    teardown(client, DisconnectReason::PEER_RESET);
}

void FakeTcpTransport::abort_local(ConnectionId client) {
    const CallbackScope scope(*this);
    teardown(client, DisconnectReason::ABORTED);
}

void FakeTcpTransport::close_peer(ConnectionId client) {
    const CallbackScope scope(*this);
    teardown(client, DisconnectReason::PEER_CLOSED);
}

std::size_t FakeTcpTransport::deliver(ConnectionId client,
                                      std::span<const std::uint8_t> data) {
    const CallbackScope scope(*this);
    detail::ClientSim* sim = clients_.get(client);
    if (sim == nullptr) {
        return 0;
    }
    const std::size_t window =
        sim->rx_window - std::min(sim->rx_unacked, sim->rx_window);
    const std::size_t accepted = std::min(data.size(), window);
    if (accepted == 0) {
        return 0;
    }
    sim->rx_unacked += accepted;
    notify_receive(client, data.subspan(0, accepted));
    return accepted;
}

std::size_t FakeTcpTransport::deliver_text(ConnectionId client,
                                           std::string_view text) {
    return deliver(client, pico_util::as_bytes(text));
}

std::size_t FakeTcpTransport::deliver_segmented(
    ConnectionId client, std::span<const std::string_view> segments) {
    const CallbackScope scope(*this);
    std::size_t total = 0;
    for (const std::string_view segment : segments) {
        // Mirrors the lwIP recv callback walking a pbuf chain: one scope, one
        // notify_receive per segment, and a stop as soon as a segment's handler
        // has torn the connection down.
        detail::ClientSim* sim = clients_.get(client);
        if (sim == nullptr) {
            break;
        }
        const std::size_t window =
            sim->rx_window - std::min(sim->rx_unacked, sim->rx_window);
        const std::span<const std::uint8_t> bytes =
            pico_util::as_bytes(segment);
        const std::size_t accepted = std::min(bytes.size(), window);
        if (accepted == 0) {
            break;
        }
        sim->rx_unacked += accepted;
        total += accepted;
        notify_receive(client, bytes.subspan(0, accepted));
    }
    return total;
}

void FakeTcpTransport::deliver_ack(ConnectionId client, std::size_t bytes) {
    const CallbackScope scope(*this);
    detail::ClientSim* sim = clients_.get(client);
    if (sim == nullptr) {
        return;
    }
    const std::size_t freed = std::min(bytes, sim->inflight);
    sim->inflight -= freed;
    if (freed > 0) {
        writable_events_.emplace_back(client, freed);
        notify_writable(client, freed);
    }
    // The writable handler may have closed the connection out from under us.
    detail::ClientSim* live = clients_.get(client);
    if (live != nullptr) {
        finish_close_if_drained(client, *live);
    }
}

std::vector<std::uint8_t> FakeTcpTransport::sent_bytes(
    ConnectionId client) const {
    auto it = sent_by_id_.find(client.raw());
    return it != sent_by_id_.end() ? it->second : std::vector<std::uint8_t>{};
}

std::string FakeTcpTransport::sent_text(ConnectionId client) const {
    auto it = sent_by_id_.find(client.raw());
    if (it == sent_by_id_.end()) {
        return {};
    }
    return {it->second.begin(), it->second.end()};
}

std::size_t FakeTcpTransport::send_inflight(ConnectionId client) const {
    const detail::ClientSim* sim = clients_.get(client);
    return sim != nullptr ? sim->inflight : 0;
}

std::size_t FakeTcpTransport::rx_unacked(ConnectionId client) const {
    const detail::ClientSim* sim = clients_.get(client);
    return sim != nullptr ? sim->rx_unacked : 0;
}

void FakeTcpTransport::release_slot_resources(ConnectionId client,
                                              detail::ClientSim& /*sim*/,
                                              DisconnectReason reason) {
    disconnects_.emplace_back(client, reason);
}

}  // namespace pico_http::testing
