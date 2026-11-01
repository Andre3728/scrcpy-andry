#include "common.h"

#include <stdbool.h>
#include <stdio.h>
#ifdef HAVE_V4L2
# include <libavdevice/avdevice.h>
#endif
#include <SDL3/SDL.h>

#include "cli.h"
#include "events.h"
#include "options.h"
#include "scrcpy.h"
#include "gui_app.h"
#ifdef SC_BUNDLE
# include "sys/win/bundle.h"
#endif
#ifdef HAVE_USB
# include "usb/scrcpy_otg.h"
#endif
#include "util/log.h"
#include "util/net.h"
#include "util/term.h"
#include "version.h"

#ifdef _WIN32
#include <windows.h>
#include "util/str.h"
#endif

#ifdef _WIN32
// Reopen a standard stream on the console, unless it is already redirected
// (to a file or a pipe)
static void
reopen_on_console(DWORD std_handle, const char *path, const char *mode,
                  FILE *stream) {
    HANDLE h = GetStdHandle(std_handle);
    if (h && h != INVALID_HANDLE_VALUE) {
        DWORD type = GetFileType(h);
        if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) {
            return; // redirected, keep it
        }
    }
    if (!freopen(path, mode, stream)) {
        // ignore, the stream is just unavailable
    }
}
#endif

static int
main_scrcpy(int argc, char *argv[]) {
#ifdef _WIN32
    // scrcpy-gui is a GUI application (no console window). When run with
    // arguments from a terminal, print to that terminal (except the streams
    // redirected to a file or a pipe).
    if (argc > 1 && !GetConsoleWindow()
            && AttachConsole(ATTACH_PARENT_PROCESS)) {
        reopen_on_console(STD_OUTPUT_HANDLE, "CONOUT$", "w", stdout);
        reopen_on_console(STD_ERROR_HANDLE, "CONOUT$", "w", stderr);
        reopen_on_console(STD_INPUT_HANDLE, "CONIN$", "r", stdin);
    }

    // disable buffering, we want logs immediately
    // even line buffering (setvbuf() with mode _IOLBF) is not sufficient
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
#endif

    printf("scrcpy " SCRCPY_VERSION
           " <https://github.com/Genymobile/scrcpy>\n");

    struct scrcpy_cli_args args = {
        .opts = scrcpy_options_default,
        .help = false,
        .version = false,
        .pause_on_exit = SC_PAUSE_ON_EXIT_UNDEFINED,
    };

#ifndef NDEBUG
    args.opts.log_level = SC_LOG_LEVEL_DEBUG;
#endif

    enum scrcpy_exit_code ret;

    bool term_title_saved = false;

    if (!scrcpy_parse_args(&args, argc, argv)) {
        ret = SCRCPY_EXIT_FAILURE;
        goto end;
    }

    if (argc <= 1) {
        // scrcpy-gui: launching without arguments opens the GUI
        args.opts.gui = true;
    }

#ifdef SC_BUNDLE
    // Single-file build: extract adb, the server and the icons
    if (!sc_bundle_init()) {
        LOGW("Could not extract the bundled files");
    } else if (!args.opts.gui && !_wgetenv(L"ADB")) {
        // In GUI mode, the app decides (the adb path setting has priority)
        const wchar_t *adb = _wgetenv(L"SCRCPY_GUI_BUNDLED_ADB");
        if (adb) {
            _wputenv_s(L"ADB", adb);
        }
    }
#endif

#ifdef _WIN32
    if (args.opts.gui && args.pause_on_exit == SC_PAUSE_ON_EXIT_IF_ERROR) {
        // Launched by double-click (see scrcpy_parse_args()): the GUI does not
        // need the console window created for it
        FreeConsole();
        args.pause_on_exit = SC_PAUSE_ON_EXIT_FALSE;
    }
#endif

    sc_set_log_level(args.opts.log_level);

    if (args.opts.update_terminal_title) {
        sc_term_save_title();
        sc_term_set_title("scrcpy");
        term_title_saved = true;
    }

    if (args.help) {
        scrcpy_print_usage(argv[0]);
        ret = SCRCPY_EXIT_SUCCESS;
        goto end;
    }

    if (args.version) {
        scrcpy_print_version();
        ret = SCRCPY_EXIT_SUCCESS;
        goto end;
    }

#ifdef HAVE_V4L2
    if (args.opts.v4l2_device) {
        avdevice_register_all();
    }
#endif

    if (!net_init()) {
        ret = SCRCPY_EXIT_FAILURE;
        goto end;
    }

    sc_log_configure();

    if (!sc_main_thread_init()) {
        ret = SCRCPY_EXIT_FAILURE;
        goto net_cleanup;
    }

#ifdef HAVE_USB
    if (args.opts.otg) {
        ret = scrcpy_otg(&args.opts);
    } else
#endif
    if (args.opts.gui) {
        ret = scrcpy_gui(&args.opts);
    } else {
        ret = scrcpy(&args.opts);
    }

    sc_main_thread_destroy();

net_cleanup:
    net_cleanup();

end:
    if (args.pause_on_exit == SC_PAUSE_ON_EXIT_TRUE ||
            (args.pause_on_exit == SC_PAUSE_ON_EXIT_IF_ERROR &&
                ret != SCRCPY_EXIT_SUCCESS)) {
        printf("Press Enter to continue...\n");
        getchar();
    }

    if (term_title_saved) {
        sc_term_set_title(""); // fallback if restore is ignored
        sc_term_restore_title();
    }

    return ret;
}

int
main(int argc, char *argv[]) {
#ifndef _WIN32
    return main_scrcpy(argc, argv);
#else
    (void) argc;
    (void) argv;
    int wargc;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) {
        LOG_OOM();
        return SCRCPY_EXIT_FAILURE;
    }

    char **argv_utf8 = malloc((wargc + 1) * sizeof(*argv_utf8));
    if (!argv_utf8) {
        LOG_OOM();
        LocalFree(wargv);
        return SCRCPY_EXIT_FAILURE;
    }

    argv_utf8[wargc] = NULL;

    for (int i = 0; i < wargc; ++i) {
        argv_utf8[i] = sc_str_from_wchars(wargv[i]);
        if (!argv_utf8[i]) {
            LOG_OOM();
            for (int j = 0; j < i; ++j) {
                free(argv_utf8[j]);
            }
            LocalFree(wargv);
            free(argv_utf8);
            return SCRCPY_EXIT_FAILURE;
        }
    }

    LocalFree(wargv);

    int ret = main_scrcpy(wargc, argv_utf8);

    for (int i = 0; i < wargc; ++i) {
        free(argv_utf8[i]);
    }
    free(argv_utf8);

    return ret;
#endif
}
