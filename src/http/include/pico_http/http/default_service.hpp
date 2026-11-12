// SPDX-License-Identifier: MIT
#pragma once

#include <memory>

#include "pico_http/http/http_service.hpp"

namespace pico_http {

/// Returns a service with LwipTcpTransport. Defined only in firmware builds.
std::unique_ptr<HttpService> make_http_service();

}  // namespace pico_http
