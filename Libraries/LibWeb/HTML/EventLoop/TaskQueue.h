/*
 * Copyright (c) 2021-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/Cell.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <AK/String.h>

namespace Web::HTML {

class TaskQueue : public JS::Cell {
    GC_CELL(TaskQueue, JS::Cell);
    GC_DECLARE_ALLOCATOR(TaskQueue);

public:
    explicit TaskQueue(HTML::EventLoop&);
    virtual ~TaskQueue() override;

    bool is_empty() const { return m_tasks.is_empty() && m_idle_tasks.is_empty(); }
    size_t size_slow() const { return m_tasks.size_slow() + m_idle_tasks.size_slow(); }

    bool has_runnable_tasks() const;

    void add(GC::Ref<HTML::Task>);
    GC::Ptr<HTML::Task> take_first_runnable();

    void enqueue(GC::Ref<HTML::Task> task) { add(task); }

    void remove_tasks_matching(Function<bool(HTML::Task const&)>);

    // https://html.spec.whatwg.org/multipage/workers.html#dom-workerglobalscope-closing
    void discard_further_tasks() { m_discards_further_tasks = true; }

    Task const* last_added_task() const;

    // Diagnostics are process-scoped, bounded, and disabled unless a controlled benchmark asks for them.
    WEB_API static void set_performance_diagnostics_enabled(bool);
    WEB_API static String performance_diagnostics_json();
    void record_completed_task(Task const&, u64 execution_duration_microseconds);

private:
    virtual void visit_edges(Visitor&) override;

    bool is_runnable_now(Task const&) const;
    void remove_without_running(Task::Queue&, Task&);
    void run_discard_steps();

    GC::Ref<HTML::EventLoop> m_event_loop;

    Task::Queue m_tasks;
    Task::Queue m_idle_tasks;
    GC::Ptr<HTML::Task const> m_last_added_task;
    Vector<GC::Ref<HTML::Task>> m_discarded_tasks;
bool m_discards_further_tasks { false };
    size_t m_normal_task_count { 0 };
    size_t m_idle_task_count { 0 };
};

}
