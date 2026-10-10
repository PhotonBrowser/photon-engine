/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

#ifdef AK_OS_MACOS

#    include <AK/ByteString.h>
#    include <AK/HashTable.h>
#    include <AK/Vector.h>
#    include <AK/kmalloc.h>
#    include <core/SkRefCnt.h>
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
    ~PhotonGaneshPersistentCache() override;

    // The configurable constructor keeps storage behavior testable without a Metal device.
    PhotonGaneshPersistentCache(ByteString root_directory, ByteString namespace_name, uint64_t size_limit);

    static Diagnostics diagnostics_snapshot();

    sk_sp<SkData> load(SkData const& key) override;
    void store(SkData const& key, SkData const& data, SkString const& description) override;

    ByteString file_path_for_key(SkData const& key) const;

    // A program a session asked for, saved so that later sessions can compile it before first use.
    struct WarmEntry {
        sk_sp<SkData> key;
        sk_sp<SkData> data;
    };

    // The programs recent sessions asked for, in the order they first did, which usually begins with what a first
    // paint draws. They count as asked for by this session too, as a compiled program is not asked for again, so the
    // list it saves keeps them ahead of the programs it adds.
    Vector<WarmEntry> load_warm_entries();

private:
    static uint64_t key_hash(SkData const& key);
    void evict_if_needed();
    void note_program_used(uint64_t hash);
    void save_warm_list() const;
    ByteString warm_list_path() const;

    // The programs this session has asked for, in first-use order, up to `max_warm_entries`.
    static constexpr size_t max_warm_entries = 256;
    Vector<uint64_t> m_used_hashes;
    HashTable<uint64_t> m_used_hash_set;

    uint64_t m_size_limit { 128ull * 1024 * 1024 };
    ByteString m_root_directory;
    ByteString m_directory;
};

}

#endif
