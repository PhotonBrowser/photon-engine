/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>

#if defined(AK_OS_MACH)
#    include <LibCore/MachPort.h>
#endif

namespace Core::Platform {

struct ProcessInfo {
    AK_ALLOC_WITH_KMALLOC;

    explicit ProcessInfo(pid_t pid)
        : pid(pid)
    {
    }

    void reset_cpu_time()
    {
        cpu_percent = 0.0f;
        has_cpu_time_baseline = false;
    }

    pid_t pid { 0 };

    u64 memory_usage_bytes { 0 };
    float cpu_percent { 0.0f };
    bool has_cpu_time_baseline { false };
    bool has_cpu_statistics { false };
    bool has_memory_statistics { false };
    Optional<u64> disk_read_bytes;
    Optional<u64> disk_write_bytes;

    u64 time_spent_in_process { 0 };

#if defined(AK_OS_MACH)
    Core::MachPort task_name_port;
#endif
};

}
