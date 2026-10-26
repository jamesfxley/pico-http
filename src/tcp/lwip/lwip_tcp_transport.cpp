// SPDX-License-Identifier: MIT
#include "pico_http/tcp/lwip_tcp_transport.hpp"

#include <hardware/timer.h>
#include <lwip/err.h>
#include <lwip/ip_addr.h>
#include <lwip/opt.h>
#include <lwip/pbuf.h>
#include <lwip/tcp.h>
#include <pico/async_context.h>
#include <pico/critical_section.h>
#include <pico/cyw43_arch.h>
#include <pico/time.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "lwipopts.h"
#include "pico_http/config/sizing.hpp"
#include "pico_http/tcp/tcp_transport.hpp"

// AR-10: what this library requires of the application's lwipopts.h, in the one
// translation unit that includes it. It names nothing this file uses: its
// static_asserts are the reason to include it.
#include "pico_http/lwipopts_assertions.hpp"  // IWYU pragma: keep

/// \file lwip_tcp_transport.cpp
/// lwIP raw-API implementation of ITcpTransport. See the header for the
/// design rationale; the load-bearing detail is that tcp_arg() points at the
/// per-connection Slot, so every callback — including tcp_err(), where lwIP
/// has already freed the pcb — reaches connection state in O(1) and tears it
/// down through one shared path.

namespace pico_http {

namespace {
// Widest window reopen a single tcp_recved() call can express: its len
// argument is a uint16_t.
constexpr std::size_t MAX_RECVED_CHUNK = 0xFFFF;
}  // namespace

// Bind the host-safe mirrors in the config module to the real lwipopts values,
// so a change to TCP_MSS/TCP_WND that is not reflected in the shared constants
// (which the HTTP service sizes its RX buffer from) fails the build here rather
// than silently under-sizing the assembly buffer (AR-6).
static_assert(TCP_MSS_BYTES == static_cast<std::size_t>(TCP_MSS),
              "TCP_MSS_BYTES must match lwipopts TCP_MSS");
static_assert(ADVERTISED_RX_WINDOW == static_cast<std::size_t>(TCP_WND),
              "ADVERTISED_RX_WINDOW must match lwipopts TCP_WND");
static_assert(TX_RING_CAPACITY <= static_cast<std::size_t>(TCP_SND_BUF),
              "TX ring must fit within the lwIP send buffer (TCP_SND_BUF)");
// consume() reopens the window with one uint16_t tcp_recved() call; a single
// consume can cover at most one full RX assembly buffer (== the advertised
// window), so the window must fit in that argument or reopening would clamp.
static_assert(ADVERTISED_RX_WINDOW <= MAX_RECVED_CHUNK,
              "advertised window must fit a single uint16_t tcp_recved()");

namespace {

constexpr std::uint8_t POLL_INTERVAL_TICKS = 4;  // lwIP poll ticks (~2 s)

// AR-3 lock-hold sampling. Plain globals (not atomics): every lwIP lock span
// runs in the same serialized cyw43 async-context, so there is no concurrent
// writer to race with.
#ifndef NDEBUG
constexpr std::uint32_t LOCK_HOLD_BUDGET_US = 500;  // spec AR-3 p99 target

struct LockHoldSampler {
    std::uint32_t samples = 0;
    std::uint32_t within_budget = 0;
    std::uint32_t max_us = 0;
};

// The sampler accumulates across every lock span, so it cannot be const; see
// above for why no synchronisation is needed.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
LockHoldSampler g_lock_hold{};

void record_lock_hold(std::uint32_t held_us) {
    ++g_lock_hold.samples;
    if (held_us <= LOCK_HOLD_BUDGET_US) {
        ++g_lock_hold.within_budget;
    }
    g_lock_hold.max_us = std::max(held_us, g_lock_hold.max_us);
}
#endif

// PR-2 receive-callback sampling, in every build type. A function-local static
// rather than a namespace variable, so the state needs no suppression; like the
// lock-hold sample, it is written only from the serialized async context.
constexpr std::uint32_t RECEIVE_BUDGET_US = 1000;  // spec PR-2 internal share

LwipTcpTransport::ReceiveStats& receive_sample() noexcept {
    static LwipTcpTransport::ReceiveStats sample{};
    return sample;
}

void record_receive(std::uint32_t processing_us,
                    std::uint32_t total_us) noexcept {
    LwipTcpTransport::ReceiveStats& sample = receive_sample();
    ++sample.samples;
    if (processing_us <= RECEIVE_BUDGET_US) {
        ++sample.within_1ms;
    }
    if (total_us <= RECEIVE_BUDGET_US) {
        ++sample.total_within_1ms;
    }
    sample.max_us = std::max(processing_us, sample.max_us);
    sample.total_max_us = std::max(total_us, sample.total_max_us);
}

class LwipLock {
   public:
#ifndef NDEBUG
    LwipLock() {
        cyw43_arch_lwip_begin();
        // Not a member initializer: those run before the constructor body, and
        // the hold time is measured from after the lock is acquired, not from
        // before the (possibly blocking) acquisition.
        // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
        start_us_ = time_us_32();
    }

    ~LwipLock() {
        const std::uint32_t held_us = time_us_32() - start_us_;
        cyw43_arch_lwip_end();
        record_lock_hold(held_us);
    }
#else
    LwipLock() { cyw43_arch_lwip_begin(); }

    ~LwipLock() { cyw43_arch_lwip_end(); }
#endif

    LwipLock(const LwipLock&) = delete;
    LwipLock& operator=(const LwipLock&) = delete;
    LwipLock(LwipLock&&) = delete;
    LwipLock& operator=(LwipLock&&) = delete;

#ifndef NDEBUG
   private:
    std::uint32_t start_us_ = 0;
#endif
};

// Marks a tracking slot with the pcb whose callback is executing for the
// callback's duration, so teardown can distinguish an in-callback self-abort
// (which must return ERR_ABRT) from one issued outside any callback. Distinct
// from TransportBase::CallbackScope, which defers disconnect delivery; every
// lwIP callback opens both.
class PcbScope {
   public:
    PcbScope(tcp_pcb*& active, tcp_pcb* pcb) : active_(active) {
        active_ = pcb;
    }

    ~PcbScope() { active_ = nullptr; }

    PcbScope(const PcbScope&) = delete;
    PcbScope& operator=(const PcbScope&) = delete;
    PcbScope(PcbScope&&) = delete;
    PcbScope& operator=(PcbScope&&) = delete;

   private:
    tcp_pcb*& active_;
};

}  // namespace

LwipTcpTransport::LockHoldStats LwipTcpTransport::lock_hold_stats() noexcept {
#ifndef NDEBUG
    return {.samples = g_lock_hold.samples,
            .within_500us = g_lock_hold.within_budget,
            .max_us = g_lock_hold.max_us};
#else
    // No sampling in release builds.
    return {.samples = 0, .within_500us = 0, .max_us = 0};
#endif
}

void LwipTcpTransport::reset_lock_hold_stats() noexcept {
#ifndef NDEBUG
    g_lock_hold = LockHoldSampler{};
#endif
}

LwipTcpTransport::ReceiveStats LwipTcpTransport::receive_stats() noexcept {
    return receive_sample();
}

void LwipTcpTransport::reset_receive_stats() noexcept {
    receive_sample() = ReceiveStats{};
}

// The completion lock's lifetime is the object's, not the server's: stop() on
// the HTTP side closes the server and then takes this lock once per slot, and a
// service destroyed after a failed listen() takes it having never listened.
LwipTcpTransport::LwipTcpTransport() {
    critical_section_init(&completion_lock_);
}

// Qualified so the call cannot be read as dispatching to an override: during
// ~LwipTcpTransport the dynamic type is already LwipTcpTransport, so a derived
// close_server() would never run here.
LwipTcpTransport::~LwipTcpTransport() {
    LwipTcpTransport::close_server();
    critical_section_deinit(&completion_lock_);
}

std::uint32_t LwipTcpTransport::now_ms() {
    return to_ms_since_boot(get_absolute_time());
}

err_t LwipTcpTransport::consume_abort(tcp_pcb* pcb) noexcept {
    if (aborted_pcb_ != nullptr && aborted_pcb_ == pcb) {
        aborted_pcb_ = nullptr;
        return ERR_ABRT;  // lwIP contract: report the in-callback abort
    }
    // A teardown that aborted a pcb outside any callback (e.g. a direct
    // close_client) may have left aborted_pcb_ set; drop it so it cannot alias
    // a future pcb reused at the same address and trigger a spurious ERR_ABRT.
    aborted_pcb_ = nullptr;
    return ERR_OK;
}

bool LwipTcpTransport::listen(std::uint16_t port) {
    const LwipLock lock;
    tcp_pcb* pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb == nullptr) {
        return false;
    }
    if (tcp_bind(pcb, IP_ANY_TYPE, port) != ERR_OK) {
        tcp_close(pcb);
        return false;
    }
    tcp_pcb* listen_pcb =
        tcp_listen_with_backlog(pcb, TCP_DEFAULT_LISTEN_BACKLOG);
    if (listen_pcb == nullptr) {
        tcp_close(pcb);
        return false;
    }
    server_pcb_ = listen_pcb;
    tcp_arg(server_pcb_, this);
    tcp_accept(server_pcb_, accept_callback);

    // Start the periodic timeout scan on the cyw43 async context (the same
    // lwIP-safe context every callback runs in), so TR-1..5 deadlines fire at
    // ~100 ms granularity (TR-1..TR-5).
    timeout_worker_.do_work = timeout_worker_entry;
    timeout_worker_.user_data = this;
    async_context_add_at_time_worker_in_ms(
        cyw43_arch_async_context(), &timeout_worker_, TIMEOUT_SCAN_INTERVAL_MS);

    // Deferred completions (AR-9). A "when pending" worker rather than a timed
    // one: respond() rings it, so the completion is serviced on the next turn
    // of the async context rather than after up to one scan interval. The
    // critical section guarding the record is the constructor's, not this
    // function's.
    completion_worker_.do_work = completion_worker_entry;
    completion_worker_.user_data = this;
    async_context_add_when_pending_worker(cyw43_arch_async_context(),
                                          &completion_worker_);
    return true;
}

void LwipTcpTransport::close_server() {
    const LwipLock lock;
    close_all_clients();
    if (server_pcb_ != nullptr) {
        tcp_arg(server_pcb_, nullptr);
        tcp_accept(server_pcb_, nullptr);
        tcp_close(server_pcb_);
        server_pcb_ = nullptr;
        async_context_remove_at_time_worker(cyw43_arch_async_context(),
                                            &timeout_worker_);
        async_context_remove_when_pending_worker(cyw43_arch_async_context(),
                                                 &completion_worker_);
    }
}

void LwipTcpTransport::close_client(ConnectionId client) {
    const LwipLock lock;
    close_client_impl(client);
}

std::size_t LwipTcpTransport::send(ConnectionId client,
                                   std::span<const std::uint8_t> data) {
    if (timing_receive_ && !receive_sent_) {
        first_send_us_ = time_us_32();  // PR-2: processing ends here
        receive_sent_ = true;
    }
    const LwipLock lock;
    Slot* slot = clients_.get(client);
    if (slot == nullptr || slot->close_pending || slot->pcb == nullptr) {
        return 0;
    }
    const std::size_t accepted = slot->tx.write(data);
    flush_tx(*slot);
    return accepted;
}

std::size_t LwipTcpTransport::send_credit(ConnectionId client) const {
    const LwipLock lock;
    const Slot* slot = clients_.get(client);
    if (slot == nullptr || slot->close_pending || slot->pcb == nullptr) {
        return 0;  // no capacity: a closed/invalid connection accepts nothing
    }
    return slot->tx.free_space();
}

void LwipTcpTransport::consume(ConnectionId client, std::size_t bytes) {
    const LwipLock lock;
    Slot* slot = clients_.get(client);
    if (slot == nullptr || slot->pcb == nullptr) {
        return;
    }
    // Reopen the receive window only now that the app has taken the data
    // (delayed tcp_recved — AR-6). Sizes per delivery are bounded by TCP_WND.
    tcp_recved(slot->pcb,
               static_cast<std::uint16_t>(std::min(bytes, MAX_RECVED_CHUNK)));
}

void LwipTcpTransport::scan_timeouts() { fire_expired_timeouts(); }

void LwipTcpTransport::timeout_worker_entry(async_context_t* context,
                                            async_at_time_worker_t* worker) {
    auto* self = static_cast<LwipTcpTransport*>(worker->user_data);
    self->scan_timeouts();
    // Reschedule: a self-renewing periodic scan on the lwIP-safe context.
    async_context_add_at_time_worker_in_ms(context, worker,
                                           TIMEOUT_SCAN_INTERVAL_MS);
}

void LwipTcpTransport::completion_worker_entry(
    async_context_t* /*context*/, async_when_pending_worker_t* worker) {
    auto* self = static_cast<LwipTcpTransport*>(worker->user_data);
    // On the async context, so the sends this leads to are lwIP-legal (AR-3).
    // Not self-rescheduling: the worker runs when respond() rings it.
    self->notify_poll();
}

void LwipTcpTransport::signal_completion() {
    async_context_set_work_pending(cyw43_arch_async_context(),
                                   &completion_worker_);
}

void LwipTcpTransport::lock_completions() {
    critical_section_enter_blocking(&completion_lock_);
}

void LwipTcpTransport::unlock_completions() {
    critical_section_exit(&completion_lock_);
}

void LwipTcpTransport::flush_tx(Slot& slot) {
    tcp_pcb* pcb = slot.pcb;
    if (pcb == nullptr) {
        return;
    }
    bool wrote = false;
    while (slot.tx.unsent() > 0) {
        const std::span<const std::uint8_t> chunk = slot.tx.peek();
        const std::size_t room =
            std::min<std::size_t>(chunk.size(), tcp_sndbuf(pcb));
        if (room == 0) {
            break;  // lwIP send buffer full; resume on sent/poll
        }
        // No TCP_WRITE_FLAG_COPY: the TxRing keeps these bytes valid until the
        // peer acks them, so lwIP references ring memory directly.
        const err_t err =
            tcp_write(pcb, chunk.data(), static_cast<std::uint16_t>(room), 0);
        if (err != ERR_OK) {
            break;
        }
        slot.tx.mark_sent(room);
        wrote = true;
    }
    if (wrote) {
        tcp_output(pcb);
    }
}

// The base has already disarmed the timeout and will release the slot and
// deliver the disconnect once this returns. A caller that knows lwIP has
// already freed the pcb (the error path) clears slot.pcb first, which skips
// the teardown below.
void LwipTcpTransport::release_slot_resources(ConnectionId /*client*/,
                                              Slot& slot,
                                              DisconnectReason /*reason*/) {
    tcp_pcb* pcb = slot.pcb;
    slot.pcb = nullptr;
    if (pcb != nullptr) {
        tcp_arg(pcb, nullptr);
        tcp_recv(pcb, nullptr);
        tcp_sent(pcb, nullptr);
        tcp_poll(pcb, nullptr, 0);
        tcp_err(pcb, nullptr);
        // The TxRing was handed to lwIP NOCOPY (tcp_write with flags=0), so
        // lwIP may still hold pointers into slot.tx for unsent/unacked
        // segments. A graceful tcp_close() leaves the pcb alive retransmitting
        // from that memory — but we are about to clear the ring and hand the
        // slot to the next connection, whose writes would overwrite bytes lwIP
        // still references (wire corruption / use-after-reuse). So whenever
        // data is still pending we tcp_abort() (RST) to drop the pcb and its
        // queued segments immediately. The orderly drain-then-close path
        // (close_client -> finish_close_if_drained) only reaches teardown once
        // the ring is empty, so it still closes gracefully.
        const bool tx_pending = !slot.tx.empty();
        if (tx_pending || tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
            // Only latch the abort when we are inside this pcb's own callback:
            // that callback must return ERR_ABRT (via consume_abort()). An
            // abort from the timeout worker or a direct close has no pending
            // callback to consume it, and latching it could alias a pcb later
            // reused at the same address.
            if (pcb == callback_pcb_) {
                aborted_pcb_ = pcb;
            }
        }
    }
    slot.tx.clear();
    slot.close_pending = false;
}

err_t LwipTcpTransport::accept_callback(void* arg, tcp_pcb* new_pcb,
                                        err_t err) {
    auto* self = static_cast<LwipTcpTransport*>(arg);
    if (err != ERR_OK || new_pcb == nullptr) {
        return ERR_VAL;
    }
    const CallbackScope scope(*self);
    const ConnectionId id = self->clients_.acquire();
    if (!id.valid()) {
        self->notify_refused();  // OR-1 refusals_slot_full
        tcp_abort(new_pcb);  // no slot: refuse at the TCP layer (BR-6, RR-3)
        return ERR_ABRT;
    }
    Slot* slot = self->clients_.get(id);
    if (slot == nullptr) {
        // Unreachable: acquire() just returned this id. Stated rather than
        // assumed so the compiler can see it — without it the whole slot setup
        // below reads as a potential null dereference.
        self->clients_.release(id);
        tcp_abort(new_pcb);
        return ERR_ABRT;
    }
    slot->pcb = new_pcb;
    slot->owner = self;
    slot->index = id.slot();
    slot->close_pending = false;
    slot->tx.clear();

    tcp_arg(new_pcb, slot);
    tcp_recv(new_pcb, recv_callback);
    tcp_sent(new_pcb, sent_callback);
    tcp_poll(new_pcb, poll_callback, POLL_INTERVAL_TICKS);
    tcp_err(new_pcb, error_callback);
    tcp_nagle_disable(new_pcb);  // AR-8: latency targets assume Nagle off

    self->notify_connect(id);
    return ERR_OK;
}

err_t LwipTcpTransport::recv_callback(void* arg, tcp_pcb* pcb, pbuf* head,
                                      err_t err) {
    auto* slot = static_cast<Slot*>(arg);
    if (slot == nullptr) {
        // release_slot_resources() unregisters every callback before it nulls
        // tcp_arg, so a callback arriving without a slot is unreachable. The
        // guard is here because error_callback has always had one, and this is
        // the file with no host-test coverage — an inconsistency here is the
        // one that survives.
        if (head != nullptr) {
            pbuf_free(head);
        }
        return ERR_OK;
    }
    LwipTcpTransport* self = slot->owner;
    const PcbScope pcb_scope(self->callback_pcb_, pcb);
    const CallbackScope scope(*self);
    if (head == nullptr) {
        self->teardown(self->clients_.id_at(slot->index),
                       DisconnectReason::PEER_CLOSED);
        return self->consume_abort(pcb);
    }
    if (err != ERR_OK) {
        pbuf_free(head);
        return err;
    }
    const ConnectionId id = self->clients_.id_at(slot->index);
    const std::uint32_t start_us = time_us_32();
    self->timing_receive_ = true;
    self->receive_sent_ = false;
    for (pbuf* chunk = head; chunk != nullptr; chunk = chunk->next) {
        // A segment can close or abort this connection. The slot is released at
        // once (only the notification is deferred), so stop as soon as the id
        // stops resolving rather than delivering the rest of the chain into a
        // connection that is already gone.
        if (!self->is_open(id)) {
            break;
        }
        const std::span<const std::uint8_t> seg(
            static_cast<const std::uint8_t*>(chunk->payload), chunk->len);
        self->notify_receive(id, seg);
    }
    // PR-2: processing ends at the first send, the total with the callback.
    const std::uint32_t end_us = time_us_32();
    self->timing_receive_ = false;
    record_receive(
        (self->receive_sent_ ? self->first_send_us_ : end_us) - start_us,
        end_us - start_us);
    // The application copies what it needs during on_receive and acknowledges
    // consumed bytes via consume(); we can release the pbuf now.
    pbuf_free(head);
    return self->consume_abort(pcb);
}

err_t LwipTcpTransport::sent_callback(void* arg, tcp_pcb* pcb,
                                      std::uint16_t len) {
    auto* slot = static_cast<Slot*>(arg);
    if (slot == nullptr) {
        return ERR_OK;  // no slot: see the note in recv_callback
    }
    LwipTcpTransport* self = slot->owner;
    const PcbScope pcb_scope(self->callback_pcb_, pcb);
    const CallbackScope scope(*self);
    const ConnectionId id = self->clients_.id_at(slot->index);
    slot->tx.ack(len);
    self->notify_writable(id, len);
    Slot* live = self->clients_.get(id);
    if (live != nullptr) {
        pico_http::LwipTcpTransport::flush_tx(*live);
        self->finish_close_if_drained(id, *live);
    }
    return self->consume_abort(pcb);
}

err_t LwipTcpTransport::poll_callback(void* arg, tcp_pcb* pcb) {
    // Liveness backstop only: keep the transmit side moving and
    // finish a drained close. All TR-1..5 deadlines are enforced by the
    // periodic timeout worker via the scheduler, not here.
    auto* slot = static_cast<Slot*>(arg);
    if (slot == nullptr) {
        return ERR_OK;  // no slot: see the note in recv_callback
    }
    LwipTcpTransport* self = slot->owner;
    const PcbScope pcb_scope(self->callback_pcb_, pcb);
    const CallbackScope scope(*self);
    const ConnectionId id = self->clients_.id_at(slot->index);
    pico_http::LwipTcpTransport::flush_tx(*slot);
    if (id.valid()) {
        self->finish_close_if_drained(id, *slot);
    }
    return self->consume_abort(pcb);
}

void LwipTcpTransport::error_callback(void* arg, err_t err) {
    auto* slot = static_cast<Slot*>(arg);
    if (slot == nullptr) {
        return;
    }
    // lwIP has already freed the pcb here; clearing it first makes the shared
    // teardown skip every tcp_* call on the dead pointer.
    LwipTcpTransport* self = slot->owner;
    const CallbackScope scope(*self);
    slot->pcb = nullptr;
    const DisconnectReason reason = (err == ERR_RST)
                                        ? DisconnectReason::PEER_RESET
                                        : DisconnectReason::ABORTED;
    self->teardown(self->clients_.id_at(slot->index), reason);
}

}  // namespace pico_http
