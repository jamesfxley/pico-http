// SPDX-License-Identifier: MIT
// The custom profile must satisfy the service sizing concept.
#include <cstddef>
#include <pico_http/config/profile.hpp>
#include <pico_http/config/sizing.hpp>

namespace doc_examples {
#include "porting_profile.inc"

static_assert(pico_http::HttpProfile<MyProfile>);
}  // namespace doc_examples
