/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibGC/RootVector.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventLoop/TaskQueue.h>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace Web::HTML {

namespace {

static constexpr size_t task_diagnostic_sample_capacity = 1024;
static constexpr size_t pending_task_diagnostic_capacity = 4096;
static constexpr size_t task_source_bucket_count = 8;
static constexpr std::array<StringView, task_source_bucket_count> task_source_bucket_names {
    "user_interaction"sv,
    "timer"sv,
    "dom_manipulation"sv,
    "networking"sv,
    "rendering"sv,
    "posted_message"sv,
    "idle"sv,
    "other"sv,
};

struct TaskDiagnosticSample {
    u64 queue_wait_nanoseconds { 0 };
    u64 execution_duration_microseconds { 0 };
    size_t queue_depth_at_enqueue { 0 };
    size_t queue_depth_at_dispatch { 0 };
    size_t source_bucket { 0 };
};

struct TaskDiagnosticState {
    std::array<TaskDiagnosticSample, task_diagnostic_sample_capacity> samples {};
    size_t next_sample_index { 0 };
    size_t retained_sample_count { 0 };
    u64 completed_task_count { 0 };
    u64 untracked_task_count { 0 };
    u64 generation { 0 };
};

struct PendingTaskDiagnostic {
    MonotonicTime enqueued_at;
    Optional<MonotonicTime> dispatched_at;
    u64 generation { 0 };
    size_t queue_depth_at_enqueue { 0 };
    size_t queue_depth_at_dispatch { 0 };
};

static auto& s_task_diagnostics_lock = *new std::mutex;
static TaskDiagnosticState s_task_diagnostics_state;
static auto& s_pending_task_diagnostics = *new std::unordered_map<Task const*, PendingTaskDiagnostic>;
static std::atomic<bool> s_task_diagnostics_enabled { false };
static std::atomic<u64> s_task_diagnostics_generation { 0 };

static size_t task_source_bucket(Task::Source source)
{
    switch (source) {
    case Task::Source::UserInteraction:
        return 0;
    case Task::Source::TimerTask:
        return 1;
    case Task::Source::DOMManipulation:
        return 2;
    case Task::Source::Networking:
        return 3;
    case Task::Source::Rendering:
        return 4;
    case Task::Source::PostedMessage:
        return 5;
    case Task::Source::IdleTask:
        return 6;
    default:
        return 7;
    }
}

struct Distribution {
    Optional<double> p50;
    Optional<double> p95;
    Optional<double> p99;
    Optional<double> maximum;
};

static Distribution summarize(Vector<u64> values, double divisor)
{
    if (values.is_empty())
        return {};
    quick_sort(values);
    auto at_percentile = [&](double percentile) {
        auto index = static_cast<size_t>(ceil(percentile * static_cast<double>(values.size()))) - 1;
        return static_cast<double>(values[min(index, values.size() - 1)]) / divisor;
    };
    return {
        .p50 = at_percentile(0.50),
        .p95 = at_percentile(0.95),
        .p99 = at_percentile(0.99),
        .maximum = static_cast<double>(values.last()) / divisor,
    };
}

static JsonObject distribution_json(Distribution const& distribution)
{
    JsonObject json;
    if (distribution.p50.has_value()) {
        json.set("p50_ms"sv, *distribution.p50);
        json.set("p95_ms"sv, *distribution.p95);
        json.set("p99_ms"sv, *distribution.p99);
        json.set("max_ms"sv, *distribution.maximum);
    } else {
        json.set("p50_ms"sv, JsonValue {});
        json.set("p95_ms"sv, JsonValue {});
        json.set("p99_ms"sv, JsonValue {});
        json.set("max_ms"sv, JsonValue {});
    }
    return json;
}

static JsonObject summarize_samples(Span<TaskDiagnosticSample const> samples)
{
    Vector<u64> queue_wait_nanoseconds;
    Vector<u64> execution_duration_microseconds;
    queue_wait_nanoseconds.ensure_capacity(samples.size());
    execution_duration_microseconds.ensure_capacity(samples.size());
    u64 enqueue_depth_total = 0;
    u64 dispatch_depth_total = 0;
    size_t enqueue_depth_max = 0;
    size_t dispatch_depth_max = 0;
    for (auto const& sample : samples) {
        queue_wait_nanoseconds.append(sample.queue_wait_nanoseconds);
        execution_duration_microseconds.append(sample.execution_duration_microseconds);
        enqueue_depth_total += sample.queue_depth_at_enqueue;
        dispatch_depth_total += sample.queue_depth_at_dispatch;
        enqueue_depth_max = max(enqueue_depth_max, sample.queue_depth_at_enqueue);
        dispatch_depth_max = max(dispatch_depth_max, sample.queue_depth_at_dispatch);
    }

    JsonObject json;
    json.set("sample_count"sv, samples.size());
    json.set("queue_wait"sv, distribution_json(summarize(move(queue_wait_nanoseconds), 1'000'000.0)));
    json.set("execution_duration"sv, distribution_json(summarize(move(execution_duration_microseconds), 1'000.0)));
    if (!samples.is_empty()) {
        json.set("queue_depth_at_enqueue_mean"sv, static_cast<double>(enqueue_depth_total) / static_cast<double>(samples.size()));
        json.set("queue_depth_at_enqueue_max"sv, enqueue_depth_max);
        json.set("queue_depth_at_dispatch_mean"sv, static_cast<double>(dispatch_depth_total) / static_cast<double>(samples.size()));
        json.set("queue_depth_at_dispatch_max"sv, dispatch_depth_max);
    } else {
        json.set("queue_depth_at_enqueue_mean"sv, JsonValue {});
        json.set("queue_depth_at_enqueue_max"sv, JsonValue {});
        json.set("queue_depth_at_dispatch_mean"sv, JsonValue {});
        json.set("queue_depth_at_dispatch_max"sv, JsonValue {});
    }
    return json;
}

}

GC_DEFINE_ALLOCATOR(TaskQueue);

TaskQueue::TaskQueue(HTML::EventLoop& event_loop)
    : m_event_loop(event_loop)
{
}

TaskQueue::~TaskQueue() = default;

void TaskQueue::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_event_loop);
    for (auto& task : m_tasks)
        visitor.visit(task);
    for (auto& task : m_idle_tasks)
        visitor.visit(task);
    visitor.visit(m_last_added_task);
    visitor.visit(m_discarded_tasks);
}

void TaskQueue::add(GC::Ref<Task> task)
{
    // https://html.spec.whatwg.org/multipage/workers.html#dom-workerglobalscope-closing
    // Once the WorkerGlobalScope's closing flag is set to true, the event loop's task queues must discard any further
    // tasks that would be added to them (tasks already on the queue are unaffected except where otherwise specified).
    if (m_discards_further_tasks) {
        task->discard();
        return;
    }

    // AD-HOC: Don't enqueue tasks for temporary (inert) documents used for fragment parsing.
    // FIXME: There's ongoing spec work to remove such documents: https://github.com/whatwg/html/pull/11970
    if (task->document() && task->document()->is_temporary_document_for_fragment_parsing()) {
        task->discard();
        return;
    }

    // AD-HOC: Don't enqueue a task for a destroyed document either: "destroy a document" removes the document's
    //         tasks from every task queue, and a destroyed document is never fully active again, so a task queued
    //         for it afterwards could never run. It would only sit in the queue — and queuing it wakes the event
    //         loop, which then scans the whole queue for a runnable task, but finds none.
    if (task->document() && task->document()->has_been_destroyed()) {
        task->discard();
        return;
    }

    m_last_added_task = task.ptr();
    if (task->priority() == Task::Priority::Idle) {
        m_idle_tasks.append(*task);
        ++m_idle_task_count;
    } else {
        m_tasks.append(*task);
        ++m_normal_task_count;
    }
    if (s_task_diagnostics_enabled.load(std::memory_order_relaxed)) {
        std::lock_guard lock(s_task_diagnostics_lock);
        if (s_task_diagnostics_enabled.load(std::memory_order_relaxed)) {
            auto generation = s_task_diagnostics_generation.load(std::memory_order_relaxed);
            if (s_pending_task_diagnostics.size() >= pending_task_diagnostic_capacity) {
                ++s_task_diagnostics_state.untracked_task_count;
            } else {
                s_pending_task_diagnostics.insert_or_assign(task.ptr(), PendingTaskDiagnostic {
                    .enqueued_at = MonotonicTime::now(),
                    .dispatched_at = {},
                    .generation = generation,
                    .queue_depth_at_enqueue = m_normal_task_count + m_idle_task_count,
                });
            }
        }
    }
    m_event_loop->schedule();
}

// A task that is runnable, and that no rendering update in flight holds back.
bool TaskQueue::is_runnable_now(Task const& task) const
{
    return task.is_runnable() && !m_event_loop->holds_tasks_of(task.document());
}

GC::Ptr<Task> TaskQueue::take_first_runnable()
{
    if (m_event_loop->execution_paused())
        return nullptr;

    ScopeGuard run_discard_steps_guard { [&] { run_discard_steps(); } };

    for (auto it = m_tasks.begin(); it != m_tasks.end();) {
        auto& task = *it;

        if (m_event_loop->running_rendering_task() && task.source() == Task::Source::Rendering) {
            ++it;
            continue;
        }

        // While the last rendering update has not finished, the next rendering task keeps its place in the queue, and
        // the tasks after it wait behind it until the update has finished.
        if (task.source() == Task::Source::Rendering && m_event_loop->holds_rendering_opportunity())
            return nullptr;

        if (is_runnable_now(task)) {
            if (s_task_diagnostics_enabled.load(std::memory_order_relaxed)) {
                std::lock_guard lock(s_task_diagnostics_lock);
                auto diagnostic = s_pending_task_diagnostics.find(&task);
                if (diagnostic != s_pending_task_diagnostics.end()
                    && diagnostic->second.generation == s_task_diagnostics_generation.load(std::memory_order_relaxed)) {
                    diagnostic->second.dispatched_at = MonotonicTime::now();
                    diagnostic->second.queue_depth_at_dispatch = m_normal_task_count + m_idle_task_count;
                }
            }
            if (m_last_added_task.ptr() == &task)
                m_last_added_task = {};
            it.erase();
            --m_normal_task_count;
            return &task;
        }

        if (task.is_permanently_unrunnable()) {
            ++it;
            remove_without_running(m_tasks, task);
            continue;
        }

        ++it;
    }

    for (auto it = m_idle_tasks.begin(); it != m_idle_tasks.end();) {
        auto& task = *it;

        if (is_runnable_now(task)) {
            if (s_task_diagnostics_enabled.load(std::memory_order_relaxed)) {
                std::lock_guard lock(s_task_diagnostics_lock);
                auto diagnostic = s_pending_task_diagnostics.find(&task);
                if (diagnostic != s_pending_task_diagnostics.end()
                    && diagnostic->second.generation == s_task_diagnostics_generation.load(std::memory_order_relaxed)) {
                    diagnostic->second.dispatched_at = MonotonicTime::now();
                    diagnostic->second.queue_depth_at_dispatch = m_normal_task_count + m_idle_task_count;
                }
            }
            if (m_last_added_task.ptr() == &task)
                m_last_added_task = {};
            it.erase();
            --m_idle_task_count;
            return &task;
        }

        if (task.is_permanently_unrunnable()) {
            ++it;
            remove_without_running(m_idle_tasks, task);
            continue;
        }

        ++it;
    }
    return nullptr;
}

bool TaskQueue::has_runnable_tasks() const
{
    if (m_event_loop->execution_paused())
        return false;

    for (auto& task : m_tasks) {
        if (m_event_loop->running_rendering_task() && task.source() == Task::Source::Rendering)
            continue;
        if (task.source() == Task::Source::Rendering && m_event_loop->holds_rendering_opportunity())
            return false;
        if (is_runnable_now(task))
            return true;
    }

    for (auto& task : m_idle_tasks) {
        if (is_runnable_now(task))
            return true;
    }
    return false;
}

void TaskQueue::remove_tasks_matching(Function<bool(HTML::Task const&)> filter)
{
    auto remove_matching_tasks = [&](auto& tasks) {
        for (auto it = tasks.begin(); it != tasks.end();) {
            auto& task = *it;
            ++it;
            if (filter(task))
                remove_without_running(tasks, task);
        }
    };
    remove_matching_tasks(m_tasks);
    remove_matching_tasks(m_idle_tasks);
    run_discard_steps();
}

void TaskQueue::remove_without_running(Task::Queue& tasks, Task& task)
{
    if (m_last_added_task.ptr() == &task)
        m_last_added_task = {};
    tasks.remove(task);
    if (&tasks == &m_tasks)
        --m_normal_task_count;
    else
        --m_idle_task_count;
    if (s_task_diagnostics_enabled.load(std::memory_order_relaxed)) {
        std::lock_guard lock(s_task_diagnostics_lock);
        s_pending_task_diagnostics.erase(&task);
    }
    m_discarded_tasks.append(task);
}

void TaskQueue::set_performance_diagnostics_enabled(bool enabled)
{
    if (!enabled) {
        s_task_diagnostics_enabled.store(false, std::memory_order_release);
        std::lock_guard lock(s_task_diagnostics_lock);
        s_pending_task_diagnostics.clear();
        return;
    }

    s_task_diagnostics_enabled.store(false, std::memory_order_release);
    std::lock_guard lock(s_task_diagnostics_lock);
    auto generation = s_task_diagnostics_generation.fetch_add(1, std::memory_order_relaxed) + 1;
    s_task_diagnostics_state = {};
    s_task_diagnostics_state.generation = generation;
    s_pending_task_diagnostics.clear();
    s_task_diagnostics_enabled.store(true, std::memory_order_release);
}

void TaskQueue::record_completed_task(Task const& task, u64 execution_duration_microseconds)
{
    if (!s_task_diagnostics_enabled.load(std::memory_order_acquire))
        return;

    std::lock_guard lock(s_task_diagnostics_lock);
    if (!s_task_diagnostics_enabled.load(std::memory_order_relaxed))
        return;
    auto diagnostic = s_pending_task_diagnostics.find(&task);
    if (diagnostic == s_pending_task_diagnostics.end())
        return;
    auto pending = diagnostic->second;
    s_pending_task_diagnostics.erase(diagnostic);
    auto generation = s_task_diagnostics_generation.load(std::memory_order_relaxed);
    if (pending.generation != generation || !pending.dispatched_at.has_value())
        return;

    auto queue_wait = *pending.dispatched_at - pending.enqueued_at;
    TaskDiagnosticSample sample {
        .queue_wait_nanoseconds = static_cast<u64>(max<i64>(0, queue_wait.to_nanoseconds())),
        .execution_duration_microseconds = execution_duration_microseconds,
        .queue_depth_at_enqueue = pending.queue_depth_at_enqueue,
        .queue_depth_at_dispatch = pending.queue_depth_at_dispatch,
        .source_bucket = task_source_bucket(task.source()),
    };
    auto& state = s_task_diagnostics_state;
    state.samples[state.next_sample_index] = sample;
    state.next_sample_index = (state.next_sample_index + 1) % state.samples.size();
    state.retained_sample_count = min(state.retained_sample_count + 1, state.samples.size());
    ++state.completed_task_count;
}

String TaskQueue::performance_diagnostics_json()
{
    TaskDiagnosticState state;
    size_t pending_task_count;
    {
        std::lock_guard lock(s_task_diagnostics_lock);
        state = s_task_diagnostics_state;
        pending_task_count = s_pending_task_diagnostics.size();
    }

    Vector<TaskDiagnosticSample> ordered_samples;
    ordered_samples.ensure_capacity(state.retained_sample_count);
    auto first = (state.next_sample_index + state.samples.size() - state.retained_sample_count) % state.samples.size();
    for (size_t i = 0; i < state.retained_sample_count; ++i)
        ordered_samples.append(state.samples[(first + i) % state.samples.size()]);

    JsonArray source_summaries;
    for (size_t source = 0; source < task_source_bucket_count; ++source) {
        Vector<TaskDiagnosticSample> source_samples;
        for (auto const& sample : ordered_samples) {
            if (sample.source_bucket == source)
                source_samples.append(sample);
        }
        JsonObject source_summary = summarize_samples(source_samples);
        source_summary.set("source"sv, task_source_bucket_names[source]);
        source_summaries.must_append(move(source_summary));
    }

    JsonObject result;
    result.set("schema_version"sv, 1);
    result.set("scope"sv, "webcontent_process"sv);
    result.set("clock"sv, "process_monotonic"sv);
    result.set("sample_capacity"sv, task_diagnostic_sample_capacity);
    result.set("pending_task_capacity"sv, pending_task_diagnostic_capacity);
    result.set("pending_task_count"sv, pending_task_count);
    result.set("completed_task_count"sv, state.completed_task_count);
    result.set("untracked_task_count"sv, state.untracked_task_count);
    result.set("sample_count"sv, state.retained_sample_count);
    result.set("dropped_sample_count"sv, state.completed_task_count - state.retained_sample_count);
    result.set("queue_wait_unit"sv, "ms"sv);
    result.set("execution_duration_unit"sv, "ms"sv);
    result.set("queue_depth_unit"sv, "tasks"sv);
    result.set("queue_depth_scope"sv, "event_loop_task_queue"sv);
    result.set("percentile_method"sv, "nearest_rank_over_retained_samples"sv);
    result.set("overall"sv, summarize_samples(ordered_samples));
    result.set("by_source"sv, move(source_summaries));
    result.set("runnable_backlog"sv, JsonValue {});
    return result.serialized();
}

// NB: Discard steps run once the queue is no longer being walked, as they may queue or remove tasks.
void TaskQueue::run_discard_steps()
{
    while (!m_discarded_tasks.is_empty()) {
        GC::RootVector discarded_tasks { move(m_discarded_tasks) };
        for (auto& task : discarded_tasks)
            task->discard();
    }
}

Task const* TaskQueue::last_added_task() const
{
    return m_last_added_task.ptr();
}

}
