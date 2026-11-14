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

// This translation unit checks the application's lwIP configuration (AR-10).
#include "pico_http/lwipopts_assertions.hpp"  // IWYU pragma: keep

/// \file lwip_tcp_transport.cpp
/// Firmware raw-TCP adapter. The header documents callback and buffer
/// lifetimes.

namespace pico_http {

namespace {
// Widest window reopen a single tcp_recved() call can express: its len
// argument is a uint16_t.
constexpr std::size_t MAX_RECVED_CHUNK = 0xFFFF;
}  // namespace

// Bind host-safe sizes to lwIP so the assembly window cannot be undersized
// (AR-6).
static_assert(TCP_MSS_BYTES == static_cast<std::size_t>(TCP_MSS),
              "TCP_MSS_BYTES must match lwipopts TCP_MSS");
static_assert(ADVERTISED_RX_WINDOW == static_cast<std::size_t>(TCP_WND),
              "ADVERTISED_RX_WINDOW must match lwipopts TCP_WND");
static_assert(TX_RING_CAPACITY <= static_cast<std::size_t>(TCP_SND_BUF),
              "TX ring must fit within the lwIP send buffer (TCP_SND_BUF)");
// The window must fit one uint16_t tcp_recved() call; consume() covers at
// most one full RX buffer.
static_assert(ADVERTISED_RX_WINDOW <= MAX_RECVED_CHUNK,
              "advertised window must fit a single uint16_t tcp_recved()");

namespace {

constexpr std::uint8_t POLL_INTERVAL_TICKS = 4;  // lwIP poll ticks (~2 s)

// All lock-hold samples are written on the serialised async context (AR-3).
#ifndef NDEBUG
constexpr std::uint32_t LOCK_HOLD_BUDGET_US = 500;  // spec AR-3 p99 target

struct LockHoldSampler {
    std::uint32_t samples = 0;
    std::uint32_t within_budget = 0;
    std::uint32_t max_us = 0;
};

// cppcoreguidelines-avoid-non-const-global-variables: the sample accumulates
// only on the serialised async context.
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

// Receive timing is recorded in every build type on the serialised async
// context (PR-2).
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
        // cppcoreguidelines-prefer-member-initializer: timing follows locking.
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

// Records the PCB whose callback is executing, so teardown can tell a
// self-abort, which must return ERR_ABRT, from an external abort.
// It is separate from CallbackScope, which defers disconnect delivery.
// recv_callback, sent_callback and poll_callback open both.
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

// The completion lock outlives close_server() and exists even if listen()
// failed.
LwipTcpTransport::LwipTcpTransport() {
    critical_section_init(&completion_lock_);
}

// The qualified call names LwipTcpTransport::close_server().
// It cannot dispatch to a derived close_server() during destruction.
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
    // The abort latch is cleared before a later PCB can reuse the address.
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

    // Scan deadlines on the async context every 100 ms (TR-1..TR-5).
    timeout_worker_.do_work = timeout_worker_entry;
    timeout_worker_.user_data = this;
    async_context_add_at_time_worker_in_ms(
        cyw43_arch_async_context(), &timeout_worker_, TIMEOUT_SCAN_INTERVAL_MS);

    // Pending work runs on the next async-context turn without waiting for
    // the timed scan (AR-9).
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

// The base disarms before this call and releases the slot afterwards. The
// error path has already cleared a freed PCB.
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
        // lwIP can retain NOCOPY pointers into the ring. Abort before
        // clearing pending data; an empty ring can close gracefully.
        const bool tx_pending = !slot.tx.empty();
        if (tx_pending || tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
            // Only a callback aborting its own PCB must return ERR_ABRT.
            // External aborts leave no pointer that could alias a later PCB.
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
        // acquire() just issued this id, so failure to resolve it violates
        // the slot invariant.
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
        // Teardown unregisters callbacks before clearing tcp_arg; a null slot
        // is a defensive guard.
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
        // A segment can tear down the connection. Stop delivering as soon as
        // its id stops resolving.
        if (!self->is_open(id)) {
            break;
        }
        const std::span<const std::uint8_t> seg(
            static_cast<const std::uint8_t*>(chunk->payload), chunk->len);
        self->notify_receive(id, seg);
    }
    // Processing ends at first send; total ends at callback return (PR-2).
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
    // Polling advances TX and drained close. The periodic worker enforces
    // deadlines (TR-1..5).
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
