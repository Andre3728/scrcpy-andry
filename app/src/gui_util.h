#ifndef SC_GUI_UTIL_H
#define SC_GUI_UTIL_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

// A file listed by "ls -la" on the device
struct sc_gui_file_entry {
    char *name;
    char *mtime;  // "YYYY-MM-DD HH:MM", may be empty
    uint64_t size;
    char type;    // 'd' directory, '-' file, 'l' symlink, other ls types
};

struct sc_gui_listing {
    struct sc_gui_file_entry *entries;
    size_t count;
    char *error; // NULL if no error
};

// Parse the output of toybox "ls -la DIR/". Return false on allocation
// failure. "ls: ..." error lines are reported in listing->error.
bool
sc_gui_parse_ls(const char *out, struct sc_gui_listing *listing);

void
sc_gui_listing_destroy(struct sc_gui_listing *listing);

// Quote a string for the device shell (single quotes). To be freed by the
// caller.
char *
sc_gui_shell_quote(const char *s);

#endif
