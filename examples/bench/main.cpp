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
// misc-include-cleaner: pico/stdlib.h supplies board constants and
// tight_loop_contents through headers that cannot stand alone.
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
/// Benchmark firmware with limits and post-init heap tracking (OR-3, AR-1).
/// The endpoint contract is in README.md.

using pico_http::Disposition;
using pico_http::HTTPRequestView;
using pico_http::HTTPResponse;
using pico_http::HttpService;
using pico_http::LwipTcpTransport;
using pico_http::StaticBody;

namespace {

constexpr uint16_t HTTP_PORT = 80;
// Largest /payload/<n> and /stream/<n> size in bytes; it exceeds the TX
// ring capacity (PR-8).
constexpr uint32_t PAYLOAD_MAX_BYTES = 256U * 1024U;
constexpr std::string_view PAYLOAD_PREFIX = "/payload/";
constexpr std::string_view STREAM_PREFIX = "/stream/";
constexpr std::string_view BENCH_STATIC_PREFIX = "/bench/static/";
constexpr std::string_view BENCH_BUFFERED_PREFIX = "/bench/buffered/";
// The largest /bench body, in bytes, matches the P8-RP2350 copied-body cap.
// /bench/buffered/<n> answers 413 above the active profile's cap (BR-14).
constexpr std::size_t BENCH_BODY_MAX = 8192;
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 500;

// The layout budgets apply to 64 B names and 256 B values under either named
// profile (AR-1).
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

// The largest response-formatting buffer a route may put on the stack, in
// bytes. The footprint gate does not measure stack frames. This budget has
// no device measurement. Stack guards default off, so overflow can be silent.
constexpr std::size_t HANDLER_STACK_BUDGET = 1536;
// The main-loop poll period in ms must stay small beside the deferred work,
// or it dominates measured deferral latency.
constexpr uint32_t LOOP_POLL_MS = 1;

// Linker bounds provide the free-heap estimate.
extern "C" {
// bugprone-reserved-identifier, readability-identifier-naming: the SDK linker
// script fixes __StackLimit.
// NOLINTNEXTLINE(bugprone-reserved-identifier,readability-identifier-naming)
extern char __StackLimit;
// bugprone-reserved-identifier, readability-identifier-naming: the SDK linker
// script fixes __bss_end__.
// NOLINTNEXTLINE(bugprone-reserved-identifier,readability-identifier-naming)
extern char __bss_end__;
}

// The heap tracker samples post-init growth (AR-1, pico_util:MR-1).
// Steady-state calls run in the async context; a main-loop caller would need
// cyw43_arch_lwip_begin()/end().
// cppcoreguidelines-avoid-non-const-global-variables: the tracker is armed
// after start() and updated by callbacks.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
pico_util::HeapWatermark g_heap;

uint32_t free_heap_bytes() {
    // clang-analyzer-security.PointerSub: the linker symbols bound one heap
    // region, which the analyser cannot infer.
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
        // misc-include-cleaner: pico/stdlib.h supplies the CYW43 board
        // constant.
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

    // misc-include-cleaner: pico/stdlib.h supplies the CYW43 constant through
    // headers that cannot stand alone. The constant has its own statement.
    // This keeps clang-format from separating this justification from the
    // suppression on the next line.
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

// The alphabet pattern depends only on byte offset, allowing resumption in
// fixed storage (AR-5).
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

    res.set_body_generator(payload_generator, nullptr, requested,
                           "application/octet-stream");
    return Disposition::COMPLETE;
}

// The context pointer carries the length by value and is never dereferenced,
// avoiding per-request storage.
std::size_t stream_generator(void* ctx, std::size_t offset,
                             std::span<std::uint8_t> out) {
    // cppcoreguidelines-pro-type-reinterpret-cast: the opaque pointer carries
    // a size and is never dereferenced.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto total = reinterpret_cast<std::uintptr_t>(ctx);
    if (offset >= total) {
        return 0;
    }
    const std::size_t n = std::min<std::size_t>(out.size(), total - offset);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<uint8_t>('A' + ((offset + i) % 26U));
    }
    return n;
}

// The unknown-length route exercises chunked framing without per-response
// heap (NR-6).
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

    // cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr:
    // the opaque pointer carries the requested size, not an address.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    void* ctx = reinterpret_cast<void*>(static_cast<std::uintptr_t>(requested));
    res.set_body_generator(stream_generator, ctx, "application/octet-stream");
    return Disposition::COMPLETE;
}

// readability-identifier-naming: g_ marks file-scope storage, as on g_heap.
// The table is const but not declared constexpr.
// NOLINTNEXTLINE(readability-identifier-naming)
const std::array<std::uint8_t, BENCH_BODY_MAX> g_bench_body = [] {
    std::array<std::uint8_t, BENCH_BODY_MAX> body{};
    for (std::size_t i = 0; i < body.size(); ++i) {
        body[i] = static_cast<std::uint8_t>('A' + (i % 26U));
    }
    return body;
}();

// Return the positive size after the URI prefix, or zero if invalid or over
// max.
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

// The two body modes use identical bytes to isolate copy cost in t17.
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

// The counters are served as JSON (OR-2).
void handle_stats(const HttpService& server, HTTPResponse& res) {
    const HttpService::Statistics& stats = server.statistics();
    const LwipTcpTransport::LockHoldStats lock =
        LwipTcpTransport::lock_hold_stats();
    const LwipTcpTransport::ReceiveStats receive =
        LwipTcpTransport::receive_stats();
    auto uptime = to_ms_since_boot(get_absolute_time());
    // Widening here avoids a -Wuseless-cast when uint32_t is unsigned long.
    // google-runtime-int: unsigned long matches snprintf %lu.
    // NOLINTNEXTLINE(google-runtime-int)
    const unsigned long free_heap = free_heap_bytes();
    // google-runtime-int: unsigned long matches snprintf %lu.
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
    using Limits = HttpService::Limits;
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

// The response identifies the build and profile (OR-4).
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
        // The driver uses the configured initial RTO for its send-gap
        // expectation (PR-9).
        static_cast<unsigned>(LWIP_TCP_RTO_TIME), ka.enabled ? "true" : "false",
        static_cast<unsigned long>(ka.idle_timeout_seconds),
        static_cast<unsigned long>(ka.max_requests_per_connection),
        static_cast<unsigned>(PAYLOAD_MAX_BYTES));
    res.set_body(std::string_view(buf, static_cast<size_t>(len)),
                 "application/json");
}

// LWIP_STATS supplies heap/pool peaks and allocation failures. pbuf_ref names
// the MEMP_PBUF pool used for PBUF_ROM headers.
void handle_lwip_stats(HTTPResponse& res) {
    const struct stats_mem& mem = lwip_stats.mem;
    const struct stats_mem& pbuf = *lwip_stats.memp[MEMP_PBUF_POOL];
    const struct stats_mem& seg = *lwip_stats.memp[MEMP_TCP_SEG];
    const struct stats_mem& pcb = *lwip_stats.memp[MEMP_TCP_PCB];
    const struct stats_mem& ref = *lwip_stats.memp[MEMP_PBUF];
    // TCP counters record protocol activity separately from pool allocation
    // failures.
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

// The synchronous route burns inside the lwIP callback. The deferred route
// runs the same work in the main loop (AR-9).
constexpr std::string_view HEAVY_PREFIX = "/heavy/";
constexpr std::string_view HEAVY_DEFERRED_PREFIX = "/heavy-deferred/";
constexpr std::uint32_t HEAVY_MAX_MS = 1000;

// The work table matches the deferral cap (BR-12).
constexpr std::size_t MAX_DEFERRED_WORK = HttpService::MAX_PENDING;

struct DeferredWork {
    // The RequestId outlives the discarded response object (AR-9).
    pico_http::RequestId id;
    std::uint32_t started_ms = 0;
    std::uint32_t burn_ms = 0;
    bool held = false;
};

void fill_heavy_deferred(void* /*ctx*/, HTTPResponse& res) {
    res.set_body_static(StaticBody::outliving_response("heavy"), "text/plain");
}

// The callback records work in this main-owned table. pump_deferred_work()
// burns time and completes it.
using DeferredWorkTable = std::array<DeferredWork, MAX_DEFERRED_WORK>;

// Return the requested duration in ms, or zero for the caller to answer 400.
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

// The SDK wait handles timer wrap (AR-7).
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

// The synchronous work runs inside the lwIP callback.
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

// The callback returns before the main loop burns time (AR-9).
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
        // The pending response is discarded; the completion filler supplies
        // its body.
        return Disposition::PENDING;
    }
    // An occupied work table falls back to an inline response instead of
    // another deferral (BR-12).
    burn_cpu(ms);
    res.set_body_static(StaticBody::outliving_response("heavy"), "text/plain");
    return Disposition::COMPLETE;
}

// The async lock protects the table from callback pre-emption. The burn runs
// outside the lock to keep network processing active. id and burn_ms are
// copied before release so replacement cannot misdirect a reply and strand
// the original request until timeout (TR-6).
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

        // A request can expire while work runs. A false result needs no
        // recovery and the filler does not run (AR-9).
        static_cast<void>(server.respond(id, &fill_heavy_deferred, nullptr));
    }
}

// Returns false if a route literal is rejected; boot reports the failure.
bool register_routes(HttpService& server, DeferredWorkTable& deferred_work) {
    bool ok = true;
    ok = server.add_route("GET", "/ping",
                          [](const HTTPRequestView&, HTTPResponse& res) {
                              // The literal needs no copy before the ring
                              // (AR-2).
                              res.set_body_static(
                                  StaticBody::outliving_response("pong"),
                                  "text/plain");
                              return Disposition::COMPLETE;
                          }) &&
         ok;

    ok = server.add_route("GET", "/payload/*", handle_payload) && ok;

    // The paired routes compare synchronous and deferred work (AR-9).
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
                 // Connections open at reset still contribute later
                 // disconnects (RR-4).
                 const std::size_t open = server.active_connections();
                 server.reset_statistics();
                 LwipTcpTransport::reset_lock_hold_stats();
                 LwipTcpTransport::reset_receive_stats();
                 arm_heap_growth_tracking();
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

    // The inline connection buffers make HttpService too large for the main
    // stack.
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

    // Heap tracking begins after service construction and route registration
    // (AR-1).
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
        // The main loop runs deferred work outside the async callback (AR-9).
        pump_deferred_work(server, deferred_work);
        sleep_ms(LOOP_POLL_MS);
    }
}
