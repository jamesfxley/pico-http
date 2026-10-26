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

/// libFuzzer target driving arbitrary bytes through the full HttpService
/// lifecycle over FakeTcpTransport (parse -> dispatch -> streamed response ->
/// teardown), under a small transmit buffer so the backpressure paths run.
/// Built only under PICO_HTTP_ENABLE_FUZZERS with a -fsanitize=fuzzer compiler.
/// Sanitizers catch any memory misuse.

namespace {

using pico_http::Disposition;
using pico_http::HTTPRequestView;
using pico_http::HTTPResponse;
using pico_http::HttpService;
using pico_http::StaticBody;
using pico_http::testing::FakeTcpTransport;
using ConnectionId = pico_http::ITcpTransport::ConnectionId;

// Length-less generator body: a bounded total, then 0 to end the body.
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

// The filler a deferring handler hands back (AR-9). Static body, so resolving a
// completion allocates nothing and the fuzzer's allocation profile stays flat.
void fill_deferred(void* /*ctx*/, HTTPResponse& res) {
    res.set_body_static(StaticBody::outliving_response("deferred"),
                        "text/plain");
}

}  // namespace

// Entry-point name fixed by libFuzzer's ABI.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    auto transport = std::make_unique<FakeTcpTransport>();
    FakeTcpTransport* wire = transport.get();
    // Small transmit buffer so responses stall and resume (backpressure paths).
    wire->set_send_capacity(64);
    HttpService service(std::move(transport));
    // Connections parked by the deferring route, resolved after the input is
    // exhausted.
    std::vector<pico_http::RequestId> deferred;
    // Registration can only fail by overflowing the route table, which would
    // be a fault in this harness rather than in the input under test. Abort
    // instead of fuzzing a service whose routes are silently missing.
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
        // Defers, and is resolved below (AR-9). Reaching this route parks the
        // connection, so the input then drives the deferred paths: pipelined
        // bytes held behind it, a teardown while the request is parked, and a
        // completion that may name a request the connection has moved past or
        // the transport has already torn down.
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

    // Resolve anything the deferring route parked, including handles the
    // transport may already have torn down — respond() has to be inert there
    // rather than resolving some other request (AR-4, AR-9). Resolving twice is
    // deliberate: a duplicate completion must be inert too.
    for (const pico_http::RequestId parked : deferred) {
        service.respond(parked, &fill_deferred, nullptr);
        service.respond(parked, &fill_deferred, nullptr);
        wire->run_pending_completions();
    }

    // Drain the final response so the teardown path runs. Bounded: the routes
    // emit a finite number of bytes.
    while (wire->is_open(id) && wire->send_inflight(id) > 0) {
        wire->deliver_ack(id, wire->send_inflight(id));
    }
    return 0;
}
