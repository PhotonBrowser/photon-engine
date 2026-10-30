/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/NeverDestroyed.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <cstdlib>
#include <cstring>
#include <Compositor/PaintingSurface.h>
#include <LibGfx/Bitmap.h>
#ifdef USE_DIRECTX
#    include <LibGfx/Direct3DContext.h>
#endif
#include <Compositor/SkiaBackendContext.h>
#ifdef AK_OS_MACOS
#    include <Compositor/PhotonGaneshPersistentCache.h>
#endif

#include <core/SkSurface.h>
#ifdef AK_OS_MACOS
#    ifdef SK_GRAPHITE
#        include <core/SkExecutor.h>
#    endif
#endif
#include <gpu/ganesh/GrContextOptions.h>
#include <gpu/ganesh/GrDirectContext.h>

#ifdef USE_DIRECTX
#    include <AK/Windows.h>
#    include <gpu/ganesh/d3d/GrD3DBackendContext.h>
#    include <gpu/ganesh/d3d/GrD3DDirectContext.h>
#endif

#ifdef USE_VULKAN
#    include <Compositor/SkiaVulkanMemoryAllocator.h>
#    include <gpu/ganesh/vk/GrVkDirectContext.h>
#    include <gpu/vk/VulkanBackendContext.h>
#    include <gpu/vk/VulkanExtensions.h>

template<>
constexpr bool AllocatedWithSystemAllocator<skgpu::VulkanExtensions> = true;
#endif

#ifdef AK_OS_MACOS
#    include <gpu/ganesh/GrBackendSurface.h>
#    include <gpu/ganesh/mtl/GrMtlBackendContext.h>
#    include <gpu/ganesh/mtl/GrMtlBackendSurface.h>
#    include <gpu/ganesh/mtl/GrMtlDirectContext.h>
#    ifdef SK_GRAPHITE
#        pragma push_macro("TODO")
#        undef TODO
#        include <gpu/graphite/Context.h>
#        include <gpu/graphite/ContextOptions.h>
#        include <gpu/graphite/GraphiteTypes.h>
#        include <gpu/graphite/mtl/MtlBackendContext.h>
#        include <gpu/graphite/mtl/MtlGraphiteTypes_cpp.h>
#        pragma pop_macro("TODO")
#    endif
#endif

namespace Gfx {

#if defined(AK_OS_MACOS) || defined(USE_DIRECTX) || defined(USE_VULKAN)
// The compositor has one Skia context shared by every tab. Keep its reusable GPU resources bounded so one heavy page
// cannot retain hundreds of MiB outside the per-tab memory totals shown by the shell.
static constexpr size_t skia_resource_cache_limit = 128 * MiB;
#endif
static constexpr auto skia_deferred_cleanup_interval = AK::Duration::from_seconds(1);
static constexpr auto skia_aggressive_cleanup_interval = AK::Duration::from_seconds(5);
static constexpr auto skia_deferred_cleanup_resource_age = std::chrono::seconds(1);
static constexpr auto skia_resource_cache_high_watermark = 192 * MiB;
static constexpr auto skia_resource_cache_critical_watermark = 256 * MiB;

static auto& main_thread_context()
{
    static NeverDestroyed<RefPtr<SkiaBackendContext>> context;
    return *context;
}

static SkiaBackendContext::SurfaceAccess surface_access_for(PaintingSurface const& surface)
{
    // Keep both contracts: Photon can require external access, and upstream marks images shared with another process.
    return surface.requires_external_access() || surface.wraps_shared_image()
        ? SkiaBackendContext::SurfaceAccess::External
        : SkiaBackendContext::SurfaceAccess::Internal;
}

#if defined(AK_OS_MACOS) || defined(USE_DIRECTX) || defined(USE_VULKAN)
static void invoke_async_flush_callback(void* context)
{
    auto* callback = static_cast<Function<void()>*>(context);
    auto callback_to_invoke = move(*callback);
    delete callback;
    callback_to_invoke();
}

#ifdef AK_OS_MACOS
#    ifdef SK_GRAPHITE
static void invoke_graphite_flush_callback(skgpu::graphite::GpuFinishedContext context, skgpu::CallbackResult)
{
    auto* callback = static_cast<Function<void()>*>(context);
    auto callback_to_invoke = move(*callback);
    delete callback;
    callback_to_invoke();
}
#    endif
#endif

static SkSurfaces::BackendSurfaceAccess to_backend_surface_access(SkiaBackendContext::SurfaceAccess access)
{
    return access == SkiaBackendContext::SurfaceAccess::External
        ? SkSurfaces::BackendSurfaceAccess::kPresent
        : SkSurfaces::BackendSurfaceAccess::kNoAccess;
}

static void flush_and_submit_async_to_context(GrDirectContext& context, SkSurface* surface, SkiaBackendContext::SurfaceAccess access, Function<void()>&& callback, GrBackendSemaphore* signal_semaphore = nullptr)
{
    GrFlushInfo flush_info {};
    if (signal_semaphore) {
        flush_info.fNumSemaphores = 1;
        flush_info.fSignalSemaphores = signal_semaphore;
    }
    flush_info.fFinishedProc = invoke_async_flush_callback;
    flush_info.fFinishedContext = new Function<void()>(move(callback));
    auto const flush_started_at = MonotonicTime::now();
    context.flush(surface, to_backend_surface_access(access), flush_info);
    auto const flush_duration = MonotonicTime::now() - flush_started_at;
    auto const submit_started_at = MonotonicTime::now();
    VERIFY(context.submit(GrSyncCpu::kNo));
    auto const submit_duration = MonotonicTime::now() - submit_started_at;
    if (std::getenv("PHOTON_VERBOSE") && (flush_duration.to_microseconds() >= 5000 || submit_duration.to_microseconds() >= 5000))
        dbgln("Photon Skia submit detail: flush={:.2f}ms metal_submit={:.2f}ms", flush_duration.to_seconds_f64() * 1000.0, submit_duration.to_seconds_f64() * 1000.0);
}
#endif

void SkiaBackendContext::check_async_work_completion()
{
#ifdef SK_GRAPHITE
    if (auto* context = graphite_context()) {
        context->checkAsyncWorkCompletion();
        return;
    }
#endif
    if (auto* context = sk_context())
        context->checkAsyncWorkCompletion();
}

void SkiaBackendContext::flush_and_submit(PaintingSurface& surface)
{
    flush_and_submit(&surface.sk_surface(), surface_access_for(surface));
}

void SkiaBackendContext::flush_and_submit_async(PaintingSurface& surface, Function<void()>&& callback, uint64_t presentation_signal_value)
{
    flush_and_submit_async(&surface.sk_surface(), surface_access_for(surface), move(callback), presentation_signal_value);
}

void SkiaBackendContext::flush_and_submit(SkSurface* surface, SurfaceAccess access)
{
    flush_and_submit_impl(surface, access);

    perform_post_flush_cleanup();
}

void SkiaBackendContext::flush_and_submit_async(SkSurface* surface, SurfaceAccess access, Function<void()>&& callback, uint64_t presentation_signal_value)
{
    flush_and_submit_async_impl(surface, access, move(callback), presentation_signal_value);

    perform_post_flush_cleanup();
}

void SkiaBackendContext::purge_resources_unused_for(AK::Duration age)
{
#ifdef SK_GRAPHITE
    if (auto* context = graphite_context()) {
        auto const cleanup_age = std::chrono::milliseconds(age.to_milliseconds());
        context->performDeferredCleanup(cleanup_age);
        if (auto* recorder = graphite_recorder())
            recorder->performDeferredCleanup(cleanup_age);
        return;
    }
#endif
    auto* context = sk_context();
    if (!context)
        return;

    size_t resource_bytes_before = 0;
    context->getResourceCacheUsage(nullptr, &resource_bytes_before);
    context->performDeferredCleanup(std::chrono::milliseconds(age.to_milliseconds()));
    if (std::getenv("PHOTON_VERBOSE")) {
        size_t resource_bytes_after = 0;
        context->getResourceCacheUsage(nullptr, &resource_bytes_after);
        dbgln("Photon Skia idle purge: cache_bytes={}->{}", resource_bytes_before, resource_bytes_after);
    }
}

void SkiaBackendContext::purge_unlocked_scratch_resources()
{
#ifdef SK_GRAPHITE
    if (auto* context = graphite_context()) {
        auto* recorder = graphite_recorder();
        auto const bytes_before = context->currentBudgetedBytes() + recorder->currentBudgetedBytes();
        context->freeGpuResources();
        recorder->freeGpuResources();
        auto const bytes_after = context->currentBudgetedBytes() + recorder->currentBudgetedBytes();
        dbgln("Photon Graphite memory-pressure GPU cache: {} -> {} bytes", bytes_before, bytes_after);
        return;
    }
#endif
    auto* context = sk_context();
    if (!context)
        return;

    size_t resource_bytes_before = 0;
    context->getResourceCacheUsage(nullptr, &resource_bytes_before);
    context->purgeUnlockedResources(GrPurgeResourceOptions::kScratchResourcesOnly);
    size_t resource_bytes_after = 0;
    context->getResourceCacheUsage(nullptr, &resource_bytes_after);
    dbgln("Photon memory-pressure GPU cache: {} -> {} bytes", resource_bytes_before, resource_bytes_after);
}

void SkiaBackendContext::perform_post_flush_cleanup()
{
#ifdef SK_GRAPHITE
    if (auto* context = graphite_context()) {
        static thread_local Optional<MonotonicTime> s_last_cleanup;
        auto const now = MonotonicTime::now();
        if (s_last_cleanup.has_value() && now - *s_last_cleanup < skia_deferred_cleanup_interval)
            return;
        s_last_cleanup = now;

        auto* recorder = graphite_recorder();
        context->performDeferredCleanup(skia_deferred_cleanup_resource_age);
        recorder->performDeferredCleanup(skia_deferred_cleanup_resource_age);
        auto const bytes_before = context->currentBudgetedBytes() + recorder->currentBudgetedBytes();
        if (bytes_before >= skia_resource_cache_critical_watermark) {
            context->performDeferredCleanup(std::chrono::milliseconds(0));
            recorder->performDeferredCleanup(std::chrono::milliseconds(0));
        }
        if (std::getenv("PHOTON_VERBOSE")) {
            auto const bytes_after = context->currentBudgetedBytes() + recorder->currentBudgetedBytes();
            dbgln("Photon Graphite cleanup: cache_bytes={}->{}", bytes_before, bytes_after);
        }
        return;
    }
#endif
    auto* context = sk_context();
    if (!context)
        return;

    static thread_local Optional<MonotonicTime> s_last_deferred_cleanup;
    static thread_local Optional<MonotonicTime> s_last_aggressive_cleanup;

    auto const now = MonotonicTime::now();
    if (s_last_deferred_cleanup.has_value() && now - *s_last_deferred_cleanup < skia_deferred_cleanup_interval)
        return;

    s_last_deferred_cleanup = now;
    int resource_count_before = 0;
    size_t resource_bytes_before = 0;
    context->getResourceCacheUsage(&resource_count_before, &resource_bytes_before);

    auto const deferred_cleanup_started_at = MonotonicTime::now();
    context->performDeferredCleanup(skia_deferred_cleanup_resource_age);
    auto const deferred_cleanup_duration = MonotonicTime::now() - deferred_cleanup_started_at;

    int resource_count_after_deferred_cleanup = 0;
    size_t resource_bytes = 0;
    context->getResourceCacheUsage(&resource_count_after_deferred_cleanup, &resource_bytes);
    auto const resource_bytes_after_deferred_cleanup = resource_bytes;
    if (resource_bytes < skia_resource_cache_high_watermark) {
        if (std::getenv("PHOTON_VERBOSE") && deferred_cleanup_duration.to_microseconds() >= 5000)
            dbgln("Photon Skia cleanup detail: deferred={:.2f}ms aggressive=skipped purge=skipped resources={}->{} cache_bytes={}->{}", deferred_cleanup_duration.to_seconds_f64() * 1000.0, resource_count_before, resource_count_after_deferred_cleanup, resource_bytes_before, resource_bytes);
        return;
    }
    if (s_last_aggressive_cleanup.has_value() && now - *s_last_aggressive_cleanup < skia_aggressive_cleanup_interval) {
        if (std::getenv("PHOTON_VERBOSE") && deferred_cleanup_duration.to_microseconds() >= 5000)
            dbgln("Photon Skia cleanup detail: deferred={:.2f}ms aggressive=throttled purge=skipped resources={}->{} cache_bytes={}->{}", deferred_cleanup_duration.to_seconds_f64() * 1000.0, resource_count_before, resource_count_after_deferred_cleanup, resource_bytes_before, resource_bytes);
        return;
    }

    s_last_aggressive_cleanup = now;
    auto const aggressive_cleanup_started_at = MonotonicTime::now();
    context->performDeferredCleanup(std::chrono::milliseconds(0));
    auto const aggressive_cleanup_duration = MonotonicTime::now() - aggressive_cleanup_started_at;
    int resource_count_after_aggressive_cleanup = 0;
    context->getResourceCacheUsage(&resource_count_after_aggressive_cleanup, &resource_bytes);
    AK::Duration purge_duration {};
    if (resource_bytes >= skia_resource_cache_critical_watermark) {
        auto const purge_started_at = MonotonicTime::now();
        context->purgeUnlockedResources(GrPurgeResourceOptions::kScratchResourcesOnly);
        purge_duration = MonotonicTime::now() - purge_started_at;
    }

    int resource_count_after_purge = 0;
    size_t resource_bytes_after_purge = 0;
    context->getResourceCacheUsage(&resource_count_after_purge, &resource_bytes_after_purge);
    auto const cleanup_duration = MonotonicTime::now() - deferred_cleanup_started_at;
    if (std::getenv("PHOTON_VERBOSE") && cleanup_duration.to_microseconds() >= 5000)
        dbgln("Photon Skia cleanup detail: total={:.2f}ms deferred={:.2f}ms aggressive={:.2f}ms purge={:.2f}ms resources={}/{}/{}/{} cache_bytes={}/{}/{}/{}", cleanup_duration.to_seconds_f64() * 1000.0, deferred_cleanup_duration.to_seconds_f64() * 1000.0, aggressive_cleanup_duration.to_seconds_f64() * 1000.0, purge_duration.to_seconds_f64() * 1000.0, resource_count_before, resource_count_after_deferred_cleanup, resource_count_after_aggressive_cleanup, resource_count_after_purge, resource_bytes_before, resource_bytes_after_deferred_cleanup, resource_bytes, resource_bytes_after_purge);
}

size_t SkiaBackendContext::resource_cache_bytes() const
{
    size_t bytes = 0;
#ifdef SK_GRAPHITE
    if (auto* context = graphite_context()) {
        bytes = context->currentBudgetedBytes();
        if (auto* recorder = graphite_recorder())
            bytes += recorder->currentBudgetedBytes();
        return bytes;
    }
#endif
    if (auto* context = sk_context())
        context->getResourceCacheUsage(nullptr, &bytes);
    return bytes;
}

void SkiaBackendContext::initialize_gpu_backend(Optional<GpuDeviceIdentity> const& preferred_device)
{
    VERIFY(!main_thread_context());

    main_thread_context() = create_independent_gpu_backend(preferred_device);
}

RefPtr<SkiaBackendContext> SkiaBackendContext::create_independent_gpu_backend([[maybe_unused]] Optional<GpuDeviceIdentity> const& preferred_device)
{
#ifdef AK_OS_MACOS
    auto metal_context = get_metal_context();
    if (!metal_context)
        return {};
    return create_metal_context(*metal_context);
#elif defined(USE_DIRECTX)
    auto maybe_direct3d_context = Gfx::create_direct3d_context();
    if (maybe_direct3d_context.is_error()) {
        dbgln("Direct3D 12 context creation failed: {}", maybe_direct3d_context.error());
        return {};
    }
    return create_direct3d_context(maybe_direct3d_context.release_value());
#elif defined(USE_VULKAN)
    auto maybe_vulkan_context = Gfx::create_vulkan_context(preferred_device);
    if (maybe_vulkan_context.is_error()) {
        dbgln("Vulkan context creation failed: {}", maybe_vulkan_context.error());
        return {};
    }
    auto vulkan_context = maybe_vulkan_context.release_value();
    return create_vulkan_context(vulkan_context);
#else
    return {};
#endif
}

RefPtr<SkiaBackendContext> SkiaBackendContext::the_main_thread_context()
{
    return main_thread_context();
}

#ifdef USE_DIRECTX
class SkiaDirect3DBackendContext final : public SkiaBackendContext {
    AK_MAKE_NONCOPYABLE(SkiaDirect3DBackendContext);
    AK_MAKE_NONMOVABLE(SkiaDirect3DBackendContext);

public:
    SkiaDirect3DBackendContext(sk_sp<GrDirectContext> context, NonnullRefPtr<Direct3DContext> direct3d_context)
        : m_context(move(context))
        , m_direct3d_context(move(direct3d_context))
    {
    }

    ~SkiaDirect3DBackendContext() override
    {
        m_context.reset();
    }

    void flush_and_submit_impl(SkSurface* surface, SurfaceAccess access) override
    {
        GrFlushInfo const flush_info {};
        m_context->flush(surface, to_backend_surface_access(access), flush_info);
        m_context->submit(GrSyncCpu::kYes);
    }

    void flush_and_submit_async_impl(SkSurface* surface, SurfaceAccess access, Function<void()>&& callback, uint64_t) override
    {
        flush_and_submit_async_to_context(*m_context, surface, access, move(callback));
    }

    GrDirectContext* sk_context() const override { return m_context.get(); }

    VulkanContext const& vulkan_context() override { VERIFY_NOT_REACHED(); }

    MetalContext& metal_context() override { VERIFY_NOT_REACHED(); }

    Direct3DContext& direct3d_context() override { return m_direct3d_context; }

private:
    sk_sp<GrDirectContext> m_context;
    NonnullRefPtr<Direct3DContext> m_direct3d_context;
};

RefPtr<SkiaBackendContext> SkiaBackendContext::create_direct3d_context(NonnullRefPtr<Direct3DContext> direct3d_context)
{
    GrD3DBackendContext backend_context;
    backend_context.fAdapter.retain(direct3d_context->adapter());
    backend_context.fDevice.retain(direct3d_context->device());
    backend_context.fQueue.retain(direct3d_context->queue());
    backend_context.fProtectedContext = GrProtected::kNo;

    auto context = GrDirectContexts::MakeD3D(backend_context);
    if (!context) {
        dbgln("Skia Direct3D context creation failed");
        return {};
    }

    context->setResourceCacheLimit(skia_resource_cache_limit);
    return adopt_ref(*new SkiaDirect3DBackendContext(move(context), move(direct3d_context)));
}
#endif

#ifdef USE_VULKAN
class SkiaVulkanBackendContext final : public SkiaBackendContext {
    AK_MAKE_NONCOPYABLE(SkiaVulkanBackendContext);
    AK_MAKE_NONMOVABLE(SkiaVulkanBackendContext);

public:
    SkiaVulkanBackendContext(sk_sp<GrDirectContext> context, VulkanContext const& vulkan_context, NonnullOwnPtr<skgpu::VulkanExtensions> extensions)
        : m_context(move(context))
        , m_extensions(move(extensions))
        , m_vulkan_context(vulkan_context)
    {
    }

    ~SkiaVulkanBackendContext() override
    {
        m_context.reset();
#    ifdef USE_VULKAN_DMABUF_IMAGES
        if (m_vulkan_context.command_pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(m_vulkan_context.logical_device, m_vulkan_context.command_pool, nullptr);
#    endif
        if (m_vulkan_context.logical_device != VK_NULL_HANDLE)
            vkDestroyDevice(m_vulkan_context.logical_device, nullptr);
        if (m_vulkan_context.instance != VK_NULL_HANDLE)
            vkDestroyInstance(m_vulkan_context.instance, nullptr);
    }

    void flush_and_submit_impl(SkSurface* surface, SurfaceAccess access) override
    {
        GrFlushInfo const flush_info {};
        m_context->flush(surface, to_backend_surface_access(access), flush_info);
        m_context->submit(GrSyncCpu::kYes);
    }

    void flush_and_submit_async_impl(SkSurface* surface, SurfaceAccess access, Function<void()>&& callback, uint64_t) override
    {
        flush_and_submit_async_to_context(*m_context, surface, access, move(callback));
    }

    skgpu::VulkanExtensions const* extensions() const { return m_extensions.ptr(); }

    GrDirectContext* sk_context() const override { return m_context.get(); }

    VulkanContext const& vulkan_context() override { return m_vulkan_context; }

    MetalContext& metal_context() override { VERIFY_NOT_REACHED(); }

private:
    sk_sp<GrDirectContext> m_context;
    NonnullOwnPtr<skgpu::VulkanExtensions> m_extensions;
    VulkanContext const m_vulkan_context;
};

RefPtr<SkiaBackendContext> SkiaBackendContext::create_vulkan_context(VulkanContext const& vulkan_context)
{
    skgpu::VulkanBackendContext backend_context;

    backend_context.fInstance = vulkan_context.instance;
    backend_context.fDevice = vulkan_context.logical_device;
    backend_context.fQueue = vulkan_context.graphics_queue;
    backend_context.fGraphicsQueueIndex = vulkan_context.graphics_queue_family;
    backend_context.fPhysicalDevice = vulkan_context.physical_device;
    backend_context.fMaxAPIVersion = vulkan_context.api_version;
    backend_context.fGetProc = [](char const* proc_name, VkInstance instance, VkDevice device) {
        if (device != VK_NULL_HANDLE) {
            return vkGetDeviceProcAddr(device, proc_name);
        }
        return vkGetInstanceProcAddr(instance, proc_name);
    };

    auto extensions = make<skgpu::VulkanExtensions>();
    extensions->init(backend_context.fGetProc, vulkan_context.instance, vulkan_context.physical_device, 0, nullptr, vulkan_context.enabled_device_extensions.size(), vulkan_context.enabled_device_extensions.data());
    backend_context.fVkExtensions = extensions.ptr();

    // Skia samples through a YCbCr conversion only once it knows the device has that enabled.
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr_conversion_features {};
    ycbcr_conversion_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    ycbcr_conversion_features.samplerYcbcrConversion = vulkan_context.can_sample_through_ycbcr_conversion ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceFeatures2 features {};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &ycbcr_conversion_features;
    backend_context.fDeviceFeatures2 = &features;

    backend_context.fMemoryAllocator = create_skia_vulkan_memory_allocator(vulkan_context);
    VERIFY(backend_context.fMemoryAllocator);

    GrContextOptions options;
    options.fReduceOpsTaskSplitting = GrContextOptions::Enable::kNo;

    sk_sp<GrDirectContext> ctx = GrDirectContexts::MakeVulkan(backend_context, options);
    VERIFY(ctx);
    ctx->setResourceCacheLimit(skia_resource_cache_limit);
    return adopt_ref(*new SkiaVulkanBackendContext(ctx, vulkan_context, move(extensions)));
}
#endif

#ifdef AK_OS_MACOS

#    ifdef SK_GRAPHITE
class SkiaGraphiteMetalBackendContext final : public SkiaBackendContext {
    AK_MAKE_NONCOPYABLE(SkiaGraphiteMetalBackendContext);
    AK_MAKE_NONMOVABLE(SkiaGraphiteMetalBackendContext);

public:
    SkiaGraphiteMetalBackendContext(std::unique_ptr<SkExecutor> executor, std::unique_ptr<skgpu::graphite::Context> context, std::unique_ptr<skgpu::graphite::Recorder> recorder, NonnullRefPtr<MetalContext> metal_context)
        : m_executor(move(executor))
        , m_context(move(context))
        , m_recorder(move(recorder))
        , m_metal_context(move(metal_context))
    {
        VERIFY(m_executor);
        VERIFY(m_context);
        VERIFY(m_recorder);
    }

    ~SkiaGraphiteMetalBackendContext() override
    {
        m_recorder.reset();
        m_context.reset();
    }

    void flush_and_submit_impl(SkSurface*, SurfaceAccess access) override
    {
        auto recording = m_recorder->snap();
        if (recording) {
            skgpu::graphite::InsertRecordingInfo insert_info;
            insert_info.fRecording = recording.get();
            auto status = m_context->insertRecording(insert_info);
            if (!status)
                dbgln("Photon Graphite recording insertion failed: {}", status.message().c_str());
        }
        skgpu::graphite::SubmitInfo submit_info;
        submit_info.fSync = skgpu::graphite::SyncToCpu::kYes;
        submit_info.fMarkBoundary = access == SurfaceAccess::External ? skgpu::graphite::MarkFrameBoundary::kYes : skgpu::graphite::MarkFrameBoundary::kNo;
        if (!m_context->submit(submit_info))
            dbgln("Photon Graphite synchronous submit failed");
    }

    void flush_and_submit_async_impl(SkSurface*, SurfaceAccess access, Function<void()>&& callback, uint64_t presentation_signal_value) override
    {
        auto recording = m_recorder->snap();
        if (!recording) {
            if (presentation_signal_value != 0)
                m_metal_context->signal_presentation_value(presentation_signal_value);
            callback();
            return;
        }

        auto* callback_context = new Function<void()>(move(callback));
        skgpu::graphite::InsertRecordingInfo insert_info;
        insert_info.fRecording = recording.get();
        insert_info.fFinishedContext = callback_context;
        insert_info.fFinishedProc = invoke_graphite_flush_callback;

        auto status = m_context->insertRecording(insert_info);
        if (!status)
            dbgln("Photon Graphite recording insertion failed: {}", status.message().c_str());
        skgpu::graphite::SubmitInfo submit_info;
        submit_info.fSync = skgpu::graphite::SyncToCpu::kNo;
        submit_info.fMarkBoundary = access == SurfaceAccess::External || presentation_signal_value != 0
            ? skgpu::graphite::MarkFrameBoundary::kYes
            : skgpu::graphite::MarkFrameBoundary::kNo;
        if (!m_context->submit(submit_info))
            dbgln("Photon Graphite asynchronous submit failed");
        if (presentation_signal_value != 0) {
            // Skia 148's Graphite Metal signal-semaphore path passes an invalid event to
            // encodeSignalEvent. Submit a signal buffer on the same Metal queue instead; queue
            // ordering places it after Graphite's committed frame commands without a CPU wait.
            m_metal_context->signal_presentation_value(presentation_signal_value);
        }
    }

    GrDirectContext* sk_context() const override { return nullptr; }
    skgpu::graphite::Context* graphite_context() const override { return m_context.get(); }
    skgpu::graphite::Recorder* graphite_recorder() const override { return m_recorder.get(); }

    VulkanContext const& vulkan_context() override { VERIFY_NOT_REACHED(); }
    MetalContext& metal_context() override { return m_metal_context; }

private:
    // Keep Graphite pipeline compilation off the compositor thread. The executor must outlive the context.
    std::unique_ptr<SkExecutor> m_executor;
    std::unique_ptr<skgpu::graphite::Context> m_context;
    std::unique_ptr<skgpu::graphite::Recorder> m_recorder;
    NonnullRefPtr<MetalContext> m_metal_context;
};
#    endif

class SkiaMetalBackendContext final : public SkiaBackendContext {
    AK_MAKE_NONCOPYABLE(SkiaMetalBackendContext);
    AK_MAKE_NONMOVABLE(SkiaMetalBackendContext);

public:
    SkiaMetalBackendContext(sk_sp<GrDirectContext> context, OwnPtr<PhotonGaneshPersistentCache> persistent_cache, NonnullRefPtr<MetalContext> metal_context)
        : m_context(move(context))
        , m_persistent_cache(move(persistent_cache))
        , m_metal_context(move(metal_context))
    {
    }

    ~SkiaMetalBackendContext() override
    {
        m_context.reset();
    }

    void flush_and_submit_impl(SkSurface* surface, SurfaceAccess access) override
    {
        GrFlushInfo const flush_info {};
        m_context->flush(surface, to_backend_surface_access(access), flush_info);
        m_context->submit(GrSyncCpu::kYes);
    }

    void flush_and_submit_async_impl(SkSurface* surface, SurfaceAccess access, Function<void()>&& callback, uint64_t presentation_signal_value) override
    {
        void* signal_semaphore = nullptr;
        if (presentation_signal_value != 0)
            signal_semaphore = m_metal_context->create_presentation_signal_semaphore(presentation_signal_value);
        flush_and_submit_async_to_context(*m_context, surface, access, move(callback), static_cast<GrBackendSemaphore*>(signal_semaphore));
        if (signal_semaphore)
            m_metal_context->destroy_presentation_signal_semaphore(signal_semaphore);
    }

    GrDirectContext* sk_context() const override { return m_context.get(); }

    VulkanContext const& vulkan_context() override { VERIFY_NOT_REACHED(); }

    MetalContext& metal_context() override { return m_metal_context; }

    bool precompile_next_shader() override
    {
        if (!m_warm_entries.has_value())
            m_warm_entries = m_persistent_cache->load_warm_entries();
        if (m_next_warm_entry >= m_warm_entries->size())
            return false;
        auto const& entry = m_warm_entries->at(m_next_warm_entry++);
        auto started_at = MonotonicTime::now();
        m_context->precompileShader(*entry.key, *entry.data);
        m_warm_duration += MonotonicTime::now() - started_at;
        auto remaining = m_next_warm_entry < m_warm_entries->size();
        if (!remaining && std::getenv("PHOTON_VERBOSE"))
            dbgln("Photon Skia precompiled {} programs in {:.2f}ms", m_warm_entries->size(), m_warm_duration.to_seconds_f64() * 1000.0);
        return remaining;
    }

private:
    sk_sp<GrDirectContext> m_context;
    OwnPtr<PhotonGaneshPersistentCache> m_persistent_cache;
    NonnullRefPtr<MetalContext> m_metal_context;
    Optional<Vector<PhotonGaneshPersistentCache::WarmEntry>> m_warm_entries;
    size_t m_next_warm_entry { 0 };
    AK::Duration m_warm_duration;
};

RefPtr<SkiaBackendContext> SkiaBackendContext::create_metal_context(NonnullRefPtr<MetalContext> metal_context)
{
#    ifdef SK_GRAPHITE
    auto* backend_override = std::getenv("PHOTON_SKIA_BACKEND");
    auto use_graphite = backend_override && std::strcmp(backend_override, "graphite") == 0;
    if (backend_override && std::strcmp(backend_override, "ganesh") != 0 && std::strcmp(backend_override, "graphite") != 0)
        dbgln("Unknown PHOTON_SKIA_BACKEND '{}'; defaulting to Ganesh", backend_override);

    if (use_graphite) {
        skgpu::graphite::MtlBackendContext graphite_backend_context;
        graphite_backend_context.fDevice.retain(metal_context->device());
        graphite_backend_context.fQueue.retain(metal_context->queue());

        auto graphite_executor = SkExecutor::MakeFIFOThreadPool(2, false);
        VERIFY(graphite_executor);
        skgpu::graphite::ContextOptions graphite_options;
        graphite_options.fGpuBudgetInBytes = skia_resource_cache_limit / 2;
        graphite_options.fRequireOrderedRecordings = true;
        graphite_options.fExecutor = graphite_executor.get();
        auto graphite_context = skgpu::graphite::ContextFactory::MakeMetal(graphite_backend_context, graphite_options);
        if (graphite_context) {
            skgpu::graphite::RecorderOptions recorder_options;
            recorder_options.fGpuBudgetInBytes = skia_resource_cache_limit / 2;
            auto graphite_recorder = graphite_context->makeRecorder(recorder_options);
            if (graphite_recorder) {
                dbgln("Photon Skia backend: Graphite on Metal (2 pipeline workers)");
                return adopt_ref(*new SkiaGraphiteMetalBackendContext(move(graphite_executor), move(graphite_context), move(graphite_recorder), move(metal_context)));
            }
        }
        dbgln("Photon Graphite initialization failed; falling back to Ganesh on Metal");
    }
#    endif

    GrMtlBackendContext backend_context;
    backend_context.fDevice.retain(metal_context->device());
    backend_context.fQueue.retain(metal_context->queue());
    auto persistent_cache = make<PhotonGaneshPersistentCache>(metal_context->device_registry_id());
    GrContextOptions options;
    options.fPersistentCache = persistent_cache.ptr();
    sk_sp<GrDirectContext> ctx = GrDirectContexts::MakeMetal(backend_context, options);
    VERIFY(ctx);
    ctx->setResourceCacheLimit(skia_resource_cache_limit);
    return adopt_ref(*new SkiaMetalBackendContext(move(ctx), move(persistent_cache), move(metal_context)));
}
#endif

}
