#ifndef SC_GUI_H
#define SC_GUI_H

// scrcpy-gui control panel, drawn with Dear ImGui into the scrcpy window.
//
// The GUI owns the window "chrome" (sidebar on the left, toolbar under the
// mirror). The screen asks for the insets to lay out the video, forwards
// every SDL event first, and calls sc_gui_render() between drawing the video
// and presenting.

#include <stdbool.h>
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

struct sc_gui_callbacks {
    // A toolbar/menu action was triggered
    void (*on_action)(enum sc_gui_action action, void *userdata);
    // The insets changed (e.g. the sidebar was toggled)
    void (*on_insets_changed)(void *userdata);
};

struct sc_gui_params {
    SDL_Window *window;
    SDL_Renderer *renderer;
    const char *device_name;
    bool control; // false if the device cannot be controlled
    // SDL modifiers of the scrcpy shortcut key (always forwarded)
    uint16_t sdl_shortcut_mods;
    // SDL event type pushed when the GUI needs another frame (animations,
    // hover state settling); the screen must call sc_gui_render() on it
    uint32_t redraw_event_type;
    const struct sc_gui_callbacks *cbs;
    void *cbs_userdata;
};

struct sc_gui *
sc_gui_create(const struct sc_gui_params *params);

void
sc_gui_destroy(struct sc_gui *gui);

// Process an event.
//
// mirror_rect is the rectangle where the video is drawn, in window
// coordinates (NULL if there is no video yet).
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

// Background color to clear the window with (0xRRGGBB)
uint32_t
sc_gui_get_background_color(struct sc_gui *gui);

// Draw the GUI on top of the current render target (does not present)
void
sc_gui_render(struct sc_gui *gui, const SDL_FRect *mirror_rect);

#ifdef __cplusplus
}
#endif

#endif
