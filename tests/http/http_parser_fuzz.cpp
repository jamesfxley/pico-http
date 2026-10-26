// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "pico_http/http/http_parser.hpp"

/// libFuzzer entry point for the incremental HTTP request parser.
/// Built only when PICO_HTTP_ENABLE_FUZZERS is set and the compiler supports
/// -fsanitize=fuzzer (Clang). The always-on corpus in http_parser_test.cpp
/// exercises the same contract in CI; this target explores it continuously.
///
/// The parser must, for any byte sequence: terminate, never consume more than
/// it was given, and never stall (consume zero bytes while still Incomplete on
/// non-empty input). Sanitizers catch any memory misuse; the asserts below
/// catch contract violations.

// Entry-point name fixed by libFuzzer's ABI.
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
        // Checked before the verdict is dispatched, so it holds on every
        // iteration rather than only the INCOMPLETE ones. That is the
        // termination argument: each pass consumes at least one byte, so the
        // loop is bounded by input.size(). A separate iteration cap phrased as
        // `> input.size() + 8` cannot fire once this trap is above it, and a
        // guarantee that cannot fail is not one.
        if (consumed == 0) {
            __builtin_trap();  // stalled
        }
        if (status == pico_http::ParseResult::COMPLETE) {
            // feed() leaves trailing bytes for the next request, so a corpus
            // entry beginning with a well-formed request has more to explore.
            // Breaking here left every byte past the first request boundary
            // unparsed, and the reset/pipelining path unfuzzed entirely.
            parser.reset();
            continue;
        }
        if (status != pico_http::ParseResult::INCOMPLETE) {
            break;  // a definite failure ends the connection
        }
    }
    return 0;
}
