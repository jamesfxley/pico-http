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
    // unsigned char may alias the input bytes.
    // bugprone-bitwise-pointer-cast: byte views preserve representation.
    // NOLINTNEXTLINE(bugprone-bitwise-pointer-cast)
    return {std::bit_cast<const std::uint8_t*>(text.data()), text.size()};
}

template <typename Limits = DefaultHttpLimits>
ParseResult parse_all(HttpParser<Limits>& parser, std::string_view request,
                      std::size_t& consumed) {
    return parser.feed(bytes_of(request), consumed);
}

// Comparing parsed fields catches corrupted cross-call state even when
// verdicts agree.
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

template <typename Limits = DefaultHttpLimits>
ParseOutcome parse_whole(std::string_view request) {
    HttpParser<Limits> parser;
    std::size_t consumed = 0;
    const ParseResult status = parser.feed(bytes_of(request), consumed);
    return snapshot(parser, status, consumed);
}

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

// Every segmentation must preserve verdict, consumed length and parsed
// fields.
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

// Boundary and conformance cases (BR-1..9, NR-1/NR-3).

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

// Raised head and line caps let the header-count limit fail first.
struct LooseHeadLimits {
    static constexpr std::size_t MAX_REQUEST_LINE = 40;
    static constexpr std::size_t MAX_METHOD = 8;
    static constexpr std::size_t MAX_URI = 16;
    static constexpr std::size_t MAX_VERSION = 16;
    static constexpr std::size_t MAX_HEADER_LINE = 64;
    static constexpr std::size_t MAX_HEADERS = 16;
    static constexpr std::size_t MAX_REQUEST_HEAD = 512;
    static constexpr std::size_t MAX_BUFFERED_BODY = 16;
};

// Every boundary and conformance case checks segmentation equivalence.
template <typename Limits>
ParseResult verdict(std::string_view request) {
    expect_segmentation_independent<Limits>(request);
    return parse_whole<Limits>(request).status;
}
}  // namespace

// At-cap cases detect limits that reject a usable byte too early.
TEST(HttpParserBoundaryTest, AUriOfExactlyMaxUriIsAccepted) {
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
    // Host prevents a missing-Host 400 from masking the cap check.
    const std::string request =
        "GET / HTTP/1.1\r\nHost: h\r\n" + line + "\r\n\r\n";

    EXPECT_EQ(verdict<TinyLimits>(request), ParseResult::COMPLETE);
    EXPECT_EQ(parse_whole<TinyLimits>(request).headers.at(1).second, value);
}

TEST(HttpParserBoundaryTest, AHeadOfExactlyMaxRequestHeadIsAccepted) {
    // Padding one header would exceed MAX_HEADER_LINE before the head cap.
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

    const std::string over = request.substr(0, request.size() - tail) + "y\r\n";
    EXPECT_EQ(verdict<TinyLimits>(over), ParseResult::HEADER_FIELDS_TOO_LARGE);
}

TEST(HttpParserBoundaryTest, ExactlyMaxHeadersAreAccepted) {
    std::string request = "GET / HTTP/1.1\r\n";
    request += "Host: h\r\n";
    for (std::size_t i = 1; i < TinyLimits::MAX_HEADERS; ++i) {
        request += "H" + std::to_string(i) + ": v\r\n";
    }
    request += "\r\n";

    using CountLimits = LooseHeadLimits;
    EXPECT_EQ(verdict<CountLimits>(request), ParseResult::COMPLETE);
    EXPECT_EQ(parse_whole<CountLimits>(request).headers.size(),
              CountLimits::MAX_HEADERS);
}

// Compile-time sizing assertions do not check token-length rejection
// boundaries.
TEST(HttpParserBoundaryTest, AMethodOfExactlyMaxMethodIsParsedNotRejected) {
    const std::string method(TinyLimits::MAX_METHOD, 'M');
    const std::string request = method + " / HTTP/1.1\r\nHost: h\r\n\r\n";

    // An unimplemented method token must not produce BAD_REQUEST in the
    // parser.
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

    const std::string mid =
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 4096\r\n\r\n";
    EXPECT_EQ(verdict<P8Rp2350HttpLimits>(mid), ParseResult::INCOMPLETE);
    EXPECT_EQ(verdict<DefaultHttpLimits>(mid), ParseResult::PAYLOAD_TOO_LARGE);
}

TEST(HttpParserTest, DefaultHeaderCountOverflowIsHeaderFieldsTooLarge) {
    std::string req = "GET / HTTP/1.1\r\n";
    for (int i = 0; i < 20; ++i) {
        req += "H" + std::to_string(i) + ": v\r\n";
    }
    req += "\r\n";
    EXPECT_EQ(verdict<DefaultHttpLimits>(req),
              ParseResult::HEADER_FIELDS_TOO_LARGE);
}

// Malformed syntax returns 400 (BR-8).
TEST(HttpParserConformanceTest, RejectsMalformedRequestLines) {
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET /only-two-fields\r\n\r\n"),
              ParseResult::BAD_REQUEST);
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET  /x HTTP/1.1\r\n\r\n"),
              ParseResult::BAD_REQUEST);
    EXPECT_EQ(verdict<DefaultHttpLimits>("GE T /x HTTP/1.1\r\n\r\n"),
              ParseResult::BAD_REQUEST);
}

TEST(HttpParserConformanceTest, RejectsBareLfInRequestLine) {
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET /x HTTP/1.1\n\r\n"),
              ParseResult::BAD_REQUEST);
}

TEST(HttpParserConformanceTest, IgnoresExactlyOneLeadingCrlf) {
    // RFC 9112 §2.2 asks servers to ignore at least one empty line before the
    // request-line. This parser tolerates exactly one CRLF. A second CRLF or a
    // bare LF is malformed.
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
    // A consumed leading CRLF starts request timing.
    HttpParser<> parser;
    std::size_t consumed = 0;
    parser.feed(bytes_of("\r\n"), consumed);
    EXPECT_EQ(consumed, 2U);
    EXPECT_TRUE(parser.in_progress());
}

TEST(HttpParserConformanceTest, TransferEncodingOnHttp10IsBadRequest) {
    // HTTP/1.0 Transfer-Encoding must be rejected as faulty framing (RFC 9112
    // §6.1).
    EXPECT_EQ(verdict<DefaultHttpLimits>(
                  "POST /u HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n"
                  "3\r\nabc\r\n0\r\n\r\n"),
              ParseResult::BAD_REQUEST);
}

TEST(HttpParserConformanceTest, UnknownExpectationIsExpectationFailed) {
    // Unimplemented expectations receive 417 (RFC 9110 §10.1.1).
    EXPECT_EQ(verdict<DefaultHttpLimits>(
                  "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 2\r\n"
                  "Expect: 200-maybe\r\n\r\nhi"),
              ParseResult::EXPECTATION_FAILED);
    EXPECT_EQ(
        verdict<DefaultHttpLimits>("POST / HTTP/1.0\r\nContent-Length: 2\r\n"
                                   "Expect: 200-maybe\r\n\r\nhi"),
        ParseResult::EXPECTATION_FAILED);
}

// Repeated field names combine (NR-10).
TEST(HttpParserConformanceTest, RepeatedHeaderFieldsCombineAsList) {
    // Replacement would let a second Connection field erase an earlier close
    // token (RFC 9110 §5.2).
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

// Combining individually valid values must fail whole on overflow (NR-10).
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
        ParseResult::BAD_REQUEST);
    EXPECT_EQ(verdict<DefaultHttpLimits>(
                  "POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n"),
              ParseResult::BAD_REQUEST);
    EXPECT_EQ(
        verdict<DefaultHttpLimits>(
            "POST / HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n"),
        ParseResult::PAYLOAD_TOO_LARGE);
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
    // Verdict alone misses a body overwritten or truncated across a feed
    // boundary.
    const ParseOutcome whole = parse_whole(req);
    ASSERT_EQ(whole.status, ParseResult::COMPLETE);
    ASSERT_EQ(whole.body, "abc");
    expect_segmentation_independent(req);
}

TEST(HttpParserChunkedTest, RejectsControlBytesInExtensionsAndTrailers) {
    // Skipped extensions and trailers still reject control octets that could
    // hide framing.
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
    // A 0x1000-byte chunk exceeds the default 2048-byte cap and must be
    // rejected during decoding.
    std::string req =
        "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
    req += "1000\r\n" + std::string(4096, 'a') + "\r\n0\r\n\r\n";
    EXPECT_EQ(verdict<DefaultHttpLimits>(req), ParseResult::PAYLOAD_TOO_LARGE);
}

TEST(HttpParserChunkedTest, EmptyChunkSizeLineIsRejectedNotLastChunk) {
    // A missing size must not become a zero last-chunk and expose body bytes
    // as a new request (RFC 9112 §7.1).
    EXPECT_EQ(
        verdict<DefaultHttpLimits>(
            "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
            "\r\n\r\n"),
        ParseResult::BAD_REQUEST);
    // A chunk extension must not replace a missing size.
    EXPECT_EQ(
        verdict<DefaultHttpLimits>(
            "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
            ";ext=1\r\n\r\n"),
        ParseResult::BAD_REQUEST);
    EXPECT_EQ(
        verdict<DefaultHttpLimits>(
            "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
            "3\r\nabc\r\n\r\n\r\n"),
        ParseResult::BAD_REQUEST);
}

TEST(HttpParserTest, InProgressTracksMidRequestWithoutStartedFlag) {
    HttpParser<> parser;
    EXPECT_FALSE(parser.in_progress());
    std::size_t consumed = 0;
    parser.feed(bytes_of("G"), consumed);
    EXPECT_TRUE(parser.in_progress());
    parser.reset();
    EXPECT_FALSE(parser.in_progress());
    parser.feed(bytes_of("GET /x HTTP/1.1\r\nHost: a\r"), consumed);
    EXPECT_TRUE(parser.in_progress());
    parser.feed(bytes_of("\n\r\n"), consumed);
    EXPECT_TRUE(parser.complete());
    EXPECT_FALSE(parser.in_progress());
}

TEST(HttpParserChunkedTest, RejectsStackedTransferEncodings) {
    // An unsupported coding stacked with chunked must produce 501 (BR-9).
    EXPECT_EQ(verdict<DefaultHttpLimits>(
                  "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip, "
                  "chunked\r\n\r\n"),
              ParseResult::NOT_IMPLEMENTED);
    EXPECT_EQ(verdict<DefaultHttpLimits>(
                  "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked, "
                  "gzip\r\n\r\n"),
              ParseResult::NOT_IMPLEMENTED);
    EXPECT_EQ(verdict<DefaultHttpLimits>(
                  "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip\r\n"
                  "Transfer-Encoding: chunked\r\n\r\n"),
              ParseResult::NOT_IMPLEMENTED);
}

TEST(HttpParserChunkedTest, OverflowingChunkSizeIsRejectedNotWrapped) {
    // The size 2^64, 17 hexadecimal digits, would wrap uint64_t to zero and
    // expose the payload as a new request. It must be rejected.
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

// HTTP conformance cases (BR-10/11, NR-1/7/8).

TEST(HttpParserConformanceTest, VersionHandling) {
    // A syntactically valid unsupported HTTP version returns 505 (RFC 9110
    // §15.6.6).
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET /x HTTP/2.0\r\n\r\n"),
              ParseResult::HTTP_VERSION_NOT_SUPPORTED);
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET /x HTTP/1.9\r\n\r\n"),
              ParseResult::HTTP_VERSION_NOT_SUPPORTED);
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET /x HTTP/9.9\r\n\r\n"),
              ParseResult::HTTP_VERSION_NOT_SUPPORTED);
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET /x HTTP/1.11\r\n\r\n"),
              ParseResult::BAD_REQUEST);
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET /x HTTP/1.\r\n\r\n"),
              ParseResult::BAD_REQUEST);
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET /x HTX/1.1\r\n\r\n"),
              ParseResult::BAD_REQUEST);
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET /x hq\r\n\r\n"),
              ParseResult::BAD_REQUEST);
}

// Host validation (NR-7 / BR-10).
TEST(HttpParserConformanceTest, Http11RequiresExactlyOneHost) {
    // An HTTP/1.1 request must carry exactly one Host field (RFC 9112 §3.2).
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET / HTTP/1.1\r\nHost: a\r\n\r\n"),
              ParseResult::COMPLETE);
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET / HTTP/1.1\r\n\r\n"),
              ParseResult::BAD_REQUEST);
    EXPECT_EQ(verdict<DefaultHttpLimits>(
                  "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n"),
              ParseResult::BAD_REQUEST);
    // HTTP/1.0 permits an absent Host field.
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET / HTTP/1.0\r\n\r\n"),
              ParseResult::COMPLETE);
}

TEST(HttpParserConformanceTest, Http10RejectsMoreThanOneHost) {
    // The rule against more than one Host field line covers every request,
    // not only HTTP/1.1 (RFC 9112 §3.2).
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET / HTTP/1.0\r\nHost: a\r\n\r\n"),
              ParseResult::COMPLETE);
    EXPECT_EQ(verdict<DefaultHttpLimits>(
                  "GET / HTTP/1.0\r\nHost: a\r\nHost: b\r\n\r\n"),
              ParseResult::BAD_REQUEST);
}

TEST(HttpParserConformanceTest, RejectsControlCharsInFieldValue) {
    // A field value may contain VCHAR, SP and HTAB, but no other controls or
    // DEL (RFC 9110 §5.5).
    EXPECT_EQ(verdict<DefaultHttpLimits>(
                  "GET / HTTP/1.1\r\nHost: h\r\nX: a\tb\r\n\r\n"),
              ParseResult::COMPLETE);
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET / HTTP/1.1\r\nHost: h\r\nX: a\x01"
                                         "b\r\n\r\n"),
              ParseResult::BAD_REQUEST);
    EXPECT_EQ(verdict<DefaultHttpLimits>("GET / HTTP/1.1\r\nHost: h\r\nX: a\x7f"
                                         "b\r\n\r\n"),
              ParseResult::BAD_REQUEST);
}

// The parser must terminate without stalling, crashing or over-consuming
// hostile input.

namespace {

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
        // Progress is checked before every verdict, bounding the loop by
        // input size.
        ASSERT_GT(consumed, 0U) << "parser stalled without consuming input";
        if (status == ParseResult::COMPLETE) {
            parser.reset();
            continue;
        }
        if (status != ParseResult::INCOMPLETE) {
            return;
        }
    }
}

std::span<const std::uint8_t> raw(std::string_view text) {
    // unsigned char may alias the input bytes.
    // bugprone-bitwise-pointer-cast: byte views preserve representation.
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
        std::string("GET /\0x HTTP/1.1\r\n\r\n", 20),
        "GET / HTTP/1.1\r\nX\x01: \x7f\r\n\r\n",
        "\xff\xfe\xfd\xfc",
        std::string(5000, 'A'),
        "GET / HTTP/1.1\r\n" + std::string(5000, 'H') + ": v\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nab",
        "POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
        // bugprone-suspicious-missing-comma: adjacent literals form one
        // corpus sample.
        // NOLINTNEXTLINE(bugprone-suspicious-missing-comma)
        "POST / HTTP/1.1\r\nTransfer-Encoding: "
        "chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
        "GET / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nx",
        "GET /\r\n\r\n",
        ":::::\r\n\r\n",
        "GET  //  HTTP/1.1\r\n\r\n",
        "\n\n\n\n",
        "\r\nGET / HTTP/1.1\r\nHost: h\r\n\r\n",
        "POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: h\r\nExpect: never\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: h\r\nHost: h\r\nA: 1\r\nA: 2\r\n\r\n",
    };
    for (const auto& sample : corpus) {
        drive_hostile_input(raw(sample));
    }
}
