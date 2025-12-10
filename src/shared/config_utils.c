#include "config_utils.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

void config_build_path(char *buf, size_t len, const char *filename)
{
    if (!buf || len == 0 || !filename) return;

    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        snprintf(buf, len, "%s/ck-core/%s", xdg, filename);
    } else {
        const char *home = getenv("HOME");
        if (!home || !home[0]) home = ".";
        snprintf(buf, len, "%s/.config/ck-core/%s", home, filename);
    }
}

int config_read_int(const char *filename, const char *key, int default_value)
{
    if (!filename || !key) return default_value;
    char path[PATH_MAX];
    config_build_path(path, sizeof(path), filename);

    FILE *f = fopen(path, "r");
    if (!f) return default_value;

    char k[128];
    int v = default_value;
    while (fscanf(f, "%127s %d", k, &v) == 2) {
        if (strcmp(k, key) == 0) {
            fclose(f);
            return v;
        }
    }
    fclose(f);
    return default_value;
}

int config_read_int_map(const char *filename, const char *key, int default_value)
{
    return config_read_int(filename, key, default_value);
}

void config_write_int(const char *filename, const char *key, int value)
{
    if (!filename || !key) return;

    char path[PATH_MAX];
    config_build_path(path, sizeof(path), filename);

    /* ensure directory exists */
    char dir[PATH_MAX];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (dir[0]) {
            mkdir(dir, 0700);
        }
    }

    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s %d\n", key, value);
    fclose(f);
}

void config_write_int_map(const char *filename, const char *key, int value)
{
    if (!filename || !key) return;

    char path[PATH_MAX];
    config_build_path(path, sizeof(path), filename);

    typedef struct {
        char key[128];
        int value;
    } Entry;

    Entry entries[256];
    size_t count = 0;

    /* read existing entries */
    FILE *rf = fopen(path, "r");
    if (rf) {
        while (count < sizeof(entries)/sizeof(entries[0])) {
            Entry e;
            if (fscanf(rf, "%127s %d", e.key, &e.value) != 2) break;
            entries[count++] = e;
        }
        fclose(rf);
    }

    /* update or add */
    size_t idx = 0;
    int found = 0;
    for (; idx < count; ++idx) {
        if (strcmp(entries[idx].key, key) == 0) {
            entries[idx].value = value;
            found = 1;
            break;
        }
    }
    if (!found && count < sizeof(entries)/sizeof(entries[0])) {
        strncpy(entries[count].key, key, sizeof(entries[count].key) - 1);
        entries[count].key[sizeof(entries[count].key) - 1] = '\0';
        entries[count].value = value;
        count++;
    }

    /* ensure directory exists */
    char dir[PATH_MAX];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (dir[0]) {
            mkdir(dir, 0700);
        }
    }

    FILE *wf = fopen(path, "w");
    if (!wf) return;
    for (size_t i = 0; i < count; ++i) {
        fprintf(wf, "%s %d\n", entries[i].key, entries[i].value);
    }
    fclose(wf);
}
