// SPDX-License-Identifier: MIT
#include <cstdint>
#include <pico_http/pico_http.hpp>
#include <span>

namespace doc_examples {
void static_body(pico_http::HTTPResponse& res,
                 std::span<const std::uint8_t> flash_page) {
#include "static_body.inc"
}
}  // namespace doc_examples
