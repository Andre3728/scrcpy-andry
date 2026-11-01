#include "gui_app.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#ifdef _WIN32
# include <winsock2.h>
# include <windows.h>
#endif

#include "adb/adb.h"
#include "adb/adb_device.h"
#include "cli.h"
#include "events.h"
#include "gui/gui.h"
#include "gui_config.h"
#include "gui_util.h"
#include "icon.h"
#include "screen.h"
#include "sdl_hints.h"
#include "shortcut_mod.h"
#include "util/intr.h"
#include "util/log.h"
#include "util/sdl.h"
#include "util/str.h"
#include "util/strbuf.h"
#include "util/thread.h"
#include "util/tick.h"

#define SC_GUI_DEVICE_POLL_INTERVAL SC_TICK_FROM_SEC(2)
#define SC_GUI_TCPIP_PORT 5555

enum sc_gui_task_type {
    SC_GUI_TASK_SWITCH_TO_WIFI,
    SC_GUI_TASK_PAIR,
    SC_GUI_TASK_CONNECT,
    SC_GUI_TASK_DISCONNECT,
    // File manager
    SC_GUI_TASK_LIST_DIR,
    SC_GUI_TASK_DELETE,
    SC_GUI_TASK_MKDIR,
};

static bool
is_file_task(enum sc_gui_task_type type) {
    return type == SC_GUI_TASK_LIST_DIR || type == SC_GUI_TASK_DELETE
        || type == SC_GUI_TASK_MKDIR;
}

// A device operation executed by the worker thread
struct sc_gui_task {
    enum sc_gui_task_type type;
    char *serial;  // SWITCH_TO_WIFI
    char *address; // PAIR, CONNECT, DISCONNECT; SWITCH_TO_WIFI (result)
    char *code;    // PAIR
    char *path;    // LIST_DIR, MKDIR
    char **paths;  // DELETE
    size_t path_count;

    // Result
    bool success;
    const char *error; // static string, may be NULL
    struct sc_gui_listing listing; // LIST_DIR
    char error_buf[256]; // file tasks

    struct sc_gui_task *next;
};

// An install or a copy, executed by the transfer thread
struct sc_gui_transfer_job {
    uint32_t id;
    enum sc_gui_transfer_type type;
    char *serial;
    char *local;
    char *remote_dir; // PUSH only, ends with '/'
    struct sc_gui_transfer_job *next;
};

struct sc_gui_transfer_update {
    uint32_t id;
    enum sc_gui_transfer_type type;
    enum sc_gui_transfer_state state;
    char detail[256];
};

struct sc_gui_app {
    // Options from the command line, the base for every session
    struct scrcpy_options *options;
    // Options of the current session (base + device profile)
    struct scrcpy_options session_options;

    struct sc_gui_config config;

    SDL_Window *window;
    SDL_Renderer *renderer;
    struct sc_gui *gui;

    // The screen of the current session, if any
    struct sc_screen *screen;
    // Insets applied to the window size while there is no screen
    struct sc_gui_insets insets;

    // Worker thread: polls the devices and executes the tasks
    sc_thread worker;
    sc_mutex mutex;
    sc_cond cond;
    struct sc_intr intr;
    bool stopped; // protected by mutex
    bool refresh_requested; // protected by mutex
    struct sc_gui_task *tasks; // protected by mutex

    // Transfer thread: installs and copies, one at a time
    sc_thread transfer_thread;
    sc_cond transfer_cond;
    struct sc_intr transfer_intr; // reset after each transfer
    struct sc_gui_transfer_job *jobs; // pending, protected by mutex
    uint32_t running_transfer; // 0 if none, protected by mutex
    uint32_t next_transfer_id;
    // Summary of the current batch (main thread)
    unsigned active_transfers;
    unsigned batch_installed;
    unsigned batch_copied;
    unsigned batch_failed;

    // Last device list (owned)
    struct sc_vec_adb_devices devices;
    bool devices_received;

    // Device to start a session for (owned), set by the user or auto-start
    char *start_serial;
    // Device to reconnect to automatically after a disconnection (owned)
    char *reconnect_serial;
    // Device which must not be auto-started until it disappears (owned)
    char *blocked_serial;
    // Serial of the current session (or NULL)
    const char *session_serial;
    // Recording: file to record in the next session (owned), and the file
    // recorded by the current session (kept string, or NULL)
    char *record_next;
    const char *recording_path;
    bool stop_requested;
    bool quit;

    // The server keeps pointers to the options strings until the end of the
    // program, so the serials passed to sessions are kept until exit
    char **kept_strings;
    size_t kept_count;
};

static void
replace_string(char **dst, const char *src) {
    free(*dst);
    *dst = src ? strdup(src) : NULL;
}

static bool
str_eq(const char *a, const char *b) {
    return a && b && !strcmp(a, b);
}

// Take ownership of s until exit
static bool
keep_owned(struct sc_gui_app *app, char *s) {
    char **list = realloc(app->kept_strings,
                          (app->kept_count + 1) * sizeof(*list));
    if (!list) {
        LOG_OOM();
        free(s);
        return false;
    }
    list[app->kept_count++] = s;
    app->kept_strings = list;
    return true;
}

static const char *
keep_string(struct sc_gui_app *app, const char *s) {
    char *dup = strdup(s);
    if (!dup) {
        LOG_OOM();
        return NULL;
    }
    return keep_owned(app, dup) ? dup : NULL;
}

static void
save_config(struct sc_gui_app *app) {
    sc_gui_config_save(&app->config);
}

// Parse the "additional arguments" of a profile on top of the options
static bool
apply_extra_args(struct sc_gui_app *app, const char *extra,
                 struct scrcpy_options *options) {
#define SC_GUI_MAX_EXTRA_ARGS 64
    char *argv[SC_GUI_MAX_EXTRA_ARGS + 2];
    argv[0] = "scrcpy-gui";
    int count = sc_gui_split_args(extra, &argv[1], SC_GUI_MAX_EXTRA_ARGS);
    if (count < 0) {
        LOGE("Invalid additional arguments: %s", extra);
        return false;
    }
    argv[count + 1] = NULL;

    // The options may reference the argument strings: keep them until exit
    for (int i = 1; i <= count; ++i) {
        if (!keep_owned(app, argv[i])) {
            // argv[i] is freed, and the next ones must be too
            for (int j = i + 1; j <= count; ++j) {
                free(argv[j]);
            }
            return false;
        }
    }

    struct scrcpy_cli_args args = {
        .opts = *options,
        .help = false,
        .version = false,
        .pause_on_exit = SC_PAUSE_ON_EXIT_FALSE,
    };
    // The serial is selected by the GUI
    args.opts.serial = NULL;
    if (!scrcpy_parse_args(&args, count + 1, argv)) {
        LOGE("Invalid additional arguments: %s", extra);
        return false;
    }
    if (args.help || args.version) {
        return false;
    }
    *options = args.opts;
    return true;
}

// ------------------------------------------------------------------- worker

static void
sc_gui_task_destroy(struct sc_gui_task *task) {
    free(task->serial);
    free(task->address);
    free(task->code);
    free(task->path);
    for (size_t i = 0; i < task->path_count; ++i) {
        free(task->paths[i]);
    }
    free(task->paths);
    sc_gui_listing_destroy(&task->listing);
    free(task);
}

// Run a shell command on the device, capturing its output
static bool
run_shell(struct sc_gui_app *app, const char *serial, const char *cmd,
          char *out, size_t out_size) {
    const char *const args[] = {"-s", serial, "shell", cmd, NULL};
    return sc_adb_run_capture(&app->intr, args, out, out_size);
}

#define SC_GUI_LS_BUFSIZE (4 * 1024 * 1024)

static void
run_task_list_dir(struct sc_gui_app *app, struct sc_gui_task *task) {
    char *quoted = sc_gui_shell_quote(task->path);
    char *cmd = quoted ? sc_str_concat("ls -la ", quoted) : NULL;
    free(quoted);
    char *out = malloc(SC_GUI_LS_BUFSIZE);
    if (!cmd || !out) {
        LOG_OOM();
        free(cmd);
        free(out);
        return;
    }

    // ls returns an error code if a file cannot be read, but the listing is
    // still useful: parse the output in all cases
    run_shell(app, task->serial, cmd, out, SC_GUI_LS_BUFSIZE);
    task->success = sc_gui_parse_ls(out, &task->listing);
    free(cmd);
    free(out);
}

// Paths which must never be deleted recursively
static bool
is_protected_path(const char *path) {
    static const char *const protected[] = {
        "", "/", "/sdcard", "/sdcard/", "/storage", "/storage/",
        "/storage/emulated", "/storage/emulated/", "/storage/emulated/0",
        "/storage/emulated/0/",
    };
    for (size_t i = 0; i < sizeof(protected) / sizeof(*protected); ++i) {
        if (!strcmp(path, protected[i])) {
            return true;
        }
    }
    return false;
}

static void
run_task_delete(struct sc_gui_app *app, struct sc_gui_task *task) {
    struct sc_strbuf buf;
    if (!sc_strbuf_init(&buf, 256)) {
        LOG_OOM();
        return;
    }
    sc_strbuf_append_staticstr(&buf, "rm -rf");
    for (size_t i = 0; i < task->path_count; ++i) {
        if (is_protected_path(task->paths[i])) {
            snprintf(task->error_buf, sizeof(task->error_buf),
                     "Refusing to delete %s", task->paths[i]);
            free(buf.s);
            return;
        }
        char *quoted = sc_gui_shell_quote(task->paths[i]);
        if (!quoted) {
            LOG_OOM();
            free(buf.s);
            return;
        }
        sc_strbuf_append_char(&buf, ' ');
        sc_strbuf_append(&buf, quoted, strlen(quoted));
        free(quoted);
    }

    char out[1024];
    task->success = run_shell(app, task->serial, buf.s, out, sizeof(out));
    if (!task->success) {
        sc_gui_adb_error(out, task->error_buf, sizeof(task->error_buf));
    }
    free(buf.s);
}

static void
run_task_mkdir(struct sc_gui_app *app, struct sc_gui_task *task) {
    char *quoted = sc_gui_shell_quote(task->path);
    char *cmd = quoted ? sc_str_concat("mkdir -p ", quoted) : NULL;
    free(quoted);
    if (!cmd) {
        LOG_OOM();
        return;
    }
    char out[1024];
    task->success = run_shell(app, task->serial, cmd, out, sizeof(out));
    if (!task->success) {
        sc_gui_adb_error(out, task->error_buf, sizeof(task->error_buf));
    }
    free(cmd);
}

static void
run_task_switch_to_wifi(struct sc_gui_app *app, struct sc_gui_task *task) {
    struct sc_intr *intr = &app->intr;
    char *ip = sc_adb_get_device_ip(intr, task->serial, SC_ADB_SILENT);
    if (!ip) {
        task->error = "Could not find the device IP address. "
                      "Is the device connected to Wi-Fi?";
        return;
    }

    char address[64];
    snprintf(address, sizeof(address), "%s:%d", ip, SC_GUI_TCPIP_PORT);
    free(ip);
    task->address = strdup(address);
    if (!task->address) {
        LOG_OOM();
        return;
    }

    if (!sc_adb_tcpip(intr, task->serial, SC_GUI_TCPIP_PORT, SC_ADB_SILENT)) {
        task->error = "Could not enable wireless debugging (adb tcpip).";
        return;
    }

    // adbd restarts in TCP/IP mode, it may take some time
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (sc_intr_is_interrupted(intr)) {
            return;
        }
        SDL_Delay(500);
        if (sc_adb_connect(intr, task->address, SC_ADB_SILENT)) {
            task->success = true;
            return;
        }
    }

    task->error = "Could not connect over Wi-Fi. Check that the computer and "
                  "the device are on the same network.";
}

static void
run_task(struct sc_gui_app *app, struct sc_gui_task *task) {
    struct sc_intr *intr = &app->intr;
    switch (task->type) {
        case SC_GUI_TASK_SWITCH_TO_WIFI:
            run_task_switch_to_wifi(app, task);
            break;
        case SC_GUI_TASK_PAIR:
            task->success = sc_adb_pair(intr, task->address, task->code,
                                        SC_ADB_SILENT);
            if (!task->success) {
                task->error = "Pairing failed. Check the IP address, the port "
                              "and the pairing code (they change each time).";
            }
            break;
        case SC_GUI_TASK_CONNECT:
            task->success = sc_adb_connect(intr, task->address, SC_ADB_SILENT);
            if (!task->success) {
                task->error = "Could not connect. Check the IP address and "
                              "port, and that the device is paired.";
            }
            break;
        case SC_GUI_TASK_DISCONNECT:
            task->success = sc_adb_disconnect(intr, task->address,
                                              SC_ADB_SILENT);
            break;
        case SC_GUI_TASK_LIST_DIR:
            run_task_list_dir(app, task);
            break;
        case SC_GUI_TASK_DELETE:
            run_task_delete(app, task);
            break;
        case SC_GUI_TASK_MKDIR:
            run_task_mkdir(app, task);
            break;
    }
}

static void
list_devices(struct sc_gui_app *app) {
    struct sc_vec_adb_devices *vec = malloc(sizeof(*vec));
    if (!vec) {
        LOG_OOM();
        return;
    }
    sc_vector_init(vec);

    if (!sc_adb_list_devices(&app->intr, SC_ADB_SILENT, vec)) {
        sc_adb_devices_destroy(vec);
        free(vec);
        return;
    }

    if (!sc_push_event_with_data(SC_EVENT_GUI_DEVICES, vec)) {
        sc_adb_devices_destroy(vec);
        free(vec);
    }
}

static int
run_worker(void *data) {
    struct sc_gui_app *app = data;

    sc_tick next_poll = 0; // poll immediately

    sc_mutex_lock(&app->mutex);
    while (!app->stopped) {
        struct sc_gui_task *task = app->tasks;
        if (task) {
            app->tasks = task->next;
            task->next = NULL;
        }
        bool refresh = app->refresh_requested;
        app->refresh_requested = false;
        sc_mutex_unlock(&app->mutex);

        bool poll = refresh || sc_tick_now() >= next_poll;
        if (task) {
            // Device tasks change the device list
            poll |= !is_file_task(task->type);
            run_task(app, task);
            if (!sc_push_event_with_data(SC_EVENT_GUI_TASK_DONE, task)) {
                sc_gui_task_destroy(task);
            }
        }

        if (poll) {
            list_devices(app);
            next_poll = sc_tick_now() + SC_GUI_DEVICE_POLL_INTERVAL;
        }

        sc_mutex_lock(&app->mutex);
        if (!app->stopped && !app->tasks && !app->refresh_requested) {
            sc_cond_timedwait(&app->cond, &app->mutex, next_poll);
        }
    }
    sc_mutex_unlock(&app->mutex);

    return 0;
}

static void
request_refresh(struct sc_gui_app *app) {
    sc_mutex_lock(&app->mutex);
    app->refresh_requested = true;
    sc_cond_signal(&app->cond);
    sc_mutex_unlock(&app->mutex);
}

static void
enqueue_task(struct sc_gui_app *app, struct sc_gui_task *task) {
    sc_mutex_lock(&app->mutex);
    struct sc_gui_task **tail = &app->tasks;
    while (*tail) {
        tail = &(*tail)->next;
    }
    *tail = task;
    sc_cond_signal(&app->cond);
    sc_mutex_unlock(&app->mutex);
}

static struct sc_gui_task *
new_file_task(struct sc_gui_app *app, enum sc_gui_task_type type,
              const char *path) {
    if (!app->session_serial) {
        return NULL;
    }
    struct sc_gui_task *task = calloc(1, sizeof(*task));
    if (!task) {
        LOG_OOM();
        return NULL;
    }
    task->type = type;
    task->serial = strdup(app->session_serial);
    task->path = path ? strdup(path) : NULL;
    if (!task->serial || (path && !task->path)) {
        LOG_OOM();
        sc_gui_task_destroy(task);
        return NULL;
    }
    return task;
}

static void
submit_task(struct sc_gui_app *app, enum sc_gui_task_type type,
            const char *serial, const char *address, const char *code) {
    struct sc_gui_task *task = calloc(1, sizeof(*task));
    if (!task) {
        LOG_OOM();
        return;
    }
    task->type = type;
    task->serial = serial ? strdup(serial) : NULL;
    task->address = address ? strdup(address) : NULL;
    task->code = code ? strdup(code) : NULL;

    sc_mutex_lock(&app->mutex);
    struct sc_gui_task **tail = &app->tasks;
    while (*tail) {
        tail = &(*tail)->next;
    }
    *tail = task;
    sc_cond_signal(&app->cond);
    sc_mutex_unlock(&app->mutex);

    sc_gui_set_task_busy(app->gui, true);
}

// ---------------------------------------------------------------- transfers

static void
add_job(struct sc_gui_app *app, struct sc_gui_transfer_job *job,
        const char *name);

static void
sc_gui_transfer_job_destroy(struct sc_gui_transfer_job *job) {
    free(job->serial);
    free(job->local);
    free(job->remote_dir);
    free(job);
}

static void
run_transfer(struct sc_gui_app *app, struct sc_gui_transfer_job *job,
             struct sc_gui_transfer_update *u) {
    char out[4096];
    bool ok;
    if (job->type == SC_GUI_TRANSFER_INSTALL) {
        const char *const args[] = {"-s", job->serial, "install", "-r",
                                    job->local, NULL};
        ok = sc_adb_run_capture(&app->transfer_intr, args, out, sizeof(out));
        // Old adb versions return 0 on failure
        ok = ok && !strstr(out, "Failure");
    } else if (job->type == SC_GUI_TRANSFER_PULL) {
        // For a pull, local is the destination folder and remote_dir the
        // file or folder on the device
        const char *const args[] = {"-s", job->serial, "pull",
                                    job->remote_dir, job->local, NULL};
        ok = sc_adb_run_capture(&app->transfer_intr, args, out, sizeof(out));
    } else {
        const char *const args[] = {"-s", job->serial, "push", job->local,
                                    job->remote_dir, NULL};
        ok = sc_adb_run_capture(&app->transfer_intr, args, out, sizeof(out));
    }

    if (sc_intr_is_interrupted(&app->transfer_intr)) {
        u->state = SC_GUI_TRANSFER_CANCELED;
        return;
    }

    if (ok) {
        u->state = SC_GUI_TRANSFER_DONE;
        if (job->type == SC_GUI_TRANSFER_INSTALL) {
            snprintf(u->detail, sizeof(u->detail), "Installed");
        } else if (job->type == SC_GUI_TRANSFER_PULL) {
            snprintf(u->detail, sizeof(u->detail), "Saved to %s",
                     job->local);
        } else {
            snprintf(u->detail, sizeof(u->detail), "Copied to %s",
                     job->remote_dir);
        }
    } else {
        u->state = SC_GUI_TRANSFER_FAILED;
        sc_gui_adb_error(out, u->detail, sizeof(u->detail));
        LOGW("Transfer failed (%s): %s", job->local, out);
    }
}

static void
post_transfer_update(uint32_t id, enum sc_gui_transfer_type type,
                     enum sc_gui_transfer_state state, const char *detail) {
    struct sc_gui_transfer_update *u = calloc(1, sizeof(*u));
    if (!u) {
        LOG_OOM();
        return;
    }
    u->id = id;
    u->type = type;
    u->state = state;
    if (detail) {
        snprintf(u->detail, sizeof(u->detail), "%s", detail);
    }
    if (!sc_push_event_with_data(SC_EVENT_GUI_TRANSFER, u)) {
        free(u);
    }
}

static int
run_transfer_thread(void *data) {
    struct sc_gui_app *app = data;

    sc_mutex_lock(&app->mutex);
    for (;;) {
        while (!app->stopped && !app->jobs) {
            sc_cond_wait(&app->transfer_cond, &app->mutex);
        }
        if (app->stopped) {
            break;
        }

        struct sc_gui_transfer_job *job = app->jobs;
        app->jobs = job->next;
        job->next = NULL;
        app->running_transfer = job->id;
        sc_mutex_unlock(&app->mutex);

        post_transfer_update(job->id, job->type, SC_GUI_TRANSFER_RUNNING,
                             NULL);

        struct sc_gui_transfer_update *u = calloc(1, sizeof(*u));
        if (u) {
            u->id = job->id;
            u->type = job->type;
            run_transfer(app, job, u);
            if (!sc_push_event_with_data(SC_EVENT_GUI_TRANSFER, u)) {
                free(u);
            }
        } else {
            LOG_OOM();
        }
        sc_gui_transfer_job_destroy(job);

        sc_mutex_lock(&app->mutex);
        app->running_transfer = 0;
        // Reset the interruptor for the next transfer (a cancellation
        // interrupts it permanently)
        sc_intr_destroy(&app->transfer_intr);
        if (!sc_intr_init(&app->transfer_intr)) {
            LOGE("Could not reset the transfer interruptor");
            app->stopped = true;
        }
    }
    sc_mutex_unlock(&app->mutex);

    return 0;
}

static void
enqueue_transfer(struct sc_gui_app *app, const char *local,
                 const char *remote_dir) {
    SDL_PathInfo info;
    bool is_file = SDL_GetPathInfo(local, &info)
                && info.type == SDL_PATHTYPE_FILE;

    struct sc_gui_transfer_job *job = calloc(1, sizeof(*job));
    if (!job) {
        LOG_OOM();
        return;
    }
    job->id = ++app->next_transfer_id;
    job->type = !remote_dir && is_file && sc_gui_is_apk(local)
              ? SC_GUI_TRANSFER_INSTALL : SC_GUI_TRANSFER_PUSH;
    job->serial = strdup(app->session_serial);
    job->local = strdup(local);

    if (job->type == SC_GUI_TRANSFER_PUSH) {
        const char *dir = remote_dir ? remote_dir
                        : app->options->push_target ? app->options->push_target
                        : "/sdcard/Download/";
        size_t len = strlen(dir);
        bool slash = len && dir[len - 1] == '/';
        job->remote_dir = sc_str_concat(dir, slash ? "" : "/");
    }

    if (!job->serial || !job->local
            || (job->type == SC_GUI_TRANSFER_PUSH && !job->remote_dir)) {
        LOG_OOM();
        sc_gui_transfer_job_destroy(job);
        return;
    }

    add_job(app, job, sc_gui_path_basename(local));
}

static void
add_job(struct sc_gui_app *app, struct sc_gui_transfer_job *job,
        const char *name) {
    sc_gui_update_transfer(app->gui, job->id, job->type, name,
                           SC_GUI_TRANSFER_PENDING, NULL);
    ++app->active_transfers;

    sc_mutex_lock(&app->mutex);
    struct sc_gui_transfer_job **tail = &app->jobs;
    while (*tail) {
        tail = &(*tail)->next;
    }
    *tail = job;
    sc_cond_signal(&app->transfer_cond);
    sc_mutex_unlock(&app->mutex);
}

// Download a file or folder from the device into local_dir
static void
enqueue_pull(struct sc_gui_app *app, const char *remote,
             const char *local_dir) {
    if (!app->session_serial) {
        return;
    }
    struct sc_gui_transfer_job *job = calloc(1, sizeof(*job));
    if (!job) {
        LOG_OOM();
        return;
    }
    job->id = ++app->next_transfer_id;
    job->type = SC_GUI_TRANSFER_PULL;
    job->serial = strdup(app->session_serial);
    job->local = strdup(local_dir);
    job->remote_dir = strdup(remote);
    if (!job->serial || !job->local || !job->remote_dir) {
        LOG_OOM();
        sc_gui_transfer_job_destroy(job);
        return;
    }

    // Name: last component, without the trailing '/'
    char name[256];
    snprintf(name, sizeof(name), "%s", remote);
    size_t len = strlen(name);
    while (len > 1 && name[len - 1] == '/') {
        name[--len] = '\0';
    }
    add_job(app, job, sc_gui_path_basename(name));
}

static const char *
get_downloads_folder(void) {
    const char *dir = SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS);
    if (!dir) {
        dir = SDL_GetUserFolder(SDL_FOLDER_HOME);
    }
    return dir ? dir : ".";
}

// Files to pull once the user chose a folder
struct sc_gui_pull_request {
    char **paths;
    size_t count;
    char *dir; // set by the dialog callback
};

static void
sc_gui_pull_request_destroy(struct sc_gui_pull_request *req) {
    for (size_t i = 0; i < req->count; ++i) {
        free(req->paths[i]);
    }
    free(req->paths);
    free(req->dir);
    free(req);
}

// Called by SDL (possibly from another thread)
static void SDLCALL
on_folder_chosen(void *userdata, const char *const *filelist, int filter) {
    (void) filter;
    struct sc_gui_pull_request *req = userdata;
    if (!filelist || !filelist[0]) {
        // Canceled or error
        sc_gui_pull_request_destroy(req);
        return;
    }
    req->dir = strdup(filelist[0]);
    if (!req->dir
            || !sc_push_event_with_data(SC_EVENT_GUI_FOLDER_CHOSEN, req)) {
        sc_gui_pull_request_destroy(req);
    }
}

static void
on_transfer_update(struct sc_gui_app *app, struct sc_gui_transfer_update *u) {
    sc_gui_update_transfer(app->gui, u->id, u->type, NULL, u->state,
                           u->detail[0] ? u->detail : NULL);

    bool finished = u->state == SC_GUI_TRANSFER_DONE
                 || u->state == SC_GUI_TRANSFER_FAILED
                 || u->state == SC_GUI_TRANSFER_CANCELED;
    if (finished) {
        if (u->state == SC_GUI_TRANSFER_DONE) {
            if (u->type == SC_GUI_TRANSFER_INSTALL) {
                ++app->batch_installed;
            } else {
                // copies and downloads
                ++app->batch_copied;
            }
        } else if (u->state == SC_GUI_TRANSFER_FAILED) {
            ++app->batch_failed;
        }
        assert(app->active_transfers);
        --app->active_transfers;
    }
    free(u);

    if (finished && !app->active_transfers) {
        // The batch is complete: summarize
        char msg[256];
        unsigned total = app->batch_installed + app->batch_copied
                       + app->batch_failed;
        if (app->batch_failed) {
            snprintf(msg, sizeof(msg), "%u of %u transfers failed. See "
                     "Transfers for details.", app->batch_failed, total);
            sc_gui_show_toast(app->gui, SC_GUI_TOAST_ERROR, msg);
        } else if (app->batch_installed || app->batch_copied) {
            if (app->batch_installed && app->batch_copied) {
                snprintf(msg, sizeof(msg), "Installed %u app(s) and "
                         "transferred %u file(s).", app->batch_installed,
                         app->batch_copied);
            } else if (app->batch_installed) {
                snprintf(msg, sizeof(msg), "Installed %u app(s).",
                         app->batch_installed);
            } else {
                snprintf(msg, sizeof(msg), "Transferred %u file(s).",
                         app->batch_copied);
            }
            sc_gui_show_toast(app->gui, SC_GUI_TOAST_SUCCESS, msg);
        }
        app->batch_installed = 0;
        app->batch_copied = 0;
        app->batch_failed = 0;
    }
}

static void
cancel_transfer(struct sc_gui_app *app, uint32_t id) {
    sc_mutex_lock(&app->mutex);
    if (app->running_transfer == id) {
        // The transfer thread reports the cancellation
        sc_intr_interrupt(&app->transfer_intr);
        sc_mutex_unlock(&app->mutex);
        return;
    }

    struct sc_gui_transfer_job *removed = NULL;
    for (struct sc_gui_transfer_job **p = &app->jobs; *p; p = &(*p)->next) {
        if ((*p)->id == id) {
            removed = *p;
            *p = removed->next;
            break;
        }
    }
    sc_mutex_unlock(&app->mutex);

    if (removed) {
        struct sc_gui_transfer_update *u = calloc(1, sizeof(*u));
        if (u) {
            u->id = id;
            u->type = removed->type;
            u->state = SC_GUI_TRANSFER_CANCELED;
            on_transfer_update(app, u);
        }
        sc_gui_transfer_job_destroy(removed);
    }
}

// ---------------------------------------------------------------- rendering

static void
render(struct sc_gui_app *app) {
    uint32_t rgb = sc_gui_get_background_color(app->gui);
    SDL_SetRenderDrawColor(app->renderer, (rgb >> 16) & 0xFF,
                           (rgb >> 8) & 0xFF, rgb & 0xFF, 0xFF);
    sc_sdl_render_clear(app->renderer);
    sc_gui_render(app->gui, NULL);
    sc_sdl_render_present(app->renderer);
}

static struct sc_size
get_reserved_size(struct sc_gui_insets insets) {
    struct sc_size size = {
        .width = (uint16_t) (insets.left + insets.right + 0.5f),
        .height = (uint16_t) (insets.top + insets.bottom + 0.5f),
    };
    return size;
}

static void
update_minimum_size(struct sc_gui_app *app) {
    float min_w;
    float min_h;
    sc_gui_get_minimum_area(app->gui, &min_w, &min_h);
    struct sc_size reserved = get_reserved_size(sc_gui_get_insets(app->gui));
    SDL_SetWindowMinimumSize(app->window, reserved.width + (int) min_w,
                             reserved.height + (int) min_h);
}

// The insets changed while there is no screen: resize the window so that the
// (empty) video area keeps its size
static void
apply_insets(struct sc_gui_app *app) {
    struct sc_size old_reserved = get_reserved_size(app->insets);
    app->insets = sc_gui_get_insets(app->gui);
    struct sc_size new_reserved = get_reserved_size(app->insets);

    update_minimum_size(app);

    SDL_WindowFlags flags = SDL_GetWindowFlags(app->window);
    if (!(flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED))) {
        int32_t dw = (int32_t) new_reserved.width - old_reserved.width;
        int32_t dh = (int32_t) new_reserved.height - old_reserved.height;
        if (dw || dh) {
            struct sc_size size = sc_sdl_get_window_size(app->window);
            struct sc_point pos = sc_sdl_get_window_position(app->window);
            size.width = MAX(1, (int32_t) size.width + dw);
            size.height = MAX(1, (int32_t) size.height + dh);
            pos.x -= dw;
            sc_sdl_set_window_size(app->window, size);
            sc_sdl_set_window_position(app->window, pos);
        }
    }

    render(app);
}

// Process an event for the GUI while there is no screen
static void
handle_gui_event(struct sc_gui_app *app, const SDL_Event *event) {
    sc_gui_handle_event(app->gui, event, NULL);
    bool redraw = sc_gui_take_redraw_request(app->gui);
    switch (event->type) {
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            redraw = true;
            break;
        default:
            break;
    }
    if (redraw) {
        render(app);
    }
}

#if defined(__APPLE__) || defined(_WIN32)
// On Windows and macOS, resizing blocks the event loop: redraw from an event
// watcher (the screen has its own watcher while it exists)
static bool SDLCALL
resize_watcher(void *data, SDL_Event *event) {
    struct sc_gui_app *app = data;
    if (!app->screen && (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED
                         || event->type == SDL_EVENT_WINDOW_RESIZED)) {
        render(app);
    }
    return true;
}
#endif

// ------------------------------------------------------------------ devices

static const struct sc_adb_device *
find_device(struct sc_gui_app *app, const char *serial) {
    for (size_t i = 0; i < app->devices.size; ++i) {
        const struct sc_adb_device *d = &app->devices.data[i];
        if (str_eq(d->serial, serial)) {
            return d;
        }
    }
    return NULL;
}

static bool
is_ready(const struct sc_adb_device *d) {
    return d && str_eq(d->state, "device");
}

static const char *
device_name(struct sc_gui_app *app, const char *serial) {
    const struct sc_adb_device *d = find_device(app, serial);
    return d && d->model ? d->model : serial;
}

static void
maybe_auto_start(struct sc_gui_app *app) {
    if (app->session_serial || app->start_serial) {
        return;
    }

    // Reconnect to the last device first
    if (app->reconnect_serial) {
        if (is_ready(find_device(app, app->reconnect_serial))) {
            LOGI("Reconnecting to %s", app->reconnect_serial);
            app->start_serial = app->reconnect_serial;
            app->reconnect_serial = NULL;
        }
        return;
    }

    // Requested on the command line (-s)
    const char *requested = app->options->serial;
    if (requested && !str_eq(requested, app->blocked_serial)) {
        if (is_ready(find_device(app, requested))) {
            replace_string(&app->start_serial, requested);
        }
        return;
    }

    // A single device is ready: mirror it
    const struct sc_adb_device *single = NULL;
    for (size_t i = 0; i < app->devices.size; ++i) {
        const struct sc_adb_device *d = &app->devices.data[i];
        if (is_ready(d)) {
            if (single) {
                return; // several devices, let the user choose
            }
            single = d;
        }
    }
    if (single && !str_eq(single->serial, app->blocked_serial)) {
        replace_string(&app->start_serial, single->serial);
    }
}

static void
on_devices(struct sc_gui_app *app, struct sc_vec_adb_devices *vec) {
    sc_adb_devices_destroy(&app->devices);
    app->devices = *vec;
    free(vec);
    app->devices_received = true;

    if (app->blocked_serial && !find_device(app, app->blocked_serial)) {
        // The device was unplugged: it may be auto-started again
        replace_string(&app->blocked_serial, NULL);
    }

    size_t count = app->devices.size;
    struct sc_gui_device *list = count ? calloc(count, sizeof(*list)) : NULL;
    if (count && !list) {
        LOG_OOM();
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        const struct sc_adb_device *d = &app->devices.data[i];
        list[i].serial = d->serial;
        list[i].model = d->model;
        list[i].state = d->state;
        list[i].tcpip =
            sc_adb_device_get_type(d->serial) == SC_ADB_DEVICE_TYPE_TCPIP;
    }
    sc_gui_set_devices(app->gui, list, count);
    free(list);

    for (size_t i = 0; i < count; ++i) {
        LOGV("Device: %s [%s]", app->devices.data[i].serial,
             app->devices.data[i].state);
    }
    LOGV("Devices: %u (session=%s reconnect=%s blocked=%s)", (unsigned) count,
         app->session_serial ? app->session_serial : "-",
         app->reconnect_serial ? app->reconnect_serial : "-",
         app->blocked_serial ? app->blocked_serial : "-");

    maybe_auto_start(app);
    if (app->start_serial && app->session_serial) {
        // Should not happen (auto-start only when idle)
        replace_string(&app->start_serial, NULL);
    }
}

static void
on_file_task_done(struct sc_gui_app *app, struct sc_gui_task *task) {
    switch (task->type) {
        case SC_GUI_TASK_LIST_DIR: {
            struct sc_gui_listing *l = &task->listing;
            struct sc_gui_file *files =
                l->count ? calloc(l->count, sizeof(*files)) : NULL;
            if (l->count && !files) {
                LOG_OOM();
                break;
            }
            for (size_t i = 0; i < l->count; ++i) {
                const struct sc_gui_file_entry *e = &l->entries[i];
                files[i].name = e->name;
                files[i].mtime = e->mtime;
                files[i].size = e->size;
                files[i].type = e->type == 'd' ? SC_GUI_FILE_DIRECTORY
                              : e->type == 'l' ? SC_GUI_FILE_LINK
                              : e->type == '-' ? SC_GUI_FILE_REGULAR
                              : SC_GUI_FILE_OTHER;
            }
            const char *error = l->error;
            if (!task->success && !error) {
                error = "Could not list the folder.";
            }
            sc_gui_set_listing(app->gui, task->path, files, l->count, error);
            free(files);
            break;
        }
        case SC_GUI_TASK_DELETE:
        case SC_GUI_TASK_MKDIR:
            if (!task->success) {
                char msg[320];
                snprintf(msg, sizeof(msg), "%s: %s",
                         task->type == SC_GUI_TASK_DELETE
                             ? "Could not delete"
                             : "Could not create the folder",
                         task->error_buf[0] ? task->error_buf : "error");
                sc_gui_show_toast(app->gui, SC_GUI_TOAST_ERROR, msg);
            }
            sc_gui_files_changed(app->gui);
            break;
        default:
            assert(!"unexpected task");
    }
    sc_gui_task_destroy(task);
}

static void
on_task_done(struct sc_gui_app *app, struct sc_gui_task *task) {
    if (is_file_task(task->type)) {
        on_file_task_done(app, task);
        return;
    }

    sc_gui_set_task_busy(app->gui, false);

    char msg[256];
    switch (task->type) {
        case SC_GUI_TASK_SWITCH_TO_WIFI:
            if (task->success) {
                snprintf(msg, sizeof(msg),
                         "Connected over Wi-Fi (%s). You can unplug the cable.",
                         task->address);
                sc_gui_show_toast(app->gui, SC_GUI_TOAST_SUCCESS, msg);
                // adbd restarted in TCP/IP mode, which terminates the USB
                // session: continue over Wi-Fi
                if (str_eq(task->serial, app->session_serial)
                        || str_eq(task->serial, app->reconnect_serial)) {
                    replace_string(&app->reconnect_serial, task->address);
                }
            }
            break;
        case SC_GUI_TASK_PAIR:
            if (task->success) {
                sc_gui_show_toast(app->gui, SC_GUI_TOAST_SUCCESS,
                                  "Paired. Now connect with the IP address and "
                                  "port shown under Wireless debugging.");
            }
            break;
        case SC_GUI_TASK_CONNECT:
            if (task->success) {
                snprintf(msg, sizeof(msg), "Connected to %s.", task->address);
                sc_gui_show_toast(app->gui, SC_GUI_TOAST_SUCCESS, msg);
            }
            break;
        case SC_GUI_TASK_DISCONNECT:
            if (task->success) {
                snprintf(msg, sizeof(msg), "Disconnected %s.", task->address);
                sc_gui_show_toast(app->gui, SC_GUI_TOAST_INFO, msg);
            }
            break;
        default:
            break;
    }

    if (!task->success) {
        sc_gui_show_toast(app->gui, SC_GUI_TOAST_ERROR,
                          task->error ? task->error : "The operation failed.");
    }

    sc_gui_task_destroy(task);
    request_refresh(app);
}

// Handle the events owned by the app. Return true if consumed.
static bool
handle_app_event(struct sc_gui_app *app, const SDL_Event *event) {
    switch (event->type) {
        case SC_EVENT_GUI_DEVICES:
            on_devices(app, event->user.data1);
            return true;
        case SC_EVENT_GUI_TASK_DONE:
            on_task_done(app, event->user.data1);
            return true;
        case SC_EVENT_GUI_TRANSFER:
            on_transfer_update(app, event->user.data1);
            return true;
        case SC_EVENT_GUI_FOLDER_CHOSEN: {
            struct sc_gui_pull_request *req = event->user.data1;
            for (size_t i = 0; i < req->count; ++i) {
                enqueue_pull(app, req->paths[i], req->dir);
            }
            sc_gui_pull_request_destroy(req);
            return true;
        }
        default:
            return false;
    }
}

// ------------------------------------------------------------------ capture

#ifdef _WIN32
# define SC_PATH_SEP "\\"
#else
# define SC_PATH_SEP "/"
#endif

// Return "<user folder>/scrcpy/<prefix>_YYYYMMDD_HHMMSS.<ext>" (to be freed),
// creating the scrcpy folder if needed
static char *
make_capture_path(SDL_Folder folder, const char *prefix, const char *ext) {
    const char *base = SDL_GetUserFolder(folder);
    if (!base) {
        base = SDL_GetUserFolder(SDL_FOLDER_HOME);
    }
    if (!base) {
        LOGE("Could not find a folder to save to: %s", SDL_GetError());
        return NULL;
    }

    char dir[1024];
    snprintf(dir, sizeof(dir), "%sscrcpy" SC_PATH_SEP, base);
    if (!SDL_CreateDirectory(dir)) {
        LOGW("Could not create %s: %s", dir, SDL_GetError());
        return NULL;
    }

    SDL_Time now;
    SDL_DateTime dt;
    if (!SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &dt, true)) {
        memset(&dt, 0, sizeof(dt));
    }

    char path[1200];
    snprintf(path, sizeof(path), "%s%s_%04d%02d%02d_%02d%02d%02d.%s", dir,
             prefix, dt.year, dt.month, dt.day, dt.hour, dt.minute,
             dt.second, ext);
    return strdup(path);
}

#ifdef _WIN32
// Copy the image to the Windows clipboard (CF_DIB)
static bool
copy_image_to_clipboard(SDL_Surface *surface) {
    SDL_Surface *bgra = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_BGRA32);
    if (!bgra) {
        return false;
    }

    int w = bgra->w;
    int h = bgra->h;
    size_t row = (size_t) w * 4;
    size_t size = sizeof(BITMAPINFOHEADER) + row * h;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem) {
        SDL_DestroySurface(bgra);
        return false;
    }

    uint8_t *data = GlobalLock(mem);
    BITMAPINFOHEADER *header = (BITMAPINFOHEADER *) data;
    memset(header, 0, sizeof(*header));
    header->biSize = sizeof(*header);
    header->biWidth = w;
    header->biHeight = h; // bottom-up
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = (DWORD) (row * h);
    uint8_t *pixels = data + sizeof(*header);
    for (int y = 0; y < h; ++y) {
        memcpy(pixels + row * (h - 1 - y),
               (uint8_t *) bgra->pixels + (size_t) bgra->pitch * y, row);
    }
    GlobalUnlock(mem);
    SDL_DestroySurface(bgra);

    bool ok = false;
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        ok = SetClipboardData(CF_DIB, mem) != NULL;
        CloseClipboard();
    }
    if (!ok) {
        GlobalFree(mem);
    }
    return ok;
}
#else
static bool
copy_image_to_clipboard(SDL_Surface *surface) {
    (void) surface;
    return false;
}
#endif

static void
take_screenshot(struct sc_gui_app *app) {
    if (!app->screen) {
        return;
    }

    SDL_Surface *surface = sc_screen_capture(app->screen);
    if (!surface) {
        sc_gui_show_toast(app->gui, SC_GUI_TOAST_ERROR,
                          "Could not capture the screen.");
        return;
    }
    sc_gui_flash(app->gui);

    char *path = make_capture_path(SDL_FOLDER_PICTURES, "Screenshot", "png");
    bool saved = path && SDL_SavePNG(surface, path);
    bool copied = copy_image_to_clipboard(surface);
    SDL_DestroySurface(surface);

    char msg[1300];
    if (saved) {
        snprintf(msg, sizeof(msg), "Screenshot saved to %s%s", path,
                 copied ? " and copied to the clipboard." : ".");
        sc_gui_show_toast(app->gui, SC_GUI_TOAST_SUCCESS, msg);
        LOGI("Screenshot saved: %s", path);
    } else {
        LOGE("Could not save the screenshot: %s", SDL_GetError());
        sc_gui_show_toast(app->gui, SC_GUI_TOAST_ERROR,
                          copied ? "Could not save the screenshot (it was "
                                   "copied to the clipboard)."
                                 : "Could not save the screenshot.");
    }
    free(path);
}

// ---------------------------------------------------------- GUI callbacks

static void
cb_on_action(enum sc_gui_action action, void *userdata) {
    struct sc_gui_app *app = userdata;
    if (app->screen) {
        sc_screen_handle_gui_action(app->screen, action);
    }
}

static void
cb_on_insets_changed(void *userdata) {
    struct sc_gui_app *app = userdata;
    if (app->screen) {
        sc_screen_handle_gui_insets_changed(app->screen);
        app->insets = sc_gui_get_insets(app->gui);
    } else {
        apply_insets(app);
    }
}

static void
cb_on_start_session(const char *serial, void *userdata) {
    struct sc_gui_app *app = userdata;
    replace_string(&app->start_serial, serial);
    replace_string(&app->reconnect_serial, NULL);
    if (app->session_serial) {
        // Stop the current session first, the new one starts after
        app->stop_requested = true;
        sc_push_event(SC_EVENT_GUI_STOP_SESSION);
    }
}

static void
cb_on_stop_session(void *userdata) {
    struct sc_gui_app *app = userdata;
    replace_string(&app->reconnect_serial, NULL);
    if (app->session_serial) {
        app->stop_requested = true;
        sc_push_event(SC_EVENT_GUI_STOP_SESSION);
    }
}

static void
cb_on_switch_to_wifi(const char *serial, void *userdata) {
    struct sc_gui_app *app = userdata;
    submit_task(app, SC_GUI_TASK_SWITCH_TO_WIFI, serial, NULL, NULL);
}

static void
cb_on_pair(const char *address, const char *code, void *userdata) {
    struct sc_gui_app *app = userdata;
    submit_task(app, SC_GUI_TASK_PAIR, NULL, address, code);
}

static void
cb_on_connect(const char *address, void *userdata) {
    struct sc_gui_app *app = userdata;
    submit_task(app, SC_GUI_TASK_CONNECT, NULL, address, NULL);
}

static void
cb_on_disconnect(const char *address, void *userdata) {
    struct sc_gui_app *app = userdata;
    if (str_eq(address, app->session_serial)) {
        cb_on_stop_session(app);
    }
    submit_task(app, SC_GUI_TASK_DISCONNECT, NULL, address, NULL);
}

static void
cb_on_profile_changed(const struct sc_gui_profile *profile, bool restart,
                      void *userdata) {
    struct sc_gui_app *app = userdata;
    const char *serial = app->session_serial;

    bool was_screen_off =
        sc_gui_config_get_profile(&app->config, serial)->turn_screen_off;
    sc_gui_config_set_profile(&app->config, serial, profile);
    save_config(app);

    if (app->screen && profile->turn_screen_off != was_screen_off) {
        sc_screen_handle_gui_action(app->screen, profile->turn_screen_off
                                                     ? SC_GUI_ACTION_SCREEN_OFF
                                                     : SC_GUI_ACTION_SCREEN_ON);
    }

    if (restart && serial) {
        // Restart the session on the same device
        char *copy = strdup(serial);
        if (copy) {
            cb_on_start_session(copy, app);
            free(copy);
        }
    }
}

static void
cb_on_drop_files(const char *const *paths, size_t count,
                 const char *remote_dir, void *userdata) {
    struct sc_gui_app *app = userdata;
    if (!app->session_serial) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        enqueue_transfer(app, paths[i], remote_dir);
    }
}

static void
cb_on_cancel_transfer(uint32_t id, void *userdata) {
    struct sc_gui_app *app = userdata;
    cancel_transfer(app, id);
}

static void
cb_on_list_dir(const char *path, void *userdata) {
    struct sc_gui_app *app = userdata;
    struct sc_gui_task *task = new_file_task(app, SC_GUI_TASK_LIST_DIR, path);
    if (task) {
        enqueue_task(app, task);
    } else {
        sc_gui_set_listing(app->gui, path, NULL, 0, "No device");
    }
}

static void
cb_on_create_dir(const char *path, void *userdata) {
    struct sc_gui_app *app = userdata;
    struct sc_gui_task *task = new_file_task(app, SC_GUI_TASK_MKDIR, path);
    if (task) {
        enqueue_task(app, task);
    }
}

static void
cb_on_delete_files(const char *const *paths, size_t count, void *userdata) {
    struct sc_gui_app *app = userdata;
    struct sc_gui_task *task = new_file_task(app, SC_GUI_TASK_DELETE, NULL);
    if (!task) {
        return;
    }
    task->paths = calloc(count, sizeof(*task->paths));
    if (!task->paths) {
        LOG_OOM();
        sc_gui_task_destroy(task);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        task->paths[i] = strdup(paths[i]);
        if (!task->paths[i]) {
            LOG_OOM();
            sc_gui_task_destroy(task);
            return;
        }
        task->path_count = i + 1;
    }
    enqueue_task(app, task);
}

static void
cb_on_pull_files(const char *const *paths, size_t count, bool choose_folder,
                 void *userdata) {
    struct sc_gui_app *app = userdata;
    if (!choose_folder) {
        const char *dir = get_downloads_folder();
        for (size_t i = 0; i < count; ++i) {
            enqueue_pull(app, paths[i], dir);
        }
        return;
    }

    struct sc_gui_pull_request *req = calloc(1, sizeof(*req));
    if (!req) {
        LOG_OOM();
        return;
    }
    req->paths = calloc(count, sizeof(*req->paths));
    if (!req->paths) {
        LOG_OOM();
        free(req);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        req->paths[i] = strdup(paths[i]);
        if (!req->paths[i]) {
            LOG_OOM();
            sc_gui_pull_request_destroy(req);
            return;
        }
        req->count = i + 1;
    }
    SDL_ShowOpenFolderDialog(on_folder_chosen, req, app->window,
                             get_downloads_folder(), false);
}

static void
cb_on_screenshot(void *userdata) {
    struct sc_gui_app *app = userdata;
    take_screenshot(app);
}

static void
cb_on_toggle_recording(void *userdata) {
    struct sc_gui_app *app = userdata;
    if (!app->session_serial) {
        return;
    }

    if (app->recording_path) {
        // Stop: restart without recording (the file is finalized when the
        // session ends)
        replace_string(&app->record_next, NULL);
    } else {
        free(app->record_next);
        app->record_next =
            make_capture_path(SDL_FOLDER_VIDEOS, "Recording", "mp4");
        if (!app->record_next) {
            sc_gui_show_toast(app->gui, SC_GUI_TOAST_ERROR,
                              "Could not create the recording file.");
            return;
        }
    }

    // The recorder is attached when a session starts: restart it
    char *serial = strdup(app->session_serial);
    if (serial) {
        cb_on_start_session(serial, app);
        free(serial);
    }
}

static void
cb_on_prefs_changed(const struct sc_gui_prefs *prefs, void *userdata) {
    struct sc_gui_app *app = userdata;
    app->config.prefs = *prefs;
    save_config(app);
}

// ------------------------------------------------------------ session host

static bool
host_handle_event(const SDL_Event *event, bool has_screen, void *userdata) {
    struct sc_gui_app *app = userdata;
    if (handle_app_event(app, event)) {
        return true;
    }
    if (!has_screen) {
        // Waiting for the server: keep the GUI alive. Do not consume the
        // events, the session must still see quit/stop/server events.
        handle_gui_event(app, event);
    }
    return false;
}

static void
host_on_screen(struct sc_screen *screen, void *userdata) {
    struct sc_gui_app *app = userdata;
    app->screen = screen;
    if (screen) {
        sc_gui_set_session(app->gui, SC_GUI_SESSION_RUNNING,
                           app->session_serial,
                           device_name(app, app->session_serial),
                           app->session_options.control);
        sc_gui_set_recording(app->gui, app->recording_path != NULL);
    }
}

static void
run_session(struct sc_gui_app *app) {
    char *serial = app->start_serial;
    app->start_serial = NULL;
    assert(serial);

    const char *kept = keep_string(app, serial);
    free(serial);
    if (!kept) {
        return;
    }

    char name[128];
    snprintf(name, sizeof(name), "%s", device_name(app, kept));

    app->session_serial = kept;
    app->stop_requested = false;
    sc_gui_set_session(app->gui, SC_GUI_SESSION_CONNECTING, kept, name,
                       false);
    render(app);

    struct scrcpy_gui_host host = {
        .window = app->window,
        .renderer = app->renderer,
        .gui = app->gui,
        .handle_event = host_handle_event,
        .on_screen = host_on_screen,
        .userdata = app,
        .quit_requested = false,
    };

    // Options for this session: command line + device profile
    const struct sc_gui_profile *profile =
        sc_gui_config_get_profile(&app->config, kept);
    sc_gui_set_profile(app->gui, profile, name);

    struct scrcpy_options *options = &app->session_options;
    *options = *app->options;
    sc_gui_profile_apply(profile, options);
    if (*profile->extra_args) {
        struct scrcpy_options with_extra = *options;
        if (apply_extra_args(app, profile->extra_args, &with_extra)) {
            *options = with_extra;
        } else {
            sc_gui_show_toast(app->gui, SC_GUI_TOAST_ERROR,
                              "Invalid additional arguments (ignored). See "
                              "Settings > Additional arguments.");
        }
    }
    options->serial = kept;
    options->window = true;

    app->recording_path = NULL;
    if (app->record_next) {
        // Consumed: a reconnection does not record (it would overwrite)
        const char *record = keep_string(app, app->record_next);
        replace_string(&app->record_next, NULL);
        if (record) {
            options->record_filename = record;
            options->record_format = SC_RECORD_FORMAT_MP4;
            app->recording_path = record;
            LOGI("Recording to %s", record);
        }
    }

    sc_strncpy(app->config.last_serial, kept, sizeof(app->config.last_serial));
    save_config(app);

    LOGI("Starting mirroring session: %s", kept);
    sc_main_thread_resume();
    enum scrcpy_exit_code ret = scrcpy_run(options, &host);

    app->screen = NULL;
    app->session_serial = NULL;
    app->insets = sc_gui_get_insets(app->gui);
    sc_gui_set_session(app->gui, SC_GUI_SESSION_NONE, NULL, NULL, false);
    sc_gui_set_profile(app->gui, &app->config.defaults, "Default");

    if (app->recording_path) {
        char rec_msg[1100];
        snprintf(rec_msg, sizeof(rec_msg), "Recording saved to %s",
                 app->recording_path);
        sc_gui_show_toast(app->gui, SC_GUI_TOAST_SUCCESS, rec_msg);
        app->recording_path = NULL;
    }
    sc_gui_set_recording(app->gui, false);

    if (host.quit_requested) {
        app->quit = true;
        return;
    }

    char msg[256];
    if (app->stop_requested) {
        // Stopped by the user: do not auto-start this device again until it
        // is unplugged
        replace_string(&app->blocked_serial, kept);
    } else if (ret == SCRCPY_EXIT_DISCONNECTED) {
        snprintf(msg, sizeof(msg), "%s disconnected. It will reconnect "
                 "automatically when available.", name);
        sc_gui_show_toast(app->gui, SC_GUI_TOAST_INFO, msg);
        if (!app->reconnect_serial) {
            replace_string(&app->reconnect_serial, kept);
        }
    } else if (ret == SCRCPY_EXIT_FAILURE) {
        snprintf(msg, sizeof(msg), "Could not mirror %s. Check that the "
                 "device is unlocked and USB debugging is allowed.", name);
        sc_gui_show_toast(app->gui, SC_GUI_TOAST_ERROR, msg);
        replace_string(&app->blocked_serial, kept);
    }

    if (!app->start_serial) {
        // Let the device list decide what to do next
        request_refresh(app);
    }
    render(app);
}

// -------------------------------------------------------------------- main

#ifdef _WIN32
static BOOL WINAPI
windows_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
        sc_push_event(SDL_EVENT_QUIT);
        return TRUE;
    }
    return FALSE;
}
#endif

static bool
create_window(struct sc_gui_app *app) {
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
                          | SDL_WINDOW_HIDDEN;
    if (app->options->always_on_top) {
        flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    }
    app->window = SDL_CreateWindow("scrcpy-gui", 1024, 768, flags);
    if (!app->window) {
        LOGE("Could not create window: %s", SDL_GetError());
        return false;
    }

    app->renderer = SDL_CreateRenderer(app->window, NULL);
    if (!app->renderer) {
        LOGE("Could not create renderer: %s", SDL_GetError());
        SDL_DestroyWindow(app->window);
        return false;
    }
    LOGI("Renderer: %s", SDL_GetRendererName(app->renderer));

    if (!SDL_StartTextInput(app->window)) {
        LOGW("Could not enable text input: %s", SDL_GetError());
    }

    SDL_Surface *icon = sc_icon_load(SC_ICON_FILENAME_SCRCPY);
    if (icon) {
        SDL_SetWindowIcon(app->window, icon);
        sc_icon_destroy(icon);
    }

    return true;
}

static void
show_window(struct sc_gui_app *app) {
    app->insets = sc_gui_get_insets(app->gui);
    struct sc_size reserved = get_reserved_size(app->insets);
    float w;
    float h;
    sc_gui_get_default_area(app->gui, &w, &h);

    // Fit the display
    SDL_Rect bounds;
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    if (display && SDL_GetDisplayUsableBounds(display, &bounds)) {
        int max_h = bounds.h - 96 - reserved.height;
        if (max_h > 0 && h > max_h) {
            w = w * max_h / h;
            h = max_h;
        }
    }

    struct sc_size size = {
        .width = reserved.width + (uint16_t) w,
        .height = reserved.height + (uint16_t) h,
    };
    sc_sdl_set_window_size(app->window, size);
    update_minimum_size(app);
    SDL_SetWindowPosition(app->window, SDL_WINDOWPOS_CENTERED,
                          SDL_WINDOWPOS_CENTERED);
    sc_sdl_show_window(app->window);
    render(app);
}

static void
drain_app_events(void) {
    SDL_Event event;
    while (sc_dequeue_event(SC_EVENT_GUI_DEVICES, &event)) {
        struct sc_vec_adb_devices *vec = event.user.data1;
        sc_adb_devices_destroy(vec);
        free(vec);
    }
    while (sc_dequeue_event(SC_EVENT_GUI_TASK_DONE, &event)) {
        sc_gui_task_destroy(event.user.data1);
    }
    while (sc_dequeue_event(SC_EVENT_GUI_TRANSFER, &event)) {
        free(event.user.data1);
    }
    while (sc_dequeue_event(SC_EVENT_GUI_FOLDER_CHOSEN, &event)) {
        sc_gui_pull_request_destroy(event.user.data1);
    }
}

enum scrcpy_exit_code
scrcpy_gui(struct scrcpy_options *options) {
    static struct sc_gui_app app_storage;
    struct sc_gui_app *app = &app_storage;
    memset(app, 0, sizeof(*app));
    app->options = options;
    sc_vector_init(&app->devices);

    enum scrcpy_exit_code ret = SCRCPY_EXIT_FAILURE;

    // The GUI always needs a window and a video stream
    options->window = true;

    if (!SDL_Init(SDL_INIT_EVENTS | SDL_INIT_VIDEO)) {
        LOGE("Could not initialize SDL: %s", SDL_GetError());
        return SCRCPY_EXIT_FAILURE;
    }
    atexit(SDL_Quit);

#ifdef _WIN32
    if (!SetConsoleCtrlHandler(windows_ctrl_handler, TRUE)) {
        LOGW("Could not set Ctrl+C handler");
    }
#endif

    sc_sdl_set_hints(options->render_driver, options->disable_screensaver);

    sc_gui_config_init(&app->config);
    sc_gui_config_load(&app->config);

    // adb path from the settings (the ADB environment variable wins)
    if (*app->config.prefs.adb_path && !SDL_getenv("ADB")) {
#ifdef _WIN32
        _putenv_s("ADB", app->config.prefs.adb_path);
#else
        setenv("ADB", app->config.prefs.adb_path, 0);
#endif
    }

    if (!sc_adb_init()) {
        return SCRCPY_EXIT_FAILURE;
    }

    if (!create_window(app)) {
        goto end_adb;
    }

    static const struct sc_gui_callbacks cbs = {
        .on_action = cb_on_action,
        .on_insets_changed = cb_on_insets_changed,
        .on_start_session = cb_on_start_session,
        .on_stop_session = cb_on_stop_session,
        .on_switch_to_wifi = cb_on_switch_to_wifi,
        .on_pair = cb_on_pair,
        .on_connect = cb_on_connect,
        .on_disconnect = cb_on_disconnect,
        .on_profile_changed = cb_on_profile_changed,
        .on_prefs_changed = cb_on_prefs_changed,
        .on_drop_files = cb_on_drop_files,
        .on_cancel_transfer = cb_on_cancel_transfer,
        .on_list_dir = cb_on_list_dir,
        .on_delete_files = cb_on_delete_files,
        .on_create_dir = cb_on_create_dir,
        .on_pull_files = cb_on_pull_files,
        .on_screenshot = cb_on_screenshot,
        .on_toggle_recording = cb_on_toggle_recording,
    };
    struct sc_gui_params gui_params = {
        .window = app->window,
        .renderer = app->renderer,
        .sdl_shortcut_mods = sc_shortcut_mods_to_sdl(options->shortcut_mods),
        .redraw_event_type = SC_EVENT_GUI_REDRAW,
        .cbs = &cbs,
        .cbs_userdata = app,
    };
    app->gui = sc_gui_create(&gui_params);
    if (!app->gui) {
        LOGE("Could not initialize the GUI");
        goto end_window;
    }

    if (!sc_mutex_init(&app->mutex)) {
        goto end_gui;
    }
    if (!sc_cond_init(&app->cond)) {
        goto end_mutex;
    }
    if (!sc_intr_init(&app->intr)) {
        goto end_cond;
    }
    if (!sc_cond_init(&app->transfer_cond)) {
        goto end_intr;
    }
    if (!sc_intr_init(&app->transfer_intr)) {
        goto end_transfer_cond;
    }
    if (!sc_thread_create(&app->worker, run_worker, "gui-worker",
                          app)) {
        LOGE("Could not start the device monitor");
        goto end_transfer_intr;
    }
    if (!sc_thread_create(&app->transfer_thread, run_transfer_thread,
                          "gui-transfer", app)) {
        LOGE("Could not start the transfer thread");
        sc_mutex_lock(&app->mutex);
        app->stopped = true;
        sc_cond_signal(&app->cond);
        sc_mutex_unlock(&app->mutex);
        sc_intr_interrupt(&app->intr);
        sc_thread_join(&app->worker, NULL);
        goto end_transfer_intr;
    }

#if defined(__APPLE__) || defined(_WIN32)
    SDL_AddEventWatch(resize_watcher, app);
#endif

    sc_gui_set_prefs(app->gui, &app->config.prefs);
    sc_gui_set_profile(app->gui, &app->config.defaults, "Default");

    show_window(app);

    while (!app->quit) {
        if (app->start_serial) {
            run_session(app);
            continue;
        }

        SDL_Event event;
        if (!SDL_WaitEvent(&event)) {
            LOGE("SDL_WaitEvent() error: %s", SDL_GetError());
            break;
        }

        if (event.type == SDL_EVENT_QUIT) {
            break;
        }
        if (handle_app_event(app, &event)) {
            continue;
        }
        handle_gui_event(app, &event);
    }

    ret = SCRCPY_EXIT_SUCCESS;

#if defined(__APPLE__) || defined(_WIN32)
    SDL_RemoveEventWatch(resize_watcher, app);
#endif

    sc_mutex_lock(&app->mutex);
    app->stopped = true;
    sc_cond_signal(&app->cond);
    sc_cond_signal(&app->transfer_cond);
    sc_intr_interrupt(&app->transfer_intr);
    sc_mutex_unlock(&app->mutex);
    sc_intr_interrupt(&app->intr);
    sc_thread_join(&app->worker, NULL);
    sc_thread_join(&app->transfer_thread, NULL);

    while (app->jobs) {
        struct sc_gui_transfer_job *job = app->jobs;
        app->jobs = job->next;
        sc_gui_transfer_job_destroy(job);
    }

    while (app->tasks) {
        struct sc_gui_task *task = app->tasks;
        app->tasks = task->next;
        sc_gui_task_destroy(task);
    }
    drain_app_events();

end_transfer_intr:
    sc_intr_destroy(&app->transfer_intr);
end_transfer_cond:
    sc_cond_destroy(&app->transfer_cond);
end_intr:
    sc_intr_destroy(&app->intr);
end_cond:
    sc_cond_destroy(&app->cond);
end_mutex:
    sc_mutex_destroy(&app->mutex);
end_gui:
    sc_gui_destroy(app->gui);
end_window:
    SDL_DestroyRenderer(app->renderer);
    SDL_DestroyWindow(app->window);
end_adb:
    sc_adb_destroy();

    sc_gui_config_destroy(&app->config);
    sc_adb_devices_destroy(&app->devices);
    free(app->start_serial);
    free(app->reconnect_serial);
    free(app->blocked_serial);
    free(app->record_next);
    // All the sessions are terminated, the serials are not referenced anymore
    for (size_t i = 0; i < app->kept_count; ++i) {
        free(app->kept_strings[i]);
    }
    free(app->kept_strings);

    return ret;
}
