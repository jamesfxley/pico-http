// SPDX-License-Identifier: MIT
#include "pico_http/http/http_parser.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pico_http/config/limits.hpp"
#include "pico_http/config/profile.hpp"
#include "pico_http/http/headers.hpp"

using pico_http::DefaultHttpLimits;
using pico_http::HttpParser;
using pico_http::P8Rp2350HttpLimits;
using pico_http::ParseResult;

namespace {

std::span<const std::uint8_t> bytes_of(std::string_view text) {
    // Viewing the same bytes through the unsigned char type the parser
    // takes. unsigned char may alias; the required bit_cast diagnostic
    // is suppressed at this test-site conversion.
    // NOLINTNEXTLINE(bugprone-bitwise-pointer-cast)
    return {std::bit_cast<const std::uint8_t*>(text.data()), text.size()};
}

// Feed the whole request in one shot.
template <typename Limits = DefaultHttpLimits>
ParseResult parse_all(HttpParser<Limits>& parser, std::string_view request,
                      std::size_t& consumed) {
    return parser.feed(bytes_of(request), consumed);
}

// Everything a parse produces, so segmentation equivalence is asserted on the
// parser's *output* and not merely on its verdict enum. These fields are the
// state that survives a feed() boundary -- scratch_ accumulating a token,
// body_len_ indexing the body -- so a defect in exactly that state is what
// comparing only the enum cannot see.
struct ParseOutcome {
    ParseResult status = ParseResult::INCOMPLETE;
    std::size_t consumed = 0;
    std::string method;
    std::string uri;
    std::string version;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    bool operator==(const ParseOutcome&) const = default;
};

std::ostream& operator<<(std::ostream& out, const ParseOutcome& outcome) {
    out << "{status=" << static_cast<int>(outcome.status)
        << " consumed=" << outcome.consumed << " method='" << outcome.method
        << "' uri='" << outcome.uri << "' version='" << outcome.version
        << "' headers=[";
    for (const auto& [name, value] : outcome.headers) {
        out << name << ": " << value << "; ";
    }
    return out << "] body='" << outcome.body << "'}";
}

template <typename Limits>
ParseOutcome snapshot(const HttpParser<Limits>& parser, ParseResult status,
                      std::size_t consumed) {
    ParseOutcome outcome;
    outcome.status = status;
    outcome.consumed = consumed;
    outcome.method = std::string(parser.method());
    outcome.uri = std::string(parser.uri());
    outcome.version = std::string(parser.version());
    for (const auto& header : parser.headers()) {
        outcome.headers.emplace_back(std::string(header.name.view()),
                                     std::string(header.value.view()));
    }
    outcome.body.assign(parser.body().begin(), parser.body().end());
    return outcome;
}

// The whole request in one feed().
template <typename Limits = DefaultHttpLimits>
ParseOutcome parse_whole(std::string_view request) {
    HttpParser<Limits> parser;
    std::size_t consumed = 0;
    const ParseResult status = parser.feed(bytes_of(request), consumed);
    return snapshot(parser, status, consumed);
}

// Cut once at `split`, then the remainder.
template <typename Limits = DefaultHttpLimits>
ParseOutcome parse_split(std::string_view request, std::size_t split) {
    HttpParser<Limits> parser;
    auto data = bytes_of(request);
    std::size_t off = 0;
    ParseResult status = ParseResult::INCOMPLETE;
    for (const std::size_t end : {split, request.size()}) {
        while (off < end && status == ParseResult::INCOMPLETE) {
            std::size_t count = 0;
            status = parser.feed(data.subspan(off, end - off), count);
            off += count;
        }
    }
    return snapshot(parser, status, off);
}

// One byte per feed() -- the finest segmentation there is.
template <typename Limits = DefaultHttpLimits>
ParseOutcome parse_per_byte(std::string_view request) {
    HttpParser<Limits> parser;
    auto data = bytes_of(request);
    ParseResult status = ParseResult::INCOMPLETE;
    std::size_t off = 0;
    while (off < data.size()) {
        std::size_t consumed = 0;
        status = parser.feed(data.subspan(off, 1), consumed);
        off += consumed;
        if (status != ParseResult::INCOMPLETE) {
            break;
        }
    }
    return snapshot(parser, status, off);
}

// EN-04.5. Segmentation is chosen by the network, so every cut of the same
// bytes must produce the same outcome -- the verdict, the bytes consumed and
// every parsed field. Asserting the verdict alone leaves the cross-call state
// unchecked, and that state is the only thing segmentation exercises.
template <typename Limits = DefaultHttpLimits>
void expect_segmentation_independent(std::string_view request) {
    const ParseOutcome expected = parse_whole<Limits>(request);
    for (std::size_t split = 0; split <= request.size(); ++split) {
        EXPECT_EQ(parse_split<Limits>(request, split), expected)
            << "split=" << split;
    }
    EXPECT_EQ(parse_per_byte<Limits>(request), expected)
        << "one byte at a time";
}

}  // namespace

TEST(HttpParserTest, ParsesSimpleGet) {
    HttpParser<> parser;
    std::size_t consumed = 0;
    auto status =
        parse_all(parser, "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n", consumed);
    EXPECT_EQ(status, ParseResult::COMPLETE);
    EXPECT_EQ(parser.method(), "GET");
    EXPECT_EQ(parser.uri(), "/hello");
    EXPECT_EQ(parser.version(), "HTTP/1.1");
    EXPECT_EQ(parser.headers().get("Host"), "x");
    EXPECT_TRUE(parser.body().empty());
}

TEST(HttpParserTest, ParsesBodyByContentLength) {
    HttpParser<> parser;
    std::size_t consumed = 0;
    auto status = parse_all(
        parser, "POST /u HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello",
        consumed);
    EXPECT_EQ(status, ParseResult::COMPLETE);
    ASSERT_EQ(parser.body().size(), 5U);
    const std::string body(parser.body().begin(), parser.body().end());
    EXPECT_EQ(body, "hello");
    EXPECT_EQ(parser.content_length(), 5U);
}

TEST(HttpParserTest, TrimsHeaderOptionalWhitespace) {
    HttpParser<> parser;
    std::size_t consumed = 0;
    parse_all(parser, "GET / HTTP/1.1\r\nX-Test:   spaced\t \r\n\r\n",
              consumed);
    EXPECT_EQ(parser.headers().get("X-Test"), "spaced");
}

TEST(HttpParserTest, AFullLengthHeaderValueIsAcceptedAndStoredWhole) {
    // The advertised cap is the enforced cap: MAX_HEADER_LINE is derived from
    // the storage caps, so a full-length name and a full-length value fit on
    // one line instead of being rejected at 431 by a shorter line limit.
    HttpParser<> parser;
    const std::string name(pico_http::HTTPHeaders::MAX_HEADER_NAME, 'N');
    const std::string value(pico_http::HTTPHeaders::MAX_HEADER_VALUE, 'v');
    const std::string request =
        "GET / HTTP/1.1\r\nHost: x\r\n" + name + ": " + value + "\r\n\r\n";

    std::size_t consumed = 0;
    const ParseResult status = parse_all(parser, request, consumed);

    ASSERT_EQ(status, ParseResult::COMPLETE);
    EXPECT_EQ(parser.headers().get(name), value);
}

TEST(HttpParserTest, AHeaderValueOneOverTheCapIsRejected) {
    HttpParser<> parser;
    const std::string name(pico_http::HTTPHeaders::MAX_HEADER_NAME, 'N');
    const std::string value(pico_http::HTTPHeaders::MAX_HEADER_VALUE + 1, 'v');
    const std::string request =
        "GET / HTTP/1.1\r\nHost: x\r\n" + name + ": " + value + "\r\n\r\n";

    std::size_t consumed = 0;
    const ParseResult status = parse_all(parser, request, consumed);

    EXPECT_EQ(status, ParseResult::HEADER_FIELDS_TOO_LARGE);
}

TEST(HttpParserTest, IncompleteWhenHeadUnterminated) {
    HttpParser<> parser;
    std::size_t consumed = 0;
    auto status = parse_all(parser, "GET / HTTP/1.1\r\nHost: x\r\n", consumed);
    EXPECT_EQ(status, ParseResult::INCOMPLETE);
    EXPECT_EQ(consumed,
              std::string_view("GET / HTTP/1.1\r\nHost: x\r\n").size());
}

TEST(HttpParserTest, SegmentationIndependenceAcrossEverySplit) {
    // The parser's defining property: identical verdict no matter
    // where the byte stream is cut. Exhaustively splits at every offset plus
    // the one-byte-at-a-time extreme.
    const std::vector<std::string> requests = {
        "GET /a HTTP/1.1\r\nHost: h\r\n\r\n",
        "POST /b HTTP/1.0\r\nContent-Length: 4\r\nX: y\r\n\r\ndata",
        "GET / HTTP/1.1\r\nHost: h\r\n\r\n",
    };
    for (const auto& req : requests) {
        SCOPED_TRACE("req='" + req + "'");
        ASSERT_EQ(parse_whole(req).status, ParseResult::COMPLETE);
        expect_segmentation_independent(req);
    }
}

TEST(HttpParserTest, PipeliningLeavesTrailingBytesForNextRequest) {
    HttpParser<> parser;
    const std::string two =
        "GET /1 HTTP/1.1\r\nHost: h\r\n\r\nGET /2 HTTP/1.1\r\nHost: h\r\n\r\n";
    auto data = bytes_of(two);

    std::size_t consumed = 0;
    EXPECT_EQ(parser.feed(data, consumed), ParseResult::COMPLETE);
    EXPECT_EQ(parser.uri(), "/1");
    EXPECT_LT(consumed, two.size());

    parser.reset();
    std::size_t consumed2 = 0;
    EXPECT_EQ(parser.feed(data.subspan(consumed), consumed2),
              ParseResult::COMPLETE);
    EXPECT_EQ(parser.uri(), "/2");
}

// --- Boundary + conformance matrix (BR-1..9, NR-1/NR-3) ---------------------

namespace {
struct TinyLimits {
    static constexpr std::size_t MAX_REQUEST_LINE = 40;
    static constexpr std::size_t MAX_METHOD = 8;
    static constexpr std::size_t MAX_URI = 16;
    static constexpr std::size_t MAX_VERSION = 16;
    static constexpr std::size_t MAX_HEADER_LINE = 24;
    static constexpr std::size_t MAX_HEADERS = 16;
    static constexpr std::size_t MAX_REQUEST_HEAD = 80;
    static constexpr std::size_t MAX_BUFFERED_BODY = 16;
};

// As TinyLimits, but with the head and line caps raised so that the header
// *count* is the limit reached first. Without this the head cap bites and a
// test named for the count would be measuring something else.
struct LooseHeadLimits {
    static constexpr std::size_t MAX_REQUEST_LINE = 40;
    static constexpr std::size_t MAX_METHOD = 8;
    static constexpr std::size_t MAX_URI = 16;
    static constexpr std::size_t MAX_VERSION = 16;
    static constexpr std::size_t MAX_HEADER_LINE = 64;
    // MAX_HEADERS is pinned to HTTPHeaders::MAX_HEADERS by a static_assert in
    // the parser, so only the head and line caps move here.
    static constexpr std::size_t MAX_HEADERS = 16;
    static constexpr std::size_t MAX_REQUEST_HEAD = 512;
    static constexpr std::size_t MAX_BUFFERED_BODY = 16;
};

// The verdict for `request`, having first established that it is the same
// verdict -- and the same everything else -- at every segmentation.
//
// The check lives here rather than at the call sites because every boundary and
// conformance case in this file routes through this helper, and those are the
// branches carrying the most state across a feed() boundary: host_count_,
// cl_present_, te_present_, chunk_size_has_digit_, overhead_bytes_, rl_bytes_.
// Feeding each of them exactly once, whole, tested every one at one
// segmentation and no other.
template <typename Limits>
ParseResult verdict(std::string_view request) {
    expect_segmentation_independent<Limits>(request);
    return parse_whole<Limits>(request).status;
}
}  // namespace

// EN-04.4. Every cap below had a test on the "one over" side and none on the
// cap itself, so an off-by-one that reads as a correction -- `>` where the code
// says `>=` -- moved the rejection by one byte with nothing to notice. The
// at-cap case is the contract half: the library advertises these constants over
// /limits, so "the advertised cap is a usable length" is a promise, not an
// implementation detail. Each pair below is one below-or-at, one over.
TEST(HttpParserBoundaryTest, AUriOfExactlyMaxUriIsAccepted) {
    // 16 characters, the advertised cap.
    const std::string uri = "/" + std::string(TinyLimits::MAX_URI - 1, 'u');
    ASSERT_EQ(uri.size(), TinyLimits::MAX_URI);
    const std::string request = "GET " + uri + " HTTP/1.1\r\nHost: h\r\n\r\n";

    EXPECT_EQ(verdict<TinyLimits>(request), ParseResult::COMPLETE);
    EXPECT_EQ(parse_whole<TinyLimits>(request).uri, uri);
}

TEST(HttpParserBoundaryTest, AHeaderLineOfExactlyMaxHeaderLineIsAccepted) {
    // "X: " plus padding, totalling exactly MAX_HEADER_LINE before the CRLF.
    const std::string value(TinyLimits::MAX_HEADER_LINE - 3, 'v');
    const std::string line = "X: " + value;
    ASSERT_EQ(line.size(), TinyLimits::MAX_HEADER_LINE);
    // Host is not optional on HTTP/1.1 -- without it the verdict is
    // BAD_REQUEST for a reason that has nothing to do with the cap under test.
    const std::string request =
        "GET / HTTP/1.1\r\nHost: h\r\n" + line + "\r\n\r\n";

    EXPECT_EQ(verdict<TinyLimits>(request), ParseResult::COMPLETE);
    // Entry 0 is Host; entry 1 is the line under test, stored whole.
    EXPECT_EQ(parse_whole<TinyLimits>(request).headers.at(1).second, value);
}

TEST(HttpParserBoundaryTest, AHeadOfExactlyMaxRequestHeadIsAccepted) {
    // Filled with short header lines, then one final line sized to land the
    // head exactly on the cap. Padding a single header instead would exceed
    // MAX_HEADER_LINE and be rejected for the wrong reason.
    const std::size_t tail = std::string_view("\r\n").size();
    std::string request = "GET / HTTP/1.1\r\nHost: h\r\n";
    int filler = 0;
    while (TinyLimits::MAX_REQUEST_HEAD - request.size() - tail >
           TinyLimits::MAX_HEADER_LINE) {
        request += "H" + std::to_string(filler++) + ": v\r\n";
    }
    const std::size_t last =
        TinyLimits::MAX_REQUEST_HEAD - request.size() - tail;
    ASSERT_GE(last, std::string_view("Z: \r\n").size());
    request +=
        "Z: " + std::string(last - std::string_view("Z: \r\n").size(), 'z') +
        "\r\n";
    request += "\r\n";
    ASSERT_EQ(request.size(), TinyLimits::MAX_REQUEST_HEAD);

    EXPECT_EQ(verdict<TinyLimits>(request), ParseResult::COMPLETE);

    // And one byte more is over: the pair is what pins the comparison.
    const std::string over = request.substr(0, request.size() - tail) + "y\r\n";
    EXPECT_EQ(verdict<TinyLimits>(over), ParseResult::HEADER_FIELDS_TOO_LARGE);
}

TEST(HttpParserBoundaryTest, ExactlyMaxHeadersAreAccepted) {
    std::string request = "GET / HTTP/1.1\r\n";
    // Host counts towards the header total, so it is one of the MAX_HEADERS.
    request += "Host: h\r\n";
    for (std::size_t i = 1; i < TinyLimits::MAX_HEADERS; ++i) {
        request += "H" + std::to_string(i) + ": v\r\n";
    }
    request += "\r\n";

    // Head cap would bite first under TinyLimits, so this one uses the default
    // limits with their own header count.
    using CountLimits = LooseHeadLimits;
    EXPECT_EQ(verdict<CountLimits>(request), ParseResult::COMPLETE);
    EXPECT_EQ(parse_whole<CountLimits>(request).headers.size(),
              CountLimits::MAX_HEADERS);
}

// MAX_METHOD and MAX_VERSION had no test on either side. Their static_asserts
// bound them against the scratch buffer, which is a sizing relation rather than
// a behavioural one: neither says what the parser does with a token of exactly
// that length, or one byte more.
TEST(HttpParserBoundaryTest, AMethodOfExactlyMaxMethodIsParsedNotRejected) {
    const std::string method(TinyLimits::MAX_METHOD, 'M');
    const std::string request = method + " / HTTP/1.1\r\nHost: h\r\n\r\n";

    // Parsed as a token; whether the server implements it is the router's
    // question, not the parser's, so the verdict must not be BAD_REQUEST.
    const ParseResult status = verdict<TinyLimits>(request);
    EXPECT_NE(status, ParseResult::BAD_REQUEST);
    EXPECT_EQ(parse_whole<TinyLimits>(request).method, method);
}

TEST(HttpParserBoundaryTest, AMethodOneOverMaxMethodIsRejected) {
    const std::string method(TinyLimits::MAX_METHOD + 1, 'M');
    const std::string request = method + " / HTTP/1.1\r\nHost: h\r\n\r\n";

    EXPECT_EQ(verdict<TinyLimits>(request), ParseResult::BAD_REQUEST);
}

TEST(HttpParserBoundaryTest, AVersionOneOverMaxVersionIsRejected) {
    const std::string version(TinyLimits::MAX_VERSION + 1, 'V');
    const std::string request = "GET / " + version + "\r\nHost: h\r\n\r\n";

    EXPECT_EQ(verdict<TinyLimits>(request), ParseResult::BAD_REQUEST);
}

TEST(HttpParserBoundaryTest, OverlongUriIsUriTooLong) {
    // URI exceeds TinyLimits::MAX_URI (16).
    EXPECT_EQ(
        verdict<TinyLimits>("GET /this-uri-is-way-too-long HTTP/1.1\r\n\r\n"),
        ParseResult::URI_TOO_LONG);
}

TEST(HttpParserBoundaryTest, OverlongHeaderLineIsHeaderFieldsTooLarge) {
    EXPECT_EQ(verdict<TinyLimits>(
                  "GET / HTTP/1.1\r\nX: aaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n\r\n"),
              ParseResult::HEADER_FIELDS_TOO_LARGE);
}

TEST(HttpParserBoundaryTest, OverlongHeadIsHeaderFieldsTooLarge) {
    // Many small headers that together exceed MAX_REQUEST_HEAD (80).
    std::string req = "GET / HTTP/1.1\r\n";
    for (int i = 0; i < 12; ++i) {
        req += "Ab: cd\r\n";
    }
    req += "\r\n";
    EXPECT_EQ(verdict<TinyLimits>(req), ParseResult::HEADER_FIELDS_TOO_LARGE);
}

TEST(HttpParserBoundaryTest, OversizedBodyIsPayloadTooLarge) {
    EXPECT_EQ(verdict<TinyLimits>(
                  "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 999\r\n\r\n"),
              ParseResult::PAYLOAD_TOO_LARGE);
}

TEST(HttpParserProfileTest, Rp2350ProfileRaisesBufferedBodyCapToEightKiB) {
    // The P8-RP2350 profile raises MAX_BUFFERED_BODY from the
    // default 2048 to 8192. A declared length at the cap parses to completion;
    // one byte over is rejected; and a length the RP2350 cap admits is still
    // rejected under the default profile — proving the profile is what moved
    // it.
    const std::string over =
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 8193\r\n\r\n";
    EXPECT_EQ(verdict<P8Rp2350HttpLimits>(over),
              ParseResult::PAYLOAD_TOO_LARGE);

    const std::string at_cap =
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 8192\r\n\r\n" +
        std::string(8192, 'a');
    HttpParser<P8Rp2350HttpLimits> parser;
    std::size_t consumed = 0;
    EXPECT_EQ(parser.feed(bytes_of(at_cap), consumed), ParseResult::COMPLETE);
    EXPECT_EQ(parser.body().size(), 8192U);

    // 4096 is under 8192 but over the default 2048: accepted here, rejected by
    // the default profile.
    const std::string mid =
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 4096\r\n\r\n";
    EXPECT_EQ(verdict<P8Rp2350HttpLimits>(mid), ParseResult::INCOMPLETE);
    EXPECT_EQ(verdict<DefaultHttpLimits>(mid), ParseResult::PAYLOAD_TOO_LARGE);
}

TEST(HttpParserTest, DefaultHeaderCountOverflowIsHeaderFieldsTooLarge) {
    // HTTPHeaders holds 16 entries; the 17th distinct header overflows -> 431.
    std::string req = "GET / HTTP/1.1\r\n";
    for (int i = 0; i < 20; ++i) {
        req += "H" + std::to_string(i) + ": v\r\n";
    }
    req += "\r\n";
    EXPECT_EQ(verdict<DefaultHttpLimits>(req),
              ParseResult::HEADER_FIELDS_TOO_LARGE);
}

// BR-8: malformed syntax (charset, request-line spacing, header framing,
// control chars in a field value) is a 400.
TEST(HttpParserConformanceTest, RejectsMalformedRequestLines) {
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET /only-two-fields\r\n\r\n"),
              ParseResult::BAD_REQUEST);
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET  /x HTTP/1.1\r\n\r\n"),
              ParseResult::BAD_REQUEST);  // double space -> empty URI field
    EXPECT_EQ(verdict<DefaultHttpLimits>("GE T /x HTTP/1.1\r\n\r\n"),
              ParseResult::BAD_REQUEST);  // space in method
}

TEST(HttpParserConformanceTest, RejectsBareLfInRequestLine) {
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET /x HTTP/1.1\n\r\n"),
              ParseResult::BAD_REQUEST);
}

TEST(HttpParserConformanceTest, IgnoresExactlyOneLeadingCrlf) {
    // RFC 9112 §2.2: ignore at least one empty line before the request-line
    // (clients commonly emit a stray CRLF after a request body). Exactly one
    // is tolerated; a second, or a bare LF, is still malformed.
    EXPECT_EQ(
        verdict<DefaultHttpLimits>("\r\nGET / HTTP/1.1\r\nHost: h\r\n\r\n"),
        ParseResult::COMPLETE);
    EXPECT_EQ(
        verdict<DefaultHttpLimits>("\r\n\r\nGET / HTTP/1.1\r\nHost: h\r\n\r\n"),
        ParseResult::BAD_REQUEST);
    EXPECT_EQ(verdict<DefaultHttpLimits>("\nGET / HTTP/1.1\r\nHost: h\r\n\r\n"),
              ParseResult::BAD_REQUEST);
}

TEST(HttpParserConformanceTest, LeadingCrlfCountsAsRequestInProgress) {
    // A consumed leading CRLF means bytes were taken: the connection is
    // mid-request for timeout purposes, not idle.
    HttpParser<> parser;
    std::size_t consumed = 0;
    parser.feed(bytes_of("\r\n"), consumed);
    EXPECT_EQ(consumed, 2U);
    EXPECT_TRUE(parser.in_progress());
}

TEST(HttpParserConformanceTest, TransferEncodingOnHttp10IsBadRequest) {
    // Transfer-Encoding did not exist in HTTP/1.0; a 1.0 message carrying it
    // has faulty framing and must be rejected, never decoded (RFC 9112 §6.1).
    EXPECT_EQ(verdict<DefaultHttpLimits>(
                  "POST /u HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n"
                  "3\r\nabc\r\n0\r\n\r\n"),
              ParseResult::BAD_REQUEST);
}

TEST(HttpParserConformanceTest, UnknownExpectationIsExpectationFailed) {
    // The only expectation implemented is 100-continue; silently ignoring any
    // other would misrepresent the server's behaviour (RFC 9110 §10.1.1).
    EXPECT_EQ(verdict<DefaultHttpLimits>(
                  "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 2\r\n"
                  "Expect: 200-maybe\r\n\r\nhi"),
              ParseResult::EXPECTATION_FAILED);
    EXPECT_EQ(
        verdict<DefaultHttpLimits>("POST / HTTP/1.0\r\nContent-Length: 2\r\n"
                                   "Expect: 200-maybe\r\n\r\nhi"),
        ParseResult::EXPECTATION_FAILED);
}

// NR-10: repeated field names combine into one comma-separated value.
TEST(HttpParserConformanceTest, RepeatedHeaderFieldsCombineAsList) {
    // Repeated field names combine into one comma-separated value (RFC 9110
    // §5.2). Last-one-wins replacement would let a second Connection line
    // silently erase an earlier "close".
    HttpParser<> parser;
    std::size_t consumed = 0;
    auto status = parse_all(parser,
                            "GET / HTTP/1.1\r\nHost: h\r\n"
                            "X-Token: a\r\nX-Token: b\r\n"
                            "Connection: close\r\nConnection: keep-alive\r\n"
                            "\r\n",
                            consumed);
    EXPECT_EQ(status, ParseResult::COMPLETE);
    EXPECT_EQ(parser.headers().get("X-Token"), "a, b");
    EXPECT_EQ(parser.headers().get("Connection"), "close, keep-alive");
}

// NR-10: each field fits by itself; combining must fail whole with 431.
TEST(HttpParserConformanceTest, RepeatedHeaderValueOverflowIs431) {
    const std::string first(pico_http::HTTPHeaders::MAX_HEADER_VALUE - 4, 'a');
    const std::string request =
        "GET / HTTP/1.1\r\nHost: h\r\nX: " + first + "\r\nX: bbbb\r\n\r\n";
    EXPECT_EQ(verdict<DefaultHttpLimits>(request),
              ParseResult::HEADER_FIELDS_TOO_LARGE);
}

TEST(HttpParserConformanceTest, RejectsObsoleteLineFolding) {
    EXPECT_EQ(
        verdict<DefaultHttpLimits>("GET / HTTP/1.1\r\nX: a\r\n b\r\n\r\n"),
        ParseResult::BAD_REQUEST);
}

TEST(HttpParserConformanceTest, RejectsSpaceBeforeColon) {
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET / HTTP/1.1\r\nBad : v\r\n\r\n"),
              ParseResult::BAD_REQUEST);
}

TEST(HttpParserConformanceTest, ContentLengthHygiene) {
    EXPECT_EQ(
        verdict<DefaultHttpLimits>(
            "POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n"
            "\r\nx"),
        ParseResult::BAD_REQUEST);  // duplicate CL
    EXPECT_EQ(verdict<DefaultHttpLimits>(
                  "POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n"),
              ParseResult::BAD_REQUEST);  // non-numeric CL
    EXPECT_EQ(
        verdict<DefaultHttpLimits>(
            "POST / HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n"),
        ParseResult::PAYLOAD_TOO_LARGE);  // overflow
}

TEST(HttpParserChunkedTest, DecodesChunkedBody) {
    HttpParser<> parser;
    std::size_t consumed = 0;
    auto status = parse_all(
        parser,
        "POST /u HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n",
        consumed);
    EXPECT_EQ(status, ParseResult::COMPLETE);
    const std::string body(parser.body().begin(), parser.body().end());
    EXPECT_EQ(body, "hello world");
}

TEST(HttpParserChunkedTest, DecodesChunkedAcrossEverySplit) {
    const std::string req =
        "POST /u HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3\r\nabc\r\n0\r\n\r\n";
    // The decoded body is the point: CHUNK_DATA appends at body_len_, which is
    // state carried across the boundary, so a split that lost it would still
    // report COMPLETE while producing a truncated or overwritten body.
    const ParseOutcome whole = parse_whole(req);
    ASSERT_EQ(whole.status, ParseResult::COMPLETE);
    ASSERT_EQ(whole.body, "abc");
    expect_segmentation_independent(req);
}

TEST(HttpParserChunkedTest, RejectsControlBytesInExtensionsAndTrailers) {
    // Extensions and trailers are skipped, not parsed, but control octets
    // (e.g. a bare LF or NUL) are still rejected — the same field-syntax rule
    // as headers, so nothing can hide framing from a laxer parser downstream.
    EXPECT_EQ(
        verdict<DefaultHttpLimits>(
            "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
            "3;ext=\na\r\nabc\r\n0\r\n\r\n"),
        ParseResult::BAD_REQUEST);
    std::string nul_trailer =
        "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3\r\nabc\r\n0\r\nX-T: ";
    nul_trailer += '\0';
    nul_trailer += "v\r\n\r\n";
    EXPECT_EQ(verdict<DefaultHttpLimits>(nul_trailer),
              ParseResult::BAD_REQUEST);
}

TEST(HttpParserChunkedTest, IgnoresChunkExtensionsAndTrailers) {
    HttpParser<> parser;
    std::size_t consumed = 0;
    auto status = parse_all(
        parser,
        "POST /u HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3;name=value\r\nabc\r\n0\r\nX-Trailer: t\r\n\r\n",
        consumed);
    EXPECT_EQ(status, ParseResult::COMPLETE);
    const std::string body(parser.body().begin(), parser.body().end());
    EXPECT_EQ(body, "abc");
}

TEST(HttpParserChunkedTest, RejectsUnsupportedTransferEncoding) {
    EXPECT_EQ(
        verdict<DefaultHttpLimits>(
            "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip\r\n\r\n"),
        ParseResult::NOT_IMPLEMENTED);
}

TEST(HttpParserChunkedTest, OversizedDecodedChunkBodyIsPayloadTooLarge) {
    // Default buffered-body cap is 2048; a single 0x1000 (4096) byte chunk
    // overflows it and must be rejected during decode (not silently truncated).
    std::string req =
        "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
    req += "1000\r\n" + std::string(4096, 'a') + "\r\n0\r\n\r\n";
    EXPECT_EQ(verdict<DefaultHttpLimits>(req), ParseResult::PAYLOAD_TOO_LARGE);
}

TEST(HttpParserChunkedTest, EmptyChunkSizeLineIsRejectedNotLastChunk) {
    // RFC 9112 §7.1 requires 1*HEXDIG. A bare CRLF where a chunk size belongs
    // must not alias the "0" last-chunk: an early terminator would let the
    // remaining chunk data be reparsed as the next request (a
    // request-smuggling shape).
    EXPECT_EQ(
        verdict<DefaultHttpLimits>(
            "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
            "\r\n\r\n"),
        ParseResult::BAD_REQUEST);
    // A chunk extension must not stand in for the missing size either.
    EXPECT_EQ(
        verdict<DefaultHttpLimits>(
            "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
            ";ext=1\r\n\r\n"),
        ParseResult::BAD_REQUEST);
    // Same rule for every subsequent chunk-size line, not just the first.
    EXPECT_EQ(
        verdict<DefaultHttpLimits>(
            "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
            "3\r\nabc\r\n\r\n\r\n"),
        ParseResult::BAD_REQUEST);
}

TEST(HttpParserTest, InProgressTracksMidRequestWithoutStartedFlag) {
    HttpParser<> parser;
    EXPECT_FALSE(parser.in_progress());  // fresh parser: idle, not mid-request
    std::size_t consumed = 0;
    parser.feed(bytes_of("G"), consumed);
    EXPECT_TRUE(parser.in_progress());  // one method byte taken
    parser.reset();
    EXPECT_FALSE(parser.in_progress());  // reset clears it
    parser.feed(bytes_of("GET /x HTTP/1.1\r\nHost: a\r"), consumed);
    EXPECT_TRUE(parser.in_progress());  // mid-header
    parser.feed(bytes_of("\n\r\n"), consumed);
    EXPECT_TRUE(parser.complete());
    EXPECT_FALSE(parser.in_progress());  // completed: no longer in progress
}

TEST(HttpParserChunkedTest, RejectsStackedTransferEncodings) {
    // Only a lone "chunked" can be framed; any coding stacked with it (which we
    // cannot decode) must be 501, never decoded as if the extra coding were
    // absent (BR-9).
    EXPECT_EQ(verdict<DefaultHttpLimits>(
                  "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip, "
                  "chunked\r\n\r\n"),
              ParseResult::NOT_IMPLEMENTED);
    EXPECT_EQ(verdict<DefaultHttpLimits>(
                  "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked, "
                  "gzip\r\n\r\n"),
              ParseResult::NOT_IMPLEMENTED);
    // Two Transfer-Encoding headers combine to more than one coding: also 501.
    EXPECT_EQ(verdict<DefaultHttpLimits>(
                  "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip\r\n"
                  "Transfer-Encoding: chunked\r\n\r\n"),
              ParseResult::NOT_IMPLEMENTED);
}

TEST(HttpParserChunkedTest, OverflowingChunkSizeIsRejectedNotWrapped) {
    // A 17-hex-digit size (2^64) would wrap uint64 to 0 and be misread as the
    // last chunk, letting the real payload be reparsed as a new request. It
    // must be rejected instead (request-smuggling defense).
    std::string req =
        "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
    req += "10000000000000000\r\nZZZZ\r\n0\r\n\r\n";
    EXPECT_EQ(verdict<DefaultHttpLimits>(req), ParseResult::PAYLOAD_TOO_LARGE);
}

TEST(HttpParserConformanceTest, DetectsExpectContinue) {
    HttpParser<> parser;
    std::size_t consumed = 0;
    parse_all(parser,
              "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 2\r\n"
              "Expect: 100-continue\r\n\r\nhi",
              consumed);
    EXPECT_TRUE(parser.expect_continue());
}

TEST(HttpParserConformanceTest, ContentLengthPlusTransferEncodingIsBadRequest) {
    EXPECT_EQ(verdict<DefaultHttpLimits>(
                  "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n"
                  "Transfer-Encoding: chunked\r\n\r\nabc"),
              ParseResult::BAD_REQUEST);
}

// --- RFC 9110/9112 compliance matrix (BR-10/11, NR-1/7/8) -------------------

TEST(HttpParserConformanceTest, VersionHandling) {
    // A well-formed HTTP-version the server does not speak is 505, not 400
    // (RFC 9110 §15.6.6): the request line is valid, the version is the
    // problem.
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET /x HTTP/2.0\r\n\r\n"),
              ParseResult::HTTP_VERSION_NOT_SUPPORTED);
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET /x HTTP/1.9\r\n\r\n"),
              ParseResult::HTTP_VERSION_NOT_SUPPORTED);
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET /x HTTP/9.9\r\n\r\n"),
              ParseResult::HTTP_VERSION_NOT_SUPPORTED);
    // A token that is not HTTP-version syntax ("HTTP/" DIGIT "." DIGIT) is a
    // malformed request line -> 400, never 505.
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET /x HTTP/1.11\r\n\r\n"),
              ParseResult::BAD_REQUEST);
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET /x HTTP/1.\r\n\r\n"),
              ParseResult::BAD_REQUEST);
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET /x HTX/1.1\r\n\r\n"),
              ParseResult::BAD_REQUEST);
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET /x hq\r\n\r\n"),
              ParseResult::BAD_REQUEST);
}

// NR-7 / BR-10: missing or duplicated Host on HTTP/1.1 is a 400.
TEST(HttpParserConformanceTest, Http11RequiresExactlyOneHost) {
    // RFC 9112 §3.2: an HTTP/1.1 request must carry exactly one Host header.
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET / HTTP/1.1\r\nHost: a\r\n\r\n"),
              ParseResult::COMPLETE);
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET / HTTP/1.1\r\n\r\n"),
              ParseResult::BAD_REQUEST);  // missing Host
    EXPECT_EQ(verdict<DefaultHttpLimits>(
                  "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n"),
              ParseResult::BAD_REQUEST);  // duplicate Host
    // HTTP/1.0 has no Host requirement: a missing Host is still a valid
    // request.
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET / HTTP/1.0\r\n\r\n"),
              ParseResult::COMPLETE);
}

TEST(HttpParserConformanceTest, RejectsControlCharsInFieldValue) {
    // A field value may hold VCHAR, SP and HTAB — never other controls or DEL
    // (RFC 9110 §5.5). HTAB stays valid; a C0 control or DEL is rejected.
    EXPECT_EQ(verdict<DefaultHttpLimits>(
                  "GET / HTTP/1.1\r\nHost: h\r\nX: a\tb\r\n\r\n"),
              ParseResult::COMPLETE);  // HTAB is allowed in a value
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET / HTTP/1.1\r\nHost: h\r\nX: a\x01"
                                         "b\r\n\r\n"),
              ParseResult::BAD_REQUEST);  // C0 control
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET / HTTP/1.1\r\nHost: h\r\nX: a\x7f"
                                         "b\r\n\r\n"),
              ParseResult::BAD_REQUEST);  // DEL
}

// --- Fuzz-shaped robustness: the parser must always terminate and never stall,
// crash, or over-consume on hostile input (the libFuzzer target drives the same
// invariants over generated inputs; this keeps the corpus in CI). ------------

namespace {

// Drive the parser over `input`, chopped into small data-dependent chunks to
// exercise segmentation. Asserts it upholds its contract on any byte sequence.
void drive_hostile_input(std::span<const std::uint8_t> input) {
    HttpParser<DefaultHttpLimits> parser;
    std::size_t off = 0;
    while (off < input.size()) {
        std::size_t chunk = 1U + (input[off] % 7U);
        chunk = std::min(chunk, input.size() - off);
        std::size_t consumed = 0;
        const ParseResult status =
            parser.feed(input.subspan(off, chunk), consumed);
        ASSERT_LE(consumed, chunk);
        off += consumed;
        // Above the verdict dispatch, so progress is required on every pass.
        // That bounds the loop by input.size() on its own; the iteration cap
        // this replaces sat below the stall check and so could never fire.
        ASSERT_GT(consumed, 0U) << "parser stalled without consuming input";
        if (status == ParseResult::COMPLETE) {
            parser.reset();
            continue;  // trailing bytes belong to the next request
        }
        if (status != ParseResult::INCOMPLETE) {
            return;  // a definite failure ends the connection
        }
    }
}

std::span<const std::uint8_t> raw(std::string_view text) {
    // Viewing the same bytes through the unsigned char type the parser
    // takes. unsigned char may alias; the required bit_cast diagnostic
    // is suppressed at this test-site conversion.
    // NOLINTNEXTLINE(bugprone-bitwise-pointer-cast)
    return {std::bit_cast<const std::uint8_t*>(text.data()), text.size()};
}

}  // namespace

TEST(HttpParserFuzzCorpus, HostileInputsTerminateSafely) {
    const std::vector<std::string> corpus = {
        "",
        " ",
        "\r\n\r\n",
        "GET",
        "GET ",
        "GET / ",
        "GET / HTTP/1.1",
        "GET / HTTP/1.1\r\n",
        std::string("GET /\0x HTTP/1.1\r\n\r\n", 20),  // embedded NUL
        "GET / HTTP/1.1\r\nX\x01: \x7f\r\n\r\n",       // CTL bytes
        "\xff\xfe\xfd\xfc",                            // high bytes
        std::string(5000, 'A'),                        // huge token
        "GET / HTTP/1.1\r\n" + std::string(5000, 'H') + ": v\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nab",  // truncated body
        "POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
        // One sample, split by the formatter at the column limit — the
        // implicit concatenation is intended.
        // NOLINTNEXTLINE(bugprone-suspicious-missing-comma)
        "POST / HTTP/1.1\r\nTransfer-Encoding: "
        "chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
        "GET / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nx",
        "GET /\r\n\r\n",
        ":::::\r\n\r\n",
        "GET  //  HTTP/1.1\r\n\r\n",
        "\n\n\n\n",
        "\r\nGET / HTTP/1.1\r\nHost: h\r\n\r\n",  // leading CRLF prelude
        "POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: h\r\nExpect: never\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: h\r\nHost: h\r\nA: 1\r\nA: 2\r\n\r\n",
    };
    for (const auto& sample : corpus) {
        drive_hostile_input(raw(sample));
    }
}
