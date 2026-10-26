// SPDX-License-Identifier: MIT
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "pico_http/http/http_service.hpp"
#include "pico_http/tcp/tcp_transport.hpp"
#include "pico_http/testing/fake_tcp_transport.hpp"

/// \file service_harness.hpp
/// An HttpService wired to a FakeTcpTransport, with the transport still
/// reachable so a test can drive the wire: connect, deliver bytes, acknowledge
/// sends, reset the peer, advance the clock.

namespace pico_http::testing {

/// An HttpService bound to a FakeTcpTransport, with the transport still
/// reachable. Test support, and it *ships*: `pico_http::testing` is in the
/// export set on host builds, because its consumers are downstream projects'
/// test trees — a hand-written double drifts from the seam it stands in for.
/// Nothing a consumer links in production depends on it.
struct ServiceHarness {
    /// A send capacity far above any single test response, so a test that is
    /// not about backpressure never hits it by accident. Pass a smaller value
    /// to exercise the stalled-writer paths.
    static constexpr std::size_t DEFAULT_SEND_CAPACITY = 65536;

    /// The port the harness binds. Arbitrary — the fake records it and never
    /// opens a socket.
    static constexpr std::uint16_t PORT = 80;

    /// Owned by `service`; retained here so tests can drive the wire.
    FakeTcpTransport* wire = nullptr;
    /// The service under test. Owns the transport `wire` points into.
    std::unique_ptr<HttpService> service;

    /// Build a started service on PORT. Aborts rather than returning a broken
    /// harness if the bind fails — the fake always binds unless a test asked it
    /// not to, so a failure here is a defect in the harness itself.
    explicit ServiceHarness(std::size_t send_capacity = DEFAULT_SEND_CAPACITY) {
        auto transport = std::make_unique<FakeTcpTransport>();
        wire = transport.get();
        wire->set_send_capacity(send_capacity);
        service = std::make_unique<HttpService>(std::move(transport));
        // The fake always binds unless a test asked it not to, so a false here
        // means the harness itself is broken; swallowing it would surface as a
        // confusing failure in whichever test happened to construct it.
        if (!service->start(PORT)) {
            std::abort();
        }
    }

    /// Open a connection on the fake wire and return its id.
    [[nodiscard]] ITcpTransport::ConnectionId connect() const {
        return wire->connect();
    }

    /// Acknowledge outstanding transmit bytes until the writer has nothing left
    /// in flight. The peer "reading" the socket is what frees credit and drives
    /// the writable events a streamed response resumes on.
    void flush(ITcpTransport::ConnectionId id) const {
        // Bounded so a writer that never drains fails the test by assertion
        // rather than by hanging the suite. The assert is what makes that
        // sentence true: without it the loop simply falls out, round_trip()
        // returns a truncated response, and the failure surfaces as a missing
        // substring somewhere downstream rather than as the drain that never
        // completed.
        //
        // assert() rather than GoogleTest's FAIL(): this header is installed
        // and a consumer's test tree need not be built on GoogleTest, so the
        // diagnostic must not drag a framework into the shipped surface.
        for (int i = 0; i < FLUSH_LIMIT && wire->is_open(id); ++i) {
            const std::size_t inflight = wire->send_inflight(id);
            if (inflight == 0) {
                return;
            }
            wire->deliver_ack(id, inflight);
        }
        assert(!wire->is_open(id) &&
               "ServiceHarness::flush: the writer did not drain within "
               "FLUSH_LIMIT acknowledgement rounds");
    }

    /// Deliver a request, drain the response, and return everything written to
    /// this connection so far.
    [[nodiscard]] std::string round_trip(ITcpTransport::ConnectionId id,
                                         std::string_view request) const {
        wire->deliver_text(id, request);
        flush(id);
        return wire->sent_text(id);
    }

    /// Acknowledgement rounds flush() performs before giving up, so a writer
    /// that never drains fails the test by assertion rather than by hanging it.
    static constexpr int FLUSH_LIMIT = 100000;
};

}  // namespace pico_http::testing
