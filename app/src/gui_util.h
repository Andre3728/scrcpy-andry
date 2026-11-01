#ifndef SC_GUI_UTIL_H
#define SC_GUI_UTIL_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>

// Pure helpers for scrcpy-gui (unit tested)

// Extract a human-readable error from the output of adb install/push
void
sc_gui_adb_error(const char *out, char *detail, size_t size);

// Return true if the path has the .apk extension (case-insensitive)
bool
sc_gui_is_apk(const char *path);

// Return the last component of a path ('/' or '\' separated)
const char *
sc_gui_path_basename(const char *path);

#endif
