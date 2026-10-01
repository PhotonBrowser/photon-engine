/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

#ifdef AK_OS_MACOS

#    include <AK/kmalloc.h>
#    include <AK/ByteString.h>
#    include <AK/Vector.h>
#    include <gpu/ganesh/GrContextOptions.h>

class SkData;
class SkString;

namespace Gfx {

class PhotonGaneshPersistentCache final : public GrContextOptions::PersistentCache {
    AK_ALLOC_WITH_KMALLOC;

public:
    struct Diagnostics {
        size_t descriptor_requests { 0 };
        size_t misses { 0 };
        size_t hits { 0 };
        size_t stores { 0 };
        Vector<uint64_t> descriptor_hashes;
    };

    explicit PhotonGaneshPersistentCache(uint64_t metal_device_registry_id);

    // The configurable constructor keeps storage behavior testable without a Metal device.
    PhotonGaneshPersistentCache(ByteString root_directory, ByteString namespace_name, uint64_t size_limit);

    static Diagnostics diagnostics_snapshot();

    sk_sp<SkData> load(SkData const& key) override;
    void store(SkData const& key, SkData const& data, SkString const& description) override;

    ByteString file_path_for_key(SkData const& key) const;

private:
    static uint64_t key_hash(SkData const& key);
    void evict_if_needed();

    uint64_t m_size_limit { 128ull * 1024 * 1024 };
    ByteString m_root_directory;
    ByteString m_directory;
};

}

#endif
