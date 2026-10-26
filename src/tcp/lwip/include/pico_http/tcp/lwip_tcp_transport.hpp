// SPDX-License-Identifier: MIT
#pragma once

#include <lwip/err.h>
#include <pico/async_context.h>
#include <pico/critical_section.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "pico_http/config/sizing.hpp"
#include "pico_http/tcp/tcp_transport.hpp"
#include "pico_http/tcp/transport_base.hpp"
#include "pico_http/util/tx_ring.hpp"

// Forward declarations of lwIP types; both names are lwIP's, not ours, so the
// case style is fixed by the library this header declares against.
// NOLINTNEXTLINE(readability-identifier-naming)
struct tcp_pcb;
// lwIP's name, as above.
// NOLINTNEXTLINE(readability-identifier-naming)
struct pbuf;

/// \file lwip_tcp_transport.hpp
/// Firmware transport implementing ITcpTransport over the lwIP raw API (spec
/// AR-1). Slot handling, timeouts, the drain-close lifecycle and deferred
/// disconnect delivery come from TransportBase; what is specific here is the
/// lwIP binding. The load-bearing detail is that the lwIP callback arg is the
/// per-connection Slot (not the server), so the error path — where lwIP has
/// already freed the pcb — reaches connection state in O(1). Outbound data is
/// staged in a per-slot zero-copy TxRing and handed to tcp_write() without
/// TCP_WRITE_FLAG_COPY (AR-2/AR-5); inbound bytes are acknowledged to lwIP only
/// when the application consume()s them (AR-6).

namespace pico_http {

class LwipTcpTransport;

namespace detail {

/// Per-connection transport state. `owner` lets a raw lwIP callback, whose arg
/// is this slot, reach the transport; `index` is the slot's physical position.
struct LwipSlot {
    /// The lwIP protocol control block, or nullptr when the slot is free.
    tcp_pcb* pcb = nullptr;
    /// The transport owning this slot, so a raw lwIP callback can reach it.
    LwipTcpTransport* owner = nullptr;
    /// The slot's physical position in the table.
    std::uint8_t index = 0;
    /// Whether a close is waiting for the outbound data to drain.
    bool close_pending = false;
    /// Outbound bytes staged for this connection, handed to lwIP without a
    /// copy.
    TxRing<TX_RING_CAPACITY> tx{};
};

}  // namespace detail

/// The lwIP-backed TCP transport: the one component touching the lwIP raw API
/// and the cyw43 async context.
///
/// Firmware only. A host build takes pico_http::testing's fake instead, which
/// stands in for this at the same seam.
class LwipTcpTransport
    : public tcp::TransportBase<detail::LwipSlot,
                                ::pico_http::MAX_CONNECTIONS> {
   public:
    /// Per-connection state this transport keeps.
    using Slot = detail::LwipSlot;

    // AR-3 lock-discipline sampling. Every span the library holds the lwIP lock
    // (cyw43_arch_lwip_begin/end) is timed in debug builds; the spec requires a
    // p99 hold time <= 500 us under load (measured in t5). `within_500us /
    // samples >= 0.99` establishes the p99 bound without storing a histogram.
    // Zero-cost in release builds (NDEBUG): no sample is taken.
    /// AR-3's lock-discipline sample. `within_500us / samples >= 0.99`
    /// establishes the p99 bound without storing a histogram.
    struct LockHoldStats {
        /// Total lock spans timed.
        std::uint32_t samples;
        /// Spans that held the lock for 500 us or less.
        std::uint32_t within_500us;
        /// The worst hold observed, in microseconds.
        std::uint32_t max_us;
    };

    /// The lock-hold sample so far. All zero in a release build, which takes no
    /// sample. Call from the async context (a route handler), which the lock
    /// spans that write it also run in.
    static LockHoldStats lock_hold_stats() noexcept;
    /// Discard the sample and start a fresh measurement window. Call from the
    /// async context, as lock_hold_stats().
    static void reset_lock_hold_stats() noexcept;

    /// PR-2's internal-processing sample, taken in every build type because
    /// PR-2 is measured on a release build. Each receive callback that delivers
    /// data is one sample, timed two ways:
    ///   - processing: from the callback's start to its first send(), which is
    ///     the parsing, routing, handler and serialization of the response it
    ///     starts; or the whole callback when it sends nothing;
    ///   - total: the whole callback, the handoff of the response to lwIP and
    ///     the radio included.
    /// A request split across callbacks is several samples, and a deferred
    /// handler (AR-9) runs outside any of them. `within_1ms / samples`
    /// establishes a share without storing a histogram.
    struct ReceiveStats {
        /// Receive callbacks timed.
        std::uint32_t samples;
        /// Callbacks whose processing took 1 ms or less.
        std::uint32_t within_1ms;
        /// The longest processing observed, in microseconds.
        std::uint32_t max_us;
        /// Callbacks that took 1 ms or less in total.
        std::uint32_t total_within_1ms;
        /// The longest whole callback observed, in microseconds.
        std::uint32_t total_max_us;
    };

    /// The receive-callback sample so far. Call from the async context (a
    /// route handler), which the callbacks that write it also run in.
    static ReceiveStats receive_stats() noexcept;
    /// Discard the sample and start a fresh measurement window. Call from the
    /// async context, as receive_stats().
    static void reset_receive_stats() noexcept;

    LwipTcpTransport();
    ~LwipTcpTransport() override;

    LwipTcpTransport(const LwipTcpTransport&) = delete;
    LwipTcpTransport& operator=(const LwipTcpTransport&) = delete;
    LwipTcpTransport(LwipTcpTransport&&) = delete;
    LwipTcpTransport& operator=(LwipTcpTransport&&) = delete;

    bool listen(std::uint16_t port) override;
    void close_server() override;
    /// Overridden only to take the lwIP stack lock: the drain-close logic
    /// itself is TransportBase::close_client_impl(). arm_timeout/clear_timeout
    /// need no override — they run only from inside lwIP callbacks, which are
    /// already on the async context.
    void close_client(ConnectionId client) override;
    std::size_t send(ConnectionId client,
                     std::span<const std::uint8_t> data) override;
    [[nodiscard]] std::size_t send_credit(ConnectionId client) const override;
    void consume(ConnectionId client, std::size_t bytes) override;
    // set_idle_timeout is intentionally not overridden: idle timing (TR-3) is
    // armed by the HTTP layer through arm_timeout(), so the base no-op is
    // correct here.

    /// AR-9 doorbell. Safe from core 1 and from an ISR: async_context.h
    /// documents set_work_pending as callable "from any context including
    /// IRQs", and nothing else here runs.
    void signal_completion() override;
    void lock_completions() override;
    void unlock_completions() override;

   protected:
    // --- TransportBase hooks ------------------------------------------------
    [[nodiscard]] std::uint32_t clock_ms() const override { return now_ms(); }

    [[nodiscard]] bool slot_drained(const Slot& slot) const override {
        return slot.tx.empty();
    }

    void set_close_pending(Slot& slot) override { slot.close_pending = true; }

    void release_slot_resources(ConnectionId client, Slot& slot,
                                DisconnectReason reason) override;

   private:
    // lwIP raw callbacks. `arg` is the transport for accept, else the Slot*.
    static err_t accept_callback(void* arg, tcp_pcb* new_pcb, err_t err);
    static err_t recv_callback(void* arg, tcp_pcb* pcb, pbuf* head, err_t err);
    static err_t sent_callback(void* arg, tcp_pcb* pcb, std::uint16_t len);
    static err_t poll_callback(void* arg, tcp_pcb* pcb);
    static void error_callback(void* arg, err_t err);

    static constexpr std::uint32_t TIMEOUT_SCAN_INTERVAL_MS = 100;

    static void flush_tx(Slot& slot);
    void scan_timeouts();
    static void timeout_worker_entry(async_context_t* context,
                                     async_at_time_worker_t* worker);
    static void completion_worker_entry(async_context_t* context,
                                        async_when_pending_worker_t* worker);
    static std::uint32_t now_ms();

    // lwIP requires a callback that aborts its own pcb (tcp_abort) to return
    // ERR_ABRT so lwIP stops touching the freed pcb. Teardown records an
    // aborted pcb here — but only when it is the pcb of the callback currently
    // executing (callback_pcb_), so an abort from the timeout worker or a
    // direct close cannot leave a pointer that aliases a later pcb reused at
    // the same address. The active callback consumes it via consume_abort().
    err_t consume_abort(tcp_pcb* pcb) noexcept;

    async_at_time_worker_t timeout_worker_{};
    // Deferred completions (AR-9). signal_completion() may run on core 1 or in
    // an ISR, so it does the only two things safe there: set the worker pending
    // (documented safe from any context) and return. The work itself runs in
    // completion_worker_entry, on the async context, where lwIP calls are
    // legal.
    async_when_pending_worker_t completion_worker_{};
    // Guards the HTTP layer's completion record, not any state here (AR-3).
    // A spin lock plus IRQ disable: the writer may be an ISR on either core, so
    // a mutex would be wrong twice over.
    //
    // Initialised by the constructor and released by the destructor, so it is
    // valid for as long as an object exists to be asked for it. Tying it to
    // listen()/close_server() instead would leave HttpService::stop() -- which
    // closes the server and then takes this lock once per slot -- holding a
    // spin lock already returned to the free pool, and would leave it null
    // when listen() never ran.
    critical_section_t completion_lock_{};
    tcp_pcb* server_pcb_ = nullptr;
    tcp_pcb* aborted_pcb_ = nullptr;
    // The pcb whose lwIP callback is currently on the stack (nullptr outside
    // any callback), so teardown can tell an in-callback self-abort — which
    // must return ERR_ABRT — from an abort issued by the timeout worker or a
    // direct close, which must not latch aborted_pcb_.
    tcp_pcb* callback_pcb_ = nullptr;
    // PR-2: the receive callback being timed, and when it first sent.
    bool timing_receive_ = false;
    bool receive_sent_ = false;
    std::uint32_t first_send_us_ = 0;
};

}  // namespace pico_http
