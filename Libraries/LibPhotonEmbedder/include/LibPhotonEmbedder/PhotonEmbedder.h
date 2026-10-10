/*
 * Copyright (c) 2026, PhotonBrowser contributors
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibPhotonEmbedder/Export.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Photon {

struct PresentedFrame {
    int width { 0 };
    int height { 0 };
    size_t stride { 0 };
    double device_pixel_ratio { 1.0 };
    uint64_t copy_time_microseconds { 0 };
    uint64_t engine_paint_interval_microseconds { 0 };
    uint64_t bitmap_acquisition_microseconds { 0 };
    uint64_t paint_to_callback_microseconds { 0 };
    // BGRA8888, premultiplied alpha. Pixels are owned by this value.
    std::vector<uint8_t> pixels;
};

#if defined(__APPLE__)
using NativeReleaseDrainCallback = void (*)(void*);

// Native, copy-free presentation metadata. The IOSurface Mach send right is
// transferred to native_backing_registered and must be adopted by its caller.
struct NativeGpuBacking {
    uint64_t backing_id { 0 };
    uint64_t generation { 0 };
    uint32_t width { 0 };
    uint32_t height { 0 };
    uint32_t pixel_format { 0 };
    uint32_t iosurface_mach_port { 0 };
};

struct NativeGpuFrame {
    uint64_t backing_id { 0 };
    uint64_t generation { 0 };
    uint64_t frame_id { 0 };
    uint64_t signal_value { 0 };
    int width { 0 };
    int height { 0 };
    double device_pixel_ratio { 1.0 };
};
#endif

struct ViewState {
    std::string url;
    std::string title;
    bool loading { false };
    bool can_go_back { false };
    bool can_go_forward { false };
};

// A page's icon. Pixels are BGRA8888 with straight (unpremultiplied) alpha,
// tightly packed rows, and owned by this value.
struct Favicon {
    int width { 0 };
    int height { 0 };
    std::vector<uint8_t> pixels;
};

// An Engine service process that is restarted when it stops.
enum class EngineService : uint8_t { Compositor, Network };

enum class DialogType : uint8_t { Alert, Confirm, Prompt };

// A JavaScript alert, confirm or prompt. The page waits until the embedder
// answers with the matching View::*_closed call.
struct DialogRequest {
    DialogType type { DialogType::Alert };
    // The page's origin, or its scheme when the origin is opaque.
    std::string title;
    std::string message;
    // The prompt's initial text.
    std::string default_text;
};

// A page asked the embedder to create a top-level browsing context. The
// traversable is opaque outside LibPhotonEmbedder and is valid only during
// the callback. The callback may create a View for it synchronously and
// returns that context's browser window handle.
struct NewWebViewRequest {
    bool popup { false };
    bool activate { false };
    bool has_width { false };
    int width { 0 };
    bool has_height { false };
    int height { 0 };
    bool has_screen_x { false };
    int screen_x { 0 };
    bool has_screen_y { false };
    int screen_y { 0 };
    void* traversable { nullptr };
};

struct PerformanceStats {
    bool has_cpu_percent { false };
    double cpu_percent { 0 };
    bool has_memory_bytes { false };
    uint64_t memory_bytes { 0 };
    bool has_managed_heap_bytes { false };
    uint64_t managed_heap_bytes { 0 };
    uint64_t download_bytes_per_second { 0 };
    uint64_t upload_bytes_per_second { 0 };
    bool has_frames_per_second { false };
    double frames_per_second { 0 };
};

// One item in a page's context menu: an action, or a separator between
// groups of actions.
struct ContextMenuItem {
    bool separator { false };
    std::string text;
    bool enabled { true };
    bool checkable { false };
    bool checked { false };
};

// The page asked for a context menu at a point in the view, in logical
// pixels. The embedder shows the items and answers with
// View::activate_context_menu_item, or nothing if the menu is dismissed.
struct ContextMenuRequest {
    double x { 0 };
    double y { 0 };
    std::vector<ContextMenuItem> items;
};

// One representation of the clipboard's contents, such as "text/plain" or
// "image/png" data.
struct ClipboardEntry {
    std::string mime_type;
    std::string data;
};

// Reads and writes the host's system clipboard. Without one, the Engine
// keeps its own clipboard that other apps cannot see.
struct Clipboard {
    std::function<std::vector<ClipboardEntry>()> read;
    std::function<void(std::vector<ClipboardEntry> const&)> write;
};

// Website data to delete. Data last used before `since_unix_seconds` is kept.
struct ClearBrowsingData {
    int64_t since_unix_seconds { 0 };
    // The network cache.
    bool cache { false };
    // Cookies and site storage such as localStorage and IndexedDB.
    bool site_data { false };
};

enum class Cursor : uint8_t {
    Arrow, Hidden, Crosshair, IBeam, ResizeHorizontal, ResizeVertical,
    ResizeDiagonalTLBR, ResizeDiagonalBLTR, ResizeColumn, ResizeRow, Hand,
    Help, OpenHand, Drag, DragCopy, Move, Wait, Disallowed,
};

struct ViewCallbacks {
    std::function<void(ViewState const&)> state_changed;
    std::function<void(std::shared_ptr<PresentedFrame const>)> frame_ready;
    std::function<void(PerformanceStats const&)> performance_stats_changed;
#if defined(__APPLE__)
    // Enable only after the shell has an operational native surface consumer.
    bool native_metal_presentation { false };
    std::function<bool(NativeGpuBacking const&)> native_backing_registered;
    std::function<void(NativeGpuFrame const&)> native_frame_ready;
#endif
    std::function<void(Cursor)> cursor_changed;
    // Called with the page's new icon, or null when the page has none, such
    // as when a navigation starts.
    std::function<void(Favicon const*)> favicon_changed;
    // Called when the tab starts or stops playing audio and when its mute
    // state changes.
    std::function<void(bool playing, bool muted)> audio_state_changed;
    std::function<void(DialogRequest const&)> dialog_requested;
    // A top-level navigation committed, a load finished, or a history
    // traversal completed: the page now shown may open dialogs.
    std::function<void()> navigation_committed;
    std::function<std::string(NewWebViewRequest const&)> new_web_view_requested;
    std::function<void(std::string const&)> failed;
    std::function<void(std::string const&)> crashed;
    // The page that replaced a crashed one presented its first frame.
    std::function<void()> crash_recovered;
    // A find-in-page result: the current match's index and, once known, how
    // many matches there are (zero when the text is not found).
    std::function<void(size_t current_match_index, std::optional<size_t> total_match_count)> find_result;
    // The page's zoom level changed, as a factor (1.0 is 100%), whether by
    // the embedder or by restoring a site's saved zoom on navigation.
    std::function<void(double zoom_level)> zoom_changed;
    // A user input has gone unanswered by the page for several seconds.
    std::function<void(bool unresponsive)> page_unresponsive_changed;
    std::function<void(ContextMenuRequest const&)> context_menu_requested;
    // A context menu action opened a link in a new tab.
    std::function<void(std::string const& url, bool activate)> open_in_new_tab_requested;
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

enum class PreferredColorScheme : uint8_t { Auto, Dark, Light };

class View;

class PHOTONEMBEDDER_API Runtime {
public:
    // Keeps cookies, site storage and the cache in `profile_path`. An empty
    // path uses a temporary profile that is removed on a clean shutdown.
    static std::unique_ptr<Runtime> create(std::string const& helper_directory, std::string const& profile_path, std::string& error);
    ~Runtime();
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;
    Runtime(Runtime const&) = delete;
    Runtime& operator=(Runtime const&) = delete;

    // Call regularly on the thread that created the runtime. Engine callbacks
    // and ViewCallbacks are delivered synchronously from this call.
    void pump();
#if defined(__APPLE__)
    // Queue a callback on the run loop that created this runtime and wake it.
    // The callback runs independently of Runtime::pump().
    void set_native_release_drain_callback(void*, NativeReleaseDrainCallback);
    void schedule_native_release_drain();
#endif
    // Set the host system's reduced-motion preference used by the app's Auto
    // setting. Call again when the system accessibility setting changes.
    void set_system_reduced_motion_preference(bool reduce_motion);
    // Called with `restarted` false when a service process stops and true once
    // its replacement is running. The network service reports only its restart.
    void set_service_callback(std::function<void(EngineService, bool restarted)>);
    // Use the host's system clipboard for copying and pasting.
    void set_clipboard(Clipboard);
    // Deletes website data, then calls `done`.
    void clear_browsing_data(ClearBrowsingData const&, std::function<void()> done);
    std::unique_ptr<View> create_view(int width, int height, double device_pixel_ratio, ViewCallbacks);
    std::unique_ptr<View> create_view_for_traversable(View& opener, void* traversable, int width, int height, double device_pixel_ratio, ViewCallbacks);

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
    void stop_loading();
    void go_back();
    void go_forward();
    bool toggle_audio_mute();
    void resize(int logical_width, int logical_height, double device_pixel_ratio);
    void set_visible(bool visible);
    void set_performance_monitor_enabled(bool enabled);
    // Tell the engine which display shows this view and how often it refreshes.
    // Pace page rendering and compositor vsync from them. Call again when the
    // view moves to another display or the refresh rate changes. A display_id
    // of 0 means unknown, which falls back to a timer at refresh_rate.
    void set_display_metadata(uint64_t display_id, double refresh_rate);
#if defined(__APPLE__)
    void release_native_frame(uint64_t backing_id, uint64_t generation, uint64_t frame_id);
    void set_native_metal_presentation(bool enabled);
#endif
    void set_focus(bool);
    void notify_state();
    std::string window_handle() const;
    void send_pointer_event(PointerEvent const&);
    void send_key_event(Key, bool pressed, uint32_t code_point, bool shift, bool control, bool alt, bool meta, bool repeat, bool insert_text);
    void set_preferred_color_scheme(PreferredColorScheme);
    // Find text in the page; an empty query clears the search.
    void find_in_page(std::string const& query, bool case_sensitive, bool highlight_all_matches);
    void find_in_page_next_match();
    void find_in_page_previous_match();
    // End the search and remove its highlights.
    void find_in_page_end();
    // Zoom in or out a step, or back to 100%. Ladybird remembers each site's
    // zoom and restores it when the site is visited again.
    void zoom_in();
    void zoom_out();
    void reset_zoom();
    // Stop and restart the WebContent process currently holding unresponsive input.
    void restart_unresponsive_page();
    // Run an item of the context menu most recently requested, by its index
    // in ContextMenuRequest::items.
    void activate_context_menu_item(size_t index);
    // Answer the open dialog. A prompt's response is empty when cancelled.
    void alert_closed();
    void confirm_closed(bool accepted);
    void prompt_closed(std::optional<std::string> const& response);
    void shutdown();

private:
    friend class Runtime;
    struct Impl;
    explicit View(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> m_impl;
};

}
