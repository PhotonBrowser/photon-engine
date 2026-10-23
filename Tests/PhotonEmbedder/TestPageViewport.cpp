/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibCore/EventLoop.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/RemoteNavigable.h>
#include <LibWeb/InvalidateDisplayList.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace {

void install_web_platform_support()
{
    static Core::EventLoop event_loop;
    static Web::Platform::FontPlugin font_plugin { false };
    (void)event_loop;
    Web::Platform::FontPlugin::install(font_plugin);
}

}

TEST_CASE(viewport_size_handles_local_and_remote_top_level_traversables)
{
    install_web_platform_support();

    auto local_realm = Web::Bindings::create_a_principal_javascript_realm();
    auto& local_page = Web::Bindings::principal_host_defined_page(*local_realm);
    auto local_traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(local_page, nullptr, {});
    local_page.set_top_level_traversable(local_traversable);

    EXPECT(local_page.has_local_traversable());
    EXPECT(local_page.set_viewport_size_from_device_pixels_if_local({ 640, 480 }, Web::InvalidateDisplayList::No));
    EXPECT_EQ(local_traversable->viewport_size(), (Web::CSSPixelSize { Web::CSSPixels { 640 }, Web::CSSPixels { 480 } }));

    auto remote_realm = Web::Bindings::create_a_principal_javascript_realm();
    auto& remote_page = Web::Bindings::principal_host_defined_page(*remote_realm);
    auto initial_traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(remote_page, nullptr, {});
    auto replicated_state = initial_traversable->replicated_state();
    auto remote_traversable = Web::HTML::RemoteNavigable::create(remote_page, { 1, 1 }, nullptr, move(replicated_state));
    remote_page.set_top_level_traversable(remote_traversable);

    EXPECT(!remote_page.has_local_traversable());
    EXPECT(!remote_page.set_viewport_size_from_device_pixels_if_local({ 640, 480 }, Web::InvalidateDisplayList::No));
}

TEST_CASE(permission_response_requires_matching_request_and_document_and_resolves_once)
{
    auto realm = Web::Bindings::create_a_principal_javascript_realm();
    auto& page = Web::Bindings::principal_host_defined_page(*realm);
    auto document_id = Web::HTML::EnvironmentId::generate();
    auto stale_document_id = Web::HTML::EnvironmentId::generate();
    Optional<bool> decision;
    auto callback = GC::create_function(realm->heap(), [&](bool granted) { decision = granted; });

    auto origin = URL::Origin::create_opaque();
    page.request_permission("microphone"_utf16, origin, origin, document_id, callback);

    page.permission_request_completed(0, stale_document_id, true);
    EXPECT(!decision.has_value());

    page.permission_request_completed(0, document_id, false);
    EXPECT(decision.has_value());
    EXPECT(!*decision);

    page.permission_request_completed(0, document_id, true);
    EXPECT(!*decision);
}
