// SPDX-License-Identifier: MIT
#pragma once

#include "pico_http/http/message.hpp"

/// \file default_handlers.hpp
/// The canned welcome / 404 handlers used by HttpService. Kept in one place so
/// the default responses live in a single spot. Not part of the stable public
/// API.

namespace pico_http::detail {

inline Disposition default_welcome_handler(const HTTPRequestView& /*request*/,
                                           HTTPResponse& res) {
    res.status_code = HTTPResponse::STATUS_OK;
    // A literal well inside STATUS_TEXT_CAPACITY: cannot truncate.
    static_cast<void>(res.status_text.assign("OK"));
    res.set_body_static(
        StaticBody::outliving_response("Welcome to Pico HTTP Server"),
        "text/plain");
    return Disposition::COMPLETE;
}

inline Disposition default_not_found_handler(const HTTPRequestView& /*request*/,
                                             HTTPResponse& res) {
    res.status_code = HTTPResponse::STATUS_NOT_FOUND;
    // As above.
    static_cast<void>(res.status_text.assign("Not Found"));
    res.set_body_static(StaticBody::outliving_response("404 - Not Found"),
                        "text/plain");
    return Disposition::COMPLETE;
}

}  // namespace pico_http::detail
