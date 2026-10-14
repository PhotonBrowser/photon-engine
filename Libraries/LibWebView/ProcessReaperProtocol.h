/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// Shared by the Browser and the ProcessReaper helper, which uses nothing but system libraries.

#include <libproc.h>
#include <stdint.h>
#include <sys/proc_info.h>
#include <sys/types.h>

namespace WebView {

// A PID alone does not identify a process: once the helper has exited and the Browser has reaped it, the system may
// give its PID to another process. The start time tells the two apart.
struct ProcessReaperRecord {
    pid_t pid;
    uint32_t padding;
    uint64_t start_time_in_microseconds;
};
static_assert(sizeof(ProcessReaperRecord) == 16);

struct ProcessIdentitySnapshot {
    pid_t pid;
    pid_t parent_pid;
    uint64_t start_time_in_microseconds;
    uint64_t parent_start_time_in_microseconds;
    bool has_parent_start_time;
};

inline bool process_identity_snapshot(pid_t pid, ProcessIdentitySnapshot& identity)
{
    proc_bsdinfo info {};
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info))
        return false;
    if (pid <= 0 || info.pbi_pid != static_cast<uint32_t>(pid))
        return false;

    identity.pid = static_cast<pid_t>(info.pbi_pid);
    identity.parent_pid = info.pbi_ppid;
    identity.start_time_in_microseconds = info.pbi_start_tvsec * 1'000'000 + info.pbi_start_tvusec;
    identity.parent_start_time_in_microseconds = 0;
    identity.has_parent_start_time = false;
    if (info.pbi_ppid > 0) {
        proc_bsdinfo parent_info {};
        if (proc_pidinfo(info.pbi_ppid, PROC_PIDTBSDINFO, 0, &parent_info, sizeof(parent_info)) == sizeof(parent_info)
            && parent_info.pbi_pid == static_cast<uint32_t>(info.pbi_ppid)) {
            identity.parent_start_time_in_microseconds = parent_info.pbi_start_tvsec * 1'000'000 + parent_info.pbi_start_tvusec;
            identity.has_parent_start_time = true;
        }
    }
    return true;
}

inline bool process_start_time_in_microseconds(pid_t pid, uint64_t& start_time)
{
    proc_bsdinfo info {};
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info))
        return false;
    start_time = info.pbi_start_tvsec * 1'000'000 + info.pbi_start_tvusec;
    return true;
}

}
