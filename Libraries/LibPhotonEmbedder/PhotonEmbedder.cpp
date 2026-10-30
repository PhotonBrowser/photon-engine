/*
 * Copyright (c) 2026, PhotonBrowser contributors
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PhotonEmbedderInternal.h"
#include <LibCore/EventLoop.h>
#include <LibCore/MachPort.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Palette.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibPhotonEmbedder/PhotonEmbedder.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/CSS/PreferredColorScheme.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/Permissions/Permissions.h>
#include <LibWebCommon/PixelUnits.h>
#include <LibWebCommon/WebView/ConsoleOutput.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Menu.h>
#include <LibWebView/PlatformColors.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/ProcessSnapshot.h>
#include <LibWebView/ProcessType.h>
#if defined(AK_OS_MACOS)
#    include <LibWebView/ProcessReaper.h>
#endif
#include <LibWebView/TabPerformanceMonitor.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/ViewImplementation.h>

#if defined(__APPLE__)
#    include <CoreFoundation/CoreFoundation.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace WebView {

static bool photon_frame_trace_enabled()
{
    static bool enabled = std::getenv("PHOTON_CORE_RUNLOOP_TRACE") != nullptr;
    return enabled;
}

static bool external_image_lease_trace_enabled()
{
    static bool enabled = std::getenv("EXTERNAL_IMAGE_LEASE_TRACE") != nullptr;
    return enabled;
}

static constexpr int hidden_page_gc_delay_ms = 10'000;

static uint64_t next_native_backing_id()
{
    static std::atomic<uint64_t> next_id { 1 };
    return next_id.fetch_add(1, std::memory_order_relaxed);
}

class PhotonHeadlessWebView;

class CpuFallbackFramePool : public std::enable_shared_from_this<CpuFallbackFramePool> {
public:
    std::shared_ptr<Photon::PresentedFrame> acquire()
    {
        std::unique_ptr<Photon::PresentedFrame> frame;
        {
            std::lock_guard lock(m_mutex);
            if (!m_available.empty()) {
                frame = move(m_available.back());
                m_available.pop_back();
            }
        }
        if (!frame)
            frame = std::make_unique<Photon::PresentedFrame>();

        auto* raw_frame = frame.release();
        return std::shared_ptr<Photon::PresentedFrame>(raw_frame,
            [pool = shared_from_this()](Photon::PresentedFrame* released) {
                pool->recycle(released);
            });
    }

private:
    void recycle(Photon::PresentedFrame* frame)
    {
        {
            std::lock_guard lock(m_mutex);
            if (m_available.size() < maximum_cached_frames) {
                m_available.emplace_back(frame);
                return;
            }
        }
        delete frame;
    }

    static constexpr size_t maximum_cached_frames = 2;
    std::mutex m_mutex;
    std::vector<std::unique_ptr<Photon::PresentedFrame>> m_available;
};

class PhotonApplication final : public Application {
    WEB_VIEW_APPLICATION(PhotonApplication)

public:
    explicit PhotonApplication(ByteString helper_directory)
        : Application(move(helper_directory))
    {
    }

    // The view that application actions such as Copy and Reload act on: the
    // one most recently focused or asked for a context menu.
    void set_active_view(PhotonHeadlessWebView&);
    void set_clipboard(Photon::Clipboard clipboard) { m_system_clipboard = move(clipboard); }
    void set_elastic_scroll_enabled(bool enabled)
    {
        m_elastic_scroll_enabled = enabled;
        set_scrolls_elastically(enabled);
    }

private:
    // Each Photon process owns its embedder runtime and its view callbacks.
    // Joining another Ladybird browser process would leave this runtime only
    // partially initialized before Photon creates its first headless view.
    bool should_coordinate_browser_process() const override { return false; }

    virtual Optional<ViewImplementation&> active_web_view() const override;
    virtual void open_url_in_new_tab(URL::URL const&, Web::HTML::ActivateTab) const override;
    virtual void open_navigation_in_new_tab(Web::HTML::PreparedNavigationDescriptor, Web::HTML::ActivateTab) const override;
    virtual void open_navigation_in_peek(Web::HTML::PreparedNavigationDescriptor) const override;

    virtual bool platform_scrolls_elastically() const override
    {
        return m_elastic_scroll_enabled;
    }

    virtual Utf16String clipboard_text(ClipboardType type) const override
    {
        if (!m_system_clipboard.read)
            return Application::clipboard_text(type);
        for (auto const& entry : m_system_clipboard.read()) {
            if (entry.mime_type == "text/plain")
                return Utf16String::from_utf8(StringView { entry.data.data(), entry.data.size() });
        }
        return {};
    }

    virtual void set_clipboard_text(String text, ClipboardType type) override
    {
        if (!m_system_clipboard.write) {
            Application::set_clipboard_text(move(text), type);
            return;
        }
        auto bytes = text.bytes_as_string_view();
        m_system_clipboard.write({ { "text/plain", std::string(bytes.characters_without_null_termination(), bytes.length()) } });
    }

    virtual Web::Clipboard::SystemClipboardItem clipboard_item() const override
    {
        if (!m_system_clipboard.read)
            return Application::clipboard_item();
        Web::Clipboard::SystemClipboardItem item;
        for (auto const& entry : m_system_clipboard.read()) {
            item.system_clipboard_representations.append({
                .name = MUST(String::from_utf8(StringView { entry.mime_type.data(), entry.mime_type.size() })),
                .data = ByteString { entry.data.data(), entry.data.size() },
            });
        }
        return item;
    }

    virtual void insert_clipboard_item(Web::Clipboard::SystemClipboardItem item) override
    {
        if (!m_system_clipboard.write) {
            Application::insert_clipboard_item(move(item));
            return;
        }
        std::vector<Photon::ClipboardEntry> entries;
        for (auto const& representation : item.system_clipboard_representations) {
            // Files copied in the system file manager are only ever read.
            auto const* data = representation.data.get_pointer<ByteString>();
            if (!data)
                continue;
            auto name = representation.name.bytes_as_string_view();
            entries.push_back({
                std::string(name.characters_without_null_termination(), name.length()),
                std::string(data->characters(), data->length()),
            });
        }
        m_system_clipboard.write(entries);
    }

    virtual void create_platform_options(BrowserOptions&, RequestServerOptions&, WebContentOptions& content) override
    {
        // Reuse fresh responses and JavaScript bytecode during history traversals, matching the desktop default.
        content.enable_http_memory_cache = EnableMemoryHTTPCache::Yes;

        // Viewport scrollbars are painted into the page's shared GPU surface.
        content.paint_viewport_scrollbars = PaintViewportScrollbars::Yes;

        // Deprecated diagnostic fallback. Normal Photon painting uses Vulkan;
        // remove this option once the GPU path is reliable on supported devices.
        if (std::getenv("PHOTON_FORCE_CPU_PAINTING"))
            content.force_cpu_painting = ForceCPUPainting::Yes;
    }

    Photon::Clipboard m_system_clipboard;
    WeakPtr<PhotonHeadlessWebView> m_active_view;
    bool m_elastic_scroll_enabled { false };
};

class PhotonHeadlessWebView final : public HeadlessWebView {
public:
    bool has_completed_document_load() const { return m_document_load_finished; }
    uint64_t document_generation() const { return m_document_generation; }

    static OwnPtr<PhotonHeadlessWebView> create(int width, int height, double dpr, Photon::ViewCallbacks callbacks)
    {
        auto resource_root = s_ladybird_resource_root;
        auto theme_path = LexicalPath::join(resource_root, "themes"sv, "Default.ini"sv);
        auto theme = Gfx::load_system_theme(theme_path.string());
        if (theme.is_error())
            return nullptr;
#if defined(__APPLE__)
        // Match the Qt and AppKit embedders: page selections use the macOS colors
        // rather than the theme file's.
        Gfx::Palette palette(Gfx::PaletteImpl::create_with_anonymous_buffer(theme.value()));
        palette.set_color(Gfx::ColorRole::Selection, macos_web_selection_color());
        palette.set_color(Gfx::ColorRole::InactiveSelection, macos_web_inactive_selection_color());
        palette.set_color(Gfx::ColorRole::InactiveSelectionText, macos_web_inactive_selection_text_color());
#endif

        auto physical_width = max(1, static_cast<int>(std::lround(width * dpr)));
        auto physical_height = max(1, static_cast<int>(std::lround(height * dpr)));
        auto view = adopt_own(*new PhotonHeadlessWebView(theme.release_value(), { physical_width, physical_height }, dpr, move(callbacks)));
        if (!view->m_callbacks.native_metal_presentation
            || !view->m_callbacks.native_backing_registered
            || !view->m_callbacks.native_frame_ready)
            Application::the().notify_compositor_gpu_presentation_unavailable();
        view->initialize_tab(Web::HTML::VisibilityState::Visible);
        // initialize_tab() establishes the WebContent client and can
        // initialize its viewport from the platform screen before the
        // embedder's requested size has propagated. Reapply the view's actual
        // initial viewport after that setup so the first presented frame uses
        // the requested bounds.
        view->resize(width, height, dpr);
        view->notify_state();
        (void)TabPerformanceMonitor::the();
        return view;
    }

    static OwnPtr<PhotonHeadlessWebView> create_for_traversable(
        PhotonHeadlessWebView& opener,
        CanonicalTraversable& traversable,
        int width,
        int height,
        double dpr,
        Photon::ViewCallbacks callbacks)
    {
        auto physical_width = max(1, static_cast<int>(std::lround(width * dpr)));
        auto physical_height = max(1, static_cast<int>(std::lround(height * dpr)));
        auto view = adopt_own(*new PhotonHeadlessWebView(opener.m_theme, { physical_width, physical_height }, dpr, move(callbacks)));
        view->initialize_tab(Web::HTML::VisibilityState::Hidden, traversable);
        view->resize(width, height, dpr);
        view->notify_state();
        (void)TabPerformanceMonitor::the();
        return view;
    }

    void resize(int width, int height, double dpr)
    {
        auto physical_width = max(1, static_cast<int>(std::lround(width * dpr)));
        auto physical_height = max(1, static_cast<int>(std::lround(height * dpr)));
        // NB: A resize keeps the presentation generation. The compositor pads its pool while a resize is in progress
        //     and keeps the same backings, which the client must not re-register; each frame carries its own content
        //     size. A replaced pool advances the generation in on_backing_store_pool_changed.
        m_device_pixel_ratio = dpr;
        if (std::getenv("PHOTON_CORE_RUNLOOP_TRACE")) {
            on_console_message = [](WebView::ConsoleOutput output) {
                output.output.visit(
                    [](WebView::ConsoleError const& error) { dbgln("[HTMLScript] error name={} message={}", error.name, error.message); },
                    [](WebView::ConsoleLog const&) { dbgln("[HTMLScript] console_message at_ns={}", MonotonicTime::now().nanoseconds()); },
                    [](WebView::ConsoleTrace const&) { });
            };
        }
        reset_viewport_size({ physical_width, physical_height });
    }

#if defined(AK_OS_MACOS)
    void release_native_frame(uint64_t backing_id, uint64_t generation, uint64_t frame_id, bool consumer_gpu_work_complete)
    {
        auto it = m_native_leases.find(frame_id);
        VERIFY(it != m_native_leases.end());
        VERIFY(it->second.first == backing_id && it->second.second == generation);
        m_native_leases.erase(it);
        auto backing = m_native_backing_to_bitmap.find(backing_id);
        VERIFY(backing != m_native_backing_to_bitmap.end());
        auto bitmap_id = backing->second;
        if (!consumer_gpu_work_complete)
            m_native_backing_requires_iosurface_release.emplace(bitmap_id);
        bool backing_still_leased = false;
        for (auto const& lease : m_native_leases) {
            if (lease.second.first == backing_id && lease.second.second == generation) {
                backing_still_leased = true;
                break;
            }
        }
        if (external_image_lease_trace_enabled())
            dbgln("[ExternalImageLease][Embedder] at_ns={} backing_id={} generation={} frame_id={} state=RELEASE_RECEIVED remaining_leases={} backing_still_leased={} deferred_release={}", MonotonicTime::now().nanoseconds(), backing_id, generation, frame_id, m_native_leases.size(), backing_still_leased, m_deferred_backing_releases.contains(bitmap_id));
        if (!backing_still_leased) {
            m_native_leased_backings.erase(bitmap_id);
            bool previously_had_incomplete_consumer = m_native_backing_requires_iosurface_release.erase(bitmap_id) > 0;
            bool all_consumer_gpu_work_complete = consumer_gpu_work_complete && !previously_had_incomplete_consumer;
            if (m_deferred_backing_releases.erase(bitmap_id) > 0) {
                if (external_image_lease_trace_enabled())
                    dbgln("[ExternalImageLease][Embedder] at_ns={} backing_id={} generation={} frame_id={} state=DEFERRED_RELEASE->ENGINE_BACKING_RELEASE", MonotonicTime::now().nanoseconds(), backing_id, generation, frame_id);
                release_backing_store(bitmap_id, all_consumer_gpu_work_complete);
            }
        }
    }
#endif

    void navigate(StringView url)
    {
        auto parsed = URL::Parser::basic_parse(url);
        if (!parsed.has_value()) {
            if (m_callbacks.failed)
                m_callbacks.failed("Invalid URL");
            return;
        }
        load(*parsed);
    }

    void update_zoom() override
    {
        HeadlessWebView::update_zoom();
        if (m_callbacks.zoom_changed)
            m_callbacks.zoom_changed(zoom_level());
    }

    void open_in_new_tab(URL::URL const& url, Web::HTML::ActivateTab activate_tab)
    {
        if (!m_callbacks.open_in_new_tab_requested)
            return;
        auto serialized = url.serialize().bytes_as_string_view();
        m_callbacks.open_in_new_tab_requested(
            std::string(serialized.characters_without_null_termination(), serialized.length()),
            activate_tab == Web::HTML::ActivateTab::Yes);
    }

    void peek(URL::URL const& url)
    {
        if (!m_callbacks.peek_requested)
            return;
        auto serialized = url.serialize().bytes_as_string_view();
        m_callbacks.peek_requested(std::string(serialized.characters_without_null_termination(), serialized.length()));
    }

    void activate_context_menu_item(size_t index)
    {
        if (index >= m_context_menu_actions.size() || !m_context_menu_actions[index])
            return;
        auto action = m_context_menu_actions[index];
        if (!action->enabled())
            return;
        static_cast<PhotonApplication&>(Application::the()).set_active_view(*this);
        action->activate();
    }

    void request_select_dropdown(Gfx::IntPoint widget_position, i32 minimum_width, Vector<Web::HTML::SelectItem> const& items)
    {
        if (!m_callbacks.select_dropdown_requested) {
            select_dropdown_closed({});
            return;
        }
        Photon::SelectDropdownRequest request;
        request.x = widget_position.x() / m_device_pixel_ratio;
        request.y = widget_position.y() / m_device_pixel_ratio;
        request.minimum_width = minimum_width;
        auto to_std_string = [](Utf16String const& text) {
            auto utf8 = text.to_utf8();
            return std::string(utf8.bytes_as_string_view().characters_without_null_termination(), utf8.bytes().size());
        };
        auto add_option = [&](Web::HTML::SelectItemOption const& option) {
            request.items.push_back({
                .kind = Photon::SelectDropdownItem::Kind::Option,
                .label = to_std_string(option.label),
                .id = option.id,
                .selected = option.selected,
                .enabled = !option.disabled,
            });
        };
        for (auto const& item : items) {
            item.visit(
                [&](Web::HTML::SelectItemOption const& option) { add_option(option); },
                [&](Web::HTML::SelectItemOptionGroup const& group) {
                    request.items.push_back({
                        .kind = Photon::SelectDropdownItem::Kind::GroupLabel,
                        .label = to_std_string(group.label),
                        .id = 0,
                        .selected = false,
                        .enabled = false,
                    });
                    for (auto const& option : group.items)
                        add_option(option);
                },
                [&](Web::HTML::SelectItemSeparator const&) {
                    Photon::SelectDropdownItem separator;
                    separator.kind = Photon::SelectDropdownItem::Kind::Separator;
                    request.items.push_back(move(separator));
                });
        }
        m_callbacks.select_dropdown_requested(request);
    }

    void request_file_picker(Web::HTML::AllowMultipleFiles allow_multiple_files)
    {
        if (!m_callbacks.file_picker_requested) {
            file_picker_closed({});
            return;
        }
        m_callbacks.file_picker_requested(allow_multiple_files == Web::HTML::AllowMultipleFiles::Yes);
    }

    void choose_files(std::vector<std::string> const& paths)
    {
        Vector<Web::HTML::SelectedFile> selected_files;
        for (auto const& path : paths) {
            ByteString file_path { path.data(), path.size() };
            if (auto file = WebView::create_selected_file(file_path); file.is_error())
                warnln("Unable to open file {}: {}", file_path, file.error());
            else
                selected_files.append(file.release_value());
        }
        file_picker_closed(move(selected_files));
    }

    void notify_navigation_timing(Photon::NavigationTimingEvent event, URL::URL const& url)
    {
        if (!m_callbacks.navigation_timing)
            return;
        auto serialized_url = url.serialize().bytes_as_string_view();
        std::string url_string(
            serialized_url.characters_without_null_termination(),
            serialized_url.length());
        m_callbacks.navigation_timing(event, url_string);
    }

    void notify_state()
    {
        if (!m_callbacks.state_changed)
            return;
        Photon::ViewState state;
        auto serialized_url = url().serialize().bytes_as_string_view();
        state.url.assign(serialized_url.characters_without_null_termination(), serialized_url.length());
        auto title_string = MUST(String::formatted("{}", title()));
        auto title_bytes = title_string.bytes_as_string_view();
        state.title.assign(title_bytes.characters_without_null_termination(), title_bytes.length());
        state.loading = is_loading();
        state.can_go_back = navigate_back_action().enabled();
        state.can_go_forward = navigate_forward_action().enabled();
        if (url().scheme() == "https"sv)
            state.connection_secure = true;
        else if (url().scheme() == "http"sv)
            state.connection_secure = false;
        m_callbacks.state_changed(state);
    }

    void request_dialog(Photon::DialogType type, Utf16String const& message, Utf16String const& default_text)
    {
        if (!m_callbacks.dialog_requested) {
            // Nobody can answer, so answer as if dismissed rather than leave the page waiting.
            switch (type) {
            case Photon::DialogType::Alert:
                alert_closed();
                break;
            case Photon::DialogType::Confirm:
                confirm_closed(false);
                break;
            case Photon::DialogType::Prompt:
                prompt_closed({});
                break;
            }
            return;
        }
        auto to_std_string = [](auto const& value) {
            auto string = MUST(String::formatted("{}", value));
            auto bytes = string.bytes_as_string_view();
            return std::string(bytes.characters_without_null_termination(), bytes.length());
        };
        Photon::DialogRequest request;
        request.type = type;
        auto origin = url().origin();
        request.title = origin.is_opaque()
            ? to_std_string(MUST(String::formatted("{}://", url().scheme())))
            : to_std_string(origin.serialize());
        request.message = to_std_string(message);
        request.default_text = to_std_string(default_text);
        m_callbacks.dialog_requested(request);
    }

    void notify_navigation_committed()
    {
        if (m_callbacks.navigation_committed)
            m_callbacks.navigation_committed();
    }

    void clear_callbacks() { m_callbacks = {}; }

    void set_native_metal_presentation(bool enabled)
    {
        m_native_metal_presentation = enabled;
        if (enabled)
            Application::the().notify_compositor_gpu_presentation_available();
        else
            Application::the().notify_compositor_gpu_presentation_unavailable();
    }

    void set_performance_monitor_enabled(bool enabled)
    {
        TabPerformanceMonitor::set_view_enabled(view_id(), enabled);
    }

    void set_residency_monitor_enabled(bool enabled)
    {
        TabPerformanceMonitor::set_view_residency_enabled(view_id(), enabled);
    }

    bool set_task_queue_diagnostics_enabled(bool enabled)
    {
        return TabPerformanceMonitor::set_view_task_queue_diagnostics_enabled(view_id(), enabled);
    }

    String task_queue_diagnostics_json() const
    {
        return TabPerformanceMonitor::task_queue_diagnostics_json(view_id());
    }

    void release_hidden_memory()
    {
        if (page().is_open() && !is_loading() && traversable().system_visibility_state() == Web::HTML::VisibilityState::Hidden) {
            if (m_hidden_gc_timer)
                m_hidden_gc_timer->stop();
            debug_request("collect-garbage"sv, "release-hidden-layout"sv);
        }
    }

    void set_visible(bool visible)
    {
        auto was_hidden = traversable().system_visibility_state() == Web::HTML::VisibilityState::Hidden;
        set_system_visibility_state(visible ? Web::HTML::VisibilityState::Visible : Web::HTML::VisibilityState::Hidden);

        if (visible) {
            if (m_hidden_gc_timer)
                m_hidden_gc_timer->stop();
        } else {
            // Give a background page time to become active again before discarding its rebuildable layout tree and
            // collecting unreachable DOM and JavaScript objects.
            if (!m_hidden_gc_timer) {
                m_hidden_gc_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(hidden_page_gc_delay_ms), [weak_this = make_weak_ptr<PhotonHeadlessWebView>()] {
                    if (!weak_this || !weak_this->page().is_open() || weak_this->traversable().system_visibility_state() != Web::HTML::VisibilityState::Hidden)
                        return;
                    if (weak_this->is_loading()) {
                        weak_this->m_hidden_gc_timer->start();
                        return;
                    }
                    weak_this->debug_request("collect-garbage"sv, "release-hidden-layout"sv);
                });
            }
            if (!m_hidden_gc_timer->is_active())
                m_hidden_gc_timer->start();
        }

        if (visible && was_hidden)
            start_page_responsiveness_check();
    }

    void set_display_metadata(Optional<u64> display_id, double refresh_rate)
    {
        if (!std::isfinite(refresh_rate) || refresh_rate <= 0)
            return;
        if (m_display_id == display_id && m_maximum_frames_per_second == refresh_rate)
            return;
        m_display_id = display_id;
        m_maximum_frames_per_second = refresh_rate;
        page().async_set_maximum_frames_per_second(m_maximum_frames_per_second);
        update_compositor_display_metadata(page());
    }

    // Allows or blocks the microphone for the origin in every process hosting the view's pages. Blocking ends the
    // origin's capture there, unless another open page of the process keeps its own grant.
    void set_microphone_permission(StringView origin_url, bool granted)
    {
        auto parsed = URL::Parser::basic_parse(origin_url);
        if (!parsed.has_value() || parsed->origin().is_opaque())
            return;
        auto origin = parsed->origin();
        auto decision = Optional<PermissionDecision> {
            granted ? PermissionDecision::Allow : PermissionDecision::Block
        };
        set_site_permission(origin, Web::Permissions::PermissionName::Microphone,
            decision);
    }

    bool toggle_audio_mute()
    {
        toggle_page_mute_state();
        auto muted = page_mute_state() == Web::HTML::MuteState::Muted;
        notify_audio_state(audibility());
        return muted;
    }

private:
    // Context menu actions Photon has no UI for yet: dictionary lookup, a
    // save panel, more windows, and screenshot confirmation.
    static bool is_unsupported_context_menu_action(ActionID id)
    {
        switch (id) {
        case ActionID::LookUpSelectedText:
        case ActionID::OpenInNewWindow:
        case ActionID::OpenInNewPrivateWindow:
        case ActionID::DownloadLinkedFileAs:
        case ActionID::SaveImage:
        case ActionID::TakeScreenshot:
        case ActionID::ViewSource:
            return true;
        default:
            return false;
        }
    }

    // Lists the menu's shown actions for the embedder, with one separator
    // between groups, and keeps them to activate by index.
    void request_context_menu(Menu& menu, Gfx::IntPoint widget_position)
    {
        if (!m_callbacks.context_menu_requested)
            return;
        static_cast<PhotonApplication&>(Application::the()).set_active_view(*this);

        Photon::ContextMenuRequest request;
        request.x = widget_position.x() / m_device_pixel_ratio;
        request.y = widget_position.y() / m_device_pixel_ratio;
        m_context_menu_actions.clear();
        bool separator_pending = false;
        for (auto& item : menu.items()) {
            item.visit(
                [&](NonnullRefPtr<Action>& action) {
                    if (!action->visible() || is_unsupported_context_menu_action(action->id()))
                        return;
                    if (separator_pending && !request.items.empty()) {
                        Photon::ContextMenuItem separator;
                        separator.separator = true;
                        request.items.push_back(move(separator));
                        m_context_menu_actions.append(nullptr);
                    }
                    separator_pending = false;
                    auto text = action->text();
                    request.items.push_back({
                        .text = std::string(text.characters_without_null_termination(), text.length()),
                        .enabled = action->enabled(),
                        .checkable = action->is_checkable(),
                        .checked = action->is_checkable() && action->checked(),
                    });
                    m_context_menu_actions.append(action);
                },
                // Page context menus have no submenus.
                [](NonnullRefPtr<Menu>&) { },
                [&](Separator) { separator_pending = true; });
        }
        if (!request.items.empty())
            m_callbacks.context_menu_requested(request);
    }

    void notify_audio_state(Audio::Audibility audibility)
    {
        if (m_callbacks.audio_state_changed)
            m_callbacks.audio_state_changed(
                audibility == Audio::Audibility::Audible,
                page_mute_state() == Web::HTML::MuteState::Muted);
    }

    // A tab can move to a page in another WebContent process; its compositor
    // context needs the display metadata as well. This hook runs both when a
    // page starts hosting the tab's navigable and when it becomes the tab's
    // displayed page.
    void prepare_page_for_navigable(WebContentPage& page) override
    {
        HeadlessWebView::prepare_page_for_navigable(page);
        update_compositor_display_metadata(page);
    }

    void update_compositor_display_metadata(WebContentPage& page)
    {
        Application::the().update_compositor_display_metadata(page.compositor_context_id(), m_display_id, m_maximum_frames_per_second);
    }

#if defined(AK_OS_MACOS)
    uint64_t native_backing_id(i32 bitmap_id)
    {
        auto existing = m_native_backing_ids.find(bitmap_id);
        if (existing != m_native_backing_ids.end())
            return existing->second;
        auto backing_id = next_native_backing_id();
        m_native_backing_ids.emplace(bitmap_id, backing_id);
        m_native_backing_to_bitmap.emplace(backing_id, bitmap_id);
        return backing_id;
    }
#endif

    PhotonHeadlessWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize size, double dpr, Photon::ViewCallbacks callbacks)
        : HeadlessWebView(move(theme), size)
        , m_callbacks(move(callbacks))
    {
        m_device_pixel_ratio = dpr;
        for (auto* menu : { &page_context_menu(), &link_context_menu(), &selected_text_link_context_menu(), &image_context_menu(), &media_context_menu() }) {
            menu->on_activation = [this, menu](Gfx::IntPoint position) {
                request_context_menu(*menu, position);
            };
        }
        on_audibility_changed = [this](Audio::Audibility audibility) {
            notify_audio_state(audibility);
        };
        on_request_permission = [this](PermissionPromptRequest const& request, PermissionPromptCallback on_outcome) {
            if (!m_callbacks.permission_requested) {
                on_outcome(Web::Permissions::PromptOutcome::Block);
                return;
            }
            auto requesting_origin_string = request.requesting_origin.serialize();
            auto top_level_origin_string = request.top_level_origin.serialize();
            auto capability = Web::Permissions::to_string(request.name);
            auto document = ByteString::formatted("{}", request.requesting_document_id);
            auto bytes = [](auto const& value) {
                auto view = [&] {
                    if constexpr (requires { value.view(); })
                        return value.view();
                    else
                        return value.bytes_as_string_view();
                }();
                return std::string(view.characters_without_null_termination(), view.length());
            };
            auto doc_id = bytes(document);
            auto embedder_request_id = m_next_permission_request_id++;
            if (embedder_request_id == 0)
                embedder_request_id = m_next_permission_request_id++;
            m_pending_permission_requests.set(embedder_request_id, PendingPermission {
                                                                       .document_id = doc_id,
                                                                       .document_generation = m_document_generation,
                                                                       .top_level_origin = bytes(top_level_origin_string),
                                                                       .capability = std::string(capability.characters_without_null_termination(), capability.length()),
                                                                       .on_decided = [on_outcome = move(on_outcome)](bool granted) mutable {
                                                                           on_outcome(granted ? Web::Permissions::PromptOutcome::AllowAlways : Web::Permissions::PromptOutcome::Block);
                                                                       },
                                                                   });
            m_callbacks.permission_requested(Photon::PermissionRequest {
                .request_id = embedder_request_id,
                .document_id = move(doc_id),
                .requesting_origin = bytes(requesting_origin_string),
                .top_level_origin = bytes(top_level_origin_string),
                .capability = std::string(capability.characters_without_null_termination(), capability.length()),
            });
        };
        on_cancel_permission_prompt = [this](PermissionPromptRequest const& request) {
            auto top_level_origin = request.top_level_origin.serialize();
            auto top_level_origin_view = top_level_origin.bytes_as_string_view();
            auto top_level_origin_string = std::string(top_level_origin_view.characters_without_null_termination(), top_level_origin_view.length());
            auto capability = Web::Permissions::to_string(request.name);
            auto capability_string = std::string(capability.characters_without_null_termination(), capability.length());
            auto document = ByteString::formatted("{}", request.requesting_document_id);
            auto document_view = document.view();
            auto document_string = std::string(document_view.characters_without_null_termination(), document_view.length());
            Optional<uint64_t> embedder_request_id;
            for (auto const& entry : m_pending_permission_requests) {
                if (entry.value.document_id == document_string && entry.value.top_level_origin == top_level_origin_string && entry.value.capability == capability_string) {
                    embedder_request_id = entry.key;
                    break;
                }
            }
            if (!embedder_request_id.has_value())
                return;
            auto pending = m_pending_permission_requests.take(*embedder_request_id).release_value();
            if (m_callbacks.permission_cancelled)
                m_callbacks.permission_cancelled(*embedder_request_id, pending.document_id);
        };
        on_page_unresponsive_changed = [this](bool unresponsive) {
            if (m_callbacks.page_unresponsive_changed)
                m_callbacks.page_unresponsive_changed(unresponsive);
        };
        on_history_swipe_candidate = [this](u64 gesture_id, bool back, float unconsumed_delta) {
            if (m_callbacks.history_swipe_candidate)
                m_callbacks.history_swipe_candidate(gesture_id, back, unconsumed_delta);
        };
        on_url_change = [this](URL::URL const& url) {
            if (external_image_lease_trace_enabled()) {
                auto serialized_url = url.serialize();
#if defined(AK_OS_MACOS)
                dbgln(
                    "[ExternalImageLease][Embedder] at_ns={} state=NAVIGATION_STARTED url={} generation={} last_frame_id={}",
                    MonotonicTime::now().nanoseconds(), serialized_url, m_native_generation, m_native_next_frame_id);
#else
                dbgln("[ExternalImageLease][Embedder] at_ns={} state=NAVIGATION_STARTED url={}", MonotonicTime::now().nanoseconds(), serialized_url);
#endif
            }
            notify_state();
        };
        on_load_start = [this] {
            m_document_load_finished = false;
            invalidate_pending_permission_requests();
            ++m_document_generation;
            if (m_document_generation == 0)
                ++m_document_generation;
            m_navigation_timing_waiting_for_first_frame = false;
            notify_navigation_timing(Photon::NavigationTimingEvent::Started, url());
        };
#if defined(AK_OS_MACOS)
        on_backing_store_pool_changed = [this] {
            ++m_native_generation;
            if (external_image_lease_trace_enabled()) {
                dbgln(
                    "[ExternalImageLease][Embedder] at_ns={} state=PRESENTATION_EPOCH_CHANGED reason=backing_store_pool_replaced generation={} last_frame_id={}",
                    MonotonicTime::now().nanoseconds(), m_native_generation, m_native_next_frame_id);
            }
        };
#endif
        on_title_change = [this](Utf16String const&) { notify_state(); };
        on_link_hover = [this](URL::URL const& url) {
            if (!m_callbacks.link_hovered)
                return;
            auto serialized = url.serialize().bytes_as_string_view();
            m_callbacks.link_hovered(std::string(serialized.characters_without_null_termination(), serialized.length()));
        };
        on_fullscreen_window = [this] {
            if (m_callbacks.fullscreen_requested)
                m_callbacks.fullscreen_requested(true);
        };
        on_exit_fullscreen_window = [this] {
            if (m_callbacks.fullscreen_requested)
                m_callbacks.fullscreen_requested(false);
        };
        on_link_unhover = [this] {
            if (m_callbacks.link_hovered)
                m_callbacks.link_hovered({});
        };
        // As Qt's IOSurface layer does, so uncovered page area matches the page.
        on_page_background_color_change = [this](Gfx::Color color) {
            if (m_callbacks.page_background_changed)
                m_callbacks.page_background_changed(color.red(), color.green(), color.blue());
        };
        on_loading_state_change = [this](bool) { notify_state(); };
        on_performance_stats = [this](WebView::TabPerformanceStats const& stats) {
            if (!m_callbacks.performance_stats_changed)
                return;
            Photon::PerformanceStats snapshot {
                .has_cpu_percent = stats.cpu_percent.has_value(),
                .cpu_percent = stats.cpu_percent.value_or(0),
                .has_memory_bytes = stats.memory_bytes.has_value(),
                .memory_bytes = stats.memory_bytes.value_or(0),
                .has_managed_heap_bytes = stats.managed_heap_bytes.has_value(),
                .managed_heap_bytes = stats.managed_heap_bytes.value_or(0),
                .download_bytes_per_second = stats.download_bytes_per_second,
                .upload_bytes_per_second = stats.upload_bytes_per_second,
                .has_frames_per_second = stats.frames_per_second.has_value(),
                .frames_per_second = stats.frames_per_second.value_or(0),
            };
            m_callbacks.performance_stats_changed(snapshot);
        };
        on_cursor_change = [this](Gfx::Cursor const& cursor) {
            if (!m_callbacks.cursor_changed)
                return;
            cursor.visit(
                [this](Gfx::StandardCursor standard) {
                    auto mapped = Photon::Cursor::Arrow;
                    switch (standard) {
                    case Gfx::StandardCursor::Hidden:
                        mapped = Photon::Cursor::Hidden;
                        break;
                    case Gfx::StandardCursor::Crosshair:
                        mapped = Photon::Cursor::Crosshair;
                        break;
                    case Gfx::StandardCursor::IBeam:
                        mapped = Photon::Cursor::IBeam;
                        break;
                    case Gfx::StandardCursor::ResizeHorizontal:
                        mapped = Photon::Cursor::ResizeHorizontal;
                        break;
                    case Gfx::StandardCursor::ResizeVertical:
                        mapped = Photon::Cursor::ResizeVertical;
                        break;
                    case Gfx::StandardCursor::ResizeDiagonalTLBR:
                        mapped = Photon::Cursor::ResizeDiagonalTLBR;
                        break;
                    case Gfx::StandardCursor::ResizeDiagonalBLTR:
                        mapped = Photon::Cursor::ResizeDiagonalBLTR;
                        break;
                    case Gfx::StandardCursor::ResizeColumn:
                        mapped = Photon::Cursor::ResizeColumn;
                        break;
                    case Gfx::StandardCursor::ResizeRow:
                        mapped = Photon::Cursor::ResizeRow;
                        break;
                    case Gfx::StandardCursor::Hand:
                        mapped = Photon::Cursor::Hand;
                        break;
                    case Gfx::StandardCursor::Help:
                        mapped = Photon::Cursor::Help;
                        break;
                    case Gfx::StandardCursor::OpenHand:
                        mapped = Photon::Cursor::OpenHand;
                        break;
                    case Gfx::StandardCursor::Drag:
                        mapped = Photon::Cursor::Drag;
                        break;
                    case Gfx::StandardCursor::DragCopy:
                        mapped = Photon::Cursor::DragCopy;
                        break;
                    case Gfx::StandardCursor::Move:
                        mapped = Photon::Cursor::Move;
                        break;
                    case Gfx::StandardCursor::Wait:
                        mapped = Photon::Cursor::Wait;
                        break;
                    case Gfx::StandardCursor::Disallowed:
                        mapped = Photon::Cursor::Disallowed;
                        break;
                    case Gfx::StandardCursor::None:
                    case Gfx::StandardCursor::Arrow:
                    case Gfx::StandardCursor::Eyedropper:
                    case Gfx::StandardCursor::Zoom:
                        break;
                    }
                    m_callbacks.cursor_changed(mapped);
                },
                [this](Gfx::ImageCursor const&) { m_callbacks.cursor_changed(Photon::Cursor::Arrow); });
        };
        on_favicon_change = [this](Optional<Gfx::Bitmap const&> bitmap) {
            if (!m_callbacks.favicon_changed)
                return;
            if (!bitmap.has_value()) {
                m_callbacks.favicon_changed(nullptr);
                return;
            }
            Photon::Favicon favicon;
            favicon.width = bitmap->width();
            favicon.height = bitmap->height();
            favicon.pixels.reserve(static_cast<size_t>(favicon.width) * favicon.height * 4);
            auto premultiplied = bitmap->alpha_type() == Gfx::AlphaType::Premultiplied;
            for (int y = 0; y < favicon.height; ++y) {
                for (int x = 0; x < favicon.width; ++x) {
                    auto color = bitmap->get_pixel(x, y);
                    u8 red = color.red();
                    u8 green = color.green();
                    u8 blue = color.blue();
                    u8 alpha = color.alpha();
                    if (premultiplied && alpha != 0 && alpha != 255) {
                        auto unpremultiply = [alpha](u8 channel) {
                            return static_cast<u8>(min(255, (channel * 255 + alpha / 2) / alpha));
                        };
                        red = unpremultiply(red);
                        green = unpremultiply(green);
                        blue = unpremultiply(blue);
                    }
                    favicon.pixels.push_back(blue);
                    favicon.pixels.push_back(green);
                    favicon.pixels.push_back(red);
                    favicon.pixels.push_back(alpha);
                }
            }
            m_callbacks.favicon_changed(&favicon);
        };
        on_new_web_view = [this](Web::HTML::ActivateTab activate_tab, Web::HTML::WebViewHints hints, CanonicalTraversable& traversable) {
            if (!m_callbacks.new_web_view_requested)
                return String {};

            Photon::NewWebViewRequest request {
                .popup = hints.popup,
                .activate = activate_tab == Web::HTML::ActivateTab::Yes,
                .has_width = hints.width.has_value(),
                .width = hints.width.has_value() ? hints.width->value() : 0,
                .has_height = hints.height.has_value(),
                .height = hints.height.has_value() ? hints.height->value() : 0,
                .has_screen_x = hints.screen_x.has_value(),
                .screen_x = hints.screen_x.has_value() ? hints.screen_x->value() : 0,
                .has_screen_y = hints.screen_y.has_value(),
                .screen_y = hints.screen_y.has_value() ? hints.screen_y->value() : 0,
                .traversable = &traversable,
            };
            auto window_handle = m_callbacks.new_web_view_requested(request);
            if (window_handle.empty())
                return String {};
            return MUST(String::from_utf8(StringView { window_handle.data(), window_handle.size() }));
        };
        on_request_select_dropdown = [this](Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items) {
            request_select_dropdown(content_position, minimum_width, items);
        };
        on_request_file_picker = [this](Web::HTML::FileFilter const&, Web::HTML::AllowMultipleFiles allow_multiple_files) {
            request_file_picker(allow_multiple_files);
        };
        on_request_alert = [this](Utf16String const& message) {
            request_dialog(Photon::DialogType::Alert, message, {});
        };
        on_request_confirm = [this](Utf16String const& message) {
            request_dialog(Photon::DialogType::Confirm, message, {});
        };
        on_request_prompt = [this](Utf16String const& message, Utf16String const& default_text) {
            request_dialog(Photon::DialogType::Prompt, message, default_text);
        };
        on_browser_history_traversal_complete = [this] {
            notify_state();
            notify_navigation_committed();
        };
        on_top_level_navigation_commit = [this] {
            m_navigation_timing_waiting_for_first_frame = true;
            notify_navigation_timing(Photon::NavigationTimingEvent::DocumentCommitted, url());
            notify_navigation_committed();
        };
        on_load_finish = [this](URL::URL const& loaded_url) {
            m_document_load_finished = true;
            notify_navigation_timing(Photon::NavigationTimingEvent::LoadFinished, loaded_url);
            notify_navigation_committed();
        };
        on_find_in_page = [this](size_t current_match_index, Optional<size_t> const& total_match_count) {
            if (!m_callbacks.find_result)
                return;
            m_callbacks.find_result(current_match_index,
                total_match_count.has_value() ? std::optional<size_t>(*total_match_count) : std::nullopt);
        };
        on_crash_overlay_state_change = [this](bool crashed) {
            if (crashed)
                m_document_load_finished = false;
            if (!crashed && m_callbacks.crash_recovered)
                m_callbacks.crash_recovered();
        };
        on_web_content_crashed = [this](auto) {
            if (!m_callbacks.crashed)
                return;
            auto serialized_url = url().serialize().bytes_as_string_view();
            std::string crashed_url(
                serialized_url.characters_without_null_termination(),
                serialized_url.length());
            m_callbacks.crashed(crashed_url);
        };
        on_ready_to_paint = [this] {
            if (m_navigation_timing_waiting_for_first_frame) {
                m_navigation_timing_waiting_for_first_frame = false;
                notify_navigation_timing(Photon::NavigationTimingEvent::FirstFrameReady, url());
            }
            auto paint_completed = std::chrono::steady_clock::now();
            static uint64_t trace_frame_id = 0;
            auto current_trace_frame_id = ++trace_frame_id;
            auto const& front = m_front_bitmap;
            if (photon_frame_trace_enabled())
                dbgln("[Photon] frame_callback id={} at_ns={} bitmap={} native={}", current_trace_frame_id, MonotonicTime::now().nanoseconds(), front.id,
#if defined(AK_OS_MACOS)
                    m_callbacks.native_metal_presentation && !m_native_presentation_failed
#else
                    false
#endif
                );
#if defined(AK_OS_MACOS)
            if (m_callbacks.native_metal_presentation && m_native_metal_presentation && !m_native_presentation_failed
                && m_callbacks.native_backing_registered
                && m_callbacks.native_frame_ready
                && front.shared_image_buffer) {
                auto backing_id = native_backing_id(front.id);
                auto generation = m_native_generation;
                VERIFY(backing_id != 0 && generation != 0);
                if (!m_registered_native_backings.contains({ backing_id, generation })) {
                    auto const& surface = front.shared_image_buffer->iosurface_handle();
                    auto mach_port = surface.create_mach_port();
                    Photon::NativeGpuBacking backing {
                        .backing_id = backing_id,
                        .generation = generation,
                        .width = static_cast<uint32_t>(surface.width()),
                        .height = static_cast<uint32_t>(surface.height()),
                        .pixel_format = surface.pixel_format(),
                        .iosurface_mach_port = static_cast<uint32_t>(mach_port.release()),
                    };
                    if (!m_callbacks.native_backing_registered(backing)) {
                        m_native_presentation_failed = true;
                        Application::the().notify_compositor_gpu_presentation_unavailable();
                        dbgln("Photon embedder: native Metal backing registration failed; using CPU frames");
                    } else {
                        m_registered_native_backings.emplace(backing_id, generation);
                    }
                }

                if (m_native_presentation_failed)
                    goto cpu_fallback;

                auto frame_id = ++m_native_next_frame_id;
                m_native_leases.emplace(frame_id, std::pair { backing_id, generation });
                m_native_leased_backings.emplace(front.id);
                if (external_image_lease_trace_enabled())
                    dbgln("[ExternalImageLease][Embedder] at_ns={} backing_id={} generation={} frame_id={} bitmap_id={} state=PUBLISHED signal_value={} leases={} leased_backings={}", MonotonicTime::now().nanoseconds(), backing_id, generation, frame_id, front.id, front.presentation_signal_value, m_native_leases.size(), m_native_leased_backings.size());
                auto content_size = front.last_painted_size.to_type<int>();
                m_callbacks.native_frame_ready(Photon::NativeGpuFrame {
                    .backing_id = backing_id,
                    .generation = generation,
                    .frame_id = frame_id,
                    .signal_value = front.presentation_signal_value,
                    .width = content_size.width(),
                    .height = content_size.height(),
                    .device_pixel_ratio = m_device_pixel_ratio,
                });
                return;
            }
#endif
        cpu_fallback:
            if (!front.shared_image_buffer || !m_callbacks.frame_ready) {
                dbgln("Photon embedder: paint callback without a shared image buffer");
                return;
            }
            auto bitmap_started = std::chrono::steady_clock::now();
            auto bitmap = front.shared_image_buffer->bitmap_if_present();
            auto bitmap_acquisition_time = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - bitmap_started).count();
            if (!bitmap) {
                dbgln("Photon embedder: shared image has no CPU bitmap");
                return;
            }
            if (!m_cpu_fallback_frame_pool)
                m_cpu_fallback_frame_pool = std::make_shared<CpuFallbackFramePool>();
            auto const pixel_data_size = bitmap->data_size();
            auto allocation_started = std::chrono::steady_clock::now();
            auto frame = m_cpu_fallback_frame_pool->acquire();
            frame->pixels.resize(pixel_data_size);
            frame->buffer_allocation_microseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - allocation_started)
                    .count());
            frame->width = bitmap->width();
            frame->height = bitmap->height();
            frame->stride = bitmap->pitch();
            frame->device_pixel_ratio = m_device_pixel_ratio;
            if (m_last_paint_completed.has_value())
                frame->engine_paint_interval_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(paint_completed - *m_last_paint_completed).count();
            m_last_paint_completed = paint_completed;
            frame->bitmap_acquisition_microseconds = static_cast<uint64_t>(bitmap_acquisition_time);
            auto copy_started = std::chrono::steady_clock::now();
            for (int row = 0; row < frame->height; ++row)
                std::copy_n(bitmap->scanline_u8(row), frame->stride, frame->pixels.data() + row * frame->stride);
            frame->copy_time_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - copy_started).count();
            frame->paint_to_callback_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - paint_completed).count();
            m_callbacks.frame_ready(move(frame));
        };
    }

public:
    ~PhotonHeadlessWebView() override
    {
        invalidate_pending_permission_requests();
        on_cancel_permission_prompt = {};
    }

    void permission_request_completed(uint64_t request_id, std::string const& document_id, bool granted)
    {
        auto pending = m_pending_permission_requests.get(request_id);
        if (!pending.has_value() || pending->document_id != document_id
            || pending->document_generation != m_document_generation)
            return;
        auto request = m_pending_permission_requests.take(request_id).release_value();
        request.on_decided(granted);
    }

private:
    void invalidate_pending_permission_requests()
    {
        auto pending = move(m_pending_permission_requests);
        m_pending_permission_requests.clear();
        for (auto& entry : pending) {
            if (m_callbacks.permission_cancelled)
                m_callbacks.permission_cancelled(entry.key, entry.value.document_id);
            entry.value.on_decided(false);
        }
    }

    // Open links in a Photon tab rather than a hidden headless view.
    virtual ViewImplementation* create_view_for_new_tab_or_window(IsPrivate) override { return nullptr; }

    Photon::ViewCallbacks m_callbacks;
    struct PendingPermission {
        std::string document_id;
        uint64_t document_generation { 0 };
        std::string top_level_origin;
        std::string capability;
        Function<void(bool)> on_decided;
    };
    HashMap<uint64_t, PendingPermission> m_pending_permission_requests;
    uint64_t m_next_permission_request_id { 1 };
    RefPtr<Core::Timer> m_hidden_gc_timer;
    bool m_navigation_timing_waiting_for_first_frame { false };
    std::shared_ptr<CpuFallbackFramePool> m_cpu_fallback_frame_pool;
    // The shown context menu's actions, with null for separators.
    Vector<RefPtr<Action>> m_context_menu_actions;
    Optional<std::chrono::steady_clock::time_point> m_last_paint_completed;
#if defined(AK_OS_MACOS)
    virtual bool defer_backing_store_release(i32 bitmap_id) override
    {
        if (!m_native_leased_backings.contains(bitmap_id)) {
            if (external_image_lease_trace_enabled())
                dbgln("[ExternalImageLease][Embedder] at_ns={} backing_id={} generation={} frame_id=not-available state=BACKING_RELEASE_NOT_DEFERRED reason=no_active_native_lease", MonotonicTime::now().nanoseconds(), native_backing_id(bitmap_id), m_native_generation);
            return false;
        }
        m_deferred_backing_releases.emplace(bitmap_id);
        if (external_image_lease_trace_enabled())
            dbgln("[ExternalImageLease][Embedder] at_ns={} backing_id={} generation={} frame_id=active state=BACKING_RELEASE_DEFERRED native_leases={} deferred_backings={}", MonotonicTime::now().nanoseconds(), native_backing_id(bitmap_id), m_native_generation, m_native_leases.size(), m_deferred_backing_releases.size());
        return true;
    }

    uint64_t m_native_generation { 1 };
    uint64_t m_native_next_frame_id { 0 };
    std::set<std::pair<uint64_t, uint64_t>> m_registered_native_backings;
    std::unordered_map<i32, uint64_t> m_native_backing_ids;
    std::unordered_map<uint64_t, i32> m_native_backing_to_bitmap;
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> m_native_leases;
    std::unordered_set<i32> m_native_leased_backings;
    std::unordered_set<i32> m_native_backing_requires_iosurface_release;
    std::unordered_set<i32> m_deferred_backing_releases;
    bool m_native_presentation_failed { false };
    bool m_native_metal_presentation { false };
#endif
    bool m_document_load_finished { false };
    uint64_t m_document_generation { 0 };
};

void PhotonApplication::set_active_view(PhotonHeadlessWebView& view)
{
    m_active_view = view.make_weak_ptr<PhotonHeadlessWebView>();
}

Optional<ViewImplementation&> PhotonApplication::active_web_view() const
{
    if (!m_active_view)
        return {};
    return *m_active_view;
}

void PhotonApplication::open_url_in_new_tab(URL::URL const& url, Web::HTML::ActivateTab activate_tab) const
{
    if (m_active_view)
        m_active_view->open_in_new_tab(url, activate_tab);
}

// Photon opens the tab by URL, like a link the user entered.
void PhotonApplication::open_navigation_in_new_tab(Web::HTML::PreparedNavigationDescriptor navigation, Web::HTML::ActivateTab activate_tab) const
{
    open_url_in_new_tab(navigation.url, activate_tab);
}

void PhotonApplication::open_navigation_in_peek(Web::HTML::PreparedNavigationDescriptor navigation) const
{
    if (m_active_view)
        m_active_view->peek(navigation.url);
}

}

namespace Photon {

#if defined(__APPLE__)
struct NativeReleaseDrainState {
    std::mutex mutex;
    void* context { nullptr };
    NativeReleaseDrainCallback callback { nullptr };
};

static void perform_native_release_drain(void* info)
{
    auto& state = *static_cast<NativeReleaseDrainState*>(info);
    NativeReleaseDrainCallback callback;
    void* context;
    {
        std::lock_guard lock(state.mutex);
        callback = state.callback;
        context = state.context;
    }
    if (callback)
        callback(context);
}
#endif

namespace {

Photon::Download download_state(WebView::FileDownloader::Download const& download)
{
    auto to_std = [](StringView view) { return std::string(view.characters_without_null_termination(), view.length()); };
    auto status = Photon::DownloadStatus::InProgress;
    switch (download.status) {
    case WebView::FileDownloader::DownloadStatus::InProgress:
        status = Photon::DownloadStatus::InProgress;
        break;
    case WebView::FileDownloader::DownloadStatus::Paused:
        status = Photon::DownloadStatus::Paused;
        break;
    case WebView::FileDownloader::DownloadStatus::Completed:
        status = Photon::DownloadStatus::Completed;
        break;
    case WebView::FileDownloader::DownloadStatus::Canceled:
        status = Photon::DownloadStatus::Canceled;
        break;
    case WebView::FileDownloader::DownloadStatus::Failed:
        status = Photon::DownloadStatus::Failed;
        break;
    }
    Photon::Download state;
    state.id = download.id;
    state.url = to_std(download.url.serialize().bytes_as_string_view());
    state.path = to_std(download.destination.string().view());
    state.status = status;
    state.received_bytes = download.downloaded_size;
    if (download.total_size.has_value())
        state.total_bytes = *download.total_size;
    if (download.bytes_per_second.has_value())
        state.bytes_per_second = *download.bytes_per_second;
    if (download.error.has_value())
        state.error = to_std(download.error->bytes_as_string_view());
    state.created_time_ms = download.created_time.milliseconds_since_epoch();
    return state;
}

// Reports the profile's downloads to the embedder.
class DownloadObserver final : public WebView::FileDownloaderObserver {
public:
    AK_ALLOC_WITH_KMALLOC;

    DownloadObserver(std::function<void(Photon::Download const&)> changed, std::function<void(uint64_t)> removed)
        : m_changed(move(changed))
        , m_removed(move(removed))
    {
        for (auto const& download : WebView::Application::the().file_downloader().downloads())
            download_added(download);
    }

private:
    virtual void download_added(WebView::FileDownloader::Download const& download) override { download_updated(download); }

    virtual void download_updated(WebView::FileDownloader::Download const& download) override
    {
        if (download.is_private == WebView::IsPrivate::No && m_changed)
            m_changed(download_state(download));
    }

    virtual void download_removed(u64 id, WebView::IsPrivate is_private) override
    {
        if (is_private == WebView::IsPrivate::No && m_removed)
            m_removed(id);
    }

    std::function<void(Photon::Download const&)> m_changed;
    std::function<void(uint64_t)> m_removed;
};

}

struct Runtime::Impl {
    // The command line the Engine application parses. It keeps pointers into
    // `strings`, so the Impl must not move once it is built.
    std::vector<std::string> strings;
    std::vector<char*> argv;
    std::vector<AK::StringView> argument_views;
    Main::Arguments arguments;

    void set_arguments(std::string const& profile_path)
    {
        strings = { "photon", profile_path.empty() ? "--temporary-profile" : "--profile-path=" + profile_path };
        for (auto& string : strings) {
            argv.push_back(string.data());
            argument_views.emplace_back(string.data(), string.size());
        }
        argv.push_back(nullptr);
        arguments.argc = static_cast<int>(strings.size());
        arguments.argv = argv.data();
        arguments.strings = { argument_views.data(), argument_views.size() };
    }

    OwnPtr<WebView::PhotonApplication> application;
#if defined(__APPLE__)
    CFRunLoopRef owner_run_loop { nullptr };
    CFRunLoopSourceRef native_release_drain_source { nullptr };
    CFRunLoopTimerRef diagnostic_pump_timer { nullptr };
    std::shared_ptr<NativeReleaseDrainState> native_release_drain { std::make_shared<NativeReleaseDrainState>() };

    ~Impl()
    {
        if (diagnostic_pump_timer) {
            CFRunLoopTimerInvalidate(diagnostic_pump_timer);
            CFRelease(diagnostic_pump_timer);
        }
        if (native_release_drain_source) {
            CFRunLoopSourceInvalidate(native_release_drain_source);
            if (owner_run_loop)
                CFRunLoopRemoveSource(owner_run_loop, native_release_drain_source, kCFRunLoopCommonModes);
            CFRelease(native_release_drain_source);
        }
        if (owner_run_loop)
            CFRelease(owner_run_loop);
    }
#endif
    // Declared last so it stops observing before the application goes away.
    OwnPtr<DownloadObserver> downloads;
};

View::Impl::~Impl() = default;

bool View::Impl::has_completed_document_load() const
{
    return view && view->has_completed_document_load();
}

uint64_t View::Impl::document_generation() const
{
    return view ? view->document_generation() : 0;
}

void View::Impl::permission_request_completed(uint64_t request_id, std::string const& document_id, bool granted)
{
    if (view)
        view->permission_request_completed(request_id, document_id, granted);
}

void View::Impl::run_javascript(String script)
{
    view->run_javascript(move(script));
}

Runtime::Runtime(std::unique_ptr<Impl> impl)
    : m_impl(move(impl))
{
}

std::unique_ptr<Runtime> Runtime::create(std::string const& helper_directory, std::string const& profile_path, std::string& error)
{
    auto impl = std::make_unique<Impl>();
    impl->set_arguments(profile_path);
    auto app = WebView::PhotonApplication::create(impl->arguments, ByteString(helper_directory.c_str()));
    if (app.is_error()) {
        error = "Photon Engine runtime initialization failed";
        return {};
    }
    impl->application = app.release_value();
#if defined(__APPLE__)
    impl->owner_run_loop = CFRunLoopGetCurrent();
    CFRetain(impl->owner_run_loop);
    CFRunLoopSourceContext source_context {};
    source_context.info = impl->native_release_drain.get();
    source_context.perform = perform_native_release_drain;
    impl->native_release_drain_source = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &source_context);
    if (!impl->native_release_drain_source) {
        error = "Could not create the native frame-release run-loop source";
        return {};
    }
    CFRunLoopAddSource(impl->owner_run_loop, impl->native_release_drain_source, kCFRunLoopCommonModes);
    // Opt-in, one-shot diagnostic comparison. Never used for normal progress.
    if (auto* delay_text = std::getenv("PHOTON_DIAGNOSTIC_PUMP_AFTER_MS")) {
        char* end = nullptr;
        auto delay_ms = std::strtol(delay_text, &end, 10);
        if (end != delay_text && *end == '\0' && delay_ms > 0 && delay_ms <= 60000) {
            CFRunLoopTimerContext context {};
            impl->diagnostic_pump_timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + static_cast<double>(delay_ms) / 1000.0, 0, 0, 0, [](CFRunLoopTimerRef, void*) {
                    dbgln("[DiagnosticPump] event=BEGIN at_ns={} one_shot=true", MonotonicTime::now().nanoseconds());
                    auto processed = Core::EventLoop::current().pump(Core::EventLoop::WaitMode::PollForEvents);
                    dbgln("[DiagnosticPump] event=END at_ns={} processed={}", MonotonicTime::now().nanoseconds(), processed); }, &context);
            if (impl->diagnostic_pump_timer)
                CFRunLoopAddTimer(impl->owner_run_loop, impl->diagnostic_pump_timer, kCFRunLoopCommonModes);
        }
    }
#endif
    return std::unique_ptr<Runtime>(new Runtime(move(impl)));
}

Runtime::~Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

void Runtime::pump()
{
    (void)Core::EventLoop::current().pump(Core::EventLoop::WaitMode::PollForEvents);
}

void Runtime::set_system_reduced_motion_preference(bool reduce_motion)
{
    m_impl->application->set_system_reduced_motion_preference(reduce_motion);
}

void Runtime::set_memory_pressure(bool pressured)
{
    m_impl->application->set_memory_pressure(pressured);
}

void Runtime::set_elastic_scroll_enabled(bool enabled)
{
    m_impl->application->set_elastic_scroll_enabled(enabled);
}

void Runtime::set_service_callback(std::function<void(EngineService, bool restarted)> callback)
{
    auto& application = *m_impl->application;
    application.on_compositor_process_death = [callback] {
        callback(EngineService::Compositor, false);
        // The Application queues the Compositor's relaunch before telling us it
        // died, so this runs once the replacement is up.
        Core::deferred_invoke([callback] { callback(EngineService::Compositor, true); });
    };
    application.on_request_server_process_death = [callback] {
        callback(EngineService::Network, false);
    };
    application.on_request_server_restarted = [callback] {
        callback(EngineService::Network, true);
    };
}

std::vector<RegisteredEngineProcess> Runtime::registered_engine_processes() const
{
    std::vector<RegisteredEngineProcess> processes;
    WebView::Application::process_manager().for_each_process([&](WebView::Process& process) {
        char const* role = nullptr;
        switch (process.type()) {
        case WebView::ProcessType::Browser:
            role = "application";
            break;
        case WebView::ProcessType::Compositor:
            role = "compositor";
            break;
        case WebView::ProcessType::WebContent:
            role = "web_content";
            break;
        case WebView::ProcessType::WebWorker:
            role = "web_worker";
            break;
        case WebView::ProcessType::RequestServer:
            role = "request_server";
            break;
        case WebView::ProcessType::ImageDecoder:
            role = "image_decoder";
            break;
        case WebView::ProcessType::MediaServer:
            role = "media_server";
            break;
        case WebView::ProcessType::VideoDecoderServer:
            role = "video_decoder_server";
            break;
        case WebView::ProcessType::AudioServer:
            role = "audio_server";
            break;
        }
        processes.push_back({
            .pid = static_cast<int32_t>(process.pid()),
            .role = role,
            .start_time_in_microseconds = process.start_time_in_microseconds().has_value()
                ? std::optional<uint64_t> { *process.start_time_in_microseconds() }
                : std::nullopt,
            .parent_pid = process.parent_pid().has_value()
                ? std::optional<int32_t> { static_cast<int32_t>(*process.parent_pid()) }
                : std::nullopt,
            .parent_start_time_in_microseconds = process.parent_start_time_in_microseconds().has_value()
                ? std::optional<uint64_t> { *process.parent_start_time_in_microseconds() }
                : std::nullopt,
        });
    });
#if defined(AK_OS_MACOS)
    if (auto process_reaper = WebView::ProcessReaper::active_identity(); process_reaper.has_value()) {
        processes.push_back({
            .pid = static_cast<int32_t>(process_reaper->pid),
            .role = "process_reaper",
            .start_time_in_microseconds = process_reaper->start_time_in_microseconds,
            .parent_pid = process_reaper->parent_pid.has_value()
                ? std::optional<int32_t> { static_cast<int32_t>(*process_reaper->parent_pid) }
                : std::nullopt,
            .parent_start_time_in_microseconds = process_reaper->parent_start_time_in_microseconds.has_value()
                ? std::optional<uint64_t> { *process_reaper->parent_start_time_in_microseconds }
                : std::nullopt,
        });
    }
#endif
    std::sort(processes.begin(), processes.end(), [](auto const& left, auto const& right) {
        return left.pid < right.pid;
    });
    return processes;
}

std::vector<EngineProcessSnapshot> Runtime::process_snapshots() const
{
    auto process_manager = WebView::Application::process_manager_if_available();
    if (!process_manager.has_value())
        return {};

    auto snapshots = WebView::process_snapshots(*process_manager);
    std::vector<EngineProcessSnapshot> processes;
    processes.reserve(snapshots.size());
    for (auto const& snapshot : snapshots) {
        std::optional<std::string> title;
        if (snapshot.title.has_value()) {
            auto utf8 = snapshot.title->to_utf8();
            title.emplace(utf8.bytes_as_string_view().characters_without_null_termination(), utf8.bytes().size());
        }

        std::optional<std::string> page_url;
        bool page_is_frame = false;
        if (snapshot.page.has_value()) {
            auto serialized_url = snapshot.page->url.serialize().bytes_as_string_view();
            page_url.emplace(serialized_url.characters_without_null_termination(), serialized_url.length());
            page_is_frame = snapshot.page->kind == WebView::ProcessPageKind::Frame;
        }

        processes.push_back({
            .child_id = snapshot.child_id,
            .pid = static_cast<int32_t>(snapshot.pid),
            .role = [&] {
                auto role = WebView::process_name_from_type(snapshot.type);
                return std::string(role.characters_without_null_termination(), role.length());
            }(),
            .title = move(title),
            .owner_child_id = snapshot.owner_child_id.has_value() ? std::optional<uint64_t> { *snapshot.owner_child_id } : std::nullopt,
            .cpu_percent = snapshot.has_cpu_statistics ? std::optional<double> { snapshot.cpu_percent } : std::nullopt,
            .memory_bytes = snapshot.has_memory_statistics ? std::optional<uint64_t> { snapshot.memory_usage_bytes } : std::nullopt,
            .disk_read_bytes = snapshot.disk_read_bytes.has_value() ? std::optional<uint64_t> { *snapshot.disk_read_bytes } : std::nullopt,
            .disk_write_bytes = snapshot.disk_write_bytes.has_value() ? std::optional<uint64_t> { *snapshot.disk_write_bytes } : std::nullopt,
            .network_download_bytes_per_second = snapshot.has_network_statistics ? std::optional<uint64_t> { snapshot.network_download_bytes_per_second } : std::nullopt,
            .network_upload_bytes_per_second = snapshot.has_network_statistics ? std::optional<uint64_t> { snapshot.network_upload_bytes_per_second } : std::nullopt,
            .page_url = move(page_url),
            .page_is_frame = page_is_frame,
        });
    }
    return processes;
}

void Runtime::set_download_callbacks(std::function<void(Download const&)> changed, std::function<void(uint64_t id)> removed)
{
    m_impl->downloads = nullptr;
    m_impl->downloads = make<DownloadObserver>(move(changed), move(removed));
}

void Runtime::pause_download(uint64_t id) { m_impl->application->file_downloader().pause_download(id); }
void Runtime::resume_download(uint64_t id) { m_impl->application->file_downloader().resume_download(id); }
void Runtime::cancel_download(uint64_t id) { m_impl->application->file_downloader().cancel_download(id); }

void Runtime::clear_inactive_downloads()
{
    m_impl->application->file_downloader().prune_inactive_downloads(WebView::IsPrivate::No);
}

void Runtime::set_clipboard(Clipboard clipboard)
{
    m_impl->application->set_clipboard(move(clipboard));
}

void Runtime::clear_browsing_data(ClearBrowsingData const& request, std::function<void()> done)
{
    using Delete = WebView::Application::ClearBrowsingDataOptions::Delete;
    WebView::Application::ClearBrowsingDataOptions options {
        .since = UnixDateTime::from_seconds_since_epoch(request.since_unix_seconds),
        .delete_cached_files = request.cache ? Delete::Yes : Delete::No,
        // Photon keeps its own history; it never uses the Engine's.
        .delete_history = Delete::No,
        .delete_download_history = Delete::No,
        .delete_site_data = request.site_data ? Delete::Yes : Delete::No,
    };
    m_impl->application->clear_browsing_data(options)->when_resolved([done = move(done)](auto) {
        if (done)
            done();
    });
}

#if defined(__APPLE__)
void Runtime::set_native_release_drain_callback(void* context, NativeReleaseDrainCallback callback)
{
    std::lock_guard lock(m_impl->native_release_drain->mutex);
    m_impl->native_release_drain->context = context;
    m_impl->native_release_drain->callback = callback;
}

void Runtime::schedule_native_release_drain()
{
    CFRunLoopSourceSignal(m_impl->native_release_drain_source);
    CFRunLoopWakeUp(m_impl->owner_run_loop);
}
#endif

std::unique_ptr<View> Runtime::create_view(int width, int height, double device_pixel_ratio, ViewCallbacks callbacks)
{
    auto impl = std::make_unique<View::Impl>();
    impl->view = WebView::PhotonHeadlessWebView::create(width, height, device_pixel_ratio, move(callbacks));
    if (!impl->view)
        return {};
    impl->last_viewport_width = max(1, static_cast<int>(std::lround(width * device_pixel_ratio)));
    impl->last_viewport_height = max(1, static_cast<int>(std::lround(height * device_pixel_ratio)));
    impl->last_device_pixel_ratio = device_pixel_ratio;
    return std::unique_ptr<View>(new View(move(impl)));
}

std::unique_ptr<View> Runtime::create_view_for_traversable(
    View& opener,
    void* traversable,
    int width,
    int height,
    double device_pixel_ratio,
    ViewCallbacks callbacks)
{
    if (!opener.m_impl || !opener.m_impl->view || !traversable)
        return {};

    auto impl = std::make_unique<View::Impl>();
    impl->view = WebView::PhotonHeadlessWebView::create_for_traversable(
        *opener.m_impl->view,
        *static_cast<WebView::CanonicalTraversable*>(traversable),
        width,
        height,
        device_pixel_ratio,
        move(callbacks));
    if (!impl->view)
        return {};
    impl->last_viewport_width = max(1, static_cast<int>(std::lround(width * device_pixel_ratio)));
    impl->last_viewport_height = max(1, static_cast<int>(std::lround(height * device_pixel_ratio)));
    impl->last_device_pixel_ratio = device_pixel_ratio;
    return std::unique_ptr<View>(new View(move(impl)));
}

View::View(std::unique_ptr<Impl> impl)
    : m_impl(move(impl))
{
}
View::~View() = default;
View::View(View&&) noexcept = default;
View& View::operator=(View&&) noexcept = default;
void View::navigate(std::string const& url) { m_impl->view->navigate({ url.data(), url.size() }); }
void View::reload() { m_impl->view->reload(); }
void View::stop_loading() { m_impl->view->stop_loading(); }
void View::go_back() { m_impl->view->traverse_the_history_by_delta(-1); }
void View::go_forward() { m_impl->view->traverse_the_history_by_delta(1); }
bool View::toggle_audio_mute() { return m_impl->view->toggle_audio_mute(); }
void View::set_microphone_permission(std::string const& origin, bool granted)
{
    m_impl->view->set_microphone_permission(StringView { origin.data(), origin.size() }, granted);
}
void View::resize(int width, int height, double device_pixel_ratio)
{
    auto physical_width = max(1, static_cast<int>(std::lround(width * device_pixel_ratio)));
    auto physical_height = max(1, static_cast<int>(std::lround(height * device_pixel_ratio)));
    if (m_impl->last_viewport_width == physical_width && m_impl->last_viewport_height == physical_height && m_impl->last_device_pixel_ratio == device_pixel_ratio)
        return;
    m_impl->last_viewport_width = physical_width;
    m_impl->last_viewport_height = physical_height;
    m_impl->last_device_pixel_ratio = device_pixel_ratio;
    m_impl->view->resize(width, height, device_pixel_ratio);
}
void View::set_performance_monitor_enabled(bool enabled)
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->set_performance_monitor_enabled(enabled);
}
void View::set_residency_monitor_enabled(bool enabled)
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->set_residency_monitor_enabled(enabled);
}
bool View::set_task_queue_diagnostics_enabled(bool enabled)
{
    VERIFY(m_impl && m_impl->view);
    return m_impl->view->set_task_queue_diagnostics_enabled(enabled);
}
size_t View::copy_task_queue_diagnostics(char* output, size_t capacity) const
{
    VERIFY(m_impl && m_impl->view);
    auto diagnostics = m_impl->view->task_queue_diagnostics_json();
    auto bytes = diagnostics.bytes_as_string_view();
    if (output && capacity > 0) {
        auto copied = min(capacity - 1, bytes.length());
        memcpy(output, bytes.characters_without_null_termination(), copied);
        output[copied] = '\0';
    }
    return bytes.length();
}
void View::release_hidden_memory()
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->release_hidden_memory();
}
bool View::needs_beforeunload_check() const
{
    VERIFY(m_impl && m_impl->view);
    return m_impl->view->needs_beforeunload_check();
}
void View::set_display_metadata(uint64_t display_id, double refresh_rate)
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->set_display_metadata(display_id == 0 ? Optional<u64> {} : Optional<u64> { display_id }, refresh_rate);
}
void View::set_visible(bool visible)
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->set_visible(visible);
}
#if defined(AK_OS_MACOS)
void View::release_native_frame(uint64_t backing_id, uint64_t generation, uint64_t frame_id, bool consumer_gpu_work_complete)
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->release_native_frame(backing_id, generation, frame_id, consumer_gpu_work_complete);
}

void View::set_native_metal_presentation(bool enabled)
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->set_native_metal_presentation(enabled);
}
#endif
void View::set_focus(bool focused)
{
    m_impl->view->set_has_system_focus(focused);
    if (focused)
        static_cast<WebView::PhotonApplication&>(WebView::Application::the()).set_active_view(*m_impl->view);
}
void View::notify_state()
{
    if (m_impl && m_impl->view)
        m_impl->view->notify_state();
}
std::string View::window_handle() const
{
    VERIFY(m_impl && m_impl->view);
    auto handle = m_impl->view->handle().to_byte_string();
    return { handle.characters(), handle.length() };
}

static Web::UIEvents::KeyModifier modifiers(bool shift, bool control, bool alt, bool meta)
{
    auto result = Web::UIEvents::KeyModifier::Mod_None;
    if (shift)
        result |= Web::UIEvents::KeyModifier::Mod_Shift;
    if (control)
        result |= Web::UIEvents::KeyModifier::Mod_Ctrl;
    if (alt)
        result |= Web::UIEvents::KeyModifier::Mod_Alt;
    if (meta)
        result |= Web::UIEvents::KeyModifier::Mod_Super;
    return result;
}

static Web::UIEvents::MouseButton mouse_button(PointerButton button)
{
    switch (button) {
    case PointerButton::None:
        return Web::UIEvents::MouseButton::None;
    case PointerButton::Primary:
        return Web::UIEvents::MouseButton::Primary;
    case PointerButton::Secondary:
        return Web::UIEvents::MouseButton::Secondary;
    case PointerButton::Middle:
        return Web::UIEvents::MouseButton::Middle;
    case PointerButton::Back:
        return Web::UIEvents::MouseButton::Backward;
    case PointerButton::Forward:
        return Web::UIEvents::MouseButton::Forward;
    }
    return Web::UIEvents::MouseButton::None;
}

void View::send_pointer_event(PointerEvent const& event)
{
    auto type = Web::MouseEvent::Type::MouseMove;
    switch (event.type) {
    case PointerType::Move:
        type = Web::MouseEvent::Type::MouseMove;
        break;
    case PointerType::Leave:
        type = Web::MouseEvent::Type::MouseLeave;
        break;
    case PointerType::Press:
        type = Web::MouseEvent::Type::MouseDown;
        break;
    case PointerType::Release:
        type = Web::MouseEvent::Type::MouseUp;
        break;
    case PointerType::Wheel:
        type = Web::MouseEvent::Type::MouseWheel;
        break;
    }
    auto to_device = [&](double value) { return Web::DevicePixels(static_cast<int>(std::lround(value * m_impl->view->device_pixel_ratio()))); };
    auto button = mouse_button(event.button);
    Web::UIEvents::MouseButton buttons = Web::UIEvents::MouseButton::None;
    for (auto candidate : { PointerButton::Primary, PointerButton::Secondary, PointerButton::Middle, PointerButton::Back, PointerButton::Forward }) {
        if ((event.buttons & static_cast<uint8_t>(candidate)) != 0)
            buttons |= mouse_button(candidate);
    }
    auto precision = event.precise_wheel ? Web::WheelDeltaPrecision::Precise : Web::WheelDeltaPrecision::Discrete;
    auto phase = static_cast<Web::ScrollGesturePhase>(event.scroll_phase);
    Web::MouseEvent native_event {
        .type = type,
        .position = { to_device(event.x), to_device(event.y) },
        .screen_position = { to_device(event.screen_x), to_device(event.screen_y) },
        .button = button,
        .buttons = buttons,
        .modifiers = modifiers(event.shift, event.control, event.alt, event.meta),
        .wheel_delta_x = event.wheel_x,
        .wheel_delta_y = event.wheel_y,
        .wheel_delta_precision = precision,
        .scroll_gesture_phase = phase,
        .click_count = event.click_count,
        .browser_data = {},
        .async_scroll_performed_default_action = false,
        .history_swipe_back_enabled = event.history_swipe_back_enabled,
        .history_swipe_forward_enabled = event.history_swipe_forward_enabled,
        .id = 0,
        .history_swipe_gesture_id = event.history_swipe_gesture_id,
    };
    m_impl->view->enqueue_input_event(move(native_event));
}

static Web::UIEvents::KeyCode engine_key_code(Key key, uint32_t code_point)
{
    if (key == Key::Unknown)
        return Web::UIEvents::code_point_to_key_code(code_point);
    // Photon::Key deliberately uses the browser virtual-key values documented in
    // this API; convert at the boundary into the engine's distinct enum type.
    return static_cast<Web::UIEvents::KeyCode>(static_cast<uint16_t>(key));
}

void View::send_key_event(Key key, bool pressed, uint32_t code_point, bool shift, bool control, bool alt, bool meta, bool repeat, bool insert_text)
{
    Web::KeyEvent event {
        .type = pressed ? Web::KeyEvent::Type::KeyDown : Web::KeyEvent::Type::KeyUp,
        .key = engine_key_code(key, code_point),
        .modifiers = modifiers(shift, control, alt, meta),
        .code_point = code_point,
        .repeat = repeat,
        .should_insert_text = pressed && insert_text,
        .browser_data = {},
        .async_scroll_performed_default_action = false,
        .id = 0,
    };
    m_impl->view->enqueue_input_event(move(event));
}

void View::set_fullscreen(bool fullscreen)
{
    m_impl->view->set_is_fullscreen(fullscreen ? Web::ViewportIsFullscreen::Yes : Web::ViewportIsFullscreen::No);
}

void View::exit_fullscreen() { m_impl->view->exit_fullscreen(); }

void View::set_preferred_color_scheme(PreferredColorScheme color_scheme)
{
    auto engine_color_scheme = Web::CSS::PreferredColorScheme::Auto;
    switch (color_scheme) {
    case PreferredColorScheme::Auto:
        engine_color_scheme = Web::CSS::PreferredColorScheme::Auto;
        break;
    case PreferredColorScheme::Dark:
        engine_color_scheme = Web::CSS::PreferredColorScheme::Dark;
        break;
    case PreferredColorScheme::Light:
        engine_color_scheme = Web::CSS::PreferredColorScheme::Light;
        break;
    }
    m_impl->view->set_preferred_color_scheme(engine_color_scheme);
}

void View::find_in_page(std::string const& query, bool case_sensitive, bool highlight_all_matches)
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->find_in_page(Utf16String::from_utf8(StringView { query.data(), query.size() }),
        case_sensitive ? CaseSensitivity::CaseSensitive : CaseSensitivity::CaseInsensitive,
        highlight_all_matches);
}

void View::find_in_page_next_match()
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->find_in_page_next_match();
}

void View::find_in_page_previous_match()
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->find_in_page_previous_match();
}

void View::zoom_in()
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->zoom_in();
}

void View::zoom_out()
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->zoom_out();
}

void View::reset_zoom()
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->reset_zoom();
}

void View::restart_unresponsive_page()
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->restart_unresponsive_page();
}

void View::activate_context_menu_item(size_t index)
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->activate_context_menu_item(index);
}

void View::select_dropdown_closed(std::optional<uint32_t> selected_id)
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->select_dropdown_closed(selected_id.has_value() ? Optional<u32> { *selected_id } : Optional<u32> {});
}

void View::file_picker_closed(std::vector<std::string> const& paths)
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->choose_files(paths);
}

void View::permission_request_completed(uint64_t request_id, std::string const& document_id, bool granted)
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->permission_request_completed(request_id, document_id, granted);
}

void View::find_in_page_end()
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->find_in_page_end();
}

void View::alert_closed()
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->alert_closed();
}

void View::confirm_closed(bool accepted)
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->confirm_closed(accepted);
}

void View::prompt_closed(std::optional<std::string> const& response)
{
    VERIFY(m_impl && m_impl->view);
    if (!response.has_value()) {
        m_impl->view->prompt_closed({});
        return;
    }
    m_impl->view->prompt_closed(Utf16String::from_utf8(StringView { response->data(), response->size() }));
}

void View::shutdown()
{
    if (!m_impl || !m_impl->view)
        return;
    m_impl->view->clear_callbacks();
    m_impl->view = nullptr;
}

}
