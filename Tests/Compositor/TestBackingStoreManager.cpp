/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/BackingStoreManager.h>
#include <LibGfx/PaintingSurface.h>
#include <LibTest/TestCase.h>

TEST_CASE(backing_store_padding_is_reused_during_live_resize)
{
    Compositor::BackingStoreManager manager;
    auto first = manager.resize_backing_stores_if_needed(
        { 100, 100 }, Compositing::WindowResizingInProgress::Yes, true);
    VERIFY(first.has_value());
    EXPECT_EQ(first->size, (Gfx::IntSize { 356, 356 }));

    auto unchanged = manager.resize_backing_stores_if_needed(
        { 120, 120 }, Compositing::WindowResizingInProgress::Yes, true);
    EXPECT(!unchanged.has_value());

    auto grown = manager.resize_backing_stores_if_needed(
        { 357, 120 }, Compositing::WindowResizingInProgress::Yes, true);
    VERIFY(grown.has_value());
    EXPECT_EQ(grown->size, (Gfx::IntSize { 613, 376 }));

    auto settled = manager.resize_backing_stores_if_needed(
        { 357, 120 }, Compositing::WindowResizingInProgress::No, true);
    VERIFY(settled.has_value());
    EXPECT_EQ(settled->size, (Gfx::IntSize { 357, 120 }));
}

#ifdef AK_OS_MACOS
TEST_CASE(a_client_that_samples_backing_stores_on_its_gpu_gets_a_fourth_store)
{
    Compositor::BackingStoreManager manager;
    auto allocation = manager.resize_backing_stores_if_needed(
        { 4, 4 }, Compositing::WindowResizingInProgress::No, true,
        Compositor::BackingStoreManager::ClientSamplesOnGpu::Yes);
    VERIFY(allocation.has_value());
    EXPECT_EQ(allocation->bitmap_ids.size(), 4u);

    Compositor::BackingStoreManager copying_manager;
    auto copying_allocation = copying_manager.resize_backing_stores_if_needed(
        { 4, 4 }, Compositing::WindowResizingInProgress::No, true,
        Compositor::BackingStoreManager::ClientSamplesOnGpu::No);
    VERIFY(copying_allocation.has_value());
    EXPECT_EQ(copying_allocation->bitmap_ids.size(), 3u);
}
#endif
