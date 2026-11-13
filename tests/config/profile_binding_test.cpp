// SPDX-License-Identifier: MIT
// Host sizing constants must agree with lwIP configuration.
#include <gtest/gtest.h>

#include <cstddef>

#include "lwipopts.h"
#include "pico_http/config/profile.hpp"
#include "pico_http/config/sizing.hpp"

namespace {

// The option macros use int arithmetic. size_t keeps comparisons between like
// types.
constexpr auto LWIP_TCP_WND = static_cast<std::size_t>(TCP_WND);
constexpr auto LWIP_TCP_MSS = static_cast<std::size_t>(TCP_MSS);
constexpr auto LWIP_PBUF_POOL_SIZE = static_cast<std::size_t>(PBUF_POOL_SIZE);

// The host RX sizing window must match the window lwIP advertises (AR-6).
static_assert(pico_http::ADVERTISED_RX_WINDOW == LWIP_TCP_WND,
              "ADVERTISED_RX_WINDOW must equal lwipopts TCP_WND; the profile "
              "and lwipopts have drifted");

static_assert(pico_http::TCP_MSS_BYTES == LWIP_TCP_MSS,
              "TCP_MSS_BYTES must equal lwipopts TCP_MSS");

// The pbuf pool has to back a full advertised window on every slot at once.
static_assert(LWIP_PBUF_POOL_SIZE * LWIP_TCP_MSS >=
                  pico_http::MAX_CONNECTIONS * pico_http::ADVERTISED_RX_WINDOW,
              "pbuf pool must cover MAX_CONNECTIONS full receive windows");

static_assert(pico_http::HttpProfile<PICO_HTTP_ACTIVE_PROFILE>);

using SelectedProfile = PICO_HTTP_ACTIVE_PROFILE;

static_assert(SelectedProfile::RX_CAPACITY >= pico_http::ADVERTISED_RX_WINDOW,
              "RX assembly buffer must cover a full advertised window (AR-6)");

static_assert(SelectedProfile::MAX_CONNECTIONS == pico_http::MAX_CONNECTIONS,
              "profile slot count must match the transport's slot count");

// The deferral cap is service-wide. HttpService::MAX_PENDING states why it
// lies between one and the slot count (BR-12).
static_assert(SelectedProfile::MAX_PENDING_RESPONSES >= 1,
              "the deferred-response pool must hold at least one entry");
static_assert(SelectedProfile::MAX_PENDING_RESPONSES <=
                  SelectedProfile::MAX_CONNECTIONS,
              "the deferred-response pool cannot exceed the slot count");

TEST(ProfileBindingTest, SelectedProfileMatchesTheLwipConfiguration) {
    EXPECT_EQ(pico_http::ADVERTISED_RX_WINDOW, LWIP_TCP_WND);
    EXPECT_EQ(SelectedProfile::RX_CAPACITY, pico_http::ADVERTISED_RX_WINDOW);
}

TEST(ProfileBindingTest, ProfileSelectionIsInternallyConsistent) {
#ifdef PICO_HTTP_PROFILE_P8_RP2350
    EXPECT_EQ(pico_http::ADVERTISED_RX_WINDOW, 4U * pico_http::TCP_MSS_BYTES);
    EXPECT_EQ(SelectedProfile::Limits::MAX_BUFFERED_BODY, 8192U);
#else
    EXPECT_EQ(pico_http::ADVERTISED_RX_WINDOW, 2U * pico_http::TCP_MSS_BYTES);
    EXPECT_EQ(SelectedProfile::Limits::MAX_BUFFERED_BODY, 2048U);
#endif
}

}  // namespace
