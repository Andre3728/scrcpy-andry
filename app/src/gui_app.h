#ifndef SC_GUI_APP_H
#define SC_GUI_APP_H

#include "common.h"

#include "options.h"
#include "scrcpy.h"

// Run the scrcpy-gui application: a window that outlives mirroring sessions,
// lists the devices, and starts/stops sessions on demand.
//
// The options are used for every session (options->serial is overwritten).
enum scrcpy_exit_code
scrcpy_gui(struct scrcpy_options *options);

#endif
