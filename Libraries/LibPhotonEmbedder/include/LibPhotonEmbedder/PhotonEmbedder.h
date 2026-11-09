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

// A server asked the user to sign in with HTTP authentication. The embedder
// answers with View::http_credentials_provided, also when cancelled.
struct HTTPCredentialsRequest {
    uint64_t request_id { 0 };
    std::string url;
    std::string origin;
    // The server's name for the protected area, which may be empty.
    std::string realm;
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

// Opens links the browser cannot load itself, such as mailto: and tel:, in
// other apps. Without it, such links do nothing.
struct ExternalApps {
    // The name of the app that opens `url`, or nothing when no app does.
    std::function<std::optional<std::string>(std::string const& url)> app_for_url;
    // Opens `url` in that app, returning whether it could.
    std::function<bool(std::string const& url)> open_url;
};

// The certificate a host presented, as a certificate viewer shows it.
struct CertificateInfo {
    std::string subject_common_name;
    std::string subject_organization;
    std::string issuer_common_name;
    std::string issuer_organization;
    std::string valid_from;
    std::string valid_until;
    std::string sha256_fingerprint;
    // False once the user chose to proceed past a certificate error.
    bool verified { false };
    std::string protocol;
    std::string cipher;
    // Who each certificate the server sent is issued to, from the site's own towards its root.
    std::vector<std::string> chain;
};

// Website data to delete. Data last used before `since_unix_seconds` is kept.
struct ClearBrowsingData {
    int64_t since_unix_seconds { 0 };
    // The network cache.
    bool cache { false };
    // Cookies and web storage such as localStorage.
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

// The focused editable's state, for platform input methods (IME). Offsets
// count UTF-16 code units from the start of the editable's text; the
// surrounding text is a window around the cursor that starts at
// `cursor_position - text_before_cursor`'s UTF-16 length.
struct InputMethodState {
    bool enabled { false };
    int32_t cursor_position { 0 };
    int32_t anchor_position { 0 };
    std::string text_before_cursor;
    std::string text_after_cursor;
    // The caret in logical pixels from the view's top-left corner.
    bool has_caret_rect { false };
    double caret_x { 0 };
    double caret_y { 0 };
    double caret_width { 0 };
    double caret_height { 0 };
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
    // A file input asked for files. `accept` lists the kinds it takes, each a filename extension with its leading dot
    // (".pdf"), a MIME type ("image/png") or a MIME type family ("image/*"); empty takes any file. The embedder answers
    // with View::file_picker_closed, also when cancelled.
    std::function<void(bool allow_multiple, std::vector<std::string> const& accept)> file_picker_requested;
    std::function<void(PermissionRequest const&)> permission_requested;
    std::function<void(HTTPCredentialsRequest const&)> http_credentials_requested;
    // A download took over the view's first navigation, so it shows only its initial blank page.
    std::function<void()> download_replaced_initial_document;
    // The page failed to load because the certificate of `host` could not be verified.
    std::function<void(std::string const& url, std::string const& host)> certificate_verification_failed;
    // The request was denied by Engine because its document was replaced.
    std::function<void(uint64_t request_id, std::string const& document_id)> permission_cancelled;
    // A context menu action opened a link in a new tab.
    std::function<void(std::string const& url, bool activate)> open_in_new_tab_requested;
    // A link is to open in a new window, private or not.
    std::function<void(std::string const& url, bool is_private)> open_in_new_window_requested;
    // A page asks to open `url` in `app_name`; answer with View::external_url_answered, also when cancelled.
    std::function<void(uint64_t request_id, std::string const& url, std::string const& app_name)> external_url_requested;
    // Saving a link or image asks where to; answer with View::download_path_chosen, also when cancelled.
    std::function<void(uint64_t request_id, std::string const& suggested_name)> download_path_requested;
    // A link was shift-clicked, to be shown in a preview over the page.
    std::function<void(std::string const& url)> peek_requested;
    // The pointer moved onto a link, or off every link with an empty URL.
    std::function<void(std::string const& url)> link_hovered;
    // The focused editable or its caret changed, for platform input methods.
    std::function<void(InputMethodState const&)> input_method_state_changed;
    // The page entered or left element fullscreen. The embedder fills its
    // window with the page and reports the result with View::set_fullscreen.
    std::function<void(bool fullscreen)> fullscreen_requested;
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

enum class DownloadStatus : uint8_t { InProgress,
    Paused,
    Completed,
    Canceled,
    Failed };

// A file the Engine saves for a page, in the profile's download list.
struct Download {
    uint64_t id { 0 };
    std::string profile_identity;
    std::string url;
    std::string path;
    DownloadStatus status { DownloadStatus::InProgress };
    uint64_t received_bytes { 0 };
    std::optional<uint64_t> total_bytes;
    std::optional<uint64_t> bytes_per_second;
    std::string error;
    int64_t created_time_ms { 0 };
    // Whether a private view started it; it leaves the list with the private session.
    bool is_private { false };
};

class PHOTONEMBEDDER_API Runtime {
public:
    // Keeps cookies, site storage and the cache in `profile_path`. An empty
    // path uses a temporary profile that is removed on a clean shutdown.
    static std::unique_ptr<Runtime> create(std::string const& helper_directory, std::string const& profile_path, std::string const& default_browsing_profile_path, std::string& error);
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
    // Calls `changed` with every download now, then whenever one starts or
    // progresses, and `removed` when one leaves the list. Private downloads
    // are marked, and leave the list when the private session ends.
    void set_download_callbacks(std::function<void(Download const&)> changed, std::function<void(uint64_t id)> removed);
    void pause_download(uint64_t id);
    void resume_download(uint64_t id);
    void cancel_download(uint64_t id);
    // Removes finished, cancelled and failed downloads from the list.
    void clear_inactive_downloads(std::string const& profile_path);
    void ensure_profile_session(std::string const& profile_path);
    // Use the host's system clipboard for copying and pasting.
    void set_clipboard(Clipboard);
    void set_external_apps(ExternalApps);
    // Deletes website data, then calls `done`.
    void clear_browsing_data(ClearBrowsingData const&, std::string const& profile_path, std::function<void()> done);
    // Deletes the cookies and web storage of one site in the default profile: cookies for `host`, its subdomains and
    // the domains it shares cookies with, and the web storage of every origin on `host` or its subdomains, whatever
    // its scheme or port. IndexedDB keeps nothing past its page, so there is none to delete.
    void clear_site_data(std::string const& host, std::string const& profile_path, std::function<void()> done);
    // Lets pages in this profile load `host` despite certificate errors until the browser quits.
    void allow_certificate_errors_for_host(std::string const& profile_path, bool is_private, std::string const& host);
    // Calls `done` with this profile's certificate for `host`, or nothing if it has not connected to it.
    void peer_certificate(std::string const& profile_path, bool is_private, std::string const& host, std::function<void(std::optional<CertificateInfo> const&)> done);
    // A private view keeps nothing once it and the other private views close.
    std::unique_ptr<View> create_view(int width, int height, double device_pixel_ratio, ViewCallbacks, std::string const& profile_path, bool is_private = false);
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
    // Platform input method composition, as Qt's inputMethodEvent drives it.
    // Marked text replaces the previous marked text; an empty string ends the
    // composition. Committed text replaces the marked text, or with a
    // replacement range relative to the cursor, that range.
    void set_marked_text(std::string const& text);
    void commit_text(std::string const& text, int32_t replacement_start, int32_t replacement_length);
    void unmark_text();
    void set_preferred_color_scheme(PreferredColorScheme);
    void set_force_dark_mode_enabled(bool enabled);
    // Tells the page whether the embedder now shows it fullscreen.
    void set_fullscreen(bool fullscreen);
    // Asks the page to leave element fullscreen, as when the user presses
    // Escape. The page answers through fullscreen_requested.
    void exit_fullscreen();
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
    // Answers an HTTP sign-in request; `signed_in` is false when the user cancelled.
    void http_credentials_provided(uint64_t request_id, bool signed_in, std::string const& username, std::string const& password);
    // Answers external_url_requested.
    void external_url_answered(uint64_t request_id, bool open);
    // Answers download_path_requested; an empty path cancels.
    void download_path_chosen(uint64_t request_id, std::string const& path);
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
