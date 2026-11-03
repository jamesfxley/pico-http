// SPDX-License-Identifier: MIT
#include <lwip/opt.h>
#include <lwipopts.h>
#include <pico/stdio.h>

#include <pico_family/device_test.hpp>
#include <string_view>

#if LWIP_TIMERS && LWIP_NETIF_LOOPBACK && LWIP_HAVE_LOOPIF && \
    !LWIP_SINGLE_NETIF && !LWIP_NETIF_LOOPBACK_MULTITHREADING && NO_SYS
#include <hardware/sync.h>
#include <hardware/timer.h>
#include <lwip/err.h>
#include <lwip/ip_addr.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/tcp.h>
#include <lwip/tcpbase.h>
#include <pico/async_context.h>
#include <pico/cyw43_arch.h>
#include <pico/time.h>
#include <pico/types.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <pico_http/config/sizing.hpp>
#include <pico_http/http/http_service.hpp>
#include <pico_http/http/message.hpp>
#include <pico_http/pico_http.hpp>
#include <ranges>
#include <span>
#include <system_error>

namespace {

using pico_http::Disposition;
using pico_http::HTTPRequestView;
using pico_http::HTTPResponse;
using pico_http::HttpService;
using pico_http::RequestId;
using namespace std::literals;

constexpr std::uint16_t PORT = 8123;
constexpr std::size_t BODY_SIZE = pico_http::TX_RING_CAPACITY * 3;
constexpr std::size_t PEERS = HttpService::MAX_CONNECTIONS + 1;
constexpr std::int64_t IRQ_PERIOD_US = 10;
constexpr std::uint32_t CASE_MS = 4000;
constexpr std::uint32_t TOTAL_MS = 18500;
// Release RP2350 disassembly: g<=64 outside, s>=9 enabled instructions
// inside; (64/73)^256 < 1e-9. Eligibility ends before any network wait.
// Re-derive these bounds when the eligible interval or generated code changes.
constexpr std::size_t REQUIRED_LANDINGS = 256;
constexpr std::size_t MAX_RACES = 65536;
constexpr auto SMALL_REQUEST = "GET /small HTTP/1.1\r\nHost: loop\r\n\r\n"sv;
constexpr auto HOLD_REQUEST = "GET /hold HTTP/1.1\r\nHost: loop\r\n\r\n"sv;
constexpr auto ITEM_REQUEST = "GET /item/0000 HTTP/1.1\r\nHost: loop\r\n\r\n"sv;
constexpr auto ITEM_BODY = "/item/0000"sv;
constexpr std::size_t ITEM_NUMBER_OFFSET = "GET /item/"sv.size();
constexpr std::size_t PIPELINE_REQUESTS = (TCP_WND / ITEM_REQUEST.size()) + 8;
constexpr std::size_t PIPELINE_SIZE = PIPELINE_REQUESTS * ITEM_REQUEST.size();
// Includes status, framing, content type and keep-alive fields for each small
// reply. An enlarged header must fit this test's explicit capture budget.
constexpr std::size_t SMALL_RESPONSE_BUDGET = 256;
constexpr std::size_t CAPTURE_SIZE =
    std::max(BODY_SIZE + 512, (PIPELINE_REQUESTS + 1) * SMALL_RESPONSE_BUDGET);
static_assert(MEMP_NUM_TCP_PCB >= 2 * HttpService::MAX_CONNECTIONS + 2);
static_assert(PIPELINE_SIZE > TCP_WND);
static_assert(PIPELINE_REQUESTS < 10000);
static_assert(BODY_SIZE > pico_http::TX_RING_CAPACITY);
static_assert(std::atomic<bool>::is_always_lock_free);

class LwipLock {
   public:
    LwipLock() { cyw43_arch_lwip_begin(); }

    ~LwipLock() { cyw43_arch_lwip_end(); }

    LwipLock(const LwipLock&) = delete;
    LwipLock& operator=(const LwipLock&) = delete;
    LwipLock(LwipLock&&) = delete;
    LwipLock& operator=(LwipLock&&) = delete;
};

struct Peer {
    tcp_pcb* pcb = nullptr;
    bool connected = false;
    bool eof = false;
    bool overflow = false;
    bool hold_credit = false;
    bool large = false;
    err_t error = ERR_OK;
    std::size_t received = 0;
    std::uint16_t held = 0;
    std::array<char, 512> bytes{};
};

struct State {
    std::unique_ptr<HttpService> server;
    std::array<Peer, PEERS> peers{};
    std::array<char, CAPTURE_SIZE> capture{};
    std::array<char, PIPELINE_SIZE> pipeline{};
    RequestId parked{};
    bool pending = false;
    std::size_t fills = 0;
    absolute_time_t total_deadline{};
    // Release/acquire publishes the request id to the timer and its result
    // back to mainline. The bracket is separate from the lwIP/IRQ locks.
    std::atomic<bool> armed{false};
    std::atomic<bool> in_progress{false};
    std::atomic<bool> irq_done{false};
    std::atomic<bool> irq_won{false};
    std::atomic<bool> landed{false};
    repeating_timer_t timer{};
    std::size_t races = 0;
    std::size_t landings = 0;
};

State& state() {
    static State instance;
    return instance;
}

std::span<char> storage(Peer& peer) {
    return peer.large ? std::span<char>(state().capture)
                      : std::span<char>(peer.bytes);
}

err_t receive(void* context, tcp_pcb* pcb, pbuf* packet, err_t error) {
    auto& peer = *static_cast<Peer*>(context);
    if (packet == nullptr) {
        peer.eof = true;
        if (tcp_close(pcb) == ERR_OK) {
            peer.pcb = nullptr;
        }
        return ERR_OK;
    }
    peer.error = error;
    auto bytes = storage(peer);
    const auto count = static_cast<std::size_t>(packet->tot_len);
    if (count > bytes.size() - peer.received) {
        peer.overflow = true;
    } else {
        static_cast<void>(pbuf_copy_partial(
            packet, bytes.subspan(peer.received).data(), packet->tot_len, 0));
        peer.received += count;
    }
    if (peer.hold_credit) {
        peer.held = static_cast<std::uint16_t>(peer.held + packet->tot_len);
    } else {
        tcp_recved(pcb, packet->tot_len);
    }
    pbuf_free(packet);
    return ERR_OK;
}

void failed(void* context, err_t error) {
    auto& peer = *static_cast<Peer*>(context);
    peer.pcb = nullptr;
    peer.error = error;
}

err_t connected(void* context, tcp_pcb* /*pcb*/, err_t error) {
    auto& peer = *static_cast<Peer*>(context);
    peer.connected = error == ERR_OK;
    peer.error = error;
    return ERR_OK;
}

bool open_peer(Peer& peer, bool large = false) {
    const LwipLock lock;
    peer = Peer{};
    peer.large = large;
    peer.pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (peer.pcb == nullptr) {
        return false;
    }
    tcp_arg(peer.pcb, &peer);
    tcp_recv(peer.pcb, receive);
    tcp_err(peer.pcb, failed);
    tcp_nagle_disable(peer.pcb);
    ip_addr_t address{};
    IP_ADDR4(&address, 127, 0, 0, 1);
    return tcp_connect(peer.pcb, &address, PORT, connected) == ERR_OK;
}

void abort_peer(Peer& peer) {
    const LwipLock lock;
    if (peer.pcb != nullptr) {
        tcp_arg(peer.pcb, nullptr);
        tcp_recv(peer.pcb, nullptr);
        tcp_err(peer.pcb, nullptr);
        tcp_abort(peer.pcb);
        peer.pcb = nullptr;
    }
}

// Poll loopback outside any TCP callback; the background architecture remains
// NO_SYS and does not service netif's loopback queue itself (AR-5, AR-6).
void poll_loopback() {
    const LwipLock lock;
    netif_poll_all();
}

template <typename Predicate>
bool wait_for(Predicate ready) {
    const auto deadline = make_timeout_time_ms(CASE_MS);
    while (!time_reached(deadline) && !time_reached(state().total_deadline)) {
        poll_loopback();
        {
            const LwipLock lock;
            if (ready()) {
                return true;
            }
        }
        sleep_us(50);
    }
    return false;
}

bool await_connection(Peer& peer) {
    bool accepted = false;
    const bool completed = wait_for([&] {
        accepted = peer.connected && peer.error == ERR_OK;
        return accepted || peer.error != ERR_OK;
    });
    return completed && accepted;
}

bool send_bytes(Peer& peer, std::string_view bytes) {
    std::size_t offset = 0;
    return wait_for([&] {
        if (peer.pcb == nullptr || peer.error != ERR_OK) {
            return false;
        }
        const auto count =
            std::min<std::size_t>(bytes.size() - offset, tcp_sndbuf(peer.pcb));
        if (count != 0 && tcp_write(peer.pcb, bytes.substr(offset).data(),
                                    static_cast<std::uint16_t>(count),
                                    TCP_WRITE_FLAG_COPY) == ERR_OK) {
            offset += count;
            static_cast<void>(tcp_output(peer.pcb));
        }
        return offset == bytes.size();
    });
}

std::size_t generate(void* /*context*/, std::size_t offset,
                     std::span<std::uint8_t> out) {
    const auto count = std::min(out.size(), BODY_SIZE - offset);
    for (std::size_t index = 0; index < count; ++index) {
        out[index] = static_cast<std::uint8_t>('a' + ((offset + index) % 26));
    }
    return count;
}

void fill_ok(void* /*context*/, HTTPResponse& response) {
    static_cast<void>(response.set_body_static(
        pico_http::StaticBody::outliving_response("ok"sv)));
}

void fill_a(void* /*context*/, HTTPResponse& response) {
    async_context_lock_check(cyw43_arch_async_context());
    ++state().fills;
    static_cast<void>(response.set_body_static(
        pico_http::StaticBody::outliving_response("A"sv)));
}

void fill_b(void* /*context*/, HTTPResponse& response) {
    async_context_lock_check(cyw43_arch_async_context());
    ++state().fills;
    static_cast<void>(response.set_body_static(
        pico_http::StaticBody::outliving_response("B"sv)));
}

Disposition park(const HTTPRequestView& request, HTTPResponse& /*response*/) {
    state().parked = request.request;
    state().pending = true;
    return Disposition::PENDING;
}

// Four decimal digits make every pipelined response distinguishable (AR-6).
void write_index(std::span<char> digits, std::size_t value) {
    for (auto& digit : digits | std::views::reverse) {
        digit = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
}

bool take_response(std::string_view& remaining, std::string_view body) {
    const auto head_end = remaining.find("\r\n\r\n");
    if (!remaining.starts_with("HTTP/1.1 200 OK\r\n") ||
        head_end == std::string_view::npos) {
        return false;
    }
    const auto head = remaining.substr(0, head_end + 2);
    std::string_view length = "Content-Length: 10\r\n";
    if (body.size() == 1) {
        length = "Content-Length: 1\r\n";
    } else if (body.size() == 2) {
        length = "Content-Length: 2\r\n";
    } else if (body.size() != ITEM_BODY.size()) {
        return false;
    }
    if (head.find(length) == std::string_view::npos) {
        return false;
    }
    remaining.remove_prefix(head_end + 4);
    if (!remaining.starts_with(body)) {
        return false;
    }
    remaining.remove_prefix(body.size());
    return true;
}

// Check every byte and response boundary, including absence of a second reply.
bool small_responses(Peer& peer, std::size_t count,
                     std::string_view body = "ok") {
    if (peer.error != ERR_OK || peer.overflow) {
        return false;
    }
    auto bytes = storage(peer);
    std::string_view remaining(bytes.data(), peer.received);
    for (std::size_t index = 0; index < count; ++index) {
        if (!take_response(remaining, body)) {
            return false;
        }
    }
    return remaining.empty();
}

bool pipeline_responses(Peer& peer) {
    if (peer.error != ERR_OK || peer.overflow) {
        return false;
    }
    std::string_view remaining(state().capture.data(), peer.received);
    if (!take_response(remaining, "ok")) {
        return false;
    }
    std::array<char, ITEM_BODY.size()> expected{};
    std::ranges::copy(ITEM_BODY, expected.begin());
    for (std::size_t index = 0; index < PIPELINE_REQUESTS; ++index) {
        write_index(std::span<char>(expected).last(4), index);
        if (!take_response(remaining, {expected.data(), expected.size()})) {
            return false;
        }
    }
    return remaining.empty();
}

void clear_peers() {
    for (auto& peer : state().peers) {
        abort_peer(peer);
    }
    static_cast<void>(
        wait_for([] { return state().server->active_connections() == 0; }));
}

// AR-5: hold actual TCP receive credit, fill the server's ring, then resume.
bool ack_resumes() {
    auto& peer = state().peers.front();
    if (!open_peer(peer, true) || !await_connection(peer)) {
        return false;
    }
    {
        const LwipLock lock;
        peer.hold_credit = true;
    }
    if (!send_bytes(
            peer,
            "GET /big HTTP/1.1\r\nHost: loop\r\nConnection: close\r\n\r\n") ||
        !wait_for([&peer] {
            return peer.held >= TCP_WND &&
                   state().server->statistics().backpressure_events > 0;
        })) {
        return false;
    }
    {
        const LwipLock lock;
        if (peer.pcb == nullptr) {
            return false;
        }
        peer.hold_credit = false;
        tcp_recved(peer.pcb, peer.held);
        peer.held = 0;
        static_cast<void>(tcp_output(peer.pcb));
    }
    if (!wait_for([&peer] { return peer.eof; })) {
        return false;
    }
    const LwipLock lock;
    const std::string_view response(state().capture.data(), peer.received);
    const auto end = response.find("\r\n\r\n");
    if (peer.error != ERR_OK || peer.overflow ||
        !response.starts_with("HTTP/1.1 200 OK\r\n") ||
        end == std::string_view::npos ||
        response.size() - end - 4 != BODY_SIZE) {
        return false;
    }
    const auto body = response.substr(end + 4);
    for (std::size_t index = 0; index < body.size(); ++index) {
        if (body[index] != static_cast<char>('a' + (index % 26))) {
            return false;
        }
    }
    return true;
}

// AR-6: a pending first request stops parser consumption of the pipeline.
// The known pipeline exceeds TCP_WND; every subsequent response must survive.
bool window_reopens() {
    auto& peer = state().peers.front();
    {
        const LwipLock lock;
        state().pending = false;
        state().server->reset_statistics();
    }
    if (!open_peer(peer, true) || !await_connection(peer) ||
        !send_bytes(peer, HOLD_REQUEST) ||
        !wait_for([] { return state().pending; })) {
        return false;
    }
    // Queue at least one window without trying to synchronously queue the
    // whole pipeline: that would deadlock behind the intentionally held window.
    std::size_t sent = 0;
    if (!wait_for([&] {
            if (peer.pcb == nullptr) {
                return false;
            }
            const auto count = std::min<std::size_t>(PIPELINE_SIZE - sent,
                                                     tcp_sndbuf(peer.pcb));
            if (count != 0 &&
                tcp_write(peer.pcb, state().pipeline.data() + sent,
                          static_cast<std::uint16_t>(count),
                          TCP_WRITE_FLAG_COPY) == ERR_OK) {
                sent += count;
                static_cast<void>(tcp_output(peer.pcb));
            }
            return state().server->statistics().rx_window_throttle_events > 0 &&
                   peer.pcb->snd_wnd == 0;
        })) {
        return false;
    }
    if (!state().server->respond(state().parked, fill_ok, nullptr) ||
        !send_bytes(peer, std::string_view(state().pipeline.data() + sent,
                                           PIPELINE_SIZE - sent)) ||
        !wait_for([&peer] { return pipeline_responses(peer); })) {
        return false;
    }
    const LwipLock lock;
    return state().server->statistics().rx_window_throttle_events > 0 &&
           state().server->statistics().rx_overflow_drops == 0;
}

// RR-3: every original connection must still serve after the actual RST.
bool refuses_extra() {
    {
        const LwipLock lock;
        state().server->reset_statistics();
    }
    for (std::size_t index = 0; index < HttpService::MAX_CONNECTIONS; ++index) {
        auto& peer = state().peers[index];
        if (!open_peer(peer) || !await_connection(peer)) {
            return false;
        }
    }
    auto& extra = state().peers.back();
    if (!open_peer(extra) ||
        !wait_for([&extra] { return extra.error != ERR_OK; })) {
        return false;
    }
    {
        const LwipLock lock;
        if (extra.error != ERR_RST || extra.received != 0 ||
            state().server->statistics().refusals_slot_full != 1 ||
            state().server->active_connections() !=
                HttpService::MAX_CONNECTIONS) {
            return false;
        }
    }
    for (std::size_t index = 0; index < HttpService::MAX_CONNECTIONS; ++index) {
        auto& peer = state().peers[index];
        if (!send_bytes(peer, SMALL_REQUEST) ||
            !wait_for([&peer] { return small_responses(peer, 1); })) {
            return false;
        }
    }
    return true;
}

bool timer_complete(repeating_timer_t* /*timer*/) {
    auto& shared = state();
    if (shared.armed.exchange(false, std::memory_order_acq_rel)) {
        shared.landed.store(shared.in_progress.load(std::memory_order_acquire),
                            std::memory_order_release);
        shared.irq_won.store(
            shared.server->respond(shared.parked, fill_b, nullptr),
            std::memory_order_release);
        shared.irq_done.store(true, std::memory_order_release);
    }
    return true;
}

// Keep the bracket visible in the linked disassembly (AR-9). It encloses only
// the actual competing call, never the network wait for a parked request.
[[gnu::noinline]] bool mainline_complete(State& shared) {
    shared.in_progress.store(true, std::memory_order_release);
    const bool won = shared.server->respond(shared.parked, fill_a, nullptr);
    shared.in_progress.store(false, std::memory_order_release);
    return won;
}

// Close eligibility before waiting. Mask IRQs while retiring the arm so the
// exclusive-store retry loop cannot add observable instructions after the
// bracket. Only the timer that claims its exchange may publish filler B.
[[gnu::noinline]] bool race_publishers(State& shared, bool& main_won) {
    shared.armed.store(true, std::memory_order_release);
    main_won = mainline_complete(shared);
    const auto saved = save_and_disable_interrupts();
    const bool main_claimed =
        shared.armed.exchange(false, std::memory_order_acq_rel);
    restore_interrupts(saved);
    return !main_claimed;
}

bool contend_once(Peer& peer, bool& landed) {
    {
        const LwipLock lock;
        peer.received = 0;
        state().pending = false;
        state().fills = 0;
    }
    if (!send_bytes(peer, HOLD_REQUEST) ||
        !wait_for([] { return state().pending; })) {
        return false;
    }
    auto& shared = state();
    shared.irq_done.store(false, std::memory_order_release);
    shared.irq_won.store(false, std::memory_order_release);
    shared.landed.store(false, std::memory_order_release);
    bool main_won = false;
    const bool timer_attempted = race_publishers(shared, main_won);
    if (timer_attempted && !wait_for([&shared] {
            return shared.irq_done.load(std::memory_order_acquire);
        })) {
        return false;
    }
    const bool irq_won =
        timer_attempted && shared.irq_won.load(std::memory_order_acquire);
    landed = timer_attempted && shared.landed.load(std::memory_order_acquire);
    if (main_won == irq_won) {
        return false;
    }
    return wait_for([&peer, main_won] {
        return state().fills == 1 &&
               small_responses(peer, 1, main_won ? "A"sv : "B"sv);
    });
}

// AR-9: reject an unanswered request after teardown and after slot reuse.
bool stale_after_teardown(Peer& peer) {
    auto& shared = state();
    // AR-9: this new request has never had a completion published. Its id
    // must become inert because of teardown, independently of duplicate guards.
    {
        const LwipLock lock;
        peer.received = 0;
        shared.pending = false;
        shared.fills = 0;
    }
    if (!send_bytes(peer, HOLD_REQUEST) ||
        !wait_for([] { return state().pending; })) {
        return false;
    }
    const auto stale = shared.parked;
    {
        const LwipLock lock;
        if (peer.received != 0 || shared.fills != 0) {
            return false;
        }
    }
    abort_peer(peer);
    if (!wait_for([] { return state().server->active_connections() == 0; }) ||
        shared.server->respond(stale, fill_b, nullptr)) {
        return false;
    }
    poll_loopback();
    {
        const LwipLock lock;
        if (shared.fills != 0) {
            return false;
        }
    }
    if (!open_peer(peer) || !await_connection(peer) ||
        shared.server->respond(stale, fill_b, nullptr)) {
        return false;
    }
    poll_loopback();
    {
        const LwipLock lock;
        if (peer.received != 0 || shared.fills != 0) {
            return false;
        }
        shared.pending = false;
    }
    if (!send_bytes(peer, HOLD_REQUEST) ||
        !wait_for([] { return state().pending; })) {
        return false;
    }
    const auto replacement = shared.parked;
    if (replacement.connection.slot() != stale.connection.slot() ||
        replacement.connection.generation() == stale.connection.generation() ||
        shared.server->respond(stale, fill_b, nullptr)) {
        return false;
    }
    poll_loopback();
    {
        const LwipLock lock;
        if (peer.received != 0 || shared.fills != 0) {
            return false;
        }
    }
    if (!shared.server->respond(replacement, fill_ok, nullptr) ||
        !wait_for([&peer] { return small_responses(peer, 1); })) {
        return false;
    }
    poll_loopback();
    const LwipLock lock;
    return shared.fills == 0 && small_responses(peer, 1);
}

// AR-9: duplicate publishers race, but only the async context fills/sends.
bool deferred_contention() {
    auto& shared = state();
    auto& peer = shared.peers.front();
    if (!open_peer(peer) || !await_connection(peer) ||
        !add_repeating_timer_us(-IRQ_PERIOD_US, timer_complete, nullptr,
                                &shared.timer)) {
        return false;
    }
    std::size_t landings = 0;
    bool intact = true;
    for (std::size_t attempt = 0;
         attempt < MAX_RACES && landings < REQUIRED_LANDINGS && intact &&
         !time_reached(shared.total_deadline);
         ++attempt) {
        bool landed = false;
        intact = contend_once(peer, landed);
        landings += landed ? 1 : 0;
        shared.races = attempt + 1;
        shared.landings = landings;
    }
    shared.armed.store(false, std::memory_order_release);
    static_cast<void>(cancel_repeating_timer(&shared.timer));
    if (!intact || landings < REQUIRED_LANDINGS) {
        return false;
    }
    return stale_after_teardown(peer);
}

std::string_view contention_detail() {
    static std::array<char, 96> text{};
    auto out = std::span<char>(text);
    const auto append = [&out](std::string_view label, std::size_t value) {
        if (label.size() >= out.size()) {
            return false;
        }
        std::ranges::copy(label, out.begin());
        out = out.subspan(label.size());
        const auto result =
            std::to_chars(out.data(), std::to_address(out.end()), value);
        if (result.ec != std::errc{}) {
            return false;
        }
        out = out.subspan(static_cast<std::size_t>(result.ptr - out.data()));
        return true;
    };
    if (!append("attempts=", state().races) ||
        !append(" landings=", state().landings) ||
        !append(" required=", REQUIRED_LANDINGS)) {
        return "cannot format contention counters";
    }
    return {text.data(), text.size() - out.size()};
}

bool setup() {
    if (cyw43_arch_init() != 0) {
        return false;
    }
    auto& shared = state();
    shared.server = pico_http::make_http_service();
    HttpService::KeepAliveConfig keep_alive;
    keep_alive.max_requests_per_connection =
        static_cast<std::uint32_t>(MAX_RACES + 2);
    shared.server->set_keep_alive_config(keep_alive);
    for (std::size_t index = 0; index < PIPELINE_REQUESTS; ++index) {
        auto request =
            std::span<char>(shared.pipeline)
                .subspan(index * ITEM_REQUEST.size(), ITEM_REQUEST.size());
        std::ranges::copy(ITEM_REQUEST, request.begin());
        write_index(request.subspan(ITEM_NUMBER_OFFSET, 4), index);
    }
    return shared.server->add_route(
               "GET", "/small",
               [](const HTTPRequestView&, HTTPResponse& response) {
                   fill_ok(nullptr, response);
                   return Disposition::COMPLETE;
               }) &&
           shared.server->add_route(
               "GET", "/big",
               [](const HTTPRequestView&, HTTPResponse& response) {
                   static_cast<void>(response.set_body_generator(
                       generate, nullptr, BODY_SIZE));
                   return Disposition::COMPLETE;
               }) &&
           shared.server->add_route(
               "GET", "/item/*",
               [](const HTTPRequestView& request, HTTPResponse& response) {
                   static_cast<void>(response.set_body(request.uri));
                   return Disposition::COMPLETE;
               }) &&
           shared.server->add_route("GET", "/hold", park) &&
           shared.server->start(PORT);
}

}  // namespace
#endif

int main() {
    stdio_init_all();
    pico_family::device_test::Run run;
#if LWIP_TIMERS && LWIP_NETIF_LOOPBACK && LWIP_HAVE_LOOPIF && \
    !LWIP_SINGLE_NETIF && !LWIP_NETIF_LOOPBACK_MULTITHREADING && NO_SYS
    state().total_deadline = make_timeout_time_ms(TOTAL_MS);
    const bool initialized = setup();
    run.check("AR-5", "tcp-ack-resumes-complete-response",
              initialized && ack_resumes());
    if (initialized) {
        clear_peers();
    }
    run.check("AR-6", "tcp-window-reopens-after-consumption",
              initialized && window_reopens());
    if (initialized) {
        clear_peers();
    }
    run.check("RR-3", "tcp-refuses-one-past-slot-cap",
              initialized && refuses_extra());
    if (initialized) {
        clear_peers();
    }
    const bool completion_passed = initialized && deferred_contention();
    run.check("AR-9", "irq-deferred-completion-runs-on-async-context",
              completion_passed,
              completion_passed ? std::string_view{} : contention_detail());
#else
    constexpr std::string_view DETAIL =
        "requires PICO_LWIPOPTS_TARGET=pico_http_device_lwipopts";
    run.check("AR-5", "tcp-ack-resumes-complete-response", false, DETAIL);
    run.check("AR-6", "tcp-window-reopens-after-consumption", false, DETAIL);
    run.check("RR-3", "tcp-refuses-one-past-slot-cap", false, DETAIL);
    run.check("AR-9", "irq-deferred-completion-runs-on-async-context", false,
              DETAIL);
#endif
    run.finish();
}
