// SPDX-License-Identifier: MIT
// Binds the host-safe sizing constants to the lwIP configuration they must
// agree with. The equivalent assertions previously lived only in the lwIP
// transport, which is firmware-only, so no host build — and therefore no
// profile job in CI — ever checked the pairing. lwipopts.h is pure macros, so
// including it here costs nothing and needs no lwIP sources.
#include <gtest/gtest.h>

#include <cstddef>

#include "lwipopts.h"
#include "pico_http/config/profile.hpp"
#include "pico_http/config/sizing.hpp"

namespace {

// The lwipopts values are macros expanding to int arithmetic. Name them once as
// size_t so every comparison below is between like types.
constexpr auto LWIP_TCP_WND = static_cast<std::size_t>(TCP_WND);
constexpr auto LWIP_TCP_MSS = static_cast<std::size_t>(TCP_MSS);
constexpr auto LWIP_PBUF_POOL_SIZE = static_cast<std::size_t>(PBUF_POOL_SIZE);

// AR-6: the advertised window the host-safe layer sizes its RX buffer from must
// be the window lwIP actually advertises.
static_assert(pico_http::ADVERTISED_RX_WINDOW == LWIP_TCP_WND,
              "ADVERTISED_RX_WINDOW must equal lwipopts TCP_WND; the profile "
              "and lwipopts have drifted");

static_assert(pico_http::TCP_MSS_BYTES == LWIP_TCP_MSS,
              "TCP_MSS_BYTES must equal lwipopts TCP_MSS");

// The pbuf pool has to back a full advertised window on every slot at once.
static_assert(LWIP_PBUF_POOL_SIZE * LWIP_TCP_MSS >=
                  pico_http::MAX_CONNECTIONS * pico_http::ADVERTISED_RX_WINDOW,
              "pbuf pool must cover MAX_CONNECTIONS full receive windows");

// Every shipped profile satisfies the concept, and the selected one is what the
// service will instantiate.
static_assert(pico_http::HttpProfile<PICO_HTTP_ACTIVE_PROFILE>);

using SelectedProfile = PICO_HTTP_ACTIVE_PROFILE;

static_assert(SelectedProfile::RX_CAPACITY >= pico_http::ADVERTISED_RX_WINDOW,
              "RX assembly buffer must cover a full advertised window (AR-6)");

static_assert(SelectedProfile::MAX_CONNECTIONS == pico_http::MAX_CONNECTIONS,
              "profile slot count must match the transport's slot count");

// BR-12. The pool is service-level rather than per-slot, so nothing about the
// slot layout constrains it; these are the bounds that make it meaningful. A
// zero-sized pool would answer every request with 503, and entries past the
// slot count are unreachable because a connection holds at most one.
static_assert(SelectedProfile::MAX_PENDING_RESPONSES >= 1,
              "the deferred-response pool must hold at least one entry");
static_assert(SelectedProfile::MAX_PENDING_RESPONSES <=
                  SelectedProfile::MAX_CONNECTIONS,
              "the deferred-response pool cannot exceed the slot count");

TEST(ProfileBindingTest, SelectedProfileMatchesTheLwipConfiguration) {
    EXPECT_EQ(pico_http::ADVERTISED_RX_WINDOW, LWIP_TCP_WND);
    EXPECT_EQ(SelectedProfile::RX_CAPACITY, pico_http::ADVERTISED_RX_WINDOW);
}

// The p8-rp2350 profile is defined by raising the window and the buffered-body
// cap together; a build that defines one without the other is misconfigured.
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
