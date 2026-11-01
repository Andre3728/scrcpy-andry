#ifndef SC_GUI_H
#define SC_GUI_H

// scrcpy-gui control panel, drawn with Dear ImGui into the scrcpy window.
//
// The GUI is owned by the GUI app (gui_app.c) and outlives mirroring sessions.
// It draws the window "chrome" (sidebar on the left, toolbar under the
// mirror). The owner asks for the insets to lay out the video, forwards every
// SDL event first, and calls sc_gui_render() between drawing the video and
// presenting.
//
// Callbacks are never called from sc_gui_render(): user actions are queued
// and dispatched from the next sc_gui_handle_event() call, so that callbacks
// may safely resize the window or render.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sc_gui;

// Space reserved around the mirror, in window coordinates
struct sc_gui_insets {
    float left;
    float top;
    float right;
    float bottom;
};

enum sc_gui_action {
    SC_GUI_ACTION_BACK,
    SC_GUI_ACTION_HOME,
    SC_GUI_ACTION_APP_SWITCH,
    SC_GUI_ACTION_VOLUME_DOWN,
    SC_GUI_ACTION_VOLUME_UP,
    SC_GUI_ACTION_POWER,
    SC_GUI_ACTION_ROTATE_DEVICE,
    SC_GUI_ACTION_EXPAND_NOTIFICATION_PANEL,
    SC_GUI_ACTION_EXPAND_SETTINGS_PANEL,
    SC_GUI_ACTION_COLLAPSE_PANELS,
    SC_GUI_ACTION_SCREEN_OFF,
    SC_GUI_ACTION_SCREEN_ON,
};

enum sc_gui_codec {
    SC_GUI_CODEC_H264,
    SC_GUI_CODEC_H265,
    SC_GUI_CODEC_AV1,
};

// Mirroring settings, per device (or defaults)
struct sc_gui_profile {
    uint16_t max_size;       // 0: original resolution
    uint32_t video_bit_rate; // bps, 0: default
    uint16_t max_fps;        // 0: unlimited
    enum sc_gui_codec video_codec;
    bool audio;
    bool stay_awake;
    bool turn_screen_off;    // applied live
    bool show_touches;
    char extra_args[256];    // additional scrcpy arguments
};

enum sc_gui_page {
    SC_GUI_PAGE_DEVICES,
    SC_GUI_PAGE_FILES,
    SC_GUI_PAGE_APPS,
    SC_GUI_PAGE_SETTINGS,
};

enum sc_gui_appearance {
    SC_GUI_APPEARANCE_SYSTEM,
    SC_GUI_APPEARANCE_LIGHT,
    SC_GUI_APPEARANCE_DARK,
};

// Application preferences
struct sc_gui_prefs {
    enum sc_gui_appearance appearance;
    bool sidebar_visible;
    enum sc_gui_page page; // last page opened
    char adb_path[512]; // empty: default
};

enum sc_gui_transfer_type {
    SC_GUI_TRANSFER_INSTALL, // adb install
    SC_GUI_TRANSFER_PUSH,    // adb push
    SC_GUI_TRANSFER_PULL,    // adb pull
};

enum sc_gui_file_type {
    SC_GUI_FILE_REGULAR,
    SC_GUI_FILE_DIRECTORY,
    SC_GUI_FILE_LINK,
    SC_GUI_FILE_OTHER,
};

// A file on the device, for the file manager
struct sc_gui_file {
    const char *name;
    const char *mtime; // "YYYY-MM-DD HH:MM"
    uint64_t size;
    enum sc_gui_file_type type;
};

enum sc_gui_transfer_state {
    SC_GUI_TRANSFER_PENDING,
    SC_GUI_TRANSFER_RUNNING,
    SC_GUI_TRANSFER_DONE,
    SC_GUI_TRANSFER_FAILED,
    SC_GUI_TRANSFER_CANCELED,
};

struct sc_gui_callbacks {
    // A toolbar/menu action was triggered (only while mirroring)
    void (*on_action)(enum sc_gui_action action, void *userdata);
    // The insets changed (e.g. the sidebar was toggled)
    void (*on_insets_changed)(void *userdata);

    // Device management
    void (*on_start_session)(const char *serial, void *userdata);
    void (*on_stop_session)(void *userdata);
    void (*on_switch_to_wifi)(const char *serial, void *userdata);
    void (*on_pair)(const char *address, const char *code, void *userdata);
    void (*on_connect)(const char *address, void *userdata);
    void (*on_disconnect)(const char *address, void *userdata);

    // Settings. restart is true if the session must be restarted to apply
    // the profile.
    void (*on_profile_changed)(const struct sc_gui_profile *profile,
                               bool restart, void *userdata);
    void (*on_prefs_changed)(const struct sc_gui_prefs *prefs,
                             void *userdata);

    // Files were dropped while mirroring. remote_dir is the destination
    // folder on the device, or NULL for the default behavior (install APKs,
    // copy the other files to /sdcard/Download/).
    void (*on_drop_files)(const char *const *paths, size_t count,
                          const char *remote_dir, void *userdata);
    void (*on_cancel_transfer)(uint32_t id, void *userdata);

    // File manager (paths on the device; directories end with '/')
    void (*on_list_dir)(const char *path, void *userdata);
    void (*on_delete_files)(const char *const *paths, size_t count,
                            void *userdata);
    void (*on_create_dir)(const char *path, void *userdata);
    // Download files to the computer, in the Downloads folder, or in a
    // folder chosen by the user
    void (*on_pull_files)(const char *const *paths, size_t count,
                          bool choose_folder, void *userdata);
};

struct sc_gui_params {
    SDL_Window *window;
    SDL_Renderer *renderer;
    // SDL modifiers of the scrcpy shortcut key (always forwarded)
    uint16_t sdl_shortcut_mods;
    // SDL event type pushed when the GUI needs another frame (animations,
    // hover state settling, pending callbacks); the owner must forward it to
    // sc_gui_handle_event() and render
    uint32_t redraw_event_type;
    const struct sc_gui_callbacks *cbs;
    void *cbs_userdata;
};

// A device as listed by "adb devices -l"
struct sc_gui_device {
    const char *serial;
    const char *model; // may be NULL
    const char *state; // "device", "unauthorized", "offline"...
    bool tcpip; // connected over TCP/IP
};

enum sc_gui_session_state {
    SC_GUI_SESSION_NONE,
    SC_GUI_SESSION_CONNECTING,
    SC_GUI_SESSION_RUNNING,
};

enum sc_gui_toast_level {
    SC_GUI_TOAST_INFO,
    SC_GUI_TOAST_SUCCESS,
    SC_GUI_TOAST_ERROR,
};

struct sc_gui *
sc_gui_create(const struct sc_gui_params *params);

void
sc_gui_destroy(struct sc_gui *gui);

// Process an event.
//
// mirror_rect is the rectangle where the video is drawn, in window
// coordinates (NULL if there is no video).
//
// Return true if the event was consumed by the GUI and must not be forwarded
// to the device.
bool
sc_gui_handle_event(struct sc_gui *gui, const SDL_Event *event,
                    const SDL_FRect *mirror_rect);

// Return true (and reset the flag) if the GUI needs the window to be redrawn
bool
sc_gui_take_redraw_request(struct sc_gui *gui);

struct sc_gui_insets
sc_gui_get_insets(struct sc_gui *gui);

// Minimum size of the video area so that the GUI fits (window coordinates)
void
sc_gui_get_minimum_area(struct sc_gui *gui, float *width, float *height);

// Default size of the video area when there is no video (window coordinates)
void
sc_gui_get_default_area(struct sc_gui *gui, float *width, float *height);

// Background color to clear the window with (0xRRGGBB)
uint32_t
sc_gui_get_background_color(struct sc_gui *gui);

// Draw the GUI on top of the current render target (does not present)
void
sc_gui_render(struct sc_gui *gui, const SDL_FRect *mirror_rect);

// Replace the list of devices (the data is copied)
void
sc_gui_set_devices(struct sc_gui *gui, const struct sc_gui_device *devices,
                   size_t count);

// Set the current session state (serial and device_name may be NULL)
void
sc_gui_set_session(struct sc_gui *gui, enum sc_gui_session_state state,
                   const char *serial, const char *device_name, bool control);

// Show a transient notification
void
sc_gui_show_toast(struct sc_gui *gui, enum sc_gui_toast_level level,
                  const char *text);

// Set the profile shown in Settings, with a title such as the device name
// (the data is copied)
void
sc_gui_set_profile(struct sc_gui *gui, const struct sc_gui_profile *profile,
                   const char *title);

// Set the application preferences (the data is copied)
void
sc_gui_set_prefs(struct sc_gui *gui, const struct sc_gui_prefs *prefs);

// Add or update a transfer (install or copy) shown in the Transfers popover
void
sc_gui_update_transfer(struct sc_gui *gui, uint32_t id,
                       enum sc_gui_transfer_type type, const char *name,
                       enum sc_gui_transfer_state state, const char *detail);

// Set the content of a directory requested by on_list_dir() (the data is
// copied). error is NULL on success.
void
sc_gui_set_listing(struct sc_gui *gui, const char *path,
                   const struct sc_gui_file *files, size_t count,
                   const char *error);

// The files on the device changed (delete, new folder): reload the listing
void
sc_gui_files_changed(struct sc_gui *gui);

// Indicate that a background device task (Wi-Fi switch, pairing...) is running
void
sc_gui_set_task_busy(struct sc_gui *gui, bool busy);

#ifdef __cplusplus
}
#endif

#endif
