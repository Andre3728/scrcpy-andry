#include "gui_config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "gui/third_party/cjson/cJSON.h"
#include "util/log.h"
#include "util/str.h"

#define SC_GUI_CONFIG_VERSION 1
#define SC_GUI_CONFIG_FILENAME "config.json"

void
sc_gui_profile_init(struct sc_gui_profile *profile) {
    memset(profile, 0, sizeof(*profile));
    profile->max_size = 0;
    profile->video_bit_rate = 0;
    profile->max_fps = 0;
    profile->video_codec = SC_GUI_CODEC_H264;
    profile->audio = true;
    profile->stay_awake = false;
    profile->turn_screen_off = false;
    profile->show_touches = false;
    profile->extra_args[0] = '\0';
}

void
sc_gui_config_init(struct sc_gui_config *config) {
    memset(config, 0, sizeof(*config));
    config->prefs.appearance = SC_GUI_APPEARANCE_SYSTEM;
    config->prefs.sidebar_visible = true;
    config->prefs.adb_path[0] = '\0';
    sc_gui_profile_init(&config->defaults);
    config->devices = NULL;
    config->device_count = 0;
    config->last_serial[0] = '\0';
}

void
sc_gui_config_destroy(struct sc_gui_config *config) {
    free(config->devices);
    config->devices = NULL;
    config->device_count = 0;
}

static struct sc_gui_device_profile *
find_device(const struct sc_gui_config *config, const char *serial) {
    for (size_t i = 0; i < config->device_count; ++i) {
        if (!strcmp(config->devices[i].serial, serial)) {
            return &config->devices[i];
        }
    }
    return NULL;
}

const struct sc_gui_profile *
sc_gui_config_get_profile(const struct sc_gui_config *config,
                          const char *serial) {
    if (serial) {
        struct sc_gui_device_profile *d = find_device(config, serial);
        if (d) {
            return &d->profile;
        }
    }
    return &config->defaults;
}

bool
sc_gui_config_set_profile(struct sc_gui_config *config, const char *serial,
                          const struct sc_gui_profile *profile) {
    if (!serial) {
        config->defaults = *profile;
        return true;
    }

    if (strlen(serial) >= SC_GUI_SERIAL_MAX) {
        LOGW("Serial too long, settings not saved: %s", serial);
        return false;
    }

    struct sc_gui_device_profile *d = find_device(config, serial);
    if (!d) {
        struct sc_gui_device_profile *devices =
            realloc(config->devices,
                    (config->device_count + 1) * sizeof(*devices));
        if (!devices) {
            LOG_OOM();
            return false;
        }
        config->devices = devices;
        d = &devices[config->device_count++];
        memset(d, 0, sizeof(*d));
        sc_strncpy(d->serial, serial, sizeof(d->serial));
    }
    d->profile = *profile;
    return true;
}

// ------------------------------------------------------------------- JSON

static const char *const codec_names[] = {"h264", "h265", "av1"};
static const char *const appearance_names[] = {"system", "light", "dark"};

static int
find_name(const char *const *names, int count, const char *name) {
    for (int i = 0; i < count; ++i) {
        if (!strcmp(names[i], name)) {
            return i;
        }
    }
    return -1;
}

static cJSON *
profile_to_json(const struct sc_gui_profile *p) {
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return NULL;
    }
    cJSON_AddNumberToObject(o, "max_size", p->max_size);
    cJSON_AddNumberToObject(o, "video_bit_rate", p->video_bit_rate);
    cJSON_AddNumberToObject(o, "max_fps", p->max_fps);
    cJSON_AddStringToObject(o, "video_codec", codec_names[p->video_codec]);
    cJSON_AddBoolToObject(o, "audio", p->audio);
    cJSON_AddBoolToObject(o, "stay_awake", p->stay_awake);
    cJSON_AddBoolToObject(o, "turn_screen_off", p->turn_screen_off);
    cJSON_AddBoolToObject(o, "show_touches", p->show_touches);
    cJSON_AddStringToObject(o, "extra_args", p->extra_args);
    return o;
}

static void
read_number(const cJSON *o, const char *name, double min, double max,
            double *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(o, name);
    if (cJSON_IsNumber(item) && item->valuedouble >= min
            && item->valuedouble <= max) {
        *out = item->valuedouble;
    }
}

static void
read_bool(const cJSON *o, const char *name, bool *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(o, name);
    if (cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item);
    }
}

static void
read_string(const cJSON *o, const char *name, char *out, size_t size) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(o, name);
    if (cJSON_IsString(item) && item->valuestring) {
        sc_strncpy(out, item->valuestring, size);
    }
}

static void
profile_from_json(const cJSON *o, struct sc_gui_profile *p) {
    if (!cJSON_IsObject(o)) {
        return;
    }
    double v = p->max_size;
    read_number(o, "max_size", 0, 0xFFFF, &v);
    p->max_size = (uint16_t) v;
    v = p->video_bit_rate;
    read_number(o, "video_bit_rate", 0, 0xFFFFFFFF, &v);
    p->video_bit_rate = (uint32_t) v;
    v = p->max_fps;
    read_number(o, "max_fps", 0, 0xFFFF, &v);
    p->max_fps = (uint16_t) v;

    char codec[16] = "";
    read_string(o, "video_codec", codec, sizeof(codec));
    int index = find_name(codec_names, 3, codec);
    if (index >= 0) {
        p->video_codec = (enum sc_gui_codec) index;
    }

    read_bool(o, "audio", &p->audio);
    read_bool(o, "stay_awake", &p->stay_awake);
    read_bool(o, "turn_screen_off", &p->turn_screen_off);
    read_bool(o, "show_touches", &p->show_touches);
    read_string(o, "extra_args", p->extra_args, sizeof(p->extra_args));
}

char *
sc_gui_config_to_json(const struct sc_gui_config *config) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddNumberToObject(root, "version", SC_GUI_CONFIG_VERSION);
    cJSON_AddStringToObject(root, "appearance",
                            appearance_names[config->prefs.appearance]);
    cJSON_AddBoolToObject(root, "sidebar_visible",
                          config->prefs.sidebar_visible);
    cJSON_AddStringToObject(root, "adb_path", config->prefs.adb_path);
    cJSON_AddStringToObject(root, "last_serial", config->last_serial);
    cJSON_AddItemToObject(root, "defaults",
                          profile_to_json(&config->defaults));

    cJSON *devices = cJSON_AddObjectToObject(root, "devices");
    for (size_t i = 0; devices && i < config->device_count; ++i) {
        const struct sc_gui_device_profile *d = &config->devices[i];
        cJSON_AddItemToObject(devices, d->serial, profile_to_json(&d->profile));
    }

    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    return json;
}

bool
sc_gui_config_from_json(struct sc_gui_config *config, const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    char appearance[16] = "";
    read_string(root, "appearance", appearance, sizeof(appearance));
    int index = find_name(appearance_names, 3, appearance);
    if (index >= 0) {
        config->prefs.appearance = (enum sc_gui_appearance) index;
    }
    read_bool(root, "sidebar_visible", &config->prefs.sidebar_visible);
    read_string(root, "adb_path", config->prefs.adb_path,
                sizeof(config->prefs.adb_path));
    read_string(root, "last_serial", config->last_serial,
                sizeof(config->last_serial));
    profile_from_json(cJSON_GetObjectItemCaseSensitive(root, "defaults"),
                      &config->defaults);

    const cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    const cJSON *item;
    cJSON_ArrayForEach(item, devices) {
        if (!item->string || !*item->string) {
            continue;
        }
        // Start from the defaults, so that missing values are not zero
        struct sc_gui_profile profile = config->defaults;
        profile_from_json(item, &profile);
        sc_gui_config_set_profile(config, item->string, &profile);
    }

    cJSON_Delete(root);
    return true;
}

// ------------------------------------------------------------------- file

static char *
get_config_path(void) {
    char *dir = SDL_GetPrefPath("", "scrcpy-gui");
    if (!dir) {
        LOGW("Could not get the config directory: %s", SDL_GetError());
        return NULL;
    }
    char *path = sc_str_concat(dir, SC_GUI_CONFIG_FILENAME);
    SDL_free(dir);
    return path;
}

bool
sc_gui_config_load(struct sc_gui_config *config) {
    char *path = get_config_path();
    if (!path) {
        return false;
    }

    size_t size;
    char *data = SDL_LoadFile(path, &size);
    if (!data) {
        // First launch
        LOGD("No config file: %s", path);
        free(path);
        return true;
    }

    bool ok = sc_gui_config_from_json(config, data);
    if (!ok) {
        LOGW("Invalid config file, using defaults: %s", path);
    } else {
        LOGD("Config loaded: %s", path);
    }
    SDL_free(data);
    free(path);
    return ok;
}

bool
sc_gui_config_save(const struct sc_gui_config *config) {
    char *path = get_config_path();
    if (!path) {
        return false;
    }

    char *json = sc_gui_config_to_json(config);
    if (!json) {
        free(path);
        return false;
    }

    // Write to a temporary file, then rename, so that the config is never
    // left half-written
    char *tmp = sc_str_concat(path, ".tmp");
    bool ok = tmp && SDL_SaveFile(tmp, json, strlen(json))
           && SDL_RenamePath(tmp, path);
    if (!ok) {
        LOGW("Could not save the config: %s", SDL_GetError());
    }

    free(tmp);
    cJSON_free(json);
    free(path);
    return ok;
}

// ---------------------------------------------------------------- options

void
sc_gui_profile_apply(const struct sc_gui_profile *profile,
                     struct scrcpy_options *options) {
    if (profile->max_size) {
        options->max_size = profile->max_size;
    }
    if (profile->video_bit_rate) {
        options->video_bit_rate = profile->video_bit_rate;
    }
    if (profile->max_fps) {
        static char fps[8];
        snprintf(fps, sizeof(fps), "%u", (unsigned) profile->max_fps);
        options->max_fps = fps;
    }
    switch (profile->video_codec) {
        case SC_GUI_CODEC_H265:
            options->video_codec = SC_CODEC_H265;
            break;
        case SC_GUI_CODEC_AV1:
            options->video_codec = SC_CODEC_AV1;
            break;
        default:
            options->video_codec = SC_CODEC_H264;
            break;
    }
    if (!profile->audio) {
        options->audio = false;
        options->audio_playback = false;
    }
    options->stay_awake |= profile->stay_awake;
    options->turn_screen_off |= profile->turn_screen_off;
    options->show_touches |= profile->show_touches;
}

int
sc_gui_split_args(const char *s, char **argv, int max_args) {
    int argc = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
            ++s;
        }
        if (!*s) {
            break;
        }
        if (argc == max_args) {
            goto error;
        }

        // Each argument is at most as long as the remaining input
        char *arg = malloc(strlen(s) + 1);
        if (!arg) {
            LOG_OOM();
            goto error;
        }
        size_t len = 0;
        bool quoted = false;
        while (*s && (quoted || (*s != ' ' && *s != '\t' && *s != '\n'
                                 && *s != '\r'))) {
            if (*s == '"') {
                quoted = !quoted;
            } else {
                arg[len++] = *s;
            }
            ++s;
        }
        arg[len] = '\0';
        argv[argc++] = arg;

        if (quoted) {
            // Unterminated quote
            goto error;
        }
    }
    return argc;

error:
    for (int i = 0; i < argc; ++i) {
        free(argv[i]);
    }
    return -1;
}
