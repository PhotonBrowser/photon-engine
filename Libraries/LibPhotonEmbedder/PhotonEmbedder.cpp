/*
 * Copyright (c) 2026, PhotonBrowser contributors
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/PixelUnits.h>
#include <LibWebCommon/WebView/ConsoleOutput.h>
#include <LibCore/EventLoop.h>
#include <LibCore/MachPort.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/CSS/PreferredColorScheme.h>
#include <LibWebView/Application.h>
#include <LibWebView/Menu.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/ViewImplementation.h>
#include <LibPhotonEmbedder/PhotonEmbedder.h>

#if defined(__APPLE__)
#    include <CoreFoundation/CoreFoundation.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <set>
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

class PhotonApplication final : public Application {
    WEB_VIEW_APPLICATION(PhotonApplication)

public:
    explicit PhotonApplication(ByteString helper_directory)
        : Application(move(helper_directory))
    {
    }

private:
    virtual void create_platform_options(BrowserOptions&, RequestServerOptions&, WebContentOptions& content) override
    {
        // Deprecated diagnostic fallback. Normal Photon painting uses Vulkan;
        // remove this option once the GPU path is reliable on supported devices.
        if (std::getenv("PHOTON_FORCE_CPU_PAINTING"))
            content.force_cpu_painting = ForceCPUPainting::Yes;
    }
};

class PhotonHeadlessWebView final : public HeadlessWebView {
public:
    static OwnPtr<PhotonHeadlessWebView> create(int width, int height, double dpr, Photon::ViewCallbacks callbacks)
    {
        auto resource_root = s_ladybird_resource_root;
        auto theme_path = LexicalPath::join(resource_root, "themes"sv, "Default.ini"sv);
        auto theme = Gfx::load_system_theme(theme_path.string());
        if (theme.is_error())
            return nullptr;

        auto physical_width = max(1, static_cast<int>(std::lround(width * dpr)));
        auto physical_height = max(1, static_cast<int>(std::lround(height * dpr)));
        auto view = adopt_own(*new PhotonHeadlessWebView(theme.release_value(), { physical_width, physical_height }, dpr, move(callbacks)));
        if (!view->m_callbacks.native_metal_presentation
            || !view->m_callbacks.native_backing_registered
            || !view->m_callbacks.native_frame_ready)
            Application::the().notify_compositor_gpu_presentation_unavailable();
        view->initialize_client(CreateNewClient::Yes);
        // initialize_client() establishes the WebContent client and can
        // initialize its viewport from the platform screen before the
        // embedder's requested size has propagated. Reapply the view's actual
        // initial viewport after that setup so the first presented frame uses
        // the requested bounds.
        view->resize(width, height, dpr);
        view->notify_state();
        return view;
    }

    void resize(int width, int height, double dpr)
    {
        auto physical_width = max(1, static_cast<int>(std::lround(width * dpr)));
        auto physical_height = max(1, static_cast<int>(std::lround(height * dpr)));
        if (physical_width != m_native_width || physical_height != m_native_height) {
            m_native_width = physical_width;
            m_native_height = physical_height;
            if (m_native_generation != 0)
                ++m_native_generation;
        }
        m_device_pixel_ratio = dpr;
        if (std::getenv("PHOTON_CORE_RUNLOOP_TRACE")) {
            on_console_message = [](WebView::ConsoleOutput output) {
                output.output.visit(
                    [](WebView::ConsoleError const& error) { dbgln("[HTMLScript] error name={} message={}", error.name, error.message); },
                    [](WebView::ConsoleLog const&) { dbgln("[HTMLScript] console_message at_ns={}", MonotonicTime::now().nanoseconds()); },
                    [](WebView::ConsoleTrace const&) {});
            };
        }
        reset_viewport_size({ physical_width, physical_height });
    }

#if defined(AK_OS_MACOS)
    void release_native_frame(uint64_t backing_id, uint64_t generation, uint64_t frame_id)
    {
        auto it = m_native_leases.find(frame_id);
        VERIFY(it != m_native_leases.end());
        VERIFY(it->second.first == backing_id && it->second.second == generation);
        m_native_leases.erase(it);
        bool backing_still_leased = false;
        for (auto const& lease : m_native_leases) {
            if (lease.second.first == backing_id && lease.second.second == generation) {
                backing_still_leased = true;
                break;
            }
        }
        if (external_image_lease_trace_enabled())
            dbgln("[ExternalImageLease][Embedder] at_ns={} backing_id={} generation={} frame_id={} state=RELEASE_RECEIVED remaining_leases={} backing_still_leased={} deferred_release={}", MonotonicTime::now().nanoseconds(), backing_id, generation, frame_id, m_native_leases.size(), backing_still_leased, m_deferred_backing_releases.contains(static_cast<i32>(backing_id - 1)));
        if (!backing_still_leased) {
            auto bitmap_id = static_cast<i32>(backing_id - 1);
            m_native_leased_backings.erase(bitmap_id);
            if (m_deferred_backing_releases.erase(bitmap_id) > 0) {
                if (external_image_lease_trace_enabled())
                    dbgln("[ExternalImageLease][Embedder] at_ns={} backing_id={} generation={} frame_id={} state=DEFERRED_RELEASE->ENGINE_BACKING_RELEASE", MonotonicTime::now().nanoseconds(), backing_id, generation, frame_id);
                release_backing_store(bitmap_id);
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
        m_callbacks.state_changed(state);
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

private:
    PhotonHeadlessWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize size, double dpr, Photon::ViewCallbacks callbacks)
        : HeadlessWebView(move(theme), size)
        , m_callbacks(move(callbacks))
    {
        m_device_pixel_ratio = dpr;
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
        on_loading_state_change = [this](bool) { notify_state(); };
        on_cursor_change = [this](Gfx::Cursor const& cursor) {
            if (!m_callbacks.cursor_changed)
                return;
            cursor.visit(
                [this](Gfx::StandardCursor standard) {
                    auto mapped = Photon::Cursor::Arrow;
                    switch (standard) {
                    case Gfx::StandardCursor::Hidden: mapped = Photon::Cursor::Hidden; break;
                    case Gfx::StandardCursor::Crosshair: mapped = Photon::Cursor::Crosshair; break;
                    case Gfx::StandardCursor::IBeam: mapped = Photon::Cursor::IBeam; break;
                    case Gfx::StandardCursor::ResizeHorizontal: mapped = Photon::Cursor::ResizeHorizontal; break;
                    case Gfx::StandardCursor::ResizeVertical: mapped = Photon::Cursor::ResizeVertical; break;
                    case Gfx::StandardCursor::ResizeDiagonalTLBR: mapped = Photon::Cursor::ResizeDiagonalTLBR; break;
                    case Gfx::StandardCursor::ResizeDiagonalBLTR: mapped = Photon::Cursor::ResizeDiagonalBLTR; break;
                    case Gfx::StandardCursor::ResizeColumn: mapped = Photon::Cursor::ResizeColumn; break;
                    case Gfx::StandardCursor::ResizeRow: mapped = Photon::Cursor::ResizeRow; break;
                    case Gfx::StandardCursor::Hand: mapped = Photon::Cursor::Hand; break;
                    case Gfx::StandardCursor::Help: mapped = Photon::Cursor::Help; break;
                    case Gfx::StandardCursor::OpenHand: mapped = Photon::Cursor::OpenHand; break;
                    case Gfx::StandardCursor::Drag: mapped = Photon::Cursor::Drag; break;
                    case Gfx::StandardCursor::DragCopy: mapped = Photon::Cursor::DragCopy; break;
                    case Gfx::StandardCursor::Move: mapped = Photon::Cursor::Move; break;
                    case Gfx::StandardCursor::Wait: mapped = Photon::Cursor::Wait; break;
                    case Gfx::StandardCursor::Disallowed: mapped = Photon::Cursor::Disallowed; break;
                    case Gfx::StandardCursor::None:
                    case Gfx::StandardCursor::Arrow:
                    case Gfx::StandardCursor::Eyedropper:
                    case Gfx::StandardCursor::Zoom: break;
                    }
                    m_callbacks.cursor_changed(mapped);
                },
                [this](Gfx::ImageCursor const&) { m_callbacks.cursor_changed(Photon::Cursor::Arrow); });
        };
        on_browser_history_traversal_complete = [this] { notify_state(); };
        on_web_content_crashed = [this](auto) {
            if (m_callbacks.failed)
                m_callbacks.failed("WebContent process crashed");
        };
        on_ready_to_paint = [this] {
            auto paint_completed = std::chrono::steady_clock::now();
            static uint64_t trace_frame_id = 0;
            auto current_trace_frame_id = ++trace_frame_id;
            auto const& front = m_client_state.front_bitmap;
            if (photon_frame_trace_enabled()) dbgln("[Photon] frame_callback id={} at_ns={} bitmap={} native={}", current_trace_frame_id, MonotonicTime::now().nanoseconds(), front.id,
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
                && front.presentation_signal_value != 0
                && front.shared_image_buffer) {
                auto backing_id = static_cast<uint64_t>(front.id) + 1;
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
            auto frame = std::make_shared<Photon::PresentedFrame>();
            frame->width = bitmap->width();
            frame->height = bitmap->height();
            frame->stride = bitmap->pitch();
            frame->device_pixel_ratio = m_device_pixel_ratio;
            if (m_last_paint_completed.has_value())
                frame->engine_paint_interval_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(paint_completed - *m_last_paint_completed).count();
            m_last_paint_completed = paint_completed;
            frame->bitmap_acquisition_microseconds = static_cast<uint64_t>(bitmap_acquisition_time);
            auto copy_started = std::chrono::steady_clock::now();
            frame->pixels.resize(bitmap->data_size());
            for (int row = 0; row < frame->height; ++row)
                std::copy_n(bitmap->scanline_u8(row), frame->stride, frame->pixels.data() + row * frame->stride);
            frame->copy_time_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - copy_started).count();
            frame->paint_to_callback_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - paint_completed).count();
            m_callbacks.frame_ready(move(frame));
        };
    }

    Photon::ViewCallbacks m_callbacks;
    Optional<std::chrono::steady_clock::time_point> m_last_paint_completed;
#if defined(AK_OS_MACOS)
    virtual bool defer_backing_store_release(i32 bitmap_id) override
    {
        if (!m_native_leased_backings.contains(bitmap_id)) {
            if (external_image_lease_trace_enabled())
                dbgln("[ExternalImageLease][Embedder] at_ns={} backing_id={} generation={} frame_id=not-available state=BACKING_RELEASE_NOT_DEFERRED reason=no_active_native_lease", MonotonicTime::now().nanoseconds(), static_cast<u64>(bitmap_id) + 1, m_native_generation);
            return false;
        }
        m_deferred_backing_releases.emplace(bitmap_id);
        if (external_image_lease_trace_enabled())
            dbgln("[ExternalImageLease][Embedder] at_ns={} backing_id={} generation={} frame_id=active state=BACKING_RELEASE_DEFERRED native_leases={} deferred_backings={}", MonotonicTime::now().nanoseconds(), static_cast<u64>(bitmap_id) + 1, m_native_generation, m_native_leases.size(), m_deferred_backing_releases.size());
        return true;
    }

    uint64_t m_native_generation { 1 };
    uint64_t m_native_next_frame_id { 0 };
    int m_native_width { 0 };
    int m_native_height { 0 };
    std::set<std::pair<uint64_t, uint64_t>> m_registered_native_backings;
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> m_native_leases;
    std::unordered_set<i32> m_native_leased_backings;
    std::unordered_set<i32> m_deferred_backing_releases;
    bool m_native_presentation_failed { false };
    bool m_native_metal_presentation { false };
#endif
};

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

struct Runtime::Impl {
    std::string executable { "photon" };
    std::string temporary_profile { "--temporary-profile" };
    char* argv[3] { executable.data(), temporary_profile.data(), nullptr };
    AK::StringView argument_views[2] { { executable.data(), executable.size() }, { temporary_profile.data(), temporary_profile.size() } };
    Main::Arguments arguments { 2, argv, argument_views };
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
};

struct View::Impl {
    OwnPtr<WebView::PhotonHeadlessWebView> view;
    int last_viewport_width { 0 };
    int last_viewport_height { 0 };
    double last_device_pixel_ratio { 0 };
};

Runtime::Runtime(std::unique_ptr<Impl> impl)
    : m_impl(move(impl))
{
}

std::unique_ptr<Runtime> Runtime::create(std::string const& helper_directory, std::string& error)
{
    auto impl = std::make_unique<Impl>();
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
            impl->diagnostic_pump_timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + static_cast<double>(delay_ms) / 1000.0, 0, 0, 0,
                [](CFRunLoopTimerRef, void*) {
                    dbgln("[DiagnosticPump] event=BEGIN at_ns={} one_shot=true", MonotonicTime::now().nanoseconds());
                    auto processed = Core::EventLoop::current().pump(Core::EventLoop::WaitMode::PollForEvents);
                    dbgln("[DiagnosticPump] event=END at_ns={} processed={}", MonotonicTime::now().nanoseconds(), processed);
                },
                &context);
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

std::unique_ptr<View> Runtime::create_view(int width, int height, double dpr, ViewCallbacks callbacks)
{
    auto impl = std::make_unique<View::Impl>();
    impl->view = WebView::PhotonHeadlessWebView::create(width, height, dpr, move(callbacks));
    if (!impl->view)
        return {};
    impl->last_viewport_width = max(1, static_cast<int>(std::lround(width * dpr)));
    impl->last_viewport_height = max(1, static_cast<int>(std::lround(height * dpr)));
    impl->last_device_pixel_ratio = dpr;
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
void View::resize(int width, int height, double dpr)
{
    auto physical_width = max(1, static_cast<int>(std::lround(width * dpr)));
    auto physical_height = max(1, static_cast<int>(std::lround(height * dpr)));
    if (m_impl->last_viewport_width == physical_width && m_impl->last_viewport_height == physical_height && m_impl->last_device_pixel_ratio == dpr)
        return;
    m_impl->last_viewport_width = physical_width;
    m_impl->last_viewport_height = physical_height;
    m_impl->last_device_pixel_ratio = dpr;
    m_impl->view->resize(width, height, dpr);
}
#if defined(AK_OS_MACOS)
void View::release_native_frame(uint64_t backing_id, uint64_t generation, uint64_t frame_id)
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->release_native_frame(backing_id, generation, frame_id);
}

void View::set_native_metal_presentation(bool enabled)
{
    VERIFY(m_impl && m_impl->view);
    m_impl->view->set_native_metal_presentation(enabled);
}
#endif
void View::set_focus(bool focused) { m_impl->view->set_has_system_focus(focused); }

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
    case PointerButton::None: return Web::UIEvents::MouseButton::None;
    case PointerButton::Primary: return Web::UIEvents::MouseButton::Primary;
    case PointerButton::Secondary: return Web::UIEvents::MouseButton::Secondary;
    case PointerButton::Middle: return Web::UIEvents::MouseButton::Middle;
    case PointerButton::Back: return Web::UIEvents::MouseButton::Backward;
    case PointerButton::Forward: return Web::UIEvents::MouseButton::Forward;
    }
    return Web::UIEvents::MouseButton::None;
}

void View::send_pointer_event(PointerEvent const& event)
{
    auto type = Web::MouseEvent::Type::MouseMove;
    switch (event.type) {
    case PointerType::Move: type = Web::MouseEvent::Type::MouseMove; break;
    case PointerType::Leave: type = Web::MouseEvent::Type::MouseLeave; break;
    case PointerType::Press: type = Web::MouseEvent::Type::MouseDown; break;
    case PointerType::Release: type = Web::MouseEvent::Type::MouseUp; break;
    case PointerType::Wheel: type = Web::MouseEvent::Type::MouseWheel; break;
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
        .id = 0,
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

void View::shutdown()
{
    if (!m_impl || !m_impl->view)
        return;
    m_impl->view->clear_callbacks();
    m_impl->view = nullptr;
}

}
