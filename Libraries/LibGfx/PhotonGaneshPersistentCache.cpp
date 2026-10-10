/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/PhotonGaneshPersistentCache.h>

#ifdef AK_OS_MACOS

#    include <AK/NeverDestroyed.h>
#    include <AK/Vector.h>
#    include <LibCore/Directory.h>
#    include <LibCore/StandardPaths.h>
#    include <algorithm>
#    include <core/SkData.h>
#    include <core/SkString.h>
#    include <cstring>
#    include <filesystem>
#    include <fstream>
#    include <mutex>
#    include <sys/utsname.h>
#    include <unistd.h>

namespace Gfx {

static std::mutex& cache_io_lock()
{
    static NeverDestroyed<std::mutex> lock;
    return *lock;
}

static std::mutex& diagnostics_lock()
{
    static NeverDestroyed<std::mutex> lock;
    return *lock;
}

static PhotonGaneshPersistentCache::Diagnostics& cache_diagnostics()
{
    static NeverDestroyed<PhotonGaneshPersistentCache::Diagnostics> diagnostics;
    return *diagnostics;
}

PhotonGaneshPersistentCache::PhotonGaneshPersistentCache(uint64_t metal_device_registry_id)
    : PhotonGaneshPersistentCache([&] { return ByteString::formatted("{}/Photon/Skia/Metal", Core::StandardPaths::cache_directory()); }(), [&] {
          struct utsname system_info {};
          StringView os_build = "unknown"sv;
          if (uname(&system_info) == 0)
              os_build = StringView { system_info.release, std::strlen(system_info.release) };
          return ByteString::formatted("skia-148/cache-v1/darwin-{}-gpu-{}", os_build, metal_device_registry_id); }(), 128ull * 1024 * 1024)
{
}

PhotonGaneshPersistentCache::PhotonGaneshPersistentCache(ByteString root_directory, ByteString namespace_name, uint64_t size_limit)
    : m_size_limit(size_limit)
    , m_root_directory(move(root_directory))
    , m_directory(ByteString::formatted("{}/{}", m_root_directory, namespace_name))
{
    auto result = Core::Directory::create(m_directory, Core::Directory::CreateDirectories::Yes);
    if (result.is_error()) {
        dbgln("Photon Ganesh persistent cache: unable to create {}: {}", m_directory, result.error());
        m_directory = {};
    }
}

PhotonGaneshPersistentCache::Diagnostics PhotonGaneshPersistentCache::diagnostics_snapshot()
{
    std::lock_guard lock(diagnostics_lock());
    return cache_diagnostics();
}

PhotonGaneshPersistentCache::~PhotonGaneshPersistentCache() = default;

sk_sp<SkData> PhotonGaneshPersistentCache::load(SkData const& key)
{
    auto const hash = key_hash(key);
    note_program_used(hash);
    {
        std::lock_guard lock(diagnostics_lock());
        ++cache_diagnostics().descriptor_requests;
        cache_diagnostics().descriptor_hashes.append(hash);
    }

    if (m_directory.is_empty())
        return nullptr;

    auto path = file_path_for_key(key);
    std::lock_guard lock(cache_io_lock());
    std::ifstream file(path.characters(), std::ios::binary);
    if (!file) {
        {
            std::lock_guard diagnostics_guard(diagnostics_lock());
            ++cache_diagnostics().misses;
        }
        if (std::getenv("PHOTON_VERBOSE"))
            dbgln("Photon Ganesh persistent cache: miss key={:016x}", hash);
        return nullptr;
    }

    uint64_t stored_key_size = 0;
    uint64_t stored_data_size = 0;
    file.read(reinterpret_cast<char*>(&stored_key_size), sizeof(stored_key_size));
    file.read(reinterpret_cast<char*>(&stored_data_size), sizeof(stored_data_size));
    if (!file || stored_key_size != key.size() || stored_data_size > m_size_limit)
        return nullptr;

    Vector<u8> stored_key;
    stored_key.resize(stored_key_size);
    file.read(reinterpret_cast<char*>(stored_key.data()), stored_key.size());
    if (!file || memcmp(stored_key.data(), key.data(), key.size()) != 0)
        return nullptr;

    auto data = SkData::MakeUninitialized(stored_data_size);
    if (!data)
        return nullptr;
    file.read(static_cast<char*>(data->writable_data()), data->size());
    if (!file) {
        if (std::getenv("PHOTON_VERBOSE"))
            dbgln("Photon Ganesh persistent cache: corrupt key={:016x}", key_hash(key));
        return nullptr;
    }

    // Do not update the entry's modification time on a cache hit. The Compositor sandbox denies
    // utimensat, which terminates the process instead of returning an error. Eviction uses the
    // last successful store time as its age, so reads remain metadata-read-only.
    {
        std::lock_guard diagnostics_guard(diagnostics_lock());
        ++cache_diagnostics().hits;
    }
    if (std::getenv("PHOTON_VERBOSE"))
        dbgln("Photon Ganesh persistent cache: hit key={:016x} bytes={}", hash, data->size());
    return data;
}

void PhotonGaneshPersistentCache::store(SkData const& key, SkData const& data, SkString const&)
{
    if (m_directory.is_empty() || data.size() > m_size_limit)
        return;

    auto path = file_path_for_key(key);
    auto temporary_path = ByteString::formatted("{}.tmp-{}", path, getpid());
    std::lock_guard lock(cache_io_lock());
    std::ofstream file(temporary_path.characters(), std::ios::binary | std::ios::trunc);
    if (!file) {
        std::error_code error;
        std::filesystem::remove(temporary_path.characters(), error);
        return;
    }

    auto key_size = static_cast<uint64_t>(key.size());
    auto data_size = static_cast<uint64_t>(data.size());
    file.write(reinterpret_cast<char const*>(&key_size), sizeof(key_size));
    file.write(reinterpret_cast<char const*>(&data_size), sizeof(data_size));
    file.write(static_cast<char const*>(key.data()), key.size());
    file.write(static_cast<char const*>(data.data()), data.size());
    file.close();
    if (!file)
        return;

    std::error_code error;
    std::filesystem::rename(temporary_path.characters(), path.characters(), error);
    if (error) {
        std::filesystem::remove(temporary_path.characters(), error);
        return;
    }

    if (std::getenv("PHOTON_VERBOSE"))
        dbgln("Photon Ganesh persistent cache: store key={:016x} bytes={}", key_hash(key), data.size());
    {
        std::lock_guard diagnostics_guard(diagnostics_lock());
        ++cache_diagnostics().stores;
    }
    evict_if_needed();
}

uint64_t PhotonGaneshPersistentCache::key_hash(SkData const& key)
{
    uint64_t hash = 14695981039346656037ull;
    auto const* bytes = static_cast<uint8_t const*>(key.data());
    for (size_t i = 0; i < key.size(); ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

ByteString PhotonGaneshPersistentCache::file_path_for_key(SkData const& key) const
{
    return ByteString::formatted("{}/{:016x}.bin", m_directory, key_hash(key));
}

void PhotonGaneshPersistentCache::evict_if_needed()
{
    struct CacheEntry {
        std::filesystem::path path;
        std::filesystem::file_time_type last_used;
        uint64_t size;
    };

    std::error_code error;
    Vector<CacheEntry> entries;
    uint64_t total_size = 0;
    auto iterator = std::filesystem::recursive_directory_iterator(m_root_directory.characters(), error);
    auto const end = std::filesystem::recursive_directory_iterator {};
    while (iterator != end) {
        auto entry = *iterator;
        iterator.increment(error);
        if (error)
            return;
        if (!entry.is_regular_file(error) || error || entry.path().extension() != ".bin")
            continue;
        auto size = entry.file_size(error);
        if (error)
            continue;
        auto last_used = entry.last_write_time(error);
        if (error)
            continue;
        total_size += size;
        entries.append({ entry.path(), last_used, size });
    }

    if (total_size <= m_size_limit)
        return;
    std::sort(entries.begin(), entries.end(), [](auto const& a, auto const& b) { return a.last_used < b.last_used; });
    for (auto const& entry : entries) {
        std::filesystem::remove(entry.path, error);
        if (!error)
            total_size -= entry.size;
        if (total_size <= m_size_limit)
            break;
    }
}

// Remembers the programs this session asks for, in the order it first does. The list is saved whenever it grows, as
// the Compositor is often ended rather than shut down; a program is asked for once a session, so this is rare.
void PhotonGaneshPersistentCache::note_program_used(uint64_t hash)
{
    if (m_used_hashes.size() >= max_warm_entries || m_used_hash_set.contains(hash))
        return;
    m_used_hash_set.set(hash);
    m_used_hashes.append(hash);
    save_warm_list();
}

ByteString PhotonGaneshPersistentCache::warm_list_path() const
{
    return ByteString::formatted("{}/warm-list", m_directory);
}

void PhotonGaneshPersistentCache::save_warm_list() const
{
    if (m_directory.is_empty() || m_used_hashes.is_empty())
        return;
    auto path = warm_list_path();
    auto temporary_path = ByteString::formatted("{}.tmp-{}", path, getpid());
    std::lock_guard lock(cache_io_lock());
    {
        std::ofstream file(temporary_path.characters(), std::ios::binary | std::ios::trunc);
        if (!file)
            return;
        file.write(reinterpret_cast<char const*>(m_used_hashes.data()), m_used_hashes.size() * sizeof(uint64_t));
        if (!file) {
            std::error_code error;
            std::filesystem::remove(temporary_path.characters(), error);
            return;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary_path.characters(), path.characters(), error);
    if (error)
        std::filesystem::remove(temporary_path.characters(), error);
}

// Reads a cached program's key and data from its file.
static Optional<PhotonGaneshPersistentCache::WarmEntry> read_cached_program(ByteString const& path, uint64_t size_limit)
{
    std::ifstream file(path.characters(), std::ios::binary);
    if (!file)
        return {};
    uint64_t key_size = 0;
    uint64_t data_size = 0;
    file.read(reinterpret_cast<char*>(&key_size), sizeof(key_size));
    file.read(reinterpret_cast<char*>(&data_size), sizeof(data_size));
    if (!file || key_size > size_limit || data_size > size_limit)
        return {};
    auto key = SkData::MakeUninitialized(key_size);
    auto data = SkData::MakeUninitialized(data_size);
    if (!key || !data)
        return {};
    file.read(static_cast<char*>(key->writable_data()), key->size());
    file.read(static_cast<char*>(data->writable_data()), data->size());
    if (!file)
        return {};
    return PhotonGaneshPersistentCache::WarmEntry { move(key), move(data) };
}

Vector<PhotonGaneshPersistentCache::WarmEntry> PhotonGaneshPersistentCache::load_warm_entries()
{
    Vector<WarmEntry> entries;
    if (m_directory.is_empty())
        return entries;
    std::lock_guard lock(cache_io_lock());
    std::ifstream file(warm_list_path().characters(), std::ios::binary);
    if (!file)
        return entries;
    uint64_t hash = 0;
    while (entries.size() < max_warm_entries && file.read(reinterpret_cast<char*>(&hash), sizeof(hash))) {
        auto entry = read_cached_program(ByteString::formatted("{}/{:016x}.bin", m_directory, hash), m_size_limit);
        if (!entry.has_value() || m_used_hash_set.contains(hash))
            continue;
        entries.append(entry.release_value());
        m_used_hash_set.set(hash);
        m_used_hashes.append(hash);
    }
    return entries;
}

}

#endif
