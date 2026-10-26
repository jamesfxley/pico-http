// SPDX-License-Identifier: MIT
// The length-less generator, given a response and the reader's generator.
#include <cstddef>
#include <cstdint>
#include <pico_http/pico_http.hpp>
#include <span>

namespace doc_examples {
struct PatternState {
    std::size_t remaining = 0;
};

std::size_t pattern_until_done(void* ctx, std::size_t offset,
                               std::span<std::uint8_t> out);

void chunked_generator(pico_http::HTTPResponse& res, PatternState& state) {
#include "chunked_generator.inc"
}
}  // namespace doc_examples
