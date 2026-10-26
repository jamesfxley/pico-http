// SPDX-License-Identifier: MIT
#pragma once

// lwIP includes "lwipopts.h" by that exact name, so the reference
// configuration reaches it through this shim rather than by being installed as
// lwipopts.h itself. Naming pico_http::lwipopts_fallback in
// PICO_LWIPOPTS_TARGET is what puts this directory on the include path; an
// application supplying its own configuration never sees it.
#include "lwipopts_reference.h"  // IWYU pragma: export
