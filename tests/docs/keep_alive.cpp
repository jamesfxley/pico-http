// SPDX-License-Identifier: MIT
// Keep-alive and timeouts, given the server.
#include <pico_http/pico_http.hpp>

namespace doc_examples {
void keep_alive(pico_http::HttpService* server) {
#include "keep_alive.inc"
}
}  // namespace doc_examples
