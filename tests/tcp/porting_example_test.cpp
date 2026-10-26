// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "pico_http/config/limits.hpp"
#include "pico_http/config/profile.hpp"
#include "pico_http/config/sizing.hpp"
#include "pico_http/tcp/tcp_transport.hpp"
#include "pico_http/tcp/transport_base.hpp"

/// \file porting_example_test.cpp
/// Compiles the custom-transport and custom-profile examples printed in
/// docs/usage-guide.md ("Porting"), so the documented extension points
/// cannot drift from the interfaces they describe. If this file needs an edit
/// to build, that section of the guide needs the same edit.

namespace {

struct MySlot {
    bool close_pending = false;
};

class MyTransport
    : public pico_http::tcp::TransportBase<MySlot, pico_http::MAX_CONNECTIONS> {
   protected:
    [[nodiscard]] std::uint32_t clock_ms() const override { return now_ms_; }

    [[nodiscard]] bool slot_drained(const MySlot& /*slot*/) const override {
        return true;
    }

    void set_close_pending(MySlot& slot) override { slot.close_pending = true; }

    void release_slot_resources(
        ConnectionId /*client*/, MySlot& /*slot*/,
        pico_http::DisconnectReason /*reason*/) override {}

   public:
    bool listen(std::uint16_t port) override {
        port_ = port;
        return true;
    }

    std::size_t send(ConnectionId /*client*/,
                     std::span<const std::uint8_t> data) override {
        return data.size();
    }

    void consume(ConnectionId /*client*/, std::size_t /*bytes*/) override {}

    void close_server() override { close_all_clients(); }

    [[nodiscard]] std::uint16_t port() const { return port_; }

   private:
    std::uint32_t now_ms_ = 0;
    std::uint16_t port_ = 0;
};

struct MyProfile {
    using Limits = pico_http::DefaultHttpLimits;
    static constexpr std::size_t MAX_CONNECTIONS = pico_http::MAX_CONNECTIONS;
    static constexpr std::size_t MAX_PENDING_RESPONSES =
        pico_http::MAX_PENDING_RESPONSES;
    static constexpr std::size_t HEAD_CAPACITY = 1024;
    static constexpr std::size_t GENERATOR_CHUNK = 512;
    static constexpr std::size_t RX_CAPACITY = pico_http::ADVERTISED_RX_WINDOW;
};

static_assert(pico_http::HttpProfile<MyProfile>,
              "the profile documented in the porting guide must satisfy the "
              "concept the guide names");

}  // namespace

TEST(PortingExampleTest, DocumentedTransportSatisfiesTheInterface) {
    MyTransport transport;
    EXPECT_TRUE(transport.listen(80));
    EXPECT_EQ(transport.port(), 80);

    // The base supplies the rest of ITcpTransport, so a derived transport that
    // implements only the documented hooks is a complete, usable one.
    pico_http::ITcpTransport& as_interface = transport;
    EXPECT_FALSE(as_interface.is_open(pico_http::tcp::ConnectionId{}));
    as_interface.close_server();
}
