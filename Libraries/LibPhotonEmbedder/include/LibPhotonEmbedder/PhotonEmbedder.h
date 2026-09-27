/*
 * Copyright (c) 2026, PhotonBrowser contributors
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibPhotonEmbedder/Export.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Photon {

struct PresentedFrame {
    int width { 0 };
    int height { 0 };
    size_t stride { 0 };
    double device_pixel_ratio { 1.0 };
    uint64_t copy_time_microseconds { 0 };
    // BGRA8888, premultiplied alpha. Pixels are owned by this value.
    std::vector<uint8_t> pixels;
};

struct ViewState {
    std::string url;
    std::string title;
    bool loading { false };
    bool can_go_back { false };
    bool can_go_forward { false };
};

struct ViewCallbacks {
    std::function<void(ViewState const&)> state_changed;
    std::function<void(std::shared_ptr<PresentedFrame const>)> frame_ready;
    std::function<void(std::string const&)> failed;
};

enum class PointerType { Move, Leave, Press, Release, Wheel };
enum class PointerButton : uint8_t { None = 0, Primary = 1, Secondary = 2, Middle = 4, Back = 8, Forward = 16 };
enum class ScrollPhase { None, Ongoing, Momentum, Ended };
struct PointerEvent {
    PointerType type { PointerType::Move };
    double x { 0 };
    double y { 0 };
    double screen_x { 0 };
    double screen_y { 0 };
    PointerButton button { PointerButton::None };
    uint8_t buttons { 0 };
    bool shift { false };
    bool control { false };
    bool alt { false };
    bool meta { false };
    double wheel_x { 0 };
    double wheel_y { 0 };
    bool precise_wheel { false };
    ScrollPhase scroll_phase { ScrollPhase::None };
    int click_count { 0 };
};

enum class Key : uint16_t {
    Unknown = 0, Backspace = 0x08, Tab = 0x09, Enter = 0x0D,
    Escape = 0x1B, Space = 0x20, PageUp = 0x21, PageDown = 0x22,
    End = 0x23, Home = 0x24, Left = 0x25, Up = 0x26, Right = 0x27,
    Down = 0x28, Delete = 0x2E,
    Digit0 = '0', Digit1 = '1', Digit2 = '2', Digit3 = '3', Digit4 = '4',
    Digit5 = '5', Digit6 = '6', Digit7 = '7', Digit8 = '8', Digit9 = '9',
    A = 'A', B = 'B', C = 'C', D = 'D', E = 'E', F = 'F', G = 'G', H = 'H',
    I = 'I', J = 'J', K = 'K', L = 'L', M = 'M', N = 'N', O = 'O', P = 'P',
    Q = 'Q', R = 'R', S = 'S', T = 'T', U = 'U', V = 'V', W = 'W', X = 'X',
    Y = 'Y', Z = 'Z', F1 = 0x70, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12
};

class View;

class PHOTONEMBEDDER_API Runtime {
public:
    static std::unique_ptr<Runtime> create(std::string const& helper_directory, std::string& error);
    ~Runtime();
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;
    Runtime(Runtime const&) = delete;
    Runtime& operator=(Runtime const&) = delete;

    // Call regularly on the thread that created the runtime. Engine callbacks
    // and ViewCallbacks are delivered synchronously from this call.
    void pump();
    std::unique_ptr<View> create_view(int width, int height, double device_pixel_ratio, ViewCallbacks);

private:
    struct Impl;
    explicit Runtime(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> m_impl;
};

class PHOTONEMBEDDER_API View {
public:
    ~View();
    View(View&&) noexcept;
    View& operator=(View&&) noexcept;
    View(View const&) = delete;
    View& operator=(View const&) = delete;

    void navigate(std::string const&);
    void reload();
    void go_back();
    void go_forward();
    void resize(int logical_width, int logical_height, double device_pixel_ratio);
    void set_focus(bool);
    void send_pointer_event(PointerEvent const&);
    void send_key_event(Key, bool pressed, uint32_t code_point, bool shift, bool control, bool alt, bool meta, bool repeat, bool insert_text);
    void shutdown();

private:
    friend class Runtime;
    struct Impl;
    explicit View(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> m_impl;
};

}
