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

// An app installed on the device
struct sc_gui_app_entry {
    char *name;    // label, or the package name if unknown
    char *package;
    bool system;
};

struct sc_gui_app_list {
    struct sc_gui_app_entry *entries;
    size_t count;
};

// Build the list of apps from the output of the server run with
// list_apps=true (launchable apps, with their labels) and the output of
// "pm list packages -3" (user apps, including those without launcher
// activity). Both may be NULL. Return false on allocation failure.
bool
sc_gui_parse_apps(const char *server_out, const char *user_packages_out,
                  struct sc_gui_app_list *list);

void
sc_gui_app_list_destroy(struct sc_gui_app_list *list);

// Return true if s is a valid Android package name
bool
sc_gui_is_package_name(const char *s);

// Parse the "package:<value>" lines printed by "pm list packages" or
// "pm path". The array and its strings are to be freed with
// sc_gui_free_strings(). Return false on allocation failure.
bool
sc_gui_parse_pm_lines(const char *out, char ***values, size_t *count);

void
sc_gui_free_strings(char **values, size_t count);

#endif
