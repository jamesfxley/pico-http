// SPDX-License-Identifier: MIT
// Consumer-side view of the sizing profile. Nothing here defines a profile
// macro: every value below must arrive through pico_http::config's INTERFACE
// compile definitions. EXPECT_P8 is set by the harness to the profile the
// package was built with, so if the definitions stop propagating, the
// assertions below fail to compile instead of producing translation units whose
// HttpService layout disagrees with the archive they link against.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>

#include "pico_http/pico_http.hpp"
// The porting seam docs/usage-guide.md tells a downstream project to
// derive from. Included here so the package failing to ship it is a compile
// error in this test rather than a discovery made by the first porter.
#include "pico_http/tcp/transport_base.hpp"
// The test double the package ships for downstream tests. Included so a
// pico_http::testing that left the export set, or whose headers did not
// install, is a compile error here rather than a downstream discovery.
#include "pico_http/testing/fake_tcp_transport.hpp"

namespace {

using Profile = PICO_HTTP_ACTIVE_PROFILE;

#ifdef EXPECT_P8
static_assert(pico_http::ADVERTISED_RX_WINDOW == 4 * pico_http::TCP_MSS_BYTES,
              "p8-rp2350 package must give the consumer a 4xMSS window");
static_assert(Profile::Limits::MAX_BUFFERED_BODY == 8192,
              "p8-rp2350 package must give the consumer the 8 KiB body cap");
#else
static_assert(pico_http::ADVERTISED_RX_WINDOW == 2 * pico_http::TCP_MSS_BYTES,
              "default package must give the consumer a 2xMSS window");
static_assert(Profile::Limits::MAX_BUFFERED_BODY == 2048,
              "default package must give the consumer the 2 KiB body cap");
#endif

static_assert(Profile::RX_CAPACITY == pico_http::ADVERTISED_RX_WINDOW,
              "consumer's RX capacity must match the advertised window (AR-6)");

// The custom-transport example from the porting guide, reduced to what it takes
// to instantiate: a downstream project must be able to satisfy ITcpTransport
// from the installed package alone. Mirrors tests/tcp/porting_example_test.
//
// Synchronous-only, and deliberately so: it overrides none of
// signal_completion()/lock_completions()/unlock_completions(), so it cannot
// serve a handler that returns Disposition::PENDING -- respond() would record
// the completion, return true, and nothing would ever deliver it (AR-9). A
// real transport either signals its own context or drives notify_poll(), and
// opens a CallbackScope around whichever it does.
struct ConsumerSlot {
    bool close_pending = false;
};

class ConsumerTransport
    : public pico_http::tcp::TransportBase<ConsumerSlot,
                                           pico_http::MAX_CONNECTIONS> {
   protected:
    std::uint32_t clock_ms() const override { return 0; }

    // Nothing is ever in flight here because send() accepts everything
    // synchronously, so a slot is always drained. A transport with a real
    // transmit queue must report it: returning true unconditionally lets
    // close_client() tear a slot down inside the frame that asked for it.
    bool slot_drained(const ConsumerSlot& /*slot*/) const override {
        return true;
    }

    void set_close_pending(ConsumerSlot& slot) override {
        slot.close_pending = true;
    }

    void release_slot_resources(
        ConnectionId /*client*/, ConsumerSlot& /*slot*/,
        pico_http::DisconnectReason /*reason*/) override {}

   public:
    bool listen(std::uint16_t /*port*/) override { return true; }

    std::size_t send(ConnectionId /*client*/,
                     std::span<const std::uint8_t> data) override {
        return data.size();
    }

    void consume(ConnectionId /*client*/, std::size_t /*bytes*/) override {}

    void close_server() override { close_all_clients(); }
};

}  // namespace

int main() {
    std::printf("rx_window=%zu buffered_body=%zu\n",
                pico_http::ADVERTISED_RX_WINDOW,
                static_cast<std::size_t>(Profile::Limits::MAX_BUFFERED_BODY));

    // The shipped double satisfies the same seam, from the installed package
    // alone. Constructing it is what proves the archive linked, not just that
    // the header was found.
    pico_http::testing::FakeTcpTransport fake;
    if (!fake.listen(80)) {
        std::puts("the shipped test double did not come through the package");
        return 1;
    }

    ConsumerTransport transport;
    pico_http::ITcpTransport& as_interface = transport;
    if (!as_interface.listen(80) ||
        as_interface.is_open(pico_http::tcp::ConnectionId{})) {
        return 1;
    }

    // The layout check this file exists for, and the only thing that actually
    // runs it. The two static_asserts above read ADVERTISED_RX_WINDOW and
    // MAX_BUFFERED_BODY; HttpService's constructor compares the *caller's*
    // ProfileBinding -- its named knobs, and conn_size and conn_align, which
    // every other profile and parser knob reaches -- against the one the
    // archive was built with, and aborts on any disagreement. A definition that
    // stops propagating to a consumer translation unit is caught here and
    // nowhere else in this file: without this construction the smoke compiled,
    // linked, printed and exited 0 while the layouts disagreed.
    pico_http::HttpService service(
        std::make_unique<pico_http::testing::FakeTcpTransport>());
    if (!service.start(80)) {
        std::puts("the installed package's HttpService did not start");
        return 1;
    }
    service.stop();
    return 0;
}
