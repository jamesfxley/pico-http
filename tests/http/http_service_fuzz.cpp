// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "pico_http/http/http_service.hpp"
#include "pico_http/http/message.hpp"
#include "pico_http/tcp/tcp_transport.hpp"
#include "pico_http/testing/fake_tcp_transport.hpp"

/// The small transmit buffer forces backpressure while fuzzing the request
/// lifecycle.

namespace {

using pico_http::Disposition;
using pico_http::HTTPRequestView;
using pico_http::HTTPResponse;
using pico_http::HttpService;
using pico_http::StaticBody;
using pico_http::testing::FakeTcpTransport;
using ConnectionId = pico_http::ITcpTransport::ConnectionId;

std::size_t stream_body(void* /*ctx*/, std::size_t offset,
                        std::span<std::uint8_t> out) {
    constexpr std::size_t TOTAL = 5000;
    if (offset >= TOTAL) {
        return 0;
    }
    const std::size_t n = std::min(out.size(), TOTAL - offset);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::uint8_t>('a' + ((offset + i) % 26U));
    }
    return n;
}

// The static body keeps completion allocation-free (AR-9).
void fill_deferred(void* /*ctx*/, HTTPResponse& res) {
    res.set_body_static(StaticBody::outliving_response("deferred"),
                        "text/plain");
}

}  // namespace

// readability-identifier-naming: libFuzzer fixes the entry-point name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    auto transport = std::make_unique<FakeTcpTransport>();
    FakeTcpTransport* wire = transport.get();
    // Small transmit buffer so responses stall and resume (backpressure paths).
    wire->set_send_capacity(64);
    HttpService service(std::move(transport));
    std::vector<pico_http::RequestId> deferred;
    // Registration failure indicates a harness error, independent of fuzz
    // input.
    const bool routes_registered =
        service.add_route("GET", "/s",
                          [](const HTTPRequestView&, HTTPResponse& res) {
                              res.set_body_static(
                                  StaticBody::outliving_response("static-body"),
                                  "text/plain");
                              return Disposition::COMPLETE;
                          }) &&
        service.add_route("GET", "/g",
                          [](const HTTPRequestView&, HTTPResponse& res) {
                              res.set_body_generator(
                                  stream_body, nullptr,
                                  "application/octet-stream");
                              return Disposition::COMPLETE;
                          }) &&
        service.add_route(
            "POST", "/e",
            [](const HTTPRequestView& req, HTTPResponse& res) {
                res.set_body(std::string(req.body.begin(), req.body.end()));
                return Disposition::COMPLETE;
            }) &&
        // The parked route exercises queued input, teardown and possibly
        // stale completion (AR-9).
        service.add_route(
            "GET", "/d",
            [&deferred](const HTTPRequestView& req, HTTPResponse&) {
                deferred.push_back(req.request);
                return Disposition::PENDING;
            });
    if (!routes_registered) {
        std::abort();
    }
    service.start(80);

    const ConnectionId id = wire->connect();
    const std::span<const std::uint8_t> input(data, size);

    std::size_t off = 0;
    while (off < input.size() && wire->is_open(id)) {
        const std::size_t chunk =
            std::min<std::size_t>(1U + (input[off] % 7U), input.size() - off);
        const std::size_t delivered =
            wire->deliver(id, input.subspan(off, chunk));
        off += delivered;

        const std::size_t inflight = wire->send_inflight(id);
        if (inflight > 0) {
            wire->deliver_ack(id, inflight);  // resume a backpressured response
        } else if (delivered == 0) {
            break;  // receive window closed with nothing to drain
        }
    }

    // A stale respond() has to be inert (AR-4, AR-9). A duplicate completion
    // must also be inert.
    for (const pico_http::RequestId parked : deferred) {
        service.respond(parked, &fill_deferred, nullptr);
        service.respond(parked, &fill_deferred, nullptr);
        wire->run_pending_completions();
    }

    // Finite route output bounds this drain loop.
    while (wire->is_open(id) && wire->send_inflight(id) > 0) {
        wire->deliver_ack(id, wire->send_inflight(id));
    }
    return 0;
}
