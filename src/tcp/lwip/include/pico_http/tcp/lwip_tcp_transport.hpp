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

// readability-identifier-naming: the forward-declared type names belong to
// lwIP.
// NOLINTNEXTLINE(readability-identifier-naming)
struct tcp_pcb;
// readability-identifier-naming: pbuf is an lwIP type.
// NOLINTNEXTLINE(readability-identifier-naming)
struct pbuf;

/// \file lwip_tcp_transport.hpp
/// Firmware raw-TCP adapter with fixed slots (AR-1).
/// Each callback argument names its slot, including tcp_err after the PCB is
/// freed. Sends retain ring bytes until ACK; consume() returns receive credit
/// (AR-2, AR-5, AR-6).

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

/// Firmware transport over raw lwIP and the cyw43 async context.
class LwipTcpTransport
    : public tcp::TransportBase<detail::LwipSlot,
                                ::pico_http::MAX_CONNECTIONS> {
   public:
    /// Per-connection state this transport keeps.
    using Slot = detail::LwipSlot;

    /// Debug lock-hold counts; within_500us / samples >= 0.99 establishes the
    /// p99 bound (AR-3).
    struct LockHoldStats {
        /// Total lock spans timed.
        std::uint32_t samples;
        /// Spans that held the lock for 500 us or less.
        std::uint32_t within_500us;
        /// The worst hold observed, in microseconds.
        std::uint32_t max_us;
    };

    /// Lock-hold counts, all zero in Release. Read from the async context
    /// that writes them.
    static LockHoldStats lock_hold_stats() noexcept;
    /// Discard the sample and start a fresh measurement window. Call from the
    /// async context, as lock_hold_stats().
    static void reset_lock_hold_stats() noexcept;

    /// Receive-callback timing in every build type (PR-2).
    /// Processing ends at the first send or callback return. Total includes
    /// the full callback. Split requests contribute several samples; deferred
    /// completion is outside them (AR-9).
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
    /// The override takes the stack lock before the shared drain-close path.
    /// Timeout operations already run in the async context.
    void close_client(ConnectionId client) override;
    std::size_t send(ConnectionId client,
                     std::span<const std::uint8_t> data) override;
    [[nodiscard]] std::size_t send_credit(ConnectionId client) const override;
    void consume(ConnectionId client, std::size_t bytes) override;
    // The HTTP layer arms idle deadlines through arm_timeout(); the idle hint
    // needs no override (TR-3).

    /// Signals the async worker from any context, including an ISR (AR-9;
    /// async_context.h).
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

    // Only a self-abort inside the active PCB callback is latched so
    // consume_abort() returns ERR_ABRT. An external abort leaves no pointer
    // that could alias a reused PCB.
    err_t consume_abort(tcp_pcb* pcb) noexcept;

    async_at_time_worker_t timeout_worker_{};
    // signal_completion() can mark this worker pending from another core or
    // ISR. The worker sends on the async context (AR-9).
    async_when_pending_worker_t completion_worker_{};
    // An IRQ-disabling spin lock guards HTTP completion records across both
    // cores for the entire object lifetime (AR-3).
    critical_section_t completion_lock_{};
    tcp_pcb* server_pcb_ = nullptr;
    tcp_pcb* aborted_pcb_ = nullptr;
    // The active callback PCB, or nullptr outside callbacks. Self-abort must
    // return ERR_ABRT; external abort must not latch aborted_pcb_.
    tcp_pcb* callback_pcb_ = nullptr;
    // The timed receive callback and its first-send timestamp (PR-2).
    bool timing_receive_ = false;
    bool receive_sent_ = false;
    std::uint32_t first_send_us_ = 0;
};

}  // namespace pico_http
