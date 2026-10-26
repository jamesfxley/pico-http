// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "pico_http/http/message.hpp"
#include "pico_http/http/route_table.hpp"

using pico_http::Disposition;
using pico_http::HTTPRequestView;
using pico_http::HTTPResponse;
using pico_http::RouteTable;

TEST(RouteTableTest, FindsExactAndWildcardMatches) {
    RouteTable routes;
    bool exact_called = false;
    bool wildcard_called = false;

    EXPECT_TRUE(routes.add_route("GET", "/status",
                                 [&](const HTTPRequestView&, HTTPResponse&) {
                                     exact_called = true;
                                     return Disposition::COMPLETE;
                                 }));
    EXPECT_TRUE(routes.add_route("*", "/metrics/*",
                                 [&](const HTTPRequestView&, HTTPResponse&) {
                                     wildcard_called = true;
                                     return Disposition::COMPLETE;
                                 }));

    const HTTPRequestView dummy_request{};
    HTTPResponse response{};

    const auto* exact = routes.find_route("GET", "/status");
    ASSERT_NE(exact, nullptr);
    exact->handler(dummy_request, response);
    EXPECT_TRUE(exact_called);

    const auto* wildcard = routes.find_route("POST", "/metrics/http");
    ASSERT_NE(wildcard, nullptr);
    wildcard->handler(dummy_request, response);
    EXPECT_TRUE(wildcard_called);

    EXPECT_EQ(routes.find_route("DELETE", "/missing"), nullptr);
}

TEST(RouteTableTest, MethodWildcardMatchesAnyVerb) {
    RouteTable routes;
    bool invoked = false;

    EXPECT_TRUE(routes.add_route("*", "/status*",
                                 [&](const HTTPRequestView&, HTTPResponse&) {
                                     invoked = true;
                                     return Disposition::COMPLETE;
                                 }));

    EXPECT_NE(routes.find_route("POST", "/status"), nullptr);
    EXPECT_NE(routes.find_route("DELETE", "/status/info"), nullptr);

    const auto* route = routes.find_route("PATCH", "/status/detail");
    ASSERT_NE(route, nullptr);

    HTTPRequestView request{};
    request.uri = "/status/detail";
    HTTPResponse response{};
    route->handler(request, response);

    EXPECT_TRUE(invoked);
}

TEST(RouteTableTest, MethodComparisonIsCaseInsensitive) {
    RouteTable routes;
    bool invoked = false;

    EXPECT_TRUE(routes.add_route("gEt", "/wild/*",
                                 [&](const HTTPRequestView&, HTTPResponse&) {
                                     invoked = true;
                                     return Disposition::COMPLETE;
                                 }));

    const auto* route = routes.find_route("GET", "/wild/path");
    ASSERT_NE(route, nullptr);

    HTTPRequestView request{};
    request.uri = "/wild/path";
    HTTPResponse response{};
    route->handler(request, response);

    EXPECT_TRUE(invoked);
}

TEST(RouteTableTest, MethodComparisonRejectsDifferentMethods) {
    RouteTable routes;
    EXPECT_TRUE(routes.add_route("PUT", "/wild/*",
                                 [](const HTTPRequestView&, HTTPResponse&) {
                                     return Disposition::COMPLETE;
                                 }));

    EXPECT_EQ(routes.find_route("GET", "/wild/path"), nullptr);
}

TEST(RouteTableTest, ReRegistrationReplacesHandler) {
    RouteTable routes;
    int which = 0;
    EXPECT_TRUE(routes.add_route("GET", "/x",
                                 [&](const HTTPRequestView&, HTTPResponse&) {
                                     which = 1;
                                     return Disposition::COMPLETE;
                                 }));
    EXPECT_TRUE(routes.add_route("GET", "/x",
                                 [&](const HTTPRequestView&, HTTPResponse&) {
                                     which = 2;
                                     return Disposition::COMPLETE;
                                 }));

    const auto* route = routes.find_route("GET", "/x");
    ASSERT_NE(route, nullptr);
    const HTTPRequestView request{};
    HTTPResponse response{};
    route->handler(request, response);
    EXPECT_EQ(which, 2);  // the later registration replaced the earlier one
}

TEST(RouteTableTest, ExactMatchWinsOverWildcard) {
    RouteTable routes;
    bool exact_called = false;
    bool wildcard_called = false;
    EXPECT_TRUE(routes.add_route("GET", "/api/*",
                                 [&](const HTTPRequestView&, HTTPResponse&) {
                                     wildcard_called = true;
                                     return Disposition::COMPLETE;
                                 }));
    EXPECT_TRUE(routes.add_route("GET", "/api/v1",
                                 [&](const HTTPRequestView&, HTTPResponse&) {
                                     exact_called = true;
                                     return Disposition::COMPLETE;
                                 }));

    const auto* route = routes.find_route("GET", "/api/v1");
    ASSERT_NE(route, nullptr);
    const HTTPRequestView request{};
    HTTPResponse response{};
    route->handler(request, response);
    EXPECT_TRUE(exact_called);
    EXPECT_FALSE(wildcard_called);

    // Method still gates both tables: POST matches neither route.
    EXPECT_EQ(routes.find_route("POST", "/api/v1"), nullptr);
    EXPECT_EQ(routes.find_route("POST", "/api/other"), nullptr);
    // A non-exact URI falls through to the wildcard.
    EXPECT_NE(routes.find_route("GET", "/api/other"), nullptr);
}

TEST(RouteTableTest, StoresAMethodOfExactlyMethodCapacityCharacters) {
    // METHOD_CAPACITY is the usable length, as every other cap in the library
    // is (BR-2): a caller sizing against the public constant must get a
    // route, not a silent registration failure.
    RouteTable routes;
    const std::string method(RouteTable::METHOD_CAPACITY, 'M');

    EXPECT_TRUE(routes.add_route(method, "/long-method",
                                 [](const HTTPRequestView&, HTTPResponse&) {
                                     return Disposition::COMPLETE;
                                 }));
    EXPECT_NE(routes.find_route(method, "/long-method"), nullptr);

    // One over is still a refusal, not a truncation that would route the
    // shorter token by accident.
    const std::string too_long(RouteTable::METHOD_CAPACITY + 1, 'M');
    EXPECT_FALSE(routes.add_route(too_long, "/too-long",
                                  [](const HTTPRequestView&, HTTPResponse&) {
                                      return Disposition::COMPLETE;
                                  }));
    EXPECT_EQ(routes.find_route(too_long, "/too-long"), nullptr);
}
