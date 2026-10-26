// SPDX-License-Identifier: MIT
#pragma once

#include <memory>

#include "pico_http/http/http_service.hpp"

namespace pico_http {

/// Convenience factory building an HttpService on the lwIP-backed
/// LwipTcpTransport. Declared unconditionally so the
/// umbrella header stays host-safe, but only *defined* in firmware builds —
/// host code injects its own ITcpTransport (e.g. FakeTcpTransport) instead.
std::unique_ptr<HttpService> make_http_service();

}  // namespace pico_http
