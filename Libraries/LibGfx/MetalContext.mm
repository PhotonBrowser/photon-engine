/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibGfx/MetalContext.h>

#include <gpu/ganesh/GrBackendSemaphore.h>
#include <gpu/ganesh/mtl/GrMtlBackendSemaphore.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <atomic>
#include <cstdlib>

@protocol PhotonMetalPresentationEventChannel
- (void)registerSharedEventHandle:(MTLSharedEventHandle *)handle
                       registryID:(uint64_t)registryID
                       forChannel:(NSString *)channel
                            reply:(void (^)(BOOL accepted))reply;
@end

static bool register_presentation_event_with_broker(id<MTLSharedEvent> event, uint64_t registry_id)
{
    auto const* service_name = std::getenv("PHOTON_PRESENTATION_XPC_SERVICE");
    auto const* channel_name = std::getenv("PHOTON_PRESENTATION_CHANNEL_ID");
    if (!service_name || !channel_name)
        return false;

    @autoreleasepool {
        NSString *service = [[NSString alloc] initWithUTF8String:service_name];
        NSString *channel = [[NSString alloc] initWithUTF8String:channel_name];
        if (!service || !channel) {
            [service release];
            [channel release];
            return false;
        }

        NSXPCInterface *interface = [NSXPCInterface interfaceWithProtocol:@protocol(PhotonMetalPresentationEventChannel)];
        [interface setClasses:[NSSet setWithObject:[MTLSharedEventHandle class]]
                    forSelector:@selector(registerSharedEventHandle:registryID:forChannel:reply:)
                  argumentIndex:0
                        ofReply:NO];
        NSXPCConnection *connection = [[NSXPCConnection alloc] initWithMachServiceName:service options:0];
        [service release];
        connection.remoteObjectInterface = interface;
        [connection resume];

        id<MTLSharedEvent> shared_event = event;
        MTLSharedEventHandle *handle = [shared_event newSharedEventHandle];
        dispatch_semaphore_t completion = dispatch_semaphore_create(0);
        __block BOOL accepted = NO;
        id<PhotonMetalPresentationEventChannel> proxy = (id<PhotonMetalPresentationEventChannel>)[connection remoteObjectProxyWithErrorHandler:^(__unused NSError *error) {
            dispatch_semaphore_signal(completion);
        }];
        [proxy registerSharedEventHandle:handle registryID:registry_id forChannel:channel reply:^(BOOL result) {
            accepted = result;
            dispatch_semaphore_signal(completion);
        }];
        bool registered = dispatch_semaphore_wait(completion, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) == 0 && accepted;
        [handle release];
        [connection invalidate];
        [connection release];
        [channel release];
        return registered;
    }
}

namespace Gfx {

class MetalTextureImpl final : public MetalTexture {
public:
    AK_ALLOC_WITH_KMALLOC;

    MetalTextureImpl(id<MTLTexture> texture)
        : m_texture(texture)
    {
    }

    void const* texture() const override { return m_texture; }
    size_t width() const override { return m_texture.width; }
    size_t height() const override { return m_texture.height; }

    virtual ~MetalTextureImpl()
    {
        [m_texture release];
    }

private:
    id<MTLTexture> m_texture;
};

static MTLPixelFormat metal_pixel_format(MetalTextureFormat format)
{
    switch (format) {
    case MetalTextureFormat::BGRA8:
        return MTLPixelFormatBGRA8Unorm;
    case MetalTextureFormat::R8:
        return MTLPixelFormatR8Unorm;
    case MetalTextureFormat::RG8:
        return MTLPixelFormatRG8Unorm;
    case MetalTextureFormat::R16:
        return MTLPixelFormatR16Unorm;
    case MetalTextureFormat::RG16:
        return MTLPixelFormatRG16Unorm;
    }
    VERIFY_NOT_REACHED();
}

class MetalContextImpl final : public MetalContext {
public:
    AK_ALLOC_WITH_KMALLOC;

    MetalContextImpl(id<MTLDevice> device, id<MTLCommandQueue> queue)
        : m_device(device)
        , m_queue(queue)
        , m_presentation_event([device newSharedEvent])
    {
        if (m_presentation_event)
            m_presentation_event_registered = register_presentation_event_with_broker(m_presentation_event, device.registryID);
        if (!m_presentation_event_registered)
            dbgln("Metal presentation event was not registered with the Photon XPC broker");
    }

    void const* device() const override { return m_device; }
    void const* queue() const override { return m_queue; }
    bool presentation_event_registered() const override { return m_presentation_event_registered; }
    uint64_t next_presentation_signal_value() override
    {
        VERIFY(m_presentation_event_registered);
        return m_next_presentation_signal_value.fetch_add(1, std::memory_order_relaxed);
    }
    void* create_presentation_signal_semaphore(uint64_t value) override
    {
        VERIFY(m_presentation_event_registered);
        // This target is built without ARC: retain exactly once for Skia's
        // backend semaphore, which releases the handle when destroyed.
        auto semaphore = GrBackendSemaphores::MakeMtl((GrMTLHandle)[m_presentation_event retain], value);
        return new GrBackendSemaphore(move(semaphore));
    }
    void destroy_presentation_signal_semaphore(void* semaphore) override
    {
        delete static_cast<GrBackendSemaphore*>(semaphore);
    }
    uint64_t device_registry_id() const override { return m_device.registryID; }

    OwnPtr<MetalTexture> create_texture_from_iosurface(Core::IOSurfaceHandle const& iosurface, MetalTextureFormat format, size_t plane) override
    {
        auto* const descriptor = [[MTLTextureDescriptor alloc] init];
        descriptor.pixelFormat = metal_pixel_format(format);
        descriptor.width = iosurface.plane_count() > 0 ? iosurface.plane_width(plane) : iosurface.width();
        descriptor.height = iosurface.plane_count() > 0 ? iosurface.plane_height(plane) : iosurface.height();
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;

        id<MTLTexture> texture = [m_device newTextureWithDescriptor:descriptor iosurface:(IOSurfaceRef)iosurface.core_foundation_pointer() plane:plane];
        [descriptor release];
        if (!texture)
            return {};
        return make<MetalTextureImpl>(texture);
    }

    virtual ~MetalContextImpl() override
    {
        [m_presentation_event release];
        [m_queue release];
        [m_device release];
    }

private:
    id<MTLDevice> m_device;
    id<MTLCommandQueue> m_queue;
    id<MTLSharedEvent> m_presentation_event;
    bool m_presentation_event_registered { false };
    std::atomic<uint64_t> m_next_presentation_signal_value { 1 };
};

RefPtr<MetalContext> get_metal_context()
{
    auto device = MTLCreateSystemDefaultDevice();
    if (!device) {
        dbgln("Failed to create Metal device");
        return {};
    }

    auto queue = [device newCommandQueue];
    if (!queue) {
        dbgln("Failed to create Metal command queue");
        [device release];
        return {};
    }

    return adopt_ref(*new MetalContextImpl(device, queue));
}

}
