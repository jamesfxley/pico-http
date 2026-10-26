// SPDX-License-Identifier: MIT
// Testing on the host: the example reads a response it then ignores.
#include <memory>
#include <pico_http/pico_http.hpp>
#include <pico_http/testing/fake_tcp_transport.hpp>
#include <string>

namespace doc_examples {
void host_testing() {
#include "host_testing.inc"
    static_cast<void>(response);
}
}  // namespace doc_examples
