// SPDX-License-Identifier: MIT
#pragma once

/// \file pico_http.hpp
/// Host-safe umbrella header. The lwIP transport and factory definition
/// require firmware.

// The umbrella re-exports every module.
// IWYU pragma: begin_exports
#include "pico_http/http/default_service.hpp"
#include "pico_http/http/headers.hpp"
#include "pico_http/http/http_service.hpp"
#include "pico_http/http/message.hpp"
#include "pico_http/http/route_table.hpp"
#include "pico_http/tcp/tcp_transport.hpp"

// IWYU pragma: end_exports

namespace pico_http {

// Semantic version of the library, mirroring project(pico_http VERSION ...).
/// Major version. Changes when a documented interface breaks.
inline constexpr int VERSION_MAJOR = 0;
/// Minor version. Changes when the interface grows compatibly.
inline constexpr int VERSION_MINOR = 1;
/// Patch version. Changes when behaviour is corrected within the interface.
inline constexpr int VERSION_PATCH = 0;

}  // namespace pico_http
