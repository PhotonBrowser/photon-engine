/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/LexicalPath.h>
#include <AK/String.h>
#include <Compositor/Sandbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibSandbox/Sandbox.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

namespace Compositor {

static ErrorOr<Optional<ByteString>> application_darwin_user_cache_directory()
{
    char darwin_user_cache_directory[PATH_MAX];
    if (confstr(_CS_DARWIN_USER_CACHE_DIR, darwin_user_cache_directory, sizeof(darwin_user_cache_directory)) == 0)
        return OptionalNone {};

    auto darwin_user_cache_directory_view = StringView { darwin_user_cache_directory, strlen(darwin_user_cache_directory) };

    // Without a bundle identifier, for example when the helper runs outside an application bundle, Metal keeps its
    // shader caches in a directory shared by every such process.
    auto bundle_identifier = CFBundleGetIdentifier(CFBundleGetMainBundle());
    if (!bundle_identifier)
        return LexicalPath::join(darwin_user_cache_directory_view, "com.apple.metalfe"sv).string();

    char bundle_identifier_buffer[256];
    if (!CFStringGetCString(bundle_identifier, bundle_identifier_buffer, sizeof(bundle_identifier_buffer), kCFStringEncodingUTF8))
        return OptionalNone {};

    return LexicalPath::join(darwin_user_cache_directory_view, StringView { bundle_identifier_buffer, strlen(bundle_identifier_buffer) }).string();
}

ErrorOr<void> apply_sandbox(StringView mach_server_name, StringView cache_path, StringView resource_root)
{
    TRY(Sandbox::configure_runtime());

    auto executable_path = TRY(Core::System::current_executable_path());

    Vector<Sandbox::SeatbeltPath> paths;
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, executable_path, Sandbox::SeatbeltPath::Access::ReadOnly));

    // The helpers read their own application bundle, for example when CoreFoundation looks up the main bundle.
    // Outside an application bundle, CoreFoundation treats the executable's directory as the main bundle, and Metal
    // issues an extension for it to its compiler service.
    if (auto bundle = Sandbox::application_bundle_for_executable(executable_path); bundle.has_value())
        TRY(Sandbox::add_seatbelt_path_if_exists(paths, *bundle, Sandbox::SeatbeltPath::Access::ReadOnly));
    else
        TRY(Sandbox::add_seatbelt_path_if_exists(paths, LexicalPath::dirname(executable_path), Sandbox::SeatbeltPath::Access::ReadOnly));

    TRY(Sandbox::add_seatbelt_path_if_exists(paths, TRY(String::formatted("{}/fonts", resource_root)), Sandbox::SeatbeltPath::Access::ReadOnly));

    TRY(Core::Directory::create(cache_path, Core::Directory::CreateDirectories::Yes));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, cache_path, Sandbox::SeatbeltPath::Access::ReadWrite));

    auto photon_cache_path = ByteString::formatted("{}/Photon", Core::StandardPaths::cache_directory());
    TRY(Core::Directory::create(photon_cache_path, Core::Directory::CreateDirectories::Yes));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, photon_cache_path, Sandbox::SeatbeltPath::Access::ReadWrite));

    // Metal keeps its shader caches in the Darwin user cache directory, in a directory named after the application's
    // bundle identifier. The rest of that directory belongs to other applications. Metal also issues an extension for
    // this directory to its compiler service, which the sandbox only allows for paths named here.
    if (auto metal_cache_directory = TRY(application_darwin_user_cache_directory()); metal_cache_directory.has_value()) {
        TRY(Core::Directory::create(*metal_cache_directory, Core::Directory::CreateDirectories::Yes));
        TRY(Sandbox::add_seatbelt_path_if_exists(paths, *metal_cache_directory, Sandbox::SeatbeltPath::Access::ReadWrite));
    }

    // ANGLE's Metal backend opens one of these while creating WebGL contexts, depending on whether the GPU is real
    // hardware or the paravirtualized device of a virtual machine.
    static constexpr Array metal_iokit_user_client_classes {
        "AGXDeviceUserClient"sv,
        "AppleParavirtDeviceUserClient"sv,
    };

    return Sandbox::apply_macos_sandbox({
        .paths = paths.span(),
        .iokit_user_client_classes = metal_iokit_user_client_classes,
        .mach_server_name = mach_server_name,
        // Display: the vsync scheduler drives one CVDisplayLink per display.
        .system_services = Sandbox::SystemService::Fonts | Sandbox::SystemService::GPU | Sandbox::SystemService::IOSurface | Sandbox::SystemService::Display,
    });
}

}
