/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/kmalloc.h>
#include <LibGfx/PhotonGaneshPersistentCache.h>
#include <LibTest/TestCase.h>

#include <core/SkData.h>
#include <core/SkString.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace {

ByteString test_cache_root(StringView name)
{
    return ByteString::formatted("/tmp/photon-ganesh-cache-test-{}-{}", getpid(), name);
}

sk_sp<SkData> data(StringView bytes)
{
    return SkData::MakeWithCopy(bytes.characters_without_null_termination(), bytes.length());
}

bool data_equals(SkData const& actual, SkData const& expected)
{
    return actual.size() == expected.size() && memcmp(actual.data(), expected.data(), actual.size()) == 0;
}

}

TEST_CASE(persistent_cache_stores_and_retrieves_exact_bytes)
{
    auto root = test_cache_root("roundtrip"sv);
    std::filesystem::remove_all(root.characters());
    Gfx::PhotonGaneshPersistentCache cache(root, ByteString("namespace-a"), 1024);
    auto key = data("program-key"sv);
    auto value = data("compiled-program-bytes"sv);

    cache.store(*key, *value, SkString("roundtrip"));
    auto loaded = cache.load(*key);
    EXPECT(loaded);
    EXPECT(data_equals(*loaded, *value));
    std::filesystem::remove_all(root.characters());
}

TEST_CASE(persistent_cache_namespaces_are_isolated)
{
    auto root = test_cache_root("namespace"sv);
    std::filesystem::remove_all(root.characters());
    Gfx::PhotonGaneshPersistentCache old_namespace(root, ByteString("skia-148/cache-v1/gpu-a"), 1024);
    Gfx::PhotonGaneshPersistentCache new_namespace(root, ByteString("skia-148/cache-v2/gpu-a"), 1024);
    auto key = data("program-key"sv);
    auto value = data("compiled-program-bytes"sv);

    old_namespace.store(*key, *value, SkString("namespace"));
    EXPECT(old_namespace.load(*key));
    EXPECT(!new_namespace.load(*key));
    std::filesystem::remove_all(root.characters());
}

TEST_CASE(persistent_cache_ignores_corrupt_entries)
{
    auto root = test_cache_root("corrupt"sv);
    std::filesystem::remove_all(root.characters());
    Gfx::PhotonGaneshPersistentCache cache(root, ByteString("namespace-a"), 1024);
    auto key = data("program-key"sv);
    auto value = data("compiled-program-bytes"sv);

    cache.store(*key, *value, SkString("corrupt"));
    {
        std::ofstream file(cache.file_path_for_key(*key).characters(), std::ios::binary | std::ios::trunc);
        file.write("broken", 6);
    }
    EXPECT(!cache.load(*key));
    std::filesystem::remove_all(root.characters());
}

TEST_CASE(persistent_cache_atomically_replaces_existing_entry)
{
    auto root = test_cache_root("replace"sv);
    std::filesystem::remove_all(root.characters());
    Gfx::PhotonGaneshPersistentCache cache(root, ByteString("namespace-a"), 1024);
    auto key = data("program-key"sv);
    auto old_value = data("old-program"sv);
    auto new_value = data("new-program"sv);

    cache.store(*key, *old_value, SkString("replace"));
    cache.store(*key, *new_value, SkString("replace"));
    auto loaded = cache.load(*key);
    EXPECT(loaded);
    EXPECT(data_equals(*loaded, *new_value));
    std::filesystem::remove_all(root.characters());
}

TEST_CASE(persistent_cache_evicts_oldest_entries_across_namespaces)
{
    auto root = test_cache_root("eviction"sv);
    std::filesystem::remove_all(root.characters());
    Gfx::PhotonGaneshPersistentCache old_namespace(root, ByteString("old-namespace"), 96);
    Gfx::PhotonGaneshPersistentCache current_namespace(root, ByteString("current-namespace"), 96);
    auto old_key = data("old-key"sv);
    auto new_key = data("new-key"sv);
    auto value = data("1234567890123456789012345678901234567890"sv);

    old_namespace.store(*old_key, *value, SkString("old"));
    auto old_path = old_namespace.file_path_for_key(*old_key);
    std::error_code error;
    std::filesystem::last_write_time(old_path.characters(), std::filesystem::file_time_type::clock::now() - std::chrono::hours(2), error);
    EXPECT(!error);

    current_namespace.store(*new_key, *value, SkString("new"));
    EXPECT(!std::filesystem::exists(old_path.characters()));
    EXPECT(std::filesystem::exists(current_namespace.file_path_for_key(*new_key).characters()));
    std::filesystem::remove_all(root.characters());
}
