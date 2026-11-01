#include "common.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "gui_config.h"

static void test_defaults(void) {
    struct sc_gui_config config;
    sc_gui_config_init(&config);

    assert(config.prefs.appearance == SC_GUI_APPEARANCE_SYSTEM);
    assert(config.prefs.sidebar_visible);
    assert(config.defaults.audio);
    assert(config.defaults.video_codec == SC_GUI_CODEC_H264);

    // Unknown device: defaults
    const struct sc_gui_profile *p =
        sc_gui_config_get_profile(&config, "unknown");
    assert(p == &config.defaults);

    sc_gui_config_destroy(&config);
}

static void test_round_trip(void) {
    struct sc_gui_config config;
    sc_gui_config_init(&config);

    config.prefs.appearance = SC_GUI_APPEARANCE_DARK;
    config.prefs.sidebar_visible = false;
    strcpy(config.prefs.adb_path, "C:\\tools\\adb.exe");
    strcpy(config.last_serial, "192.168.1.2:5555");
    config.defaults.max_size = 1024;

    struct sc_gui_profile p;
    sc_gui_profile_init(&p);
    p.max_size = 1920;
    p.video_bit_rate = 16000000;
    p.max_fps = 60;
    p.video_codec = SC_GUI_CODEC_H265;
    p.audio = false;
    p.stay_awake = true;
    p.turn_screen_off = true;
    p.show_touches = true;
    strcpy(p.extra_args, "--crop \"1080:1920:0:0\"");
    assert(sc_gui_config_set_profile(&config, "R58M639E4ZJ", &p));

    char *json = sc_gui_config_to_json(&config);
    assert(json);

    struct sc_gui_config loaded;
    sc_gui_config_init(&loaded);
    assert(sc_gui_config_from_json(&loaded, json));
    free(json);

    assert(loaded.prefs.appearance == SC_GUI_APPEARANCE_DARK);
    assert(!loaded.prefs.sidebar_visible);
    assert(!strcmp(loaded.prefs.adb_path, "C:\\tools\\adb.exe"));
    assert(!strcmp(loaded.last_serial, "192.168.1.2:5555"));
    assert(loaded.defaults.max_size == 1024);
    assert(loaded.device_count == 1);

    const struct sc_gui_profile *q =
        sc_gui_config_get_profile(&loaded, "R58M639E4ZJ");
    assert(q != &loaded.defaults);
    assert(q->max_size == 1920);
    assert(q->video_bit_rate == 16000000);
    assert(q->max_fps == 60);
    assert(q->video_codec == SC_GUI_CODEC_H265);
    assert(!q->audio);
    assert(q->stay_awake);
    assert(q->turn_screen_off);
    assert(q->show_touches);
    assert(!strcmp(q->extra_args, "--crop \"1080:1920:0:0\""));

    // Updating an existing device does not duplicate it
    p.max_size = 800;
    assert(sc_gui_config_set_profile(&loaded, "R58M639E4ZJ", &p));
    assert(loaded.device_count == 1);
    assert(sc_gui_config_get_profile(&loaded, "R58M639E4ZJ")->max_size == 800);

    sc_gui_config_destroy(&loaded);
    sc_gui_config_destroy(&config);
}

static void test_invalid_json(void) {
    struct sc_gui_config config;
    sc_gui_config_init(&config);

    assert(!sc_gui_config_from_json(&config, "not json"));
    assert(!sc_gui_config_from_json(&config, "[1, 2]"));

    // Invalid values are ignored, valid ones are kept
    assert(sc_gui_config_from_json(&config,
        "{\"appearance\": \"purple\", \"sidebar_visible\": 3,"
        " \"defaults\": {\"max_size\": -5, \"max_fps\": 30,"
        " \"video_codec\": \"vp9\"}}"));
    assert(config.prefs.appearance == SC_GUI_APPEARANCE_SYSTEM);
    assert(config.prefs.sidebar_visible);
    assert(config.defaults.max_size == 0);
    assert(config.defaults.max_fps == 30);
    assert(config.defaults.video_codec == SC_GUI_CODEC_H264);

    sc_gui_config_destroy(&config);
}

static void test_split_args(void) {
    char *argv[8];

    int argc = sc_gui_split_args("", argv, 8);
    assert(argc == 0);

    argc = sc_gui_split_args("  --max-size 1024\t-b  8M ", argv, 8);
    assert(argc == 4);
    assert(!strcmp(argv[0], "--max-size"));
    assert(!strcmp(argv[1], "1024"));
    assert(!strcmp(argv[2], "-b"));
    assert(!strcmp(argv[3], "8M"));
    for (int i = 0; i < argc; ++i) {
        free(argv[i]);
    }

    argc = sc_gui_split_args("--window-title \"My phone\" x", argv, 8);
    assert(argc == 3);
    assert(!strcmp(argv[1], "My phone"));
    for (int i = 0; i < argc; ++i) {
        free(argv[i]);
    }

    // Unterminated quote
    assert(sc_gui_split_args("--window-title \"oops", argv, 8) == -1);

    // Too many arguments
    assert(sc_gui_split_args("a b c", argv, 2) == -1);
}

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;

    test_defaults();
    test_round_trip();
    test_invalid_json();
    test_split_args();
    return 0;
}
