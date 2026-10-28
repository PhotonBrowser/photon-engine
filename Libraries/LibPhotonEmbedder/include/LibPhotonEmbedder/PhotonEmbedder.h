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
    uint64_t buffer_allocation_microseconds { 0 };
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
    // Present only for HTTP(S) documents; true means the committed page uses HTTPS.
    std::optional<bool> connection_secure;
};

enum class NavigationTimingEvent : int {
    Started = 0,
    DocumentCommitted = 1,
    LoadFinished = 2,
    FirstFrameReady = 3,
};

// A page's icon. Pixels are BGRA8888 with straight (unpremultiplied) alpha,
// tightly packed rows, and owned by this value.
struct Favicon {
    int width { 0 };
    int height { 0 };
    std::vector<uint8_t> pixels;
};

// An Engine service process that is restarted when it stops.
enum class EngineService : uint8_t { Compositor,
    Network };

enum class DialogType : uint8_t { Alert,
    Confirm,
    Prompt };

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

// One entry in a <select> element's dropdown: an option to choose, the
// label of a group of options, or a separator.
struct SelectDropdownItem {
    enum class Kind {
        Option,
        GroupLabel,
        Separator,
    };
    Kind kind { Kind::Option };
    std::string label;
    // The option's id, to answer View::select_dropdown_closed with.
    uint32_t id { 0 };
    bool selected { false };
    bool enabled { true };
};

// A <select> element asked to show its options, below the element at a
// point in the view and at least as wide as it, in logical pixels. The
// embedder answers with View::select_dropdown_closed, also when dismissed.
struct SelectDropdownRequest {
    double x { 0 };
    double y { 0 };
    double minimum_width { 0 };
    std::vector<SelectDropdownItem> items;
};

// A permission request from one specific environment. Origins and the opaque
// document identity are supplied by Engine state, never by page script.
struct PermissionRequest {
    uint64_t request_id { 0 };
    std::string document_id;
    std::string requesting_origin;
    std::string top_level_origin;
    std::string capability;
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
    Arrow,
    Hidden,
    Crosshair,
    IBeam,
    ResizeHorizontal,
    ResizeVertical,
    ResizeDiagonalTLBR,
    ResizeDiagonalBLTR,
    ResizeColumn,
    ResizeRow,
    Hand,
    Help,
    OpenHand,
    Drag,
    DragCopy,
    Move,
    Wait,
    Disallowed,
};

struct ViewCallbacks {
    std::function<void(ViewState const&)> state_changed;
    std::function<void(std::shared_ptr<PresentedFrame const>)> frame_ready;
    // Opt-in navigation diagnostics. Response receipt and DOM milestones are
    // not currently exposed at the embedder boundary.
    std::function<void(NavigationTimingEvent, std::string const&)> navigation_timing;
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
    // A precise trackpad wheel delta reached the root viewport's horizontal edge and can traverse tab history.
    std::function<void(uint64_t gesture_id, bool back, float unconsumed_delta)> history_swipe_candidate;
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
    // The page's background colour changed: its canvas background, or the
    // system canvas colour before the page sets one. The embedder shows it
    // where the page has not painted, as while it lays out at a new size.
    std::function<void(uint8_t red, uint8_t green, uint8_t blue)> page_background_changed;
    // A user input has gone unanswered by the page for several seconds.
    std::function<void(bool unresponsive)> page_unresponsive_changed;
    std::function<void(ContextMenuRequest const&)> context_menu_requested;
    std::function<void(SelectDropdownRequest const&)> select_dropdown_requested;
    // A file input asked for files. The embedder answers with
    // View::file_picker_closed, also when cancelled.
    std::function<void(bool allow_multiple)> file_picker_requested;
    std::function<void(PermissionRequest const&)> permission_requested;
    // The request was denied by Engine because its document was replaced.
    std::function<void(uint64_t request_id, std::string const& document_id)> permission_cancelled;
    // A context menu action opened a link in a new tab.
    std::function<void(std::string const& url, bool activate)> open_in_new_tab_requested;
    // A link was shift-clicked, to be shown in a preview over the page.
    std::function<void(std::string const& url)> peek_requested;
};

enum class PointerType { Move,
    Leave,
    Press,
    Release,
    Wheel };
enum class PointerButton : uint8_t { None = 0,
    Primary = 1,
    Secondary = 2,
    Middle = 4,
    Back = 8,
    Forward = 16 };
enum class ScrollPhase { None,
    Ongoing,
    Momentum,
    Ended };
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
    bool history_swipe_back_enabled { false };
    bool history_swipe_forward_enabled { false };
    uint64_t history_swipe_gesture_id { 0 };
};

enum class Key : uint16_t {
    Unknown = 0,
    Backspace = 0x08,
    Tab = 0x09,
    Enter = 0x0D,
    Escape = 0x1B,
    Space = 0x20,
    PageUp = 0x21,
    PageDown = 0x22,
    End = 0x23,
    Home = 0x24,
    Left = 0x25,
    Up = 0x26,
    Right = 0x27,
    Down = 0x28,
    Delete = 0x2E,
    Digit0 = '0',
    Digit1 = '1',
    Digit2 = '2',
    Digit3 = '3',
    Digit4 = '4',
    Digit5 = '5',
    Digit6 = '6',
    Digit7 = '7',
    Digit8 = '8',
    Digit9 = '9',
    A = 'A',
    B = 'B',
    C = 'C',
    D = 'D',
    E = 'E',
    F = 'F',
    G = 'G',
    H = 'H',
    I = 'I',
    J = 'J',
    K = 'K',
    L = 'L',
    M = 'M',
    N = 'N',
    O = 'O',
    P = 'P',
    Q = 'Q',
    R = 'R',
    S = 'S',
    T = 'T',
    U = 'U',
    V = 'V',
    W = 'W',
    X = 'X',
    Y = 'Y',
    Z = 'Z',
    F1 = 0x70,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12
};

enum class PreferredColorScheme : uint8_t { Auto,
    Dark,
    Light };

class View;
namespace Automation {

class View;

}

struct RegisteredEngineProcess {
    int32_t pid { 0 };
    std::string role;
    std::optional<uint64_t> start_time_in_microseconds;
    std::optional<int32_t> parent_pid;
    std::optional<uint64_t> parent_start_time_in_microseconds;
};

struct EngineProcessSnapshot {
    uint64_t child_id { 0 };
    int32_t pid { 0 };
    std::string role;
    std::optional<std::string> title;
    std::optional<uint64_t> owner_child_id;
    std::optional<double> cpu_percent;
    std::optional<uint64_t> memory_bytes;
    std::optional<uint64_t> disk_read_bytes;
    std::optional<uint64_t> disk_write_bytes;
    std::optional<uint64_t> network_download_bytes_per_second;
    std::optional<uint64_t> network_upload_bytes_per_second;
    std::optional<std::string> page_url;
    bool page_is_frame { false };
};

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
    // Avoid launching a spare WebContent process while the host reports
    // memory pressure. Call again when pressure subsides.
    void set_memory_pressure(bool pressured);
    // Enables the platform's elastic scroll effect when supported.
    void set_elastic_scroll_enabled(bool enabled);
    // Called with `restarted` false when a service process stops and true once
    // its replacement is running. The network service reports only its restart.
    void set_service_callback(std::function<void(EngineService, bool restarted)>);
    // Returns a snapshot of active processes registered by WebView's process
    // manager. The scope is limited to that manager's process records.
    std::vector<RegisteredEngineProcess> registered_engine_processes() const;
    // Returns resource metrics and the representative page for every process
    // registered by WebView's process manager.
    std::vector<EngineProcessSnapshot> process_snapshots() const;
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
    // Allows or blocks the microphone for the origin of `origin`, a URL, as the
    // site's permission rather than an answer to one request. Blocking ends
    // capture the origin has running.
    void set_microphone_permission(std::string const& origin, bool granted);
    void resize(int logical_width, int logical_height, double device_pixel_ratio);
    void set_visible(bool visible);
    void set_performance_monitor_enabled(bool enabled);
    void set_residency_monitor_enabled(bool enabled);
    bool set_task_queue_diagnostics_enabled(bool enabled);
    size_t copy_task_queue_diagnostics(char* output, size_t capacity) const;
    void release_hidden_memory();
    // Whether closing this view would currently require a beforeunload check.
    bool needs_beforeunload_check() const;
    // Tell the engine which display shows this view and how often it refreshes.
    // Pace page rendering and compositor vsync from them. Call again when the
    // view moves to another display or the refresh rate changes. A display_id
    // of 0 means unknown, which falls back to a timer at refresh_rate.
    void set_display_metadata(uint64_t display_id, double refresh_rate);
#if defined(__APPLE__)
    void release_native_frame(uint64_t backing_id, uint64_t generation, uint64_t frame_id, bool consumer_gpu_work_complete);
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
    // Answer the open <select> dropdown with the chosen option's id, or
    // nothing when it was dismissed.
    void select_dropdown_closed(std::optional<uint32_t> selected_id);
    // Answer the open file picker with the chosen files' paths, empty when
    // cancelled.
    void file_picker_closed(std::vector<std::string> const& paths);
    // Resolves a pending permission request. The document identity must match
    // the request, and the Engine rejects replies after that document changes.
    void permission_request_completed(uint64_t request_id, std::string const& document_id, bool granted);
    // Answer the open dialog. A prompt's response is empty when cancelled.
    void alert_closed();
    void confirm_closed(bool accepted);
    void prompt_closed(std::optional<std::string> const& response);
    void shutdown();

private:
    friend class Runtime;
    friend class Automation::View;
    struct Impl;
    explicit View(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> m_impl;
};

}
