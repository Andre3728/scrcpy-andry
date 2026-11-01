#ifndef SCRCPY_H
#define SCRCPY_H

#include "common.h"

#include <stdbool.h>
#include <SDL3/SDL.h>

#include "options.h"

struct sc_gui;
struct sc_screen;

// scrcpy-gui: when a session runs inside the GUI app, the app owns the window,
// the renderer and the GUI, which outlive the session
struct scrcpy_gui_host {
    SDL_Window *window;
    SDL_Renderer *renderer;
    struct sc_gui *gui;

    // Called first for every event received by the session. has_screen is
    // false while waiting for the server (the app must draw the window
    // itself). Return true if the event was consumed.
    bool (*handle_event)(const SDL_Event *event, bool has_screen,
                         void *userdata);
    // Called when the session screen is created, and with NULL before it is
    // destroyed
    void (*on_screen)(struct sc_screen *screen, void *userdata);
    void *userdata;

    // Output: set if the user requested to quit the application
    bool quit_requested;
};

enum scrcpy_exit_code {
    // Normal program termination
    SCRCPY_EXIT_SUCCESS,

    // No connection could be established
    SCRCPY_EXIT_FAILURE,

    // Device was disconnected while running
    SCRCPY_EXIT_DISCONNECTED,
};

enum scrcpy_exit_code
scrcpy(struct scrcpy_options *options);

// Run a mirroring session hosted by the GUI app (host may be NULL)
enum scrcpy_exit_code
scrcpy_run(struct scrcpy_options *options, struct scrcpy_gui_host *host);

#endif
