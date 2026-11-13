// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "pico_http/http/http_parser.hpp"

/// For any input, the parser must terminate, consume no more than supplied
/// and make progress on non-empty incomplete input.

// readability-identifier-naming: libFuzzer fixes the entry-point name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::span<const std::uint8_t> input(data, size);
    pico_http::HttpParser<> parser;

    std::size_t off = 0;
    while (off < input.size()) {
        // Data-dependent chunking to explore arbitrary segmentation.
        std::size_t chunk = 1U + (input[off] % 7U);
        chunk = std::min(chunk, input.size() - off);
        std::size_t consumed = 0;
        auto status = parser.feed(input.subspan(off, chunk), consumed);
        if (consumed > chunk) {
            __builtin_trap();  // over-consumed
        }
        off += consumed;
        // Progress is checked before dispatching any verdict, bounding
        // iterations by input size.
        if (consumed == 0) {
            __builtin_trap();  // stalled
        }
        if (status == pico_http::ParseResult::COMPLETE) {
            // Resetting rather than breaking also parses the bytes after the
            // first complete request.
            parser.reset();
            continue;
        }
        if (status != pico_http::ParseResult::INCOMPLETE) {
            break;  // a definite failure ends the connection
        }
    }
    return 0;
}
