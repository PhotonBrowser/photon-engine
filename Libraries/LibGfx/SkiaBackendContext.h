/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/Noncopyable.h>

#include <cstdint>

#ifdef USE_DIRECTX
#    include <LibGfx/Direct3DContext.h>
#endif

#ifdef USE_VULKAN
#    include <LibGfx/VulkanContext.h>
#endif

#ifdef AK_OS_MACOS
#    include <LibGfx/MetalContext.h>
#endif

class GrDirectContext;
class SkSurface;

namespace Gfx {

struct VulkanContext;
class Direct3DContext;
class MetalContext;
class PaintingSurface;

class SkiaBackendContext : public AtomicRefCounted<SkiaBackendContext> {
    AK_MAKE_NONCOPYABLE(SkiaBackendContext);
    AK_MAKE_NONMOVABLE(SkiaBackendContext);

public:
    enum class SurfaceAccess {
        Internal,
        External,
    };

#ifdef USE_DIRECTX
    static RefPtr<SkiaBackendContext> create_direct3d_context(NonnullRefPtr<Direct3DContext>);
#endif

#ifdef USE_VULKAN
    static RefPtr<SkiaBackendContext> create_vulkan_context(const VulkanContext& vulkan_context);
#endif

#ifdef AK_OS_MACOS
    static RefPtr<SkiaBackendContext> create_metal_context(NonnullRefPtr<MetalContext>);
#endif

    static void initialize_gpu_backend();
    static RefPtr<SkiaBackendContext> create_independent_gpu_backend();
    static RefPtr<SkiaBackendContext> the_main_thread_context();

    SkiaBackendContext() { }
    virtual ~SkiaBackendContext() { }

    void flush_and_submit(PaintingSurface&);
    void flush_and_submit_async(PaintingSurface&, Function<void()>&&, uint64_t presentation_signal_value = 0);
    void flush_and_submit(SkSurface*, SurfaceAccess);
    void flush_and_submit_async(SkSurface*, SurfaceAccess, Function<void()>&&, uint64_t presentation_signal_value = 0);
    void check_async_work_completion();
    virtual GrDirectContext* sk_context() const = 0;

    virtual MetalContext& metal_context() = 0;
    virtual VulkanContext const& vulkan_context() = 0;
    virtual Direct3DContext& direct3d_context() { VERIFY_NOT_REACHED(); }

protected:
    virtual void flush_and_submit_impl(SkSurface*, SurfaceAccess) = 0;
    virtual void flush_and_submit_async_impl(SkSurface*, SurfaceAccess, Function<void()>&&, uint64_t presentation_signal_value) = 0;

private:
    void perform_post_flush_cleanup();
};

}
