/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>

namespace Web::CSS {

WEB_API void set_performance_diagnostics_enabled(bool);
WEB_API bool performance_diagnostics_enabled();

}
