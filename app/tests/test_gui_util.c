#include "common.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "gui_util.h"

static void test_adb_error_install(void) {
    char detail[128];

    // Real output of "adb install" with an invalid APK
    sc_gui_adb_error("Performing Incremental Install\r\n"
                     "Performing Streamed Install\r\n"
                     "adb.exe: failed to install C:/x/fake.apk: Failure "
                     "[INSTALL_PARSE_FAILED_NOT_APK: Failed to parse "
                     "/data/app/vmdl.tmp/base.apk]\r\n",
                     detail, sizeof(detail));
    assert(!strcmp(detail, "Not a valid APK"));

    sc_gui_adb_error("Failure [INSTALL_FAILED_VERSION_DOWNGRADE]",
                     detail, sizeof(detail));
    assert(!strcmp(detail, "A newer version is already installed"));

    // Unknown code: the code itself
    sc_gui_adb_error("Failure [INSTALL_FAILED_SOMETHING_NEW: details]",
                     detail, sizeof(detail));
    assert(!strcmp(detail, "INSTALL_FAILED_SOMETHING_NEW"));
}

static void test_adb_error_push(void) {
    char detail[128];

    sc_gui_adb_error("adb: error: cannot stat 'C:/x.txt': No such file or "
                     "directory\n", detail, sizeof(detail));
    assert(!strcmp(detail, "cannot stat 'C:/x.txt': No such file or "
                           "directory"));

    // Last non-empty line
    sc_gui_adb_error("first\n\nsomething went wrong  \r\n\r\n",
                     detail, sizeof(detail));
    assert(!strcmp(detail, "something went wrong"));

    sc_gui_adb_error("", detail, sizeof(detail));
    assert(!strcmp(detail, "Failed"));

    // Truncation
    char small[8];
    sc_gui_adb_error("adb: error: a very long message", small, sizeof(small));
    assert(!strcmp(small, "a very "));
}

static void test_is_apk(void) {
    assert(sc_gui_is_apk("app.apk"));
    assert(sc_gui_is_apk("C:\\Downloads\\My App.APK"));
    assert(!sc_gui_is_apk(".apk"));
    assert(!sc_gui_is_apk("app.apks"));
    assert(!sc_gui_is_apk("app.xapk.zip"));
    assert(!sc_gui_is_apk("photo.jpg"));
}

static void test_basename(void) {
    assert(!strcmp(sc_gui_path_basename("C:\\Users\\me\\a.apk"), "a.apk"));
    assert(!strcmp(sc_gui_path_basename("/home/me/dir/"), "dir/"));
    assert(!strcmp(sc_gui_path_basename("file.txt"), "file.txt"));
    assert(!strcmp(sc_gui_path_basename("/sdcard/DCIM"), "DCIM"));
}

static void test_parse_ls(void) {
    // Real output of "ls -la /sdcard/" (toybox 0.8.12), plus edge cases
    const char *out =
        "total 20623\r\n"
        "drwxrws--- 2 u0_a165  media_rw    3452 2026-10-29 00:53 Alarms\r\n"
        "drwxrws--x 5 media_rw media_rw    3452 2026-10-29 00:53 Android\r\n"
        "drwxrwx--x 2 root     sdcard_rw   4096 2026-10-29 00:53 .\r\n"
        "drwxrwx--x 2 root     sdcard_rw   4096 2026-10-29 00:53 ..\r\n"
        "-rw-rw---- 1 u0_a165  media_rw 1048576 2026-10-30 12:01 My  file.txt\r\n"
        "lrwxrwxrwx 1 root     root          21 2009-01-01 08:00 sdcard -> /storage/self/primary\r\n"
        "crw-rw-rw- 1 root     root       1,   3 2026-10-29 00:53 null\r\n"
        "garbage\r\n";

    struct sc_gui_listing listing;
    assert(sc_gui_parse_ls(out, &listing));
    assert(!listing.error);
    assert(listing.count == 5);

    assert(!strcmp(listing.entries[0].name, "Alarms"));
    assert(listing.entries[0].type == 'd');
    assert(listing.entries[0].size == 3452);
    assert(!strcmp(listing.entries[0].mtime, "2026-10-29 00:53"));

    assert(!strcmp(listing.entries[1].name, "Android"));

    // Spaces in names are preserved
    assert(!strcmp(listing.entries[2].name, "My  file.txt"));
    assert(listing.entries[2].type == '-');
    assert(listing.entries[2].size == 1048576);
    assert(!strcmp(listing.entries[2].mtime, "2026-10-30 12:01"));

    // Symlink target is removed
    assert(!strcmp(listing.entries[3].name, "sdcard"));
    assert(listing.entries[3].type == 'l');

    // Device: "major, minor" instead of the size
    assert(!strcmp(listing.entries[4].name, "null"));
    assert(listing.entries[4].type == 'c');
    assert(listing.entries[4].size == 0);

    sc_gui_listing_destroy(&listing);

    // Errors (real outputs)
    assert(sc_gui_parse_ls("ls: /data/: Permission denied\n", &listing));
    assert(listing.count == 0);
    assert(!strcmp(listing.error, "/data/: Permission denied"));
    sc_gui_listing_destroy(&listing);

    // Empty folder
    assert(sc_gui_parse_ls("total 0\n", &listing));
    assert(listing.count == 0);
    assert(!listing.error);
    sc_gui_listing_destroy(&listing);
}

static void test_shell_quote(void) {
    char *q = sc_gui_shell_quote("/sdcard/My Files/");
    assert(!strcmp(q, "'/sdcard/My Files/'"));
    free(q);

    q = sc_gui_shell_quote("it's");
    assert(!strcmp(q, "'it'\\''s'"));
    free(q);

    q = sc_gui_shell_quote("");
    assert(!strcmp(q, "''"));
    free(q);

    // Shell metacharacters are not interpreted inside single quotes
    q = sc_gui_shell_quote("$(rm -rf /); `x` \"y\"");
    assert(!strcmp(q, "'$(rm -rf /); `x` \"y\"'"));
    free(q);
}

static const struct sc_gui_app_entry *
find_app(const struct sc_gui_app_list *list, const char *package) {
    for (size_t i = 0; i < list->count; ++i) {
        if (!strcmp(list->entries[i].package, package)) {
            return &list->entries[i];
        }
    }
    return NULL;
}

static void test_parse_apps(void) {
    // Output of the server with list_apps=true (format of
    // LogUtils.buildAppListMessage(), column 30), with CRLF line endings
    const char *server_out =
        "[server] INFO: Device: [Google] google Pixel 7 (Android 14)\r\n"
        "[server] INFO: Processing Android apps... (this may take some "
        "time)\r\n"
        "[server] INFO: List of apps:\r\n"
        " * Camera                         com.android.camera2\r\n"
        " * Settings                       com.android.settings\r\n"
        " - My App                         org.example.myapp\r\n"
        " - A very long application name that overflows\r\n"
        "                                 org.example.longname\r\n"
        " - Caf\xC3\xA9                           org.example.cafe\r\n"
        " - Bad                            not;valid\r\n";
    const char *user_out =
        "package:org.example.myapp\r\n"
        "package:org.example.longname\r\n"
        "package:org.example.cafe\r\n"
        "package:org.example.service\r\n";

    struct sc_gui_app_list list = {0};
    bool ok = sc_gui_parse_apps(server_out, user_out, &list);
    assert(ok);
    assert(list.count == 6);

    const struct sc_gui_app_entry *a = find_app(&list, "com.android.camera2");
    assert(a && !strcmp(a->name, "Camera") && a->system);
    a = find_app(&list, "com.android.settings");
    assert(a && !strcmp(a->name, "Settings") && a->system);
    a = find_app(&list, "org.example.myapp");
    assert(a && !strcmp(a->name, "My App") && !a->system);
    a = find_app(&list, "org.example.longname");
    assert(a && !strcmp(a->name,
                        "A very long application name that overflows"));
    a = find_app(&list, "org.example.cafe");
    assert(a && !strcmp(a->name, "Caf\xC3\xA9"));
    // A user app without launcher activity: named after its package
    a = find_app(&list, "org.example.service");
    assert(a && !strcmp(a->name, "org.example.service") && !a->system);
    // Invalid package name: ignored
    assert(!find_app(&list, "not;valid"));
    sc_gui_app_list_destroy(&list);
    assert(!list.entries && !list.count);

    // No server output (the server could not run): user packages only
    ok = sc_gui_parse_apps("", "package:com.foo\npackage:com.bar\n", &list);
    assert(ok && list.count == 2);
    assert(!strcmp(list.entries[0].name, "com.foo"));
    sc_gui_app_list_destroy(&list);

    ok = sc_gui_parse_apps(NULL, NULL, &list);
    assert(ok && list.count == 0);
    sc_gui_app_list_destroy(&list);
}

static void test_is_package_name(void) {
    assert(sc_gui_is_package_name("com.example.app"));
    assert(sc_gui_is_package_name("a_b.c1"));
    assert(!sc_gui_is_package_name(""));
    assert(!sc_gui_is_package_name("com.example;rm"));
    assert(!sc_gui_is_package_name("com example"));
    assert(!sc_gui_is_package_name("1com"));
}

static void test_parse_pm_lines(void) {
    // Output of "pm path" for a split APK
    char **values;
    size_t count;
    bool ok = sc_gui_parse_pm_lines(
        "package:/data/app/~~x==/com.foo-y==/base.apk\r\n"
        "package:/data/app/~~x==/com.foo-y==/split_config.arm64_v8a.apk\r\n"
        "\r\n"
        "garbage\n", &values, &count);
    assert(ok && count == 2);
    assert(!strcmp(values[0], "/data/app/~~x==/com.foo-y==/base.apk"));
    assert(!strcmp(values[1],
                   "/data/app/~~x==/com.foo-y==/split_config.arm64_v8a.apk"));
    sc_gui_free_strings(values, count);

    ok = sc_gui_parse_pm_lines("", &values, &count);
    assert(ok && count == 0);
    sc_gui_free_strings(values, count);
}

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;

    test_parse_apps();
    test_is_package_name();
    test_parse_pm_lines();

    test_parse_ls();
    test_shell_quote();
    test_adb_error_install();
    test_adb_error_push();
    test_is_apk();
    test_basename();
    return 0;
}
