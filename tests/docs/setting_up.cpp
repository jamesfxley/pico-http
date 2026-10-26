// SPDX-License-Identifier: MIT
// Setting up: the firmware factory, inside a function as start-up code is. It
// is compiled and never linked, so the factory's firmware-only definition is
// not needed.
#include <pico_http/pico_http.hpp>

namespace doc_examples {
void setting_up() {
#include "setting_up.inc"
}
}  // namespace doc_examples
