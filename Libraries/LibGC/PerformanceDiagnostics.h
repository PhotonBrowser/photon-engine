/*
 * Copyright (c) 2026, Photon contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObject.h>
#include <LibGC/Export.h>

namespace GC {

class Heap;

// Diagnostics are opt-in and scoped to the selected heap. Calls must be made
// on the heap's owning thread, which is also where collection metrics are
// recorded.
GC_API void set_performance_diagnostics_enabled(Heap&, bool);
GC_API AK::JsonObject performance_diagnostics(Heap const&);

}
