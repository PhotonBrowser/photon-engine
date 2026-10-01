/*
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibGC/Heap.h>
#include <LibWeb/HTML/AnimationFrameCallbackDriver.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <cstdlib>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(AnimationFrameCallbackDriver);

GC::Ref<AnimationFrameCallbackDriver> AnimationFrameCallbackDriver::create()
{
    return GC::Heap::the().allocate<AnimationFrameCallbackDriver>();
}

void AnimationFrameCallbackDriver::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callbacks);
    visitor.visit(m_executing_callbacks);
}

WebIDL::UnsignedLong AnimationFrameCallbackDriver::add(Callback handler)
{
    auto id = ++m_animation_frame_callback_identifier;
    m_callbacks.set(id, handler);
    return id;
}

bool AnimationFrameCallbackDriver::remove(WebIDL::UnsignedLong id)
{
    return m_callbacks.remove(id);
}

bool AnimationFrameCallbackDriver::has_callbacks() const
{
    return !m_callbacks.is_empty();
}

void AnimationFrameCallbackDriver::run(double now)
{
    AK::ScopeGuard guard { [&]() { m_executing_callbacks.clear(); } };
    m_executing_callbacks = move(m_callbacks);

    static bool trace_enabled = std::getenv("PHOTON_CORE_RUNLOOP_TRACE") != nullptr;
    for (auto& [id, callback] : m_executing_callbacks) {
        if (trace_enabled)
            dbgln("[FrameTrace] raf_callback_begin driver={} handle={} at_ms={:.3f}", reinterpret_cast<FlatPtr>(this), id, HighResolutionTime::unsafe_shared_current_time());
        callback->function()(now);
        if (trace_enabled)
            dbgln("[FrameTrace] raf_callback_end driver={} handle={} at_ms={:.3f} pending_callbacks={}", reinterpret_cast<FlatPtr>(this), id, HighResolutionTime::unsafe_shared_current_time(), m_callbacks.size());
    }
}

}
