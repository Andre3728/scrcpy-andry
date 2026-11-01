#ifndef SC_BUNDLE_H
#define SC_BUNDLE_H

#include "common.h"

// scrcpy-gui single-file build: the runtime files (adb, scrcpy-server, icons)
// are embedded in the executable and extracted on first launch.

// Extract the bundled files (if needed) and configure the environment to use
// them:
//  - SCRCPY_SERVER_PATH and SCRCPY_ICON_DIR (unless already set)
//  - SCRCPY_GUI_BUNDLED_ADB: path of the bundled adb (the caller decides
//    whether to use it, see main.c and gui_app.c)
//
// Return false on error (the application may still work if adb and the
// server are found otherwise).
bool
sc_bundle_init(void);

#endif
