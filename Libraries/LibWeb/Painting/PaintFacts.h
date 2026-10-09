/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Painting {

// A video's paint facts as last pushed, with where they went. Frames are presented through the video's sink, so a
// steady video needs no repaint; only a change to these does.
struct VideoPaintFactsSnapshot {
    u8 representation { 0 };
    bool has_video_frame { false };
    i32 video_src_width { 0 };
    i32 video_src_height { 0 };
    u64 video_sink_resource_id { 0 };
    u64 video_sink_handle { 0 };
    void const* poster_frame { nullptr };
    void const* host { nullptr };
    u32 element { 0 };

    bool operator==(VideoPaintFactsSnapshot const&) const = default;
};

enum class StyleHoldsImageValues : u8 {
    No,
    Yes,
};

// These queue the facts a box paints from for the render state. Facts read off an element are queued for the box bound
// to it, which the render state finds as it applies them: the element may change beside a frame in flight, which holds
// the boxes. Facts read off a box are queued for its row.
WEB_API void push_paint_facts_after_style_attach(Layout::NodeWithStyle&, StyleHoldsImageValues);
WEB_API void push_video_paint_facts(HTML::HTMLVideoElement const&);
// For callers that run on every rendering update: pushes and repaints only when the facts differ from the last push.
WEB_API void push_video_paint_facts_if_changed(HTML::HTMLVideoElement const&);
WEB_API void push_form_control_paint_facts(HTML::HTMLInputElement&);
WEB_API void push_canvas_paint_facts(HTML::HTMLCanvasElement const&);
WEB_API void push_layer_image_paint_facts(Layout::NodeWithStyle&);
WEB_API void push_replaced_image_paint_facts(Layout::Node&);
// The image the element provides changed.
WEB_API void push_replaced_image_paint_facts(DOM::Element const&, Layout::ImageProvider const&);
WEB_API void reconcile_navigable_container_paint_facts(Layout::BegunRead const&, DOM::Document const&);
WEB_API void publish_image_map_area_facts_if_needed(Layout::BegunRead const&, DOM::Document&);

}
