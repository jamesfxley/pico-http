// SPDX-License-Identifier: MIT
// A custom transport's declaration, at namespace scope as a porter writes it.
// Every override must match the base, which is what this compiles.
#include <cstddef>
#include <cstdint>
#include <pico_http/pico_http.hpp>
#include <pico_http/tcp/transport_base.hpp>
#include <span>

namespace doc_examples {
#include "porting_transport.inc"
}  // namespace doc_examples
