// SPDX-License-Identifier: MIT
#pragma once

// The explicitly selected fallback exposes lwipopts.h through this shim.
// Applications with their own configuration do not include it.
#include "lwipopts_reference.h"  // IWYU pragma: export
