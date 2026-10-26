// SPDX-License-Identifier: MIT
// Routes and handlers, given the server and a handler to register.
#include <pico_http/pico_http.hpp>

namespace doc_examples {
void routes(pico_http::HttpService* server,
            const pico_http::RequestHandler& handler) {
#include "routes.inc"
}
}  // namespace doc_examples
