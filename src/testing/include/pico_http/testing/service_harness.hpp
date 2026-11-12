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
/// A service and accessible fake transport for host tests.

namespace pico_http::testing {

/// Host test support exported through pico_http::testing, with no production
/// dependants.
struct ServiceHarness {
    /// Default TX capacity in bytes, large enough to avoid backpressure in
    /// ordinary tests.
    static constexpr std::size_t DEFAULT_SEND_CAPACITY = 65536;

    /// The port the harness binds. Arbitrary — the fake records it and never
    /// opens a socket.
    static constexpr std::uint16_t PORT = 80;

    /// Owned by `service`; retained here so tests can drive the wire.
    FakeTcpTransport* wire = nullptr;
    /// The service under test. Owns the transport `wire` points into.
    std::unique_ptr<HttpService> service;

    /// Start a service on PORT. Abort if the fake cannot bind.
    explicit ServiceHarness(std::size_t send_capacity = DEFAULT_SEND_CAPACITY) {
        auto transport = std::make_unique<FakeTcpTransport>();
        wire = transport.get();
        wire->set_send_capacity(send_capacity);
        service = std::make_unique<HttpService>(std::move(transport));
        // A bind failure here indicates a broken harness.
        if (!service->start(PORT)) {
            std::abort();
        }
    }

    /// Open a connection on the fake wire and return its id.
    [[nodiscard]] ITcpTransport::ConnectionId connect() const {
        return wire->connect();
    }

    /// Acknowledge queued bytes until output drains, driving writable events.
    void flush(ITcpTransport::ConnectionId id) const {
        // Bound retries so a stuck writer fails an assertion instead of
        // hanging.
        // The installed header must not require a test framework, so it uses
        // assert().
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
