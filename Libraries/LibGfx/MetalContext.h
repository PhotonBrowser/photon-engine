/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if !defined(AK_OS_MACOS)
static_assert(false, "This file must only be used for macOS");
#endif

#include <AK/Forward.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <LibCore/IOSurface.h>

#include <cstdint>

namespace Gfx {

class MetalTexture {
public:
    virtual void const* texture() const = 0;
    virtual size_t width() const = 0;
    virtual size_t height() const = 0;

    virtual ~MetalTexture() { }
};

// The component layout a texture reads a surface's plane through, which for a planar surface differs per plane.
enum class MetalTextureFormat : u8 {
    BGRA8,
    R8,
    RG8,
    R16,
    RG16,
};

class MetalContext : public RefCounted<MetalContext> {
public:
    virtual void const* device() const = 0;
    virtual void const* queue() const = 0;

    // One event belongs to the compositor's Metal device and is shared with
    // the Photon scene consumer through its persistent XPC presentation channel.
    virtual bool presentation_event_registered() const = 0;
    virtual uint64_t next_presentation_signal_value() = 0;
    virtual void* create_presentation_signal_semaphore(uint64_t value) = 0;
    virtual void destroy_presentation_signal_semaphore(void*) = 0;
    virtual uint64_t device_registry_id() const = 0;

    virtual OwnPtr<MetalTexture> create_texture_from_iosurface(Core::IOSurfaceHandle const&, MetalTextureFormat, size_t plane) = 0;

    virtual ~MetalContext() { }
};

RefPtr<MetalContext> get_metal_context();

}
