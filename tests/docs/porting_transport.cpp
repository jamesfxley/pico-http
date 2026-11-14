// SPDX-License-Identifier: MIT
// Every override must match the transport base.
#include <cstddef>
#include <cstdint>
#include <pico_http/pico_http.hpp>
#include <pico_http/tcp/transport_base.hpp>
#include <span>

namespace doc_examples {
#include "porting_transport.inc"
}  // namespace doc_examples
