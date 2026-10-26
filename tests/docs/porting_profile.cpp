// SPDX-License-Identifier: MIT
// A custom profile, which must satisfy the concept the service is sized by.
#include <cstddef>
#include <pico_http/config/profile.hpp>
#include <pico_http/config/sizing.hpp>

namespace doc_examples {
#include "porting_profile.inc"

static_assert(pico_http::HttpProfile<MyProfile>);
}  // namespace doc_examples
