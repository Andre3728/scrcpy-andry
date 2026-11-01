#ifndef SC_GUI_CONFIG_H
#define SC_GUI_CONFIG_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>

#include "gui/gui.h"
#include "options.h"

// scrcpy-gui persistent configuration (config.json in the user config
// directory)

#define SC_GUI_SERIAL_MAX 128

struct sc_gui_device_profile {
    char serial[SC_GUI_SERIAL_MAX];
    struct sc_gui_profile profile;
};

struct sc_gui_config {
    struct sc_gui_prefs prefs;
    struct sc_gui_profile defaults;
    struct sc_gui_device_profile *devices;
    size_t device_count;
    char last_serial[SC_GUI_SERIAL_MAX];
};

// Initialize with default values
void
sc_gui_config_init(struct sc_gui_config *config);

void
sc_gui_config_destroy(struct sc_gui_config *config);

void
sc_gui_profile_init(struct sc_gui_profile *profile);

// Return the profile of the device, or the defaults if there is none
const struct sc_gui_profile *
sc_gui_config_get_profile(const struct sc_gui_config *config,
                          const char *serial);

// Store the profile for the device (or the defaults if serial is NULL)
bool
sc_gui_config_set_profile(struct sc_gui_config *config, const char *serial,
                          const struct sc_gui_profile *profile);

// Serialize to JSON (to be freed by the caller)
char *
sc_gui_config_to_json(const struct sc_gui_config *config);

// Parse JSON into an initialized config (unknown or invalid values are
// ignored, missing values keep their current value)
bool
sc_gui_config_from_json(struct sc_gui_config *config, const char *json);

// Load from the config file (keep the defaults if it does not exist)
bool
sc_gui_config_load(struct sc_gui_config *config);

bool
sc_gui_config_save(const struct sc_gui_config *config);

// Apply the profile to the options
void
sc_gui_profile_apply(const struct sc_gui_profile *profile,
                     struct scrcpy_options *options);

// Split a command line into arguments (whitespace separated, double quotes
// group words). Return the number of arguments written to argv (at most
// max_args), or -1 on error. The strings must be freed by the caller.
int
sc_gui_split_args(const char *s, char **argv, int max_args);

#endif
