/*
 * Copyright (c) 2026, PhotonBrowser contributors
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/PixelUnits.h>
#include <LibCompositing/KeyCode.h>
#include <LibCompositing/MouseButton.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/Menu.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/ViewImplementation.h>
#include <LibPhotonEmbedder/PhotonEmbedder.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace WebView {

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
        view->initialize_client(CreateNewClient::Yes);
        view->notify_state();
        return view;
    }

    void resize(int width, int height, double dpr)
    {
        m_device_pixel_ratio = dpr;
        reset_viewport_size({ max(1, static_cast<int>(std::lround(width * dpr))), max(1, static_cast<int>(std::lround(height * dpr))) });
    }

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

private:
    PhotonHeadlessWebView(Core::AnonymousBuffer theme, Compositing::DevicePixelSize size, double dpr, Photon::ViewCallbacks callbacks)
        : HeadlessWebView(move(theme), size)
        , m_callbacks(move(callbacks))
    {
        m_device_pixel_ratio = dpr;
        on_url_change = [this](URL::URL const&) { notify_state(); };
        on_title_change = [this](Utf16String const&) { notify_state(); };
        on_loading_state_change = [this](bool) { notify_state(); };
        on_browser_history_traversal_complete = [this] { notify_state(); };
        on_web_content_crashed = [this](auto) {
            if (m_callbacks.failed)
                m_callbacks.failed("WebContent process crashed");
        };
        on_ready_to_paint = [this] {
            auto const& front = m_client_state.front_bitmap;
            if (!front.shared_image_buffer || !m_callbacks.frame_ready) {
                dbgln("Photon embedder: paint callback without a shared image buffer");
                return;
            }
            auto bitmap = front.shared_image_buffer->bitmap_if_present();
            if (!bitmap) {
                dbgln("Photon embedder: shared image has no CPU bitmap");
                return;
            }
            auto frame = std::make_shared<Photon::PresentedFrame>();
            frame->width = bitmap->width();
            frame->height = bitmap->height();
            frame->stride = bitmap->pitch();
            frame->device_pixel_ratio = m_device_pixel_ratio;
            auto copy_started = std::chrono::steady_clock::now();
            frame->pixels.resize(bitmap->data_size());
            for (int row = 0; row < frame->height; ++row)
                std::copy_n(bitmap->scanline_u8(row), frame->stride, frame->pixels.data() + row * frame->stride);
            frame->copy_time_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - copy_started).count();
            m_callbacks.frame_ready(move(frame));
        };
    }

    Photon::ViewCallbacks m_callbacks;
};

}

namespace Photon {

struct Runtime::Impl {
    std::string executable { "photon" };
    std::string temporary_profile { "--temporary-profile" };
    char* argv[3] { executable.data(), temporary_profile.data(), nullptr };
    AK::StringView argument_views[2] { { executable.data(), executable.size() }, { temporary_profile.data(), temporary_profile.size() } };
    Main::Arguments arguments { 2, argv, argument_views };
    OwnPtr<WebView::PhotonApplication> application;
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
    return std::unique_ptr<Runtime>(new Runtime(move(impl)));
}

Runtime::~Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

void Runtime::pump()
{
    (void)Core::EventLoop::current().pump(Core::EventLoop::WaitMode::PollForEvents);
}

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
void View::set_focus(bool focused) { m_impl->view->set_has_system_focus(focused); }

static Compositing::KeyModifier modifiers(bool shift, bool control, bool alt, bool meta)
{
    auto result = Compositing::KeyModifier::Mod_None;
    if (shift)
        result |= Compositing::KeyModifier::Mod_Shift;
    if (control)
        result |= Compositing::KeyModifier::Mod_Ctrl;
    if (alt)
        result |= Compositing::KeyModifier::Mod_Alt;
    if (meta)
        result |= Compositing::KeyModifier::Mod_Super;
    return result;
}

static Compositing::MouseButton mouse_button(PointerButton button)
{
    switch (button) {
    case PointerButton::None: return Compositing::MouseButton::None;
    case PointerButton::Primary: return Compositing::MouseButton::Primary;
    case PointerButton::Secondary: return Compositing::MouseButton::Secondary;
    case PointerButton::Middle: return Compositing::MouseButton::Middle;
    case PointerButton::Back: return Compositing::MouseButton::Backward;
    case PointerButton::Forward: return Compositing::MouseButton::Forward;
    }
    return Compositing::MouseButton::None;
}

void View::send_pointer_event(PointerEvent const& event)
{
    auto type = Compositing::MouseEvent::Type::MouseMove;
    switch (event.type) {
    case PointerType::Move: type = Compositing::MouseEvent::Type::MouseMove; break;
    case PointerType::Leave: type = Compositing::MouseEvent::Type::MouseLeave; break;
    case PointerType::Press: type = Compositing::MouseEvent::Type::MouseDown; break;
    case PointerType::Release: type = Compositing::MouseEvent::Type::MouseUp; break;
    case PointerType::Wheel: type = Compositing::MouseEvent::Type::MouseWheel; break;
    }
    auto to_device = [&](double value) { return Compositing::DevicePixels(static_cast<int>(std::lround(value * m_impl->view->device_pixel_ratio()))); };
    auto button = mouse_button(event.button);
    Compositing::MouseButton buttons = Compositing::MouseButton::None;
    for (auto candidate : { PointerButton::Primary, PointerButton::Secondary, PointerButton::Middle, PointerButton::Back, PointerButton::Forward }) {
        if ((event.buttons & static_cast<uint8_t>(candidate)) != 0)
            buttons |= mouse_button(candidate);
    }
    auto precision = event.precise_wheel ? Compositing::WheelDeltaPrecision::Precise : Compositing::WheelDeltaPrecision::Discrete;
    auto phase = static_cast<Compositing::ScrollGesturePhase>(event.scroll_phase);
    Compositing::MouseEvent native_event {
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
        .scrollbar_dragged_by_compositor = {},
    };
    m_impl->view->enqueue_input_event(move(native_event));
}

static Compositing::KeyCode engine_key_code(Key key, uint32_t code_point)
{
    if (key == Key::Unknown)
        return Compositing::code_point_to_key_code(code_point);
    // Photon::Key deliberately uses the browser virtual-key values documented in
    // this API; convert at the boundary into the engine's distinct enum type.
    return static_cast<Compositing::KeyCode>(static_cast<uint16_t>(key));
}

void View::send_key_event(Key key, bool pressed, uint32_t code_point, bool shift, bool control, bool alt, bool meta, bool repeat, bool insert_text)
{
    Compositing::KeyEvent event {
        .type = pressed ? Compositing::KeyEvent::Type::KeyDown : Compositing::KeyEvent::Type::KeyUp,
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

void View::shutdown()
{
    if (!m_impl || !m_impl->view)
        return;
    m_impl->view->clear_callbacks();
    m_impl->view = nullptr;
}

}
