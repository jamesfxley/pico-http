// SPDX-License-Identifier: MIT
// Deferring a response, given the reader's own sensor state.
#include <pico_http/pico_http.hpp>
#include <string_view>

namespace doc_examples {
struct Reading {
    pico_http::RequestId pending{};
    void start();
    [[nodiscard]] std::string_view json() const;
};

#include "deferring.inc"
}  // namespace doc_examples
