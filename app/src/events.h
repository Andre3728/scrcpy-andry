#ifndef SC_EVENTS_H
#define SC_EVENTS_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>

enum {
    SC_EVENT_NEW_FRAME = SDL_EVENT_USER,
    SC_EVENT_OPEN_WINDOW,
    SC_EVENT_DEVICE_DISCONNECTED,
    SC_EVENT_SERVER_CONNECTION_FAILED,
    SC_EVENT_SERVER_CONNECTED,
    SC_EVENT_DEMUXER_ERROR,
    SC_EVENT_DECODER_ERROR,
    SC_EVENT_RECORDER_ERROR,
    SC_EVENT_TIME_LIMIT_REACHED,
    SC_EVENT_CONTROLLER_ERROR,
    SC_EVENT_AOA_OPEN_ERROR,
    SC_EVENT_DISCONNECTED_ICON_LOADED,
    SC_EVENT_DISCONNECTED_TIMEOUT,
    SC_EVENT_GUI_REDRAW,
    // The GUI requested to stop the current mirroring session
    SC_EVENT_GUI_STOP_SESSION,
    // The device monitor listed devices (data1: struct sc_vec_adb_devices *)
    SC_EVENT_GUI_DEVICES,
    // A GUI background task completed (data1: struct sc_gui_task *)
    SC_EVENT_GUI_TASK_DONE,
    // A transfer changed state (data1: struct sc_gui_transfer_update *)
    SC_EVENT_GUI_TRANSFER,
    // A folder was chosen to save files (data1: struct sc_gui_pull_request *)
    SC_EVENT_GUI_FOLDER_CHOSEN,
};

bool
sc_push_event_impl(uint32_t type, void *ptr, const char *name);

#define sc_push_event(TYPE) sc_push_event_impl(TYPE, NULL, # TYPE)
#define sc_push_event_with_data(TYPE, PTR) sc_push_event_impl(TYPE, PTR, # TYPE)

bool sc_dequeue_event(uint32_t type, SDL_Event *event);

typedef SDL_MainThreadCallback sc_runnable_fn;

bool
sc_main_thread_init(void);

void
sc_main_thread_destroy(void);

bool
sc_run_on_main_thread(sc_runnable_fn run, void *userdata, bool wait_complete);

// Reject new runnables after this call
void
sc_main_thread_stop(void);

// Accept runnables again after sc_main_thread_stop() (for a new session)
void
sc_main_thread_resume(void);

#endif
