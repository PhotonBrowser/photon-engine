/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <LibWebView/Forward.h>
#include <sys/types.h>

namespace WebView {

struct ProcessReaperIdentity {
    pid_t pid;
    uint64_t start_time_in_microseconds;
    Optional<pid_t> parent_pid;
    Optional<uint64_t> parent_start_time_in_microseconds;
};

// Starts the ProcessReaper helper, which kills the processes that it is told about once this process is gone. The
// kernel does that for us on Linux, but not on macOS.
class WEBVIEW_API ProcessReaper {
    AK_MAKE_NONCOPYABLE(ProcessReaper);
    AK_MAKE_NONMOVABLE(ProcessReaper);

public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<NonnullOwnPtr<ProcessReaper>> start(ByteString const& executable_path);
    static Optional<ProcessReaperIdentity> active_identity();
    ~ProcessReaper();

    ErrorOr<void> watch(pid_t);

    pid_t pid() const { return m_pid; }

private:
    ProcessReaper(int pipe_fd, pid_t pid);

    int m_pipe_fd { -1 };
    pid_t m_pid { -1 };
};

}
