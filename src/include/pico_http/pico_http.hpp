// SPDX-License-Identifier: MIT
#pragma once

/// \file pico_http.hpp
/// pico-http umbrella header. Re-exports the primary abstractions, so a
/// consumer can include this alone; including the specific components directly
/// is equally supported where finer control over dependencies is wanted.
///
/// Host-safe: the firmware-only pieces (LwipTcpTransport, the
/// make_http_service() definition) live behind
/// pico_http/tcp/lwip_tcp_transport.hpp and the firmware build of the library.

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
