#include "gui_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
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

static bool
is_blank(char c) {
    return c == ' ' || c == '\t';
}

// Read the next whitespace-separated token of the line [*p, end)
static bool
next_token(const char **p, const char *end, const char **tok, size_t *len) {
    const char *s = *p;
    while (s < end && is_blank(*s)) {
        ++s;
    }
    if (s == end) {
        return false;
    }
    const char *e = s;
    while (e < end && !is_blank(*e)) {
        ++e;
    }
    *tok = s;
    *len = e - s;
    *p = e;
    return true;
}

static char *
dup_range(const char *s, size_t len) {
    char *r = malloc(len + 1);
    if (r) {
        memcpy(r, s, len);
        r[len] = '\0';
    }
    return r;
}

static bool
parse_ls_line(const char *line, const char *end,
              struct sc_gui_file_entry *entry) {
    const char *p = line;
    const char *tok;
    size_t len;

    // permissions: "drwxrws---"
    if (!next_token(&p, end, &tok, &len) || len < 10) {
        return false;
    }
    char type = tok[0];

    // links, owner, group
    for (int i = 0; i < 3; ++i) {
        if (!next_token(&p, end, &tok, &len)) {
            return false;
        }
    }

    // size, or "major, minor" for devices
    if (!next_token(&p, end, &tok, &len)) {
        return false;
    }
    uint64_t size = 0;
    if (tok[len - 1] == ',') {
        if (!next_token(&p, end, &tok, &len)) {
            return false;
        }
    } else {
        for (size_t i = 0; i < len; ++i) {
            if (tok[i] < '0' || tok[i] > '9') {
                return false;
            }
            size = size * 10 + (tok[i] - '0');
        }
    }

    // date and time
    const char *date;
    size_t date_len;
    const char *time;
    size_t time_len;
    if (!next_token(&p, end, &date, &date_len)
            || !next_token(&p, end, &time, &time_len)) {
        return false;
    }

    // name: the rest of the line (it may contain spaces)
    while (p < end && is_blank(*p)) {
        ++p;
    }
    const char *name = p;
    const char *name_end = end;
    if (type == 'l') {
        // "name -> target"
        for (const char *q = name; q + 4 <= end; ++q) {
            if (!memcmp(q, " -> ", 4)) {
                name_end = q;
                break;
            }
        }
    }
    size_t name_len = name_end - name;
    if (!name_len || (name_len == 1 && name[0] == '.')
            || (name_len == 2 && name[0] == '.' && name[1] == '.')) {
        return false;
    }

    entry->name = dup_range(name, name_len);
    entry->mtime = malloc(date_len + 1 + time_len + 1);
    if (!entry->name || !entry->mtime) {
        free(entry->name);
        free(entry->mtime);
        return false;
    }
    memcpy(entry->mtime, date, date_len);
    entry->mtime[date_len] = ' ';
    memcpy(entry->mtime + date_len + 1, time, time_len);
    entry->mtime[date_len + 1 + time_len] = '\0';
    entry->size = size;
    entry->type = type;
    return true;
}

bool
sc_gui_parse_ls(const char *out, struct sc_gui_listing *listing) {
    listing->entries = NULL;
    listing->count = 0;
    listing->error = NULL;
    size_t capacity = 0;

    const char *line = out;
    while (*line) {
        size_t len = strcspn(line, "\r\n");
        const char *end = line + len;

        if (len >= 4 && !strncmp(line, "ls: ", 4)) {
            if (!listing->error) {
                listing->error = dup_range(line + 4, len - 4);
            }
        } else if (len && strncmp(line, "total ", 6)) {
            if (listing->count == capacity) {
                size_t new_capacity = capacity ? capacity * 2 : 64;
                struct sc_gui_file_entry *entries =
                    realloc(listing->entries,
                            new_capacity * sizeof(*entries));
                if (!entries) {
                    sc_gui_listing_destroy(listing);
                    return false;
                }
                listing->entries = entries;
                capacity = new_capacity;
            }
            struct sc_gui_file_entry *entry =
                &listing->entries[listing->count];
            if (parse_ls_line(line, end, entry)) {
                ++listing->count;
            }
        }

        line = end;
        line += strspn(line, "\r\n");
    }
    return true;
}

void
sc_gui_listing_destroy(struct sc_gui_listing *listing) {
    for (size_t i = 0; i < listing->count; ++i) {
        free(listing->entries[i].name);
        free(listing->entries[i].mtime);
    }
    free(listing->entries);
    free(listing->error);
    listing->entries = NULL;
    listing->count = 0;
    listing->error = NULL;
}

char *
sc_gui_shell_quote(const char *s) {
    // Each ' becomes '\'' (4 chars)
    size_t len = 2;
    for (const char *p = s; *p; ++p) {
        len += *p == '\'' ? 4 : 1;
    }
    char *r = malloc(len + 1);
    if (!r) {
        return NULL;
    }
    char *w = r;
    *w++ = '\'';
    for (const char *p = s; *p; ++p) {
        if (*p == '\'') {
            memcpy(w, "'\\''", 4);
            w += 4;
        } else {
            *w++ = *p;
        }
    }
    *w++ = '\'';
    *w = '\0';
    return r;
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
