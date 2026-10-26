// SPDX-License-Identifier: MIT
// Observability, given the server; the example reads values it then ignores.
#include <cstddef>
#include <pico_http/pico_http.hpp>

namespace doc_examples {
void observability(pico_http::HttpService* server) {
#include "observability.inc"
    static_cast<void>(stats);
    static_cast<void>(live);
}
}  // namespace doc_examples
