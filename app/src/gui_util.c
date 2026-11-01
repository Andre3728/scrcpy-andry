#include "gui_util.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const struct {
    const char *code;
    const char *text;
} install_errors[] = {
    {"INSTALL_FAILED_VERSION_DOWNGRADE",
        "A newer version is already installed"},
    {"INSTALL_FAILED_UPDATE_INCOMPATIBLE",
        "Signed differently than the installed app (uninstall it first)"},
    {"INSTALL_PARSE_FAILED_NOT_APK", "Not a valid APK"},
    {"INSTALL_PARSE_FAILED_NO_CERTIFICATES", "The APK is not signed"},
    {"INSTALL_FAILED_INSUFFICIENT_STORAGE", "Not enough storage"},
    {"INSTALL_FAILED_OLDER_SDK", "Requires a newer Android version"},
    {"INSTALL_FAILED_NO_MATCHING_ABIS",
        "Not compatible with this device's processor"},
    {"INSTALL_FAILED_USER_RESTRICTED",
        "Blocked on the device (check the prompt or install settings)"},
    {"INSTALL_FAILED_TEST_ONLY", "Test-only APK"},
    {"INSTALL_FAILED_ALREADY_EXISTS", "Already installed"},
    {"INSTALL_FAILED_MISSING_SPLIT",
        "Split APK: the other parts are missing"},
    {"INSTALL_FAILED_ABORTED", "Installation canceled"},
};

static bool
copy_line(const char *start, char *detail, size_t size) {
    size_t len = strcspn(start, "\r\n");
    // Trim trailing spaces
    while (len && isspace((unsigned char) start[len - 1])) {
        --len;
    }
    if (!len) {
        return false;
    }
    snprintf(detail, size, "%.*s", (int) len, start);
    return true;
}

void
sc_gui_adb_error(const char *out, char *detail, size_t size) {
    // adb install: "Failure [INSTALL_FAILED_XXX: optional message]"
    const char *failure = strstr(out, "Failure [");
    if (failure) {
        const char *code = failure + strlen("Failure [");
        size_t len = strcspn(code, ":]\r\n ");
        for (size_t i = 0; i < sizeof(install_errors) / sizeof(*install_errors);
                ++i) {
            if (strlen(install_errors[i].code) == len
                    && !strncmp(code, install_errors[i].code, len)) {
                snprintf(detail, size, "%s", install_errors[i].text);
                return;
            }
        }
        snprintf(detail, size, "%.*s", (int) len, code);
        return;
    }

    // "adb: error: ..."
    const char *error = strstr(out, "error: ");
    if (error && copy_line(error + strlen("error: "), detail, size)) {
        return;
    }

    // Last non-empty line
    const char *last = NULL;
    const char *line = out;
    while (*line) {
        size_t len = strcspn(line, "\r\n");
        if (len) {
            last = line;
        }
        line += len;
        line += strspn(line, "\r\n");
    }
    if (!last || !copy_line(last, detail, size)) {
        snprintf(detail, size, "Failed");
    }
}

bool
sc_gui_is_apk(const char *path) {
    size_t len = strlen(path);
    if (len <= 4) {
        return false;
    }
    const char *ext = path + len - 4;
    return ext[0] == '.'
        && tolower((unsigned char) ext[1]) == 'a'
        && tolower((unsigned char) ext[2]) == 'p'
        && tolower((unsigned char) ext[3]) == 'k';
}

const char *
sc_gui_path_basename(const char *path) {
    const char *name = path;
    for (const char *p = path; *p; ++p) {
        if ((*p == '/' || *p == '\\') && p[1]) {
            name = p + 1;
        }
    }
    return name;
}
