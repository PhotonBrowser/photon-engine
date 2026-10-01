/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/PhotonGaneshPersistentCache.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/SkiaUtils.h>
#include <LibTest/TestCase.h>

#include <AK/Time.h>
#include <AK/StringBuilder.h>
#include <core/SkBlender.h>
#include <core/SkBlendMode.h>
#include <core/SkCanvas.h>
#include <core/SkPaint.h>
#include <core/SkPath.h>
#include <core/SkSurface.h>
#include <gpu/ganesh/GrDirectContext.h>

static int count_red_pixels(Gfx::Bitmap const& bitmap, Gfx::IntRect const& rect)
{
    int count = 0;
    for (int y = rect.top(); y < rect.bottom(); ++y) {
        for (int x = rect.left(); x < rect.right(); ++x) {
            auto color = bitmap.get_pixel(x, y);
            if (color.red() > 128 && color.green() < 64 && color.blue() < 64)
                ++count;
        }
    }
    return count;
}

TEST_CASE(stroke_then_blend_layer_on_gpu_surface)
{
    // A stroked path selects multisampling on a GPU surface, and a layer that blends with its backdrop reads the
    // destination in the same surface. Both must survive in the same frame with a layer between them.

    auto context = Gfx::SkiaBackendContext::create_independent_gpu_backend();
    if (!context) {
        warnln("No GPU backend available, skipping");
        return;
    }

    Gfx::IntSize size { 200, 100 };
    auto surface = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, context);
    auto& canvas = surface->canvas();
    canvas.clear(SK_ColorBLACK);

    Gfx::IntRect box_rect { 120, 20, 60, 60 };
    SkPaint box_paint;
    box_paint.setColor(SK_ColorRED);
    canvas.drawRect(SkRect::MakeXYWH(box_rect.x(), box_rect.y(), box_rect.width(), box_rect.height()), box_paint);

    SkPaint stroke_paint;
    stroke_paint.setColor(SK_ColorRED);
    stroke_paint.setAntiAlias(true);
    stroke_paint.setStyle(SkPaint::kStroke_Style);
    stroke_paint.setStrokeWidth(4);
    canvas.drawPath(SkPath::Circle(50, 50, 20), stroke_paint);

    SkPaint layer_paint;
    layer_paint.setBlender(Gfx::to_skia_blender(Gfx::CompositingAndBlendingOperator::Overlay));
    canvas.saveLayer(nullptr, &layer_paint);
    SkPaint gray_paint;
    gray_paint.setColor(SkColorSetARGB(255, 0x80, 0x80, 0x80));
    canvas.drawRect(SkRect::MakeWH(100, 100), gray_paint);
    canvas.restore();

    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size));
    surface->read_into_bitmap(*bitmap);

    EXPECT_EQ(count_red_pixels(*bitmap, box_rect), box_rect.width() * box_rect.height());
    EXPECT(count_red_pixels(*bitmap, { 26, 26, 48, 48 }) > 0);
}

#ifdef AK_OS_MACOS
TEST_CASE(ganesh_persistent_cache_rect_workload)
{
    // Run this named case in two separate TestPaintingSurface processes. The fixed scene exercises real
    // Ganesh Metal pipelines while avoiding network and page-load variability.
    auto context = Gfx::SkiaBackendContext::create_independent_gpu_backend();
    if (!context) {
        warnln("No Metal backend available, skipping");
        return;
    }

    constexpr int width = 1024;
    constexpr int height = 768;
    auto surface = Gfx::PaintingSurface::create_with_size({ width, height }, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, context);
    EXPECT(surface->sk_surface().recordingContext() == context->sk_context());
    auto diagnostics_before = Gfx::PhotonGaneshPersistentCache::diagnostics_snapshot();
    auto& canvas = surface->canvas();
    canvas.clear(SK_ColorWHITE);

    constexpr SkBlendMode blend_modes[] = {
        SkBlendMode::kSrcOver,
        SkBlendMode::kMultiply,
        SkBlendMode::kScreen,
        SkBlendMode::kOverlay,
        SkBlendMode::kDarken,
        SkBlendMode::kLighten,
        SkBlendMode::kColorDodge,
        SkBlendMode::kColorBurn,
        SkBlendMode::kHardLight,
        SkBlendMode::kSoftLight,
        SkBlendMode::kDifference,
        SkBlendMode::kExclusion,
    };

    for (size_t i = 0; i < 384; ++i) {
        SkPaint paint;
        paint.setColor(SkColorSetARGB(220, (i * 37) % 255, (i * 67) % 255, (i * 97) % 255));
        paint.setBlendMode(blend_modes[i % std::size(blend_modes)]);
        paint.setAntiAlias((i % 2) != 0);
        auto x = static_cast<float>((i * 29) % width);
        auto y = static_cast<float>((i * 43) % height);
        canvas.drawRect(SkRect::MakeXYWH(x, y, 96, 72), paint);
    }

    auto const started_at = MonotonicTime::now();
    context->flush_and_submit(&surface->sk_surface(), Gfx::SkiaBackendContext::SurfaceAccess::Internal);
    auto const elapsed = MonotonicTime::now() - started_at;
    auto diagnostics_after = Gfx::PhotonGaneshPersistentCache::diagnostics_snapshot();
    auto descriptor_requests = diagnostics_after.descriptor_requests - diagnostics_before.descriptor_requests;
    EXPECT(descriptor_requests > 0);
    EXPECT(diagnostics_after.descriptor_hashes.size() > diagnostics_before.descriptor_hashes.size());

    int resource_count = 0;
    size_t resource_bytes = 0;
    context->sk_context()->getResourceCacheUsage(&resource_count, &resource_bytes);

    StringBuilder descriptor_hashes;
    for (size_t i = diagnostics_before.descriptor_hashes.size(); i < diagnostics_after.descriptor_hashes.size(); ++i) {
        if (i > diagnostics_before.descriptor_hashes.size())
            descriptor_hashes.append(',');
        descriptor_hashes.appendff("{:016x}", diagnostics_after.descriptor_hashes[i]);
    }
    dbgln("Photon Ganesh persistent-cache benchmark: scene=rect-ops-v2 draw_ops=384 gpu_surface=yes descriptor_requests={} misses={} hits={} stores={} resource_count={} resource_bytes={} descriptor_hashes={} flush_and_submit={:.2f}ms", descriptor_requests,
        diagnostics_after.misses - diagnostics_before.misses,
        diagnostics_after.hits - diagnostics_before.hits,
        diagnostics_after.stores - diagnostics_before.stores,
        resource_count, resource_bytes, descriptor_hashes.to_byte_string(), elapsed.to_seconds_f64() * 1000.0);
}
#endif
