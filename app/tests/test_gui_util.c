#include "common.h"

#include <assert.h>
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

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;

    test_adb_error_install();
    test_adb_error_push();
    test_is_apk();
    test_basename();
    return 0;
}
