/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/BackingStoreManager.h>
#include <Compositor/PaintingSurface.h>
#include <LibTest/TestCase.h>

TEST_CASE(backing_store_padding_is_reused_during_live_resize)
{
    Compositor::BackingStoreManager manager;
    auto first = manager.resize_backing_stores_if_needed(
        { 100, 100 }, Compositing::WindowResizingInProgress::Yes);
    VERIFY(first.has_value());
    EXPECT_EQ(first->size, (Gfx::IntSize { 356, 356 }));

    auto unchanged = manager.resize_backing_stores_if_needed(
        { 120, 120 }, Compositing::WindowResizingInProgress::Yes);
    EXPECT(!unchanged.has_value());

    auto grown = manager.resize_backing_stores_if_needed(
        { 357, 120 }, Compositing::WindowResizingInProgress::Yes);
    VERIFY(grown.has_value());
    EXPECT_EQ(grown->size, (Gfx::IntSize { 613, 376 }));

    auto settled = manager.resize_backing_stores_if_needed(
        { 357, 120 }, Compositing::WindowResizingInProgress::No);
    VERIFY(settled.has_value());
    EXPECT_EQ(settled->size, (Gfx::IntSize { 357, 120 }));
}

TEST_CASE(page_starts_with_a_front_buffer_and_a_render_target)
{
    Compositor::BackingStoreManager manager;
    auto allocation = manager.resize_backing_stores_if_needed(
        { 4, 4 }, Compositing::WindowResizingInProgress::No);
    VERIFY(allocation.has_value());
    EXPECT_EQ(allocation->bitmap_ids.size(), 2u);

    auto publication = manager.allocate_backing_stores(
        *allocation, {}, true, Compositor::BackingStoreManager::GpuSharing::Disallowed);
    VERIFY(publication.has_value());
    EXPECT_EQ(publication->bitmap_ids.size(), 2u);

    auto first_render_target = manager.acquire_render_target({ {}, { 4, 4 } });
    EXPECT(first_render_target.has_value());
}
