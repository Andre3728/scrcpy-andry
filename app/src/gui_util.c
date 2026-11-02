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

bool
sc_gui_is_package_name(const char *s) {
    // Segments of [A-Za-z0-9_] starting with a letter, separated by '.'
    bool segment_start = true;
    for (const char *p = s; *p; ++p) {
        unsigned char c = *p;
        if (c == '.') {
            if (segment_start) {
                return false;
            }
            segment_start = true;
        } else if (isalpha(c) || c == '_' || (isdigit(c) && !segment_start)) {
            segment_start = false;
        } else {
            return false;
        }
    }
    return *s && !segment_start;
}

// Line [start, end) without the trailing '\r' and blanks
static const char *
trim_end(const char *start, const char *end) {
    while (end > start && (end[-1] == '\r' || is_blank(end[-1]))) {
        --end;
    }
    return end;
}

static bool
app_list_add(struct sc_gui_app_list *list, size_t *cap, const char *name,
             size_t name_len, const char *package, size_t package_len,
             bool system) {
    if (list->count == *cap) {
        size_t new_cap = *cap ? *cap * 2 : 64;
        struct sc_gui_app_entry *entries =
            realloc(list->entries, new_cap * sizeof(*entries));
        if (!entries) {
            return false;
        }
        list->entries = entries;
        *cap = new_cap;
    }
    struct sc_gui_app_entry *e = &list->entries[list->count];
    e->name = dup_range(name, name_len);
    e->package = dup_range(package, package_len);
    e->system = system;
    if (!e->name || !e->package) {
        free(e->name);
        free(e->package);
        return false;
    }
    ++list->count;
    return true;
}

static bool
app_list_contains(const struct sc_gui_app_list *list, const char *package,
                  size_t len) {
    for (size_t i = 0; i < list->count; ++i) {
        const char *p = list->entries[i].package;
        if (strlen(p) == len && !strncmp(p, package, len)) {
            return true;
        }
    }
    return false;
}

static bool
is_package_range(const char *s, size_t len) {
    char buf[256];
    if (!len || len >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, s, len);
    buf[len] = '\0';
    return sc_gui_is_package_name(buf);
}

bool
sc_gui_parse_apps(const char *server_out, const char *user_packages_out,
                  struct sc_gui_app_list *list) {
    list->entries = NULL;
    list->count = 0;
    size_t cap = 0;

    // Server lines: " * <name><padding> <package>" (system app) or " - ..."
    // (user app). If the name is too long, the package is on the next line,
    // indented by more than 3 spaces.
    const char *line = server_out ? server_out : "";
    while (*line) {
        const char *eol = line + strcspn(line, "\n");
        const char *next = *eol ? eol + 1 : eol;

        if (line[0] == ' ' && (line[1] == '*' || line[1] == '-')
                && line[2] == ' ') {
            bool system = line[1] == '*';
            const char *name = line + 3;
            const char *name_end = trim_end(name, eol);
            const char *pkg;
            const char *pkg_end;

            if (!strncmp(next, "    ", 4)) {
                // Long name: the package is on the next line
                const char *next_eol = next + strcspn(next, "\n");
                pkg = next + strspn(next, " \t");
                pkg_end = trim_end(pkg, next_eol);
                next = *next_eol ? next_eol + 1 : next_eol;
            } else {
                // The package is the last token
                pkg_end = name_end;
                pkg = pkg_end;
                while (pkg > name && !is_blank(pkg[-1])) {
                    --pkg;
                }
                name_end = pkg;
                while (name_end > name && is_blank(name_end[-1])) {
                    --name_end;
                }
            }

            size_t pkg_len = pkg_end - pkg;
            if (is_package_range(pkg, pkg_len)
                    && !app_list_contains(list, pkg, pkg_len)) {
                if (name_end == name) {
                    // No label
                    name = pkg;
                    name_end = pkg_end;
                }
                if (!app_list_add(list, &cap, name, name_end - name, pkg,
                                  pkg_len, system)) {
                    sc_gui_app_list_destroy(list);
                    return false;
                }
            }
        }
        line = next;
    }

    // User apps not listed by the server (no launcher activity)
    char **packages;
    size_t count;
    if (!sc_gui_parse_pm_lines(user_packages_out ? user_packages_out : "",
                               &packages, &count)) {
        sc_gui_app_list_destroy(list);
        return false;
    }
    bool ok = true;
    for (size_t i = 0; ok && i < count; ++i) {
        const char *p = packages[i];
        size_t len = strlen(p);
        if (sc_gui_is_package_name(p) && !app_list_contains(list, p, len)) {
            ok = app_list_add(list, &cap, p, len, p, len, false);
        }
    }
    sc_gui_free_strings(packages, count);
    if (!ok) {
        sc_gui_app_list_destroy(list);
    }
    return ok;
}

void
sc_gui_app_list_destroy(struct sc_gui_app_list *list) {
    for (size_t i = 0; i < list->count; ++i) {
        free(list->entries[i].name);
        free(list->entries[i].package);
    }
    free(list->entries);
    list->entries = NULL;
    list->count = 0;
}

bool
sc_gui_parse_pm_lines(const char *out, char ***values, size_t *count) {
    *values = NULL;
    *count = 0;
    size_t cap = 0;

    const char *line = out;
    while (*line) {
        const char *eol = line + strcspn(line, "\n");
        if (!strncmp(line, "package:", 8)) {
            const char *v = line + 8;
            const char *v_end = trim_end(v, eol);
            if (v_end > v) {
                if (*count == cap) {
                    size_t new_cap = cap ? cap * 2 : 16;
                    char **a = realloc(*values, new_cap * sizeof(*a));
                    if (!a) {
                        goto error;
                    }
                    *values = a;
                    cap = new_cap;
                }
                char *s = dup_range(v, v_end - v);
                if (!s) {
                    goto error;
                }
                (*values)[(*count)++] = s;
            }
        }
        line = *eol ? eol + 1 : eol;
    }
    return true;

error:
    sc_gui_free_strings(*values, *count);
    *values = NULL;
    *count = 0;
    return false;
}

void
sc_gui_free_strings(char **values, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        free(values[i]);
    }
    free(values);
}
