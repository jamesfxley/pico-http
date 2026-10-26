// SPDX-License-Identifier: MIT
// The README's host construction, over the fake transport.
#include <memory>
#include <pico_http/pico_http.hpp>
#include <pico_http/testing/fake_tcp_transport.hpp>

namespace doc_examples {
void readme_host_fake() {
#include "readme_host_fake.inc"
}
}  // namespace doc_examples
