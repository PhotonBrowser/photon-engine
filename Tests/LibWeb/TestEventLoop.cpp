/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventLoop/RenderingSchedulerCounters.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Web::HTML {

class EventLoopTestAccess {
public:
    static void mark_rendering_task_queued(EventLoop& event_loop)
    {
        VERIFY(!event_loop.m_rendering_task_queued);
        event_loop.m_rendering_task_queued = true;
    }

    static void run_rendering_task(EventLoop& event_loop)
    {
        event_loop.m_rendering_task_function->function()();
    }

    static void dispatch_input_events_task(EventLoop& event_loop)
    {
        VERIFY(!event_loop.m_input_events_task_queued);
        event_loop.m_input_events_task_queued = true;
        event_loop.process_input_events_task();
    }
};

}

TEST_CASE(nested_pause_handles_keep_the_event_loop_paused)
{
    static Web::Platform::FontPlugin font_plugin { false };
    Web::Platform::FontPlugin::install(font_plugin);

    auto realm = Web::Bindings::create_a_principal_javascript_realm();
    URL::URL url;
    auto* window = Web::HTML::window_from_global_object(realm->global_object());
    VERIFY(window);
    auto document = Web::DOM::Document::create(Web::Bindings::principal_host_defined_page(*realm), *window, url);
    document->set_window(*window);
    window->set_associated_document(document);
    auto& event_loop = Web::HTML::main_thread_event_loop();

    {
        auto outer_pause = event_loop.pause();
        EXPECT(event_loop.execution_paused());

        {
            auto inner_pause = event_loop.pause();
            EXPECT(event_loop.execution_paused());
        }

        EXPECT(event_loop.execution_paused());
    }

    EXPECT(!event_loop.execution_paused());
}

TEST_CASE(successive_rendering_tasks_keep_the_running_state_balanced)
{
    auto& event_loop = Web::HTML::main_thread_event_loop();

    for (size_t opportunity = 0; opportunity < 2; ++opportunity) {
        EXPECT(!event_loop.rendering_task_queued_or_running());
        Web::HTML::EventLoopTestAccess::mark_rendering_task_queued(event_loop);
        EXPECT(event_loop.rendering_task_queued_or_running());
        Web::HTML::EventLoopTestAccess::run_rendering_task(event_loop);

        EXPECT(!event_loop.rendering_task_queued_or_running());
    }
}

TEST_CASE(input_event_lane_takes_do_not_leave_a_rendering_task_active)
{
    auto& event_loop = Web::HTML::main_thread_event_loop();

    for (size_t input_dispatch = 0; input_dispatch < 2; ++input_dispatch) {
        Web::HTML::EventLoopTestAccess::dispatch_input_events_task(event_loop);
        EXPECT(!event_loop.running_rendering_task());
    }
}

TEST_CASE(rendering_scheduler_duration_preserves_truncation)
{
    for (auto duration : { 0.0, 0.0005, 0.0015, 1.2349, 2.5, 1000.1234 })
        EXPECT_EQ(Web::HTML::rendering_scheduler_duration_microseconds(duration), static_cast<u64>(duration * 1000.0));
    EXPECT_EQ(Web::HTML::rendering_scheduler_duration_microseconds(0.0005), 0u);
    EXPECT_EQ(Web::HTML::rendering_scheduler_duration_microseconds(0.0015), 1u);
    EXPECT_EQ(Web::HTML::rendering_scheduler_duration_microseconds(1.2349), 1234u);
}

TEST_CASE(rendering_scheduler_duration_rejects_invalid_intervals)
{
    for (auto duration : { -0.0, -0.0005, -1.0, AK::NaN<double>, AK::Infinity<double>, -AK::Infinity<double>, NumericLimits<double>::max() })
        EXPECT_EQ(Web::HTML::rendering_scheduler_duration_microseconds(duration), 0u);
}

TEST_CASE(rendering_scheduler_duration_checks_the_unsigned_boundary)
{
    constexpr double boundary_ms = 0x1p64 / 1000.0;
    EXPECT_EQ(Web::HTML::rendering_scheduler_duration_microseconds(boundary_ms), 0u);
    EXPECT_EQ(Web::HTML::rendering_scheduler_duration_microseconds(nextafter(boundary_ms, AK::Infinity<double>)), 0u);
    EXPECT_EQ(Web::HTML::rendering_scheduler_duration_microseconds(nextafter(boundary_ms, 0.0)), 0xfffffffffffff000ULL);
    EXPECT_EQ(Web::HTML::rendering_scheduler_duration_microseconds(0x1p63 / 1000.0), 0x8000000000000000ULL);
