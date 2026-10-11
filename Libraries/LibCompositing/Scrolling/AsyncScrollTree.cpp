/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/Scrolling/AsyncScrollTree.h>

#include <AK/Debug.h>

namespace Compositing {

void AsyncScrollTree::set_state(AsyncScrollingState&& state)
{
    m_document_id = state.document_id;
    m_scroll_nodes = move(state.scroll_nodes);
    m_snap_containers = move(state.snap_containers);
    m_device_pixels_per_css_pixel = state.device_pixels_per_css_pixel;
    m_wheel_hit_test_regions = move(state.wheel_hit_test_targets);
    m_main_thread_wheel_event_regions = move(state.main_thread_wheel_event_regions);
    m_blocking_wheel_event_regions = move(state.blocking_wheel_event_regions);
    m_has_blocking_wheel_event_region_covering_viewport = state.has_blocking_wheel_event_region_covering_viewport;
    m_cached_wheel_hit_test_targets.clear();
    m_cached_main_thread_wheel_event_targets.clear();
    m_cached_blocking_wheel_event_targets.clear();
    m_visual_context_tree_structural_epoch.clear();
}

AsyncScrollNode const* AsyncScrollTree::scroll_node_for_id(AsyncScrollNodeID node_id) const
{
    for (auto const& node : m_scroll_nodes) {
        if (node.node_id == node_id)
            return &node;
    }
    return nullptr;
}

WheelHitTestResult AsyncScrollTree::hit_test_result_for_scroll_node(AsyncScrollNodeID node_id, Gfx::FloatPoint delta) const
{
    auto const* node = scroll_node_for_id(node_id);
    if (!node)
        return {};
    if (can_scroll_node_by_delta(*node, m_scroll_state_snapshot, delta))
        return { node_id, false };
    if (auto ancestor = scrollable_ancestor_for_node(node_id, m_scroll_state_snapshot, delta); ancestor.has_value())
        return { ancestor, false };
    auto can_dispatch_boundary_delta = (delta.x() != 0 && (node->is_viewport || node->can_be_wheel_scrolled_horizontally) && node->overscroll_behavior_x != ScrollOverscrollBehavior::None)
        || (delta.y() != 0 && (node->is_viewport || node->can_be_wheel_scrolled_vertically) && node->overscroll_behavior_y != ScrollOverscrollBehavior::None);
    if (can_dispatch_boundary_delta)
        return { node_id, false };
    return {};
}

AsyncSnapContainer const* AsyncScrollTree::snap_container_for_node(AsyncScrollNodeID node_id) const
{
    for (auto const& snap_container : m_snap_containers) {
        if (snap_container.node_id == node_id)
            return &snap_container;
    }
    return nullptr;
}

AsyncScrollNode const* AsyncScrollTree::scroll_node_for_stable_id(Web::AsyncScrollNodeStableID stable_node_id) const
{
    for (auto const& node : m_scroll_nodes) {
        if (node.stable_node_id == stable_node_id)
            return &node;
    }
    return nullptr;
}

Gfx::FloatPoint AsyncScrollTree::clamp_scroll_offset_to_node(AsyncScrollNode const& node, Gfx::FloatPoint scroll_offset)
{
    scroll_offset.set_x(max(node.min_scroll_offset.x(), min(scroll_offset.x(), node.max_scroll_offset.x())));
    scroll_offset.set_y(max(node.min_scroll_offset.y(), min(scroll_offset.y(), node.max_scroll_offset.y())));
    return scroll_offset;
}

Gfx::FloatPoint AsyncScrollTree::scroll_offset_for_node(AsyncScrollNode const& node, Compositing::ScrollStateSnapshot const& scroll_state_snapshot)
{
    auto device_offset = scroll_state_snapshot.device_offset_for_index(node.node_id.scroll_node_index);
    return { -device_offset.x(), -device_offset.y() };
}

bool AsyncScrollTree::can_scroll_node_by_delta(AsyncScrollNode const& node, Compositing::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint delta)
{
    auto scroll_offset = scroll_offset_for_node(node, scroll_state_snapshot);
    if (node.can_be_wheel_scrolled_horizontally && delta.x() < 0 && scroll_offset.x() > node.min_scroll_offset.x())
        return true;
    if (node.can_be_wheel_scrolled_horizontally && delta.x() > 0 && scroll_offset.x() < node.max_scroll_offset.x())
        return true;
    if (node.can_be_wheel_scrolled_vertically && delta.y() < 0 && scroll_offset.y() > node.min_scroll_offset.y())
        return true;
    if (node.can_be_wheel_scrolled_vertically && delta.y() > 0 && scroll_offset.y() < node.max_scroll_offset.y())
        return true;
    return false;
}

Optional<AsyncScrollNodeID> AsyncScrollTree::scrollable_ancestor_for_node(AsyncScrollNodeID node_id, Compositing::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint delta) const
{
    auto const* node = scroll_node_for_id(node_id);
    if (!node)
        return {};

    auto node_blocks_chaining = [&](AsyncScrollNode const& candidate) {
        if (delta.x() != 0 && candidate.overscroll_behavior_x != ScrollOverscrollBehavior::Auto)
            return true;
        if (delta.y() != 0 && candidate.overscroll_behavior_y != ScrollOverscrollBehavior::Auto)
            return true;
        return false;
    };
    if (node_blocks_chaining(*node))
        return {};

    Optional<AsyncScrollNodeID> outermost_ancestor;
    auto parent_node_id = node->parent_node_id;
    while (parent_node_id.has_value()) {
        auto const* parent_node = scroll_node_for_id(*parent_node_id);
        if (!parent_node)
            return {};
        outermost_ancestor = *parent_node_id;
        if (can_scroll_node_by_delta(*parent_node, scroll_state_snapshot, delta))
            return *parent_node_id;
        if (node_blocks_chaining(*parent_node))
            return *parent_node_id;
        parent_node_id = parent_node->parent_node_id;
    }
    // When every ancestor is already at its boundary, the outermost scrolling box owns the boundary action.
    return outermost_ancestor;
}

Gfx::FloatPoint AsyncScrollTree::apply_scroll_delta_to_node(AsyncScrollNode const& node, Gfx::FloatPoint delta, Compositing::ScrollStateSnapshot& scroll_state_snapshot)
{
    auto old_scroll_offset = scroll_offset_for_node(node, scroll_state_snapshot);
    Gfx::FloatPoint wheel_scrollable_delta {
        node.can_be_wheel_scrolled_horizontally ? delta.x() : 0,
        node.can_be_wheel_scrolled_vertically ? delta.y() : 0,
    };
    auto new_scroll_offset = clamp_scroll_offset_to_node(node, old_scroll_offset.translated(wheel_scrollable_delta));
    if (new_scroll_offset == old_scroll_offset) {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Async scroll node {} did not move for delta {},{} (offset={},{} max={},{})",
            node.node_id.scroll_node_index.value(), delta.x(), delta.y(), old_scroll_offset.x(), old_scroll_offset.y(), node.max_scroll_offset.x(), node.max_scroll_offset.y());
        return delta;
    }

    scroll_state_snapshot.set_device_offset_for_index(node.node_id.scroll_node_index, { -new_scroll_offset.x(), -new_scroll_offset.y() });

    Gfx::FloatPoint consumed_delta {
        new_scroll_offset.x() - old_scroll_offset.x(),
        new_scroll_offset.y() - old_scroll_offset.y()
    };
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Async scroll node {} moved from {},{} to {},{} (consumed={},{} remaining={},{})",
        node.node_id.scroll_node_index.value(),
        old_scroll_offset.x(), old_scroll_offset.y(),
        new_scroll_offset.x(), new_scroll_offset.y(),
        consumed_delta.x(), consumed_delta.y(),
        delta.x() - consumed_delta.x(), delta.y() - consumed_delta.y());
    return {
        delta.x() - consumed_delta.x(),
        delta.y() - consumed_delta.y()
    };
}

AsyncScrollApplication AsyncScrollTree::apply_scroll_delta(AsyncScrollNodeID node_id, Gfx::FloatPoint delta, Compositing::AccumulatedVisualContextTree const& visual_context_tree, Compositing::ScrollStateSnapshot& scroll_state_snapshot, ScrollChaining scroll_chaining)
{
    AsyncScrollApplication application;
    auto append_scroll_offset = [&](AsyncScrollNode const& node, Gfx::FloatPoint consumed_delta) {
        auto later_scroll = AsyncScrollOffset {
            .stable_node_id = node.stable_node_id,
            .compositor_scroll_offset = scroll_offset_for_node(node, scroll_state_snapshot),
            .unadopted_scroll_delta = consumed_delta,
            .last_relative_scroll_delta = {},
        };
        for (auto& existing : application.scroll_offsets) {
            if (existing.stable_node_id != later_scroll.stable_node_id)
                continue;
            existing.merge_later_scroll(later_scroll);
            return;
        }
        application.scroll_offsets.append(later_scroll);
    };

    auto apply_axis = [&](bool horizontal) {
        auto remaining = horizontal ? delta.x() : delta.y();
        if (remaining == 0)
            return;

        auto on_axis = [&](float value) { return horizontal ? Gfx::FloatPoint { value, 0 } : Gfx::FloatPoint { 0, value }; };
        // A horizontal delta left at a boundary may go to the browser, such as for a swipe through history.
        auto stop_at_boundary = [&](AsyncScrollNodeID boundary_node_id) {
            if (!horizontal)
                return;
            application.horizontal_boundary_node = boundary_node_id;
            application.unconsumed_delta.set_x(remaining);
        };

        auto current_node_id = node_id;
        for (size_t remaining_handoffs = m_scroll_nodes.size(); remaining_handoffs > 0; --remaining_handoffs) {
            auto const* node = scroll_node_for_id(current_node_id);
            if (!node)
                return;

            auto remainder = apply_scroll_delta_to_node(*node, on_axis(remaining), scroll_state_snapshot);
            auto axis_remainder = horizontal ? remainder.x() : remainder.y();
            auto consumed = remaining - axis_remainder;
            if (consumed != 0)
                append_scroll_offset(*node, on_axis(consumed));
            remaining = axis_remainder;
            if (remaining == 0)
                return;

            auto behavior = horizontal ? node->overscroll_behavior_x : node->overscroll_behavior_y;
            if (behavior == ScrollOverscrollBehavior::None)
                return;
            if (behavior == ScrollOverscrollBehavior::Contain || scroll_chaining == ScrollChaining::None) {
                stop_at_boundary(current_node_id);
                return;
            }

            auto ancestor_node_id = scrollable_ancestor_for_node(current_node_id, scroll_state_snapshot, on_axis(remaining));
            if (!ancestor_node_id.has_value()) {
                stop_at_boundary(current_node_id);
                return;
            }
            current_node_id = *ancestor_node_id;
        }
    };

    apply_axis(true);
    apply_axis(false);

    if (!application.scroll_offsets.is_empty())
        Compositing::resolve_sticky_offsets(visual_context_tree, scroll_state_snapshot);
    return application;
}

void AsyncScrollTree::rebuild_wheel_hit_test_targets(RefPtr<Compositing::DisplayList const> const& display_list, Compositing::AccumulatedVisualContextTree const* visual_context_tree, Compositing::ScrollStateSnapshot const& scroll_state_snapshot)
{
    m_cached_wheel_hit_test_targets.clear();
    m_cached_main_thread_wheel_event_targets.clear();
    m_cached_blocking_wheel_event_targets.clear();
    m_visual_context_tree_structural_epoch.clear();
    m_scroll_state_snapshot = scroll_state_snapshot;
    if (!display_list || !visual_context_tree)
        return;

    VERIFY(display_list->compatible_visual_context_tree_structural_epoch() == visual_context_tree->structural_epoch());
    m_visual_context_tree_structural_epoch = visual_context_tree->structural_epoch();

    auto context_is_valid = [&](Compositing::ContextRef context) {
        return visual_context_tree->context_is_valid(context);
    };

    auto spatial_context_has_visual_animation = visual_context_tree->spatial_nodes_in_subtrees_of_transform_animations();
    auto viewport_rect_for_context = [&](Compositing::ContextRef context, Gfx::FloatRect const& rect) -> Optional<Gfx::FloatRect> {
        if (spatial_context_has_visual_animation[context.spatial.value()])
            return {};
        return visual_context_tree->transform_rect_to_viewport(context.spatial, rect, scroll_state_snapshot);
    };

    m_cached_wheel_hit_test_targets.ensure_capacity(m_wheel_hit_test_regions.size());
    for (auto const& target : m_wheel_hit_test_regions) {
        if (!context_is_valid(target.context))
            continue;
        m_cached_wheel_hit_test_targets.append({
            .target_node_id = target.target_node_id,
            .context = target.context,
            .rect = target.rect,
            .corner_radii = target.corner_radii,
            .viewport_rect = viewport_rect_for_context(target.context, target.rect),
            .paint_order_index = target.paint_order_index,
        });
    }

    m_cached_main_thread_wheel_event_targets.ensure_capacity(m_main_thread_wheel_event_regions.size());
    for (auto const& region : m_main_thread_wheel_event_regions) {
        if (!context_is_valid(region.context))
            continue;
        m_cached_main_thread_wheel_event_targets.append({
            .context = region.context,
            .rect = region.rect,
            .viewport_rect = viewport_rect_for_context(region.context, region.rect),
        });
    }

    for (auto const& region : m_blocking_wheel_event_regions) {
        // Keep invalid blockers so hit testing fails closed when their extent cannot be determined.
        m_cached_blocking_wheel_event_targets.append({
            .context = region.context,
            .rect = region.rect,
            .viewport_rect = context_is_valid(region.context) ? viewport_rect_for_context(region.context, region.rect) : Optional<Gfx::FloatRect> {},
        });
    }
}

void AsyncScrollTree::clear_wheel_hit_test_targets()
{
    m_cached_wheel_hit_test_targets.clear();
    m_cached_main_thread_wheel_event_targets.clear();
    m_cached_blocking_wheel_event_targets.clear();
    m_visual_context_tree_structural_epoch.clear();
}

static bool wheel_hit_test_target_contains_point(CachedWheelHitTestTarget const& target, Gfx::FloatPoint position_in_context)
{
    if (!target.rect.contains(position_in_context))
        return false;
    if (!target.corner_radii.has_any_radius())
        return true;
    return target.corner_radii.contains(position_in_context.to_type<int>(), target.rect.to_type<int>());
}

Optional<Gfx::FloatPoint> AsyncScrollTree::scroll_offset_for_node(AsyncScrollNodeID node_id, Compositing::ScrollStateSnapshot const& scroll_state_snapshot) const
{
    if (auto const* node = scroll_node_for_id(node_id))
        return scroll_offset_for_node(*node, scroll_state_snapshot);
    return {};
}

Web::CSSPixelPoint AsyncScrollTree::css_pixels_from_device_offset(Gfx::FloatPoint device_offset) const
{
    return {
        Web::CSSPixels { device_offset.x() / m_device_pixels_per_css_pixel },
        Web::CSSPixels { device_offset.y() / m_device_pixels_per_css_pixel },
    };
}

Gfx::FloatPoint AsyncScrollTree::device_offset_from_css_pixels(Web::CSSPixelPoint offset) const
{
    return {
        static_cast<float>(offset.x().to_double() * m_device_pixels_per_css_pixel),
        static_cast<float>(offset.y().to_double() * m_device_pixels_per_css_pixel),
    };
}

Optional<Web::CSSPixelPoint> AsyncScrollTree::css_scroll_offset_for_node(AsyncScrollNodeID node_id, Compositing::ScrollStateSnapshot const& scroll_state_snapshot) const
{
    return scroll_offset_for_node(node_id, scroll_state_snapshot).map([&](auto device_offset) { return css_pixels_from_device_offset(device_offset); });
}

Optional<Web::UniqueNodeID> AsyncScrollTree::document_id() const
{
    // The scroll nodes belong to the same document, whose viewport need not itself be scrollable.
    if (m_scroll_nodes.is_empty())
        return m_document_id;
    return m_scroll_nodes.first().node_id.document_id;
}

Optional<AsyncScrollNodeID> AsyncScrollTree::viewport_scroll_node_id() const
{
    for (auto const& node : m_scroll_nodes) {
        if (node.is_viewport)
            return node.node_id;
    }
    return {};
}

Optional<AsyncScrollNodeID> AsyncScrollTree::scroll_node_id_for_stable_id(Web::AsyncScrollNodeStableID stable_node_id) const
{
    if (auto const* node = scroll_node_for_stable_id(stable_node_id))
        return node->node_id;
    return {};
}

bool AsyncScrollTree::blocks_wheel_event_at_position(Compositing::AccumulatedVisualContextTree const& visual_context_tree, Gfx::FloatPoint position) const
{
    if (m_has_blocking_wheel_event_region_covering_viewport || !has_wheel_hit_test_targets_for(visual_context_tree))
        return true;

    for (auto const& target : m_cached_blocking_wheel_event_targets) {
        if (!visual_context_tree.context_is_valid(target.context))
            return true;
        if (target.viewport_rect.has_value() && !target.viewport_rect->contains(position))
            continue;
        auto position_in_context = visual_context_tree.transform_point_for_hit_test(target.context, position, m_scroll_state_snapshot);
        if (position_in_context.has_value() && target.rect.contains(*position_in_context))
            return true;
    }
    return false;
}

WheelHitTestResult AsyncScrollTree::hit_test_scroll_node_for_wheel(Compositing::AccumulatedVisualContextTree const& visual_context_tree, Gfx::FloatPoint position, Gfx::FloatPoint delta) const
{
    return hit_test_scroll_node(visual_context_tree, position, delta, false);
}

WheelHitTestResult AsyncScrollTree::hit_test_scroll_node_for_history_swipe(Compositing::AccumulatedVisualContextTree const& visual_context_tree, Gfx::FloatPoint position, Gfx::FloatPoint delta) const
{
    // A native history swipe is browser navigation, so page wheel listeners must not hide the scroll boundary from it.
    // Main-thread regions still block this path because they represent nested navigables with their own scroll state.
    return hit_test_scroll_node(visual_context_tree, position, delta, true);
}

WheelHitTestResult AsyncScrollTree::hit_test_scroll_node(Compositing::AccumulatedVisualContextTree const& visual_context_tree, Gfx::FloatPoint position, Gfx::FloatPoint delta, bool ignore_blocking_wheel_event_regions) const
{
    if (m_visual_context_tree_structural_epoch != visual_context_tree.structural_epoch())
        return {};

    auto context_is_valid = [&](Compositing::ContextRef context) {
        return visual_context_tree.context_is_valid(context);
    };

    if (!ignore_blocking_wheel_event_regions && m_has_blocking_wheel_event_region_covering_viewport)
        return { {}, false, true };

    for (auto const& target : m_cached_main_thread_wheel_event_targets) {
        if (target.viewport_rect.has_value() && !target.viewport_rect->contains(position))
            continue;
        if (!context_is_valid(target.context))
            continue;
        auto position_in_context = visual_context_tree.transform_point_for_hit_test(target.context, position, m_scroll_state_snapshot);
        if (position_in_context.has_value() && target.rect.contains(*position_in_context))
            return { {}, true };
    }

    if (!ignore_blocking_wheel_event_regions && blocks_wheel_event_at_position(visual_context_tree, position))
        return { {}, false, true };

    for (auto const& target : m_cached_wheel_hit_test_targets.in_reverse()) {
        if (target.viewport_rect.has_value() && !target.viewport_rect->contains(position))
            continue;
        if (!context_is_valid(target.context))
            continue;
        auto position_in_context = visual_context_tree.transform_point_for_hit_test(target.context, position, m_scroll_state_snapshot);
        if (!position_in_context.has_value() || !wheel_hit_test_target_contains_point(target, *position_in_context))
            continue;
        if (!target.target_node_id.has_value())
            return {};
        return hit_test_result_for_scroll_node(*target.target_node_id, delta);
    }

    auto viewport_node_id = viewport_scroll_node_id();
    if (!viewport_node_id.has_value())
        return {};
    auto const* viewport_node = scroll_node_for_id(*viewport_node_id);
    if (!viewport_node || !viewport_node->scrollport_rect.to_type<float>().contains(position))
        return {};
    return hit_test_result_for_scroll_node(*viewport_node_id, delta);
}

bool AsyncScrollTree::is_covered_by_hit_test_target_painted_after(u32 paint_order_index, Compositing::AccumulatedVisualContextTree const& visual_context_tree, Gfx::FloatPoint position) const
{
    if (m_visual_context_tree_structural_epoch != visual_context_tree.structural_epoch())
        return true;

    for (auto const& target : m_cached_wheel_hit_test_targets.in_reverse()) {
        if (target.paint_order_index < paint_order_index)
            break;
        if (target.viewport_rect.has_value() && !target.viewport_rect->contains(position))
            continue;
        if (!visual_context_tree.context_is_valid(target.context))
            continue;
        auto position_in_context = visual_context_tree.transform_point_for_hit_test(target.context, position, m_scroll_state_snapshot);
        if (position_in_context.has_value() && wheel_hit_test_target_contains_point(target, *position_in_context))
            return true;
    }
    return false;
}

Optional<AsyncScrollNodeID> AsyncScrollTree::scroll_node_for_keyboard_scroll(Web::AsyncScrollNodeStableID stable_node_id, Gfx::FloatPoint delta, Compositing::ScrollStateSnapshot const& scroll_state_snapshot) const
{
    auto const* node = scroll_node_for_stable_id(stable_node_id);
    if (!node)
        return {};

    // Keyboard routing does not depend on pointer or wheel-listener regions.
    if (can_scroll_node_by_delta(*node, scroll_state_snapshot, delta))
        return node->node_id;
    return scrollable_ancestor_for_node(node->node_id, scroll_state_snapshot, delta);
}

Gfx::FloatPoint AsyncScrollTree::clamped_scroll_offset_for_node(AsyncScrollNodeID node_id, Gfx::FloatPoint scroll_offset) const
{
    auto const* node = scroll_node_for_id(node_id);
    if (!node)
        return scroll_offset;
    return clamp_scroll_offset_to_node(*node, scroll_offset);
}

bool AsyncScrollTree::scroll_node_is_viewport(AsyncScrollNodeID node_id) const
{
    auto const* node = scroll_node_for_id(node_id);
    return node && node->is_viewport;
}

Optional<Gfx::FloatPoint> AsyncScrollTree::set_scroll_offset(AsyncScrollNodeID node_id, Gfx::FloatPoint scroll_offset, Compositing::AccumulatedVisualContextTree const& visual_context_tree, Compositing::ScrollStateSnapshot& scroll_state_snapshot)
{
    auto const* node = scroll_node_for_id(node_id);
    if (!node)
        return {};

    auto new_scroll_offset = clamp_scroll_offset_to_node(*node, scroll_offset);
    scroll_state_snapshot.set_device_offset_for_index(node->node_id.scroll_node_index, { -new_scroll_offset.x(), -new_scroll_offset.y() });
    Compositing::resolve_sticky_offsets(visual_context_tree, scroll_state_snapshot);
    return new_scroll_offset;
}

}
