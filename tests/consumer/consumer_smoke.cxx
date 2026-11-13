// SPDX-License-Identifier: MIT
// Profile values must arrive through pico_http::config. EXPECT_P8 selects the
// independent expectations for this installed package.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>

#include "pico_http/pico_http.hpp"
// The installed package includes the transport extension header.
#include "pico_http/tcp/transport_base.hpp"
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

// A downstream transport must be able to implement ITcpTransport from the
// installed package alone. This example serves synchronous handlers only.
// Pending completion needs a context signal or notify_poll() inside a
// CallbackScope (AR-9).
struct ConsumerSlot {
    bool close_pending = false;
};

class ConsumerTransport
    : public pico_http::tcp::TransportBase<ConsumerSlot,
                                           pico_http::MAX_CONNECTIONS> {
   protected:
    std::uint32_t clock_ms() const override { return 0; }

    // A queued transport must report whether its slot is drained to prevent
    // close_client() tearing it down inside the requesting frame.
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

    // Construction checks archive linkage as well as installed headers.
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

    // Construction compares the consumer layout with the archive, including
    // profile fields not checked by the assertions above.
    pico_http::HttpService service(
        std::make_unique<pico_http::testing::FakeTcpTransport>());
    if (!service.start(80)) {
        std::puts("the installed package's HttpService did not start");
        return 1;
    }
    service.stop();
    return 0;
}
