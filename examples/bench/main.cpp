// SPDX-License-Identifier: MIT
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "hardware/timer.h"
#include "lwip/ip4_addr.h"
#include "lwip/memp.h"
#include "lwip/netif.h"
#include "lwip/stats.h"
#include "lwipopts.h"
#include "pico/cyw43_arch.h"
#include "pico/stdio.h"
// pico/stdlib.h is the SDK's documented public entry point, and provides
// tight_loop_contents and the cyw43 board constants used below.
// include-cleaner names the headers that physically declare them --
// pico/platform/common.h, boards/<board>.h, cyw43_ll.h -- none of which
// can be included standalone, and the board one would pin the image to
// one board.
// NOLINTNEXTLINE(misc-include-cleaner)
#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico_http/config/profile.hpp"
#include "pico_http/config/sizing.hpp"
#include "pico_http/http/headers.hpp"
#include "pico_http/http/http_service.hpp"
#include "pico_http/http/message.hpp"
#include "pico_http/tcp/lwip_tcp_transport.hpp"
#include "pico_logging/logging/logging.hpp"
#include "pico_util/containers/fixed_string.hpp"
#include "pico_util/heap/heap_watermark.hpp"

/// \file main.cpp
/// Benchmark firmware for pico-http: HttpService over
/// LwipTcpTransport. Exposes the endpoints host-side bench.py drives, plus
/// /limits (spec OR-3) and a post-init heap-allocation counter (AR-1) so the
/// static-footprint claims are checkable at runtime, not just by inspection.
/// See README.md for the endpoint contract.

using pico_http::Disposition;
using pico_http::HTTPRequestView;
using pico_http::HTTPResponse;
using pico_http::HttpService;
using pico_http::LwipTcpTransport;
using pico_http::StaticBody;

namespace {

constexpr uint16_t HTTP_PORT = 80;
constexpr uint32_t PAYLOAD_MAX_BYTES = 256U * 1024U;  // PR-8: no size ceiling
constexpr std::string_view PAYLOAD_PREFIX = "/payload/";
constexpr std::string_view STREAM_PREFIX = "/stream/";
constexpr std::string_view BENCH_STATIC_PREFIX = "/bench/static/";
constexpr std::string_view BENCH_BUFFERED_PREFIX = "/bench/buffered/";
// Upper bound for the /bench body-mode routes: the P8-RP2350 buffered-body cap.
// /bench/buffered/<n> answers 413 above the active profile's own cap, which the
// library would refuse as a copied body (BR-14).
constexpr std::size_t BENCH_BODY_MAX = 8192;
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 500;

// The per-slot sizes the specification states for this target, pinned so a
// layout change fails this build until the specification moves with it (AR-1).
// They hold for the configuration the specification's tables describe: the
// 64 B / 256 B header caps, under either named profile.
#if PICO_HTTP_MAX_HEADER_NAME == 64 && PICO_HTTP_MAX_HEADER_VALUE == 256
namespace layout {
using Profile = PICO_HTTP_ACTIVE_PROFILE;
using Limits = Profile::Limits;
template <std::size_t N>
using Field = pico_util::FixedString<N>;
static_assert(sizeof(pico_http::HTTPHeaders) == 5380,
              "parsed header storage moved: update the per-slot table");
static_assert(sizeof(Field<Limits::MAX_METHOD + 1>) +
                      sizeof(Field<Limits::MAX_URI + 1>) +
                      sizeof(Field<Limits::MAX_VERSION + 1>) ==
                  568,
              "request-line storage moved: update the per-slot table");
static_assert(!std::is_same_v<Profile, pico_http::DefaultHttpProfile> ||
                  HttpService::caller_binding().conn_size == 12616,
              "the default profile's slot moved: update the per-slot table");
static_assert(!std::is_same_v<Profile, pico_http::P8Rp2350HttpProfile> ||
                  HttpService::caller_binding().conn_size == 21680,
              "the P8-RP2350 slot moved: update the P8-RP2350 profile section");
static_assert(pico_http::TX_RING_CAPACITY == 5840,
              "the TX ring moved: update the transport-slot table and PR-5");
}  // namespace layout
#endif

// Largest response-formatting buffer any route may put on the stack.
//
// Recorded because nothing else observes it. Route handlers run on the main
// stack, on top of the cyw43 -> lwIP -> transport -> parser -> dispatch chain,
// and the footprint gate measures .data/.bss and flash only — a stack frame
// is invisible to it, so the largest one this image contains is bounded here or
// nowhere (CXX-11.4). PICO_STACK_SIZE defaults to 0x800 and the descending
// stack is backed by the 4 KiB SCRATCH_Y region, with PICO_USE_STACK_GUARDS
// off by default, so an overflow is silent.
//
// This is a budget, not a measurement: no device run backs it. It is set at the
// largest buffer the routes currently need so that adding a bigger one is a
// build failure and a decision, rather than a silent step towards the edge.
constexpr std::size_t HANDLER_STACK_BUDGET = 1536;
// The main loop's poll period, and therefore the latency floor of every
// deferred response: a request parked by the callback waits here until the next
// pass. It must stay small next to the work being deferred, or a measurement of
// deferral reports this constant instead. At the previous 50 ms a 10 ms burn
// cost ~62 ms end to end -- five parts loop to one part handler -- which
// throttled t19's deferred arm to roughly a quarter of the load its
// synchronous arm carried and inflated the improvement it appeared to buy.
constexpr uint32_t LOOP_POLL_MS = 1;

// Linker-provided heap bounds (Pico SDK layout) for a free-heap estimate.
extern "C" {
// Both names are fixed by the SDK linker script, so neither the reserved
// double-underscore prefix nor the case style is ours to change.
// NOLINTNEXTLINE(bugprone-reserved-identifier,readability-identifier-naming)
extern char __StackLimit;
// Fixed by the SDK linker script, as above.
// NOLINTNEXTLINE(bugprone-reserved-identifier,readability-identifier-naming)
extern char __bss_end__;
}

// Post-init heap-growth tracking (AR-1) comes from pico-util, which owns the
// requirement as pico_util:MR-1 (pico_util/heap/heap_watermark.hpp explains the
// mallinfo proxy and why a per-alloc hook isn't available under the Pico SDK).
// The static-body and generator response paths (used by /ping and /payload)
// touch no heap per request; only the copied set_body() path allocates: one
// exact-sized buffer per response, which the connection slot frees once the
// response is sent. Live copied-body heap is therefore at most one body per
// connection in flight, bounded by MAX_CONNECTIONS * MAX_BUFFERED_BODY, and
// zero once the connections are idle.
// The watermark is armed after start() and sampled by /stats, so it is mutable
// by construction. Every steady-state access is in the async-context callback
// — arm() from /stats/reset and growth_bytes() from /stats — with only the
// one-time arm() in main() outside it, so the callers agree with each other
// without a lock. Adding a caller from the main loop would need the same
// cyw43_arch_lwip_begin()/end() bracket pump_deferred_work uses.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
pico_util::HeapWatermark g_heap;

uint32_t free_heap_bytes() {
    // Both symbols are placed by the SDK linker script to bracket the heap,
    // so the distance between them is the heap size. The analyzer cannot see
    // that they name positions in one region rather than two objects, and
    // there is no way to express a linker-defined extent that it can.
    // NOLINTNEXTLINE(clang-analyzer-security.PointerSub)
    auto total = static_cast<uint32_t>(&__StackLimit - &__bss_end__);
    return total -
           static_cast<uint32_t>(pico_util::HeapWatermark::in_use_bytes());
}

void arm_heap_growth_tracking() { g_heap.arm(); }

uint32_t post_init_heap_growth_bytes() {
    return static_cast<uint32_t>(g_heap.growth_bytes());
}

void blink_forever(uint32_t interval_ms) {
    bool led_on = false;
    while (true) {
        // Board constant from the cyw43 headers pico/stdlib.h pulls in; see
        // the note on that include.
        // NOLINTNEXTLINE(misc-include-cleaner)
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_on);
        led_on = !led_on;
        sleep_ms(interval_ms);
    }
}

void connect_wifi() {
    // PICO_HTTP_WIFI_SSID is a compile definition and is empty only in an
    // unconfigured build, which is the build clang-tidy sees. Reading that as a
    // redundant initialiser and dropping it leaves SSID permanently empty, so
    // the guard below always fires and the firmware blinks instead of ever
    // joining. NOLINTNEXTLINE(readability-redundant-string-init)
    constexpr std::string_view SSID = PICO_HTTP_WIFI_SSID;
    constexpr uint32_t CONNECT_TIMEOUT_MS = 30000;
    constexpr uint32_t ERROR_BLINK_MS = 100;

    if (SSID.empty()) {
        PICO_LOG_ERROR << "PICO_HTTP_WIFI_SSID is empty. Configure with: "
                          "cmake -B build-firmware -DPICO_HTTP_WIFI_SSID=... "
                          "-DPICO_HTTP_WIFI_PASSWORD=...";
        blink_forever(ERROR_BLINK_MS);
    }

    cyw43_arch_enable_sta_mode();
    PICO_LOG_INFO << "Connecting to Wi-Fi SSID '" << SSID << "'...";

    // From the cyw43 headers pico/stdlib.h pulls in; see the note on that
    // include. Hoisted to its own statement so clang-format cannot wrap the
    // call and separate this justification from the suppression below it.
    // NOLINTNEXTLINE(misc-include-cleaner)
    const auto auth = CYW43_AUTH_WPA2_AES_PSK;
    while (cyw43_arch_wifi_connect_timeout_ms(PICO_HTTP_WIFI_SSID,
                                              PICO_HTTP_WIFI_PASSWORD, auth,
                                              CONNECT_TIMEOUT_MS) != 0) {
        PICO_LOG_WARN << "Wi-Fi connect failed, retrying...";
    }

    cyw43_arch_lwip_begin();
    const char* ip = ip4addr_ntoa(netif_ip4_addr(netif_default));
    cyw43_arch_lwip_end();
    PICO_LOG_INFO << "Wi-Fi connected, IP address: " << ip;
}

// Resumable body producer for /payload: the deterministic alphabet pattern the
// host verifies, body[i] = 'A' + i % 26, computed purely from the byte offset.
// Because it is a pure function of the offset (no cursor), the response writer
// can stream a body of any size through a fixed scratch buffer with zero heap
// (AR-5), and partial sends / retransmits re-read it safely.
std::size_t payload_generator(void* /*ctx*/, std::size_t offset,
                              std::span<std::uint8_t> out) {
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<uint8_t>('A' + ((offset + i) % 26U));
    }
    return out.size();
}

Disposition handle_payload(const HTTPRequestView& req, HTTPResponse& res) {
    const std::string_view size_str = req.uri.substr(PAYLOAD_PREFIX.size());
    uint32_t requested = 0;
    auto [ptr, ec] = std::from_chars(
        size_str.data(), size_str.data() + size_str.size(), requested);

    const bool valid = ec == std::errc{} &&
                       ptr == size_str.data() + size_str.size() &&
                       requested > 0 && requested <= PAYLOAD_MAX_BYTES;
    if (!valid) {
        res.status_code = HTTPResponse::STATUS_BAD_REQUEST;
        // 11 characters into STATUS_TEXT_CAPACITY: cannot truncate.
        static_cast<void>(res.status_text.assign("Bad Request"));
        res.set_body(R"({"error":"size must be 1..262144"})",
                     "application/json");
        return Disposition::COMPLETE;
    }

    // Stream the pattern through the generator API — no per-response heap and
    // no size ceiling, so a download is bounded by the transfer rather than by
    // a buffer.
    res.set_body_generator(payload_generator, nullptr, requested,
                           "application/octet-stream");
    return Disposition::COMPLETE;
}

// Length-less variant of payload_generator for /stream: emits the same
// body[i] = 'A' + i % 26 pattern but signals end-of-body by returning 0 once
// `offset` reaches the target size. The size is carried by value in the ctx
// pointer (no per-response storage to outlive the handler, and no cross-slot
// sharing) — the generator never dereferences it.
std::size_t stream_generator(void* ctx, std::size_t offset,
                             std::span<std::uint8_t> out) {
    // Recovers the size handle_stream() packed into the opaque ctx pointer;
    // never dereferenced.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto total = reinterpret_cast<std::uintptr_t>(ctx);
    if (offset >= total) {
        return 0;  // end-of-body
    }
    const std::size_t n = std::min<std::size_t>(out.size(), total - offset);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<uint8_t>('A' + ((offset + i) % 26U));
    }
    return n;
}

// GET /stream/<size> — same bytes as /payload/<size> but framed with
// Transfer-Encoding: chunked (NR-6), exercising the length-less generator path
// on device. Zero per-response heap, same as /payload.
Disposition handle_stream(const HTTPRequestView& req, HTTPResponse& res) {
    const std::string_view size_str = req.uri.substr(STREAM_PREFIX.size());
    uint32_t requested = 0;
    auto [ptr, ec] = std::from_chars(
        size_str.data(), size_str.data() + size_str.size(), requested);

    const bool valid = ec == std::errc{} &&
                       ptr == size_str.data() + size_str.size() &&
                       requested > 0 && requested <= PAYLOAD_MAX_BYTES;
    if (!valid) {
        res.status_code = HTTPResponse::STATUS_BAD_REQUEST;
        // 11 characters into STATUS_TEXT_CAPACITY: cannot truncate.
        static_cast<void>(res.status_text.assign("Bad Request"));
        res.set_body(R"({"error":"size must be 1..262144"})",
                     "application/json");
        return Disposition::COMPLETE;
    }

    // The generator context is an opaque void*; this route carries the
    // requested length in it rather than pointing at per-request storage.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    void* ctx = reinterpret_cast<void*>(static_cast<std::uintptr_t>(requested));
    res.set_body_generator(stream_generator, ctx, "application/octet-stream");
    return Disposition::COMPLETE;
}

// Fixed alphabet body backing the /bench body-mode routes; the same
// body[i] = 'A' + i % 26 pattern as /payload, materialised once in static
// storage so STATIC_VIEW can reference it with no per-request work.
// The g_ prefix marks file-scope storage, matching the other globals here;
// this is not a compile-time constant.
// NOLINTNEXTLINE(readability-identifier-naming)
const std::array<std::uint8_t, BENCH_BODY_MAX> g_bench_body = [] {
    std::array<std::uint8_t, BENCH_BODY_MAX> body{};
    for (std::size_t i = 0; i < body.size(); ++i) {
        body[i] = static_cast<std::uint8_t>('A' + (i % 26U));
    }
    return body;
}();

// Parse a positive decimal size from `uri` after `prefix`, bounded by `max`.
// Returns 0 for any malformed, zero, or out-of-range value.
std::uint32_t parse_bench_size(std::string_view uri, std::string_view prefix,
                               std::uint32_t max) {
    const std::string_view size_str = uri.substr(prefix.size());
    std::uint32_t value = 0;
    auto [ptr, ec] = std::from_chars(size_str.data(),
                                     size_str.data() + size_str.size(), value);
    if (ec != std::errc{} || ptr != size_str.data() + size_str.size() ||
        value == 0 || value > max) {
        return 0;
    }
    return value;
}

void bench_bad_request(HTTPResponse& res) {
    res.status_code = HTTPResponse::STATUS_BAD_REQUEST;
    // 11 characters into STATUS_TEXT_CAPACITY: cannot truncate.
    static_cast<void>(res.status_text.assign("Bad Request"));
    res.set_body(R"({"error":"size must be 1..8192"})", "application/json");
}

// GET /bench/static/<n> and /bench/buffered/<n> serve the identical n-byte
// body through the STATIC_VIEW and BUFFERED body modes respectively, so bench
// t17 can isolate the buffered mode's extra copy against a static baseline on
// one held-constant payload. n <= BENCH_BODY_MAX.
Disposition handle_bench_static(const HTTPRequestView& req, HTTPResponse& res) {
    const std::uint32_t n =
        parse_bench_size(req.uri, BENCH_STATIC_PREFIX,
                         static_cast<std::uint32_t>(BENCH_BODY_MAX));
    if (n == 0) {
        bench_bad_request(res);
        return Disposition::COMPLETE;
    }
    res.set_body_static(
        StaticBody::outliving_response(
            std::span<const std::uint8_t>(g_bench_body.data(), n)),
        "application/octet-stream");
    return Disposition::COMPLETE;
}

Disposition handle_bench_buffered(const HTTPRequestView& req,
                                  HTTPResponse& res) {
    const std::uint32_t n =
        parse_bench_size(req.uri, BENCH_BUFFERED_PREFIX,
                         static_cast<std::uint32_t>(BENCH_BODY_MAX));
    if (n == 0) {
        bench_bad_request(res);
        return Disposition::COMPLETE;
    }
    if (n > HttpService::Limits::MAX_BUFFERED_BODY) {
        res.status_code = HTTPResponse::STATUS_PAYLOAD_TOO_LARGE;
        // 17 characters into STATUS_TEXT_CAPACITY: cannot truncate.
        static_cast<void>(res.status_text.assign("Payload Too Large"));
        res.set_body(R"({"error":"over the buffered-body cap"})",
                     "application/json");
        return Disposition::COMPLETE;
    }
    res.set_body(std::span<const std::uint8_t>(g_bench_body.data(), n),
                 "application/octet-stream");
    return Disposition::COMPLETE;
}

Disposition handle_upload(const HTTPRequestView& req, HTTPResponse& res) {
    uint32_t checksum = 0;
    for (const uint8_t byte : req.body) {
        checksum += byte;
    }

    static_assert(128 <= HANDLER_STACK_BUDGET);
    char buf[128];
    const int len =
        std::snprintf(buf, sizeof(buf), R"({"received":%zu,"checksum":%lu})",
                      req.body.size(), static_cast<unsigned long>(checksum));
    res.set_body(std::string_view(buf, static_cast<size_t>(len)),
                 "application/json");
    return Disposition::COMPLETE;
}

// OR-2: the counter set as JSON.
void handle_stats(const HttpService& server, HTTPResponse& res) {
    const HttpService::Statistics& stats = server.statistics();
    const LwipTcpTransport::LockHoldStats lock =
        LwipTcpTransport::lock_hold_stats();
    const LwipTcpTransport::ReceiveStats receive =
        LwipTcpTransport::receive_stats();
    auto uptime = to_ms_since_boot(get_absolute_time());
    // Widened once here rather than cast at the call: uint32_t is already
    // unsigned long on this target, so a cast there is a no-op that
    // -Wuseless-cast rejects, while an implicit conversion stays correct on a
    // target where the two differ.
    // unsigned long, not uint32_t: these feed %lu in the snprintf below, and
    // the fixed-width type is what google-runtime-int would have here.
    // NOLINTNEXTLINE(google-runtime-int)
    const unsigned long free_heap = free_heap_bytes();
    // Same, for the same format specifier.
    // NOLINTNEXTLINE(google-runtime-int)
    const unsigned long heap_growth = post_init_heap_growth_bytes();

    static_assert(1536 <= HANDLER_STACK_BUDGET);
    char buf[1536];
    const int len = std::snprintf(
        buf, sizeof(buf),
        "{\"uptime_ms\":%lu,\"free_heap_bytes\":%lu,"
        "\"post_init_heap_growth_bytes\":%lu,"
        "\"active_connections\":%zu,\"slot_high_watermark\":%lu,"
        "\"accepts\":%lu,\"refusals_slot_full\":%lu,"
        "\"requests_completed\":%lu,\"parse_errors\":%lu,"
        "\"parse_errors_400\":%lu,\"parse_errors_413\":%lu,"
        "\"parse_errors_414\":%lu,\"parse_errors_417\":%lu,"
        "\"parse_errors_431\":%lu,"
        "\"parse_errors_501\":%lu,\"parse_errors_505\":%lu,"
        "\"disconnect_peer_closed\":%lu,\"disconnect_peer_reset\":%lu,"
        "\"disconnect_aborted\":%lu,"
        "\"disconnect_timeout\":%lu,\"disconnect_timeout_header\":%lu,"
        "\"disconnect_timeout_body\":%lu,\"disconnect_timeout_idle\":%lu,"
        "\"disconnect_timeout_write\":%lu,\"disconnect_timeout_drain\":%lu,"
        "\"disconnect_timeout_handler\":%lu,"
        "\"disconnect_server_closed\":%lu,"
        "\"backpressure_events\":%lu,\"rx_window_throttle_events\":%lu,"
        "\"rx_overflow_drops\":%lu,"
        "\"pending_dispatches\":%lu,\"pending_high_watermark\":%lu,"
        "\"deferrals_refused\":%lu,\"responses_refused\":%lu,"
        "\"pending_responses\":%zu,"
        "\"bytes_in\":%llu,\"bytes_out\":%llu,"
        "\"lock_hold_samples\":%lu,\"lock_hold_within_500us\":%lu,"
        "\"lock_hold_max_us\":%lu,"
        "\"receive_samples\":%lu,\"receive_within_1ms\":%lu,"
        "\"receive_max_us\":%lu,"
        "\"receive_total_within_1ms\":%lu,\"receive_total_max_us\":%lu}",
        static_cast<unsigned long>(uptime), free_heap, heap_growth,
        server.active_connections(),
        static_cast<unsigned long>(stats.slot_high_watermark),
        static_cast<unsigned long>(stats.accepts),
        static_cast<unsigned long>(stats.refusals_slot_full),
        static_cast<unsigned long>(stats.requests_completed),
        static_cast<unsigned long>(stats.parse_errors),
        static_cast<unsigned long>(stats.parse_errors_400),
        static_cast<unsigned long>(stats.parse_errors_413),
        static_cast<unsigned long>(stats.parse_errors_414),
        static_cast<unsigned long>(stats.parse_errors_417),
        static_cast<unsigned long>(stats.parse_errors_431),
        static_cast<unsigned long>(stats.parse_errors_501),
        static_cast<unsigned long>(stats.parse_errors_505),
        static_cast<unsigned long>(stats.disconnect_peer_closed),
        static_cast<unsigned long>(stats.disconnect_peer_reset),
        static_cast<unsigned long>(stats.disconnect_aborted),
        static_cast<unsigned long>(stats.disconnect_timeout),
        static_cast<unsigned long>(stats.disconnect_timeout_header),
        static_cast<unsigned long>(stats.disconnect_timeout_body),
        static_cast<unsigned long>(stats.disconnect_timeout_idle),
        static_cast<unsigned long>(stats.disconnect_timeout_write),
        static_cast<unsigned long>(stats.disconnect_timeout_drain),
        static_cast<unsigned long>(stats.disconnect_timeout_handler),
        static_cast<unsigned long>(stats.disconnect_server_closed),
        static_cast<unsigned long>(stats.backpressure_events),
        static_cast<unsigned long>(stats.rx_window_throttle_events),
        static_cast<unsigned long>(stats.rx_overflow_drops),
        static_cast<unsigned long>(stats.pending_dispatches),
        static_cast<unsigned long>(stats.pending_high_watermark),
        static_cast<unsigned long>(stats.deferrals_refused),
        static_cast<unsigned long>(stats.responses_refused),
        server.pending_responses(),
        static_cast<unsigned long long>(stats.bytes_in),
        static_cast<unsigned long long>(stats.bytes_out),
        static_cast<unsigned long>(lock.samples),
        static_cast<unsigned long>(lock.within_500us),
        static_cast<unsigned long>(lock.max_us),
        static_cast<unsigned long>(receive.samples),
        static_cast<unsigned long>(receive.within_1ms),
        static_cast<unsigned long>(receive.max_us),
        static_cast<unsigned long>(receive.total_within_1ms),
        static_cast<unsigned long>(receive.total_max_us));
    res.set_body(std::string_view(buf, static_cast<size_t>(len)),
                 "application/json");
}

void handle_limits(const HttpService& server, HTTPResponse& res) {
    using Limits = HttpService::Limits;  // active profile's caps
    const HttpService::TimeoutConfig& timeouts = server.timeout_config();
    const HttpService::KeepAliveConfig& ka = server.keep_alive_config();
    static_assert(512 <= HANDLER_STACK_BUDGET);
    char buf[512];
    const int len = std::snprintf(
        buf, sizeof(buf),
        "{\"max_request_line\":%zu,\"max_method\":%zu,\"max_uri\":%zu,"
        "\"max_header_line\":%zu,\"max_headers\":%zu,"
        "\"max_request_head\":%zu,\"max_buffered_body\":%zu,"
        "\"max_connections\":%zu,\"max_pending_responses\":%zu,"
        "\"head_capacity\":%zu,"
        "\"timeout_header_ms\":%lu,\"timeout_body_ms\":%lu,"
        "\"timeout_idle_ms\":%lu,\"timeout_write_ms\":%lu,"
        "\"timeout_drain_ms\":%lu,\"timeout_handler_ms\":%lu}",
        Limits::MAX_REQUEST_LINE, Limits::MAX_METHOD, Limits::MAX_URI,
        Limits::MAX_HEADER_LINE, Limits::MAX_HEADERS, Limits::MAX_REQUEST_HEAD,
        Limits::MAX_BUFFERED_BODY, HttpService::MAX_CONNECTIONS,
        HttpService::MAX_PENDING, PICO_HTTP_ACTIVE_PROFILE::HEAD_CAPACITY,
        static_cast<unsigned long>(timeouts.header_ms),
        static_cast<unsigned long>(timeouts.body_ms),
        static_cast<unsigned long>(ka.idle_timeout_seconds) * 1000U,
        static_cast<unsigned long>(timeouts.write_ms),
        static_cast<unsigned long>(pico_http::DRAIN_LINGER_MS),
        static_cast<unsigned long>(timeouts.handler_ms));
    res.set_body(std::string_view(buf, static_cast<size_t>(len)),
                 "application/json");
}

// OR-4: build and profile identity.
void handle_info(const HttpService& server, HTTPResponse& res) {
    const HttpService::KeepAliveConfig& ka = server.keep_alive_config();

    static_assert(512 <= HANDLER_STACK_BUDGET);
    char buf[512];
    const int len = std::snprintf(
        buf, sizeof(buf),
        "{\"version\":\"%s\",\"board\":\"%s\","
        "\"profile\":\"%s\",\"build_type\":\"%s\","
        "\"tx_ring_capacity\":%zu,\"max_connections\":%zu,"
        "\"memp_num_tcp_pcb\":%u,\"tcp_rto_ms\":%u,"
        "\"keep_alive_enabled\":%s,\"keep_alive_idle_timeout_s\":%lu,"
        "\"keep_alive_max_requests\":%lu,\"payload_max_bytes\":%u}",
        PICO_HTTP_BENCH_VERSION, PICO_HTTP_BENCH_BOARD, PICO_HTTP_BENCH_PROFILE,
        PICO_HTTP_BENCH_BUILD_TYPE, pico_http::TX_RING_CAPACITY,
        HttpService::MAX_CONNECTIONS, static_cast<unsigned>(MEMP_NUM_TCP_PCB),
        // PR-9's ceiling is a multiple of the configured initial RTO, so the
        // harness reads the value in force rather than assuming lwIP's default.
        static_cast<unsigned>(LWIP_TCP_RTO_TIME), ka.enabled ? "true" : "false",
        static_cast<unsigned long>(ka.idle_timeout_seconds),
        static_cast<unsigned long>(ka.max_requests_per_connection),
        static_cast<unsigned>(PAYLOAD_MAX_BYTES));
    res.set_body(std::string_view(buf, static_cast<size_t>(len)),
                 "application/json");
}

// Dumps lwIP's own heap/pool high-water marks (LWIP_STATS): `mem.max` is the
// peak bytes ever live in lwIP's MEM_SIZE heap, and each pool's `max` is the
// peak entries used, so the bounds the reference sizes MEM_SIZE,
// PBUF_POOL_SIZE, MEMP_NUM_TCP_SEG and MEMP_NUM_PBUF by are checked against
// real load. `pbuf_ref` is
// MEMP_PBUF: the PBUF_ROM header the zero-copy TX path takes for every segment
// it hands lwIP from the ring. `err` is non-zero only if an
// allocation was ever refused — the signal that a value was set too low.
void handle_lwip_stats(HTTPResponse& res) {
    const struct stats_mem& mem = lwip_stats.mem;
    const struct stats_mem& pbuf = *lwip_stats.memp[MEMP_PBUF_POOL];
    const struct stats_mem& seg = *lwip_stats.memp[MEMP_TCP_SEG];
    const struct stats_mem& pcb = *lwip_stats.memp[MEMP_TCP_PCB];
    const struct stats_mem& ref = *lwip_stats.memp[MEMP_PBUF];
    // TCP protocol counters, not pool counters. lwIP keeps no retransmit
    // counter, so these bound the question rather than answering it: xmit
    // against recv, and drop/memerr, say whether the stack shed anything when
    // one connection freezes and the rest stay healthy.
    const struct stats_proto& tcp = lwip_stats.tcp;

    static_assert(768 <= HANDLER_STACK_BUDGET);
    char buf[768];
    const int len = std::snprintf(
        buf, sizeof(buf),
        "{\"mem\":{\"avail\":%lu,\"used\":%lu,\"max\":%lu,\"err\":%lu},"
        "\"pbuf_pool\":{\"avail\":%lu,\"used\":%lu,\"max\":%lu,\"err\":%lu},"
        "\"tcp_seg\":{\"avail\":%lu,\"used\":%lu,\"max\":%lu,\"err\":%lu},"
        "\"tcp_pcb\":{\"avail\":%lu,\"used\":%lu,\"max\":%lu,\"err\":%lu},"
        "\"pbuf_ref\":{\"avail\":%lu,\"used\":%lu,\"max\":%lu,\"err\":%lu},"
        "\"tcp\":{\"xmit\":%lu,\"recv\":%lu,\"drop\":%lu,"
        "\"memerr\":%lu,\"err\":%lu}}",
        static_cast<unsigned long>(mem.avail),
        static_cast<unsigned long>(mem.used),
        static_cast<unsigned long>(mem.max),
        static_cast<unsigned long>(mem.err),
        static_cast<unsigned long>(pbuf.avail),
        static_cast<unsigned long>(pbuf.used),
        static_cast<unsigned long>(pbuf.max),
        static_cast<unsigned long>(pbuf.err),
        static_cast<unsigned long>(seg.avail),
        static_cast<unsigned long>(seg.used),
        static_cast<unsigned long>(seg.max),
        static_cast<unsigned long>(seg.err),
        static_cast<unsigned long>(pcb.avail),
        static_cast<unsigned long>(pcb.used),
        static_cast<unsigned long>(pcb.max),
        static_cast<unsigned long>(pcb.err),
        static_cast<unsigned long>(ref.avail),
        static_cast<unsigned long>(ref.used),
        static_cast<unsigned long>(ref.max),
        static_cast<unsigned long>(ref.err),
        static_cast<unsigned long>(tcp.xmit),
        static_cast<unsigned long>(tcp.recv),
        static_cast<unsigned long>(tcp.drop),
        static_cast<unsigned long>(tcp.memerr),
        static_cast<unsigned long>(tcp.err));
    res.set_body(std::string_view(buf, static_cast<size_t>(len)),
                 "application/json");
}

// t19: the same CPU-heavy work in both shapes, so a bench run can price what
// deferral buys on the hot path.
//
// /heavy/<ms> burns the time inside the handler, which runs in the lwIP
// callback — every other connection's network processing waits behind it.
// /heavy-deferred/<ms> parks the request (AR-9) and burns the identical time in
// the main loop, off the async-context path. /ping latency measured against
// each is the difference the deferred API makes at this version, before any
// second core is involved.
constexpr std::string_view HEAVY_PREFIX = "/heavy/";
constexpr std::string_view HEAVY_DEFERRED_PREFIX = "/heavy-deferred/";
constexpr std::uint32_t HEAVY_MAX_MS = 1000;

// Sized to the BR-12 cap: a deferral past the cap is refused with 503 before
// the handler runs, so this can never be the tighter of the two limits.
constexpr std::size_t MAX_DEFERRED_WORK = HttpService::MAX_PENDING;

struct DeferredWork {
    // The request-scoped handle, which is all a deferring handler keeps (AR-9).
    // Deliberately not a pointer to the response: that object belongs to the
    // service and is gone the moment the handler returns.
    pico_http::RequestId id;
    std::uint32_t started_ms = 0;
    std::uint32_t burn_ms = 0;
    bool held = false;
};

// The filler the service invokes at completion time, against a response it
// hands us then — not one we kept. This is the pattern a consumer copies.
void fill_heavy_deferred(void* /*ctx*/, HTTPResponse& res) {
    res.set_body_static(StaticBody::outliving_response("heavy"), "text/plain");
}

// Owned by main() and threaded through by reference: the handler runs in the
// lwIP callback and only records here, while pump_deferred_work() below is the
// only thing that burns time or responds.
using DeferredWorkTable = std::array<DeferredWork, MAX_DEFERRED_WORK>;

// Parse the trailing <ms> of a heavy route. Returns 0 when out of range, which
// the caller reports as 400.
std::uint32_t parse_burn_ms(std::string_view uri, std::string_view prefix) {
    const std::string_view ms_str = uri.substr(prefix.size());
    std::uint32_t ms = 0;
    auto [ptr, ec] =
        std::from_chars(ms_str.data(), ms_str.data() + ms_str.size(), ms);
    const bool valid = ec == std::errc{} &&
                       ptr == ms_str.data() + ms_str.size() && ms > 0 &&
                       ms <= HEAVY_MAX_MS;
    return valid ? ms : 0;
}

// busy_wait_us rather than a hand-rolled spin: it is pico/time.h's own API for
// exactly this, so it needs no suppression for an SDK symbol whose declaring
// header is not includable standalone, and it is wrap-correct by construction
// (AR-7) instead of by a signed-difference loop written here.
void burn_cpu(std::uint32_t ms) {
    constexpr std::uint32_t US_PER_MS = 1000;
    busy_wait_us(static_cast<std::uint64_t>(ms) * US_PER_MS);
}

void bad_burn_request(HTTPResponse& res) {
    res.status_code = HTTPResponse::STATUS_BAD_REQUEST;
    // 11 characters into STATUS_TEXT_CAPACITY: cannot truncate.
    static_cast<void>(res.status_text.assign("Bad Request"));
    res.set_body(R"({"error":"ms must be 1..1000"})", "application/json");
}

// Synchronous: burns inside the lwIP callback.
Disposition handle_heavy(const HTTPRequestView& req, HTTPResponse& res) {
    const std::uint32_t ms = parse_burn_ms(req.uri, HEAVY_PREFIX);
    if (ms == 0) {
        bad_burn_request(res);
        return Disposition::COMPLETE;
    }
    burn_cpu(ms);
    res.set_body_static(StaticBody::outliving_response("heavy"), "text/plain");
    return Disposition::COMPLETE;
}

// Deferred: records the work and returns immediately, so the callback path is
// free while the main loop burns the time (AR-9).
Disposition handle_heavy_deferred(DeferredWorkTable& table,
                                  const HTTPRequestView& req,
                                  HTTPResponse& res) {
    const std::uint32_t ms = parse_burn_ms(req.uri, HEAVY_DEFERRED_PREFIX);
    if (ms == 0) {
        bad_burn_request(res);
        return Disposition::COMPLETE;
    }
    for (DeferredWork& work : table) {
        if (work.held) {
            continue;
        }
        work.id = req.request;
        work.burn_ms = ms;
        work.started_ms = to_ms_since_boot(get_absolute_time());
        work.held = true;
        // Nothing is written to `res` here: a deferring handler's response is
        // discarded, and fill_heavy_deferred supplies the body at completion.
        return Disposition::PENDING;
    }
    // Unreachable while MAX_DEFERRED_WORK == MAX_PENDING: BR-12 would have
    // refused this request with 503 before the handler ran, and this scan runs
    // inside the async-context lock, so the main loop cannot be releasing a
    // slot underneath it. Answering inline rather than asserting keeps the
    // bench firmware serving if either of those ever stops holding.
    burn_cpu(ms);
    res.set_body_static(StaticBody::outliving_response("heavy"), "text/plain");
    return Disposition::COMPLETE;
}

// Resolve any parked heavy work. Called from the main loop, so the burn happens
// off the callback path — which is the whole point of the comparison.
//
// The table is shared state. handle_heavy_deferred writes it from the
// async-context worker, which under pico_cyw43_arch_lwip_threadsafe_background
// is a low-priority interrupt that preempts this loop; the accesses here are
// therefore bracketed by cyw43_arch_lwip_begin()/end() (CXX-08.1). The handler
// itself already runs inside that lock and needs no bracket of its own.
//
// Two properties the bracketing alone would not give:
//
//   - The burn happens *outside* the lock. Holding it across the burn would
//     stall the network for exactly the duration this design exists to move off
//     the callback path.
//   - `id` and `burn_ms` are copied out before the slot is released, and the
//     response is sent against the copy. Reading work.id after clearing
//     work.held let a request arriving in that window claim the slot and be
//     answered without its own burn ever running, while the request that
//     actually owned the slot was never resolved and hung to TR-6.
void pump_deferred_work(HttpService& server, DeferredWorkTable& table) {
    for (DeferredWork& work : table) {
        pico_http::RequestId id{};
        std::uint32_t burn_ms = 0;
        cyw43_arch_lwip_begin();
        const bool claimed = work.held;
        if (claimed) {
            id = work.id;
            burn_ms = work.burn_ms;
        }
        cyw43_arch_lwip_end();
        if (!claimed) {
            continue;
        }

        burn_cpu(burn_ms);

        cyw43_arch_lwip_begin();
        work.held = false;
        cyw43_arch_lwip_end();

        // Ignoring the result: false means the request went away while the
        // work ran (timeout, reset, or the connection moved on), which is the
        // expected race and needs no handling here — the filler simply never
        // runs (AR-9).
        static_cast<void>(server.respond(id, &fill_heavy_deferred, nullptr));
    }
}

// Registration only fails if a pattern exceeds RouteTable's fixed capacity,
// which is a fixed property of the literals below — so a failure here is a
// build-time mistake in this file, reported at boot rather than served as a
// silent 404. Returns false if any route was rejected.
bool register_routes(HttpService& server, DeferredWorkTable& deferred_work) {
    bool ok = true;
    ok = server.add_route("GET", "/ping",
                          [](const HTTPRequestView&, HTTPResponse& res) {
                              // Static literal: no copy before the ring,
                              // zero-heap hot path (AR-2).
                              res.set_body_static(
                                  StaticBody::outliving_response("pong"),
                                  "text/plain");
                              return Disposition::COMPLETE;
                          }) &&
         ok;

    ok = server.add_route("GET", "/payload/*", handle_payload) && ok;

    // t19: the CPU-heavy pair (AR-9).
    ok = server.add_route("GET", "/heavy/*", handle_heavy) && ok;
    ok = server.add_route(
             "GET", "/heavy-deferred/*",
             [&deferred_work](const HTTPRequestView& req, HTTPResponse& res) {
                 return handle_heavy_deferred(deferred_work, req, res);
             }) &&
         ok;

    ok = server.add_route("GET", "/stream/*", handle_stream) && ok;

    ok = server.add_route("GET", "/bench/static/*", handle_bench_static) && ok;

    ok = server.add_route("GET", "/bench/buffered/*", handle_bench_buffered) &&
         ok;

    ok = server.add_route("POST", "/upload",
                          [](const HTTPRequestView& req, HTTPResponse& res) {
                              handle_upload(req, res);
                              return Disposition::COMPLETE;
                          }) &&
         ok;

    ok = server.add_route("GET", "/stats",
                          [&server](const HTTPRequestView&, HTTPResponse& res) {
                              handle_stats(server, res);
                              return Disposition::COMPLETE;
                          }) &&
         ok;

    ok = server.add_route(
             "POST", "/stats/reset",
             [&server](const HTTPRequestView&, HTTPResponse& res) {
                 // The connections open as the counters zero,
                 // this one included: a soak needs them to check
                 // that accepts and disconnects balance (RR-4).
                 const std::size_t open = server.active_connections();
                 server.reset_statistics();
                 LwipTcpTransport::reset_lock_hold_stats();
                 LwipTcpTransport::reset_receive_stats();
                 arm_heap_growth_tracking();  // re-baseline
                 std::array<char, 48> buf{};
                 const int len = std::snprintf(
                     buf.data(), buf.size(),
                     R"({"ok":true,"active_connections":%zu})", open);
                 res.set_body(std::string_view(buf.data(),
                                               static_cast<std::size_t>(len)),
                              "application/json");
                 return Disposition::COMPLETE;
             }) &&
         ok;

    ok = server.add_route("GET", "/info",
                          [&server](const HTTPRequestView&, HTTPResponse& res) {
                              handle_info(server, res);
                              return Disposition::COMPLETE;
                          }) &&
         ok;

    ok = server.add_route("GET", "/limits",
                          [&server](const HTTPRequestView&, HTTPResponse& res) {
                              handle_limits(server, res);
                              return Disposition::COMPLETE;
                          }) &&
         ok;

    ok = server.add_route("GET", "/lwip_stats",
                          [](const HTTPRequestView&, HTTPResponse& res) {
                              handle_lwip_stats(res);
                              return Disposition::COMPLETE;
                          }) &&
         ok;

    return ok;
}

}  // namespace

int main() {
    stdio_init_all();

    if (cyw43_arch_init() != 0) {
        printf("Wi-Fi/BT chip init failed\n");
        return -1;
    }

    connect_wifi();

    DeferredWorkTable deferred_work{};

    // Heap-allocate the service: sizeof(HttpService) is ~122 KB (eight
    // per-connection slots, each holding the parser, RX assembly buffer, and
    // response-writer buffers inline), far too large to sit on main()'s stack.
    auto server_ptr =
        std::make_unique<HttpService>(std::make_unique<LwipTcpTransport>());
    HttpService& server = *server_ptr;
    if (!register_routes(server, deferred_work)) {
        PICO_LOG_ERROR << "A route was rejected: pattern exceeds the table's "
                          "fixed capacity";
        blink_forever(50);
    }

    if (!server.start(HTTP_PORT)) {
        PICO_LOG_ERROR << "Failed to start HTTP server on port " << HTTP_PORT;
        blink_forever(50);
    }
    PICO_LOG_INFO << "Benchmark server listening on port " << HTTP_PORT;

    // Everything above (route registration, service/transport construction)
    // is the one-time AR-1 init budget; baseline the heap-growth tracker now.
    arm_heap_growth_tracking();

    bool led_on = false;
    uint32_t last_toggle = to_ms_since_boot(get_absolute_time());
    while (true) {
        const uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_toggle >= HEARTBEAT_INTERVAL_MS) {
            led_on = !led_on;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_on);
            last_toggle = now;
        }
        // Off the async-context path: this is where a deferred handler's work
        // actually happens (AR-9), and what t19 measures against /heavy.
        pump_deferred_work(server, deferred_work);
        sleep_ms(LOOP_POLL_MS);
    }
}
