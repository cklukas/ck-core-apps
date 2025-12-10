#include "session_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *str_dup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, s, len + 1);
    return copy;
}

char *session_parse_argument(int *argc, char **argv)
{
    if (!argc || !argv) return NULL;
    for (int i = 1; i < *argc - 1; ++i) {
        if (strcmp(argv[i], "-session") == 0) {
            return str_dup(argv[i+1]);
        }
    }
    return NULL;
}

SessionData *session_data_create(const char *session_id)
{
    SessionData *data = (SessionData *)calloc(1, sizeof(SessionData));
    if (!data) return NULL;
    data->session_id = str_dup(session_id);
    data->items = NULL;
    return data;
}

void session_data_free(SessionData *data)
{
    if (!data) return;
    SessionKV *kv = data->items;
    while (kv) {
        SessionKV *next = kv->next;
        free(kv->key);
        free(kv->value);
        free(kv);
        kv = next;
    }
    free(data->session_id);
    free(data);
}

static SessionKV *find_kv(SessionData *data, const char *key)
{
    if (!data || !key) return NULL;
    for (SessionKV *kv = data->items; kv; kv = kv->next) {
        if (strcmp(kv->key, key) == 0) {
            return kv;
        }
    }
    return NULL;
}

const char *session_data_get(SessionData *data, const char *key)
{
    SessionKV *kv = find_kv(data, key);
    return kv ? kv->value : NULL;
}

int session_data_get_int(SessionData *data, const char *key, int default_value)
{
    const char *v = session_data_get(data, key);
    if (!v) return default_value;
    return atoi(v);
}

void session_data_set(SessionData *data, const char *key, const char *value)
{
    if (!data || !key) return;
    SessionKV *kv = find_kv(data, key);
    if (kv) {
        free(kv->value);
        kv->value = str_dup(value);
        return;
    }
    kv = (SessionKV *)calloc(1, sizeof(SessionKV));
    if (!kv) return;
    kv->key = str_dup(key);
    kv->value = str_dup(value ? value : "");
    kv->next = data->items;
    data->items = kv;
}

void session_data_set_int(SessionData *data, const char *key, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    session_data_set(data, key, buf);
}

/* Clamp geometry to visible screen area */
static void clamp_to_screen(Display *dpy, int *x, int *y, int *w, int *h)
{
    if (!dpy || !x || !y || !w || !h) return;

    int screen = DefaultScreen(dpy);
    int sw = DisplayWidth(dpy, screen);
    int sh = DisplayHeight(dpy, screen);

    if (sw > 0 && *w > sw) *w = sw;
    if (sh > 0 && *h > sh) *h = sh;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (sw > 0 && *x + *w > sw) *x = sw - *w;
    if (sh > 0 && *y + *h > sh) *y = sh - *h;
}

void session_capture_geometry(Widget toplevel, SessionData *data,
                              const char *key_x, const char *key_y,
                              const char *key_w, const char *key_h)
{
    if (!toplevel || !data) return;

    Position x, y;
    Dimension width, height;
    XtVaGetValues(toplevel,
                  XmNx,     &x,
                  XmNy,     &y,
                  XmNwidth, &width,
                  XmNheight,&height,
                  NULL);

    if (key_x) session_data_set_int(data, key_x, (int)x);
    if (key_y) session_data_set_int(data, key_y, (int)y);
    if (key_w) session_data_set_int(data, key_w, (int)width);
    if (key_h) session_data_set_int(data, key_h, (int)height);
}

Boolean session_apply_geometry(Widget toplevel, SessionData *data,
                               const char *key_x, const char *key_y,
                               const char *key_w, const char *key_h)
{
    if (!toplevel || !data) return False;
    int have_any = 0;
    int x = session_data_get_int(data, key_x, 0);
    int y = session_data_get_int(data, key_y, 0);
    int w = session_data_get_int(data, key_w, 0);
    int h = session_data_get_int(data, key_h, 0);

    if (key_w && session_data_get(data, key_w)) have_any = 1;
    if (key_h && session_data_get(data, key_h)) have_any = 1;
    if (!have_any) return False;

    Display *dpy = XtDisplay(toplevel);
    clamp_to_screen(dpy, &x, &y, &w, &h);

    XtVaSetValues(toplevel,
                  XmNx,      (Position)x,
                  XmNy,      (Position)y,
                  XmNwidth,  (Dimension)w,
                  XmNheight, (Dimension)h,
                  NULL);
    return True;
}

Boolean session_load(Widget toplevel, SessionData *data)
{
    if (!data || !data->session_id || !data->session_id[0])
        return False;

    char *path = NULL;

    if (!DtSessionRestorePath(toplevel, &path, data->session_id)) {
        return False;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        XtFree(path);
        return False;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char key[64];
        char *val = NULL;
        if (sscanf(line, "%63s", key) != 1) continue;
        val = line + strlen(key);
        while (*val == ' ' || *val == '\t') val++;
        /* trim leading spaces handled; trim newline */
        size_t len = strlen(val);
        while (len > 0 && (val[len-1] == '\n' || val[len-1] == '\r')) {
            val[--len] = '\0';
        }
        session_data_set(data, key, val);
    }
    fclose(fp);
    XtFree(path);
    return True;
}

void session_save(Widget toplevel, SessionData *data, const char *exec_path)
{
    if (!data) return;

    char *savePath = NULL;
    char *saveFile = NULL;

    if (!DtSessionSavePath(toplevel, &savePath, &saveFile)) {
        return;
    }

    FILE *fp = fopen(savePath, "w");
    if (!fp) {
        XtFree(savePath);
        XtFree(saveFile);
        return;
    }

    for (SessionKV *kv = data->items; kv; kv = kv->next) {
        fprintf(fp, "%s %s\n", kv->key, kv->value ? kv->value : "");
    }
    fclose(fp);

    /* Restart command: <exec_path> -session <id> */
    if (exec_path && saveFile) {
        char *cmd_argv[3];
        int   cmd_argc = 0;
        cmd_argv[cmd_argc++] = (char *)exec_path;
        cmd_argv[cmd_argc++] = "-session";
        cmd_argv[cmd_argc++] = saveFile;
        XSetCommand(XtDisplay(toplevel), XtWindow(toplevel), cmd_argv, cmd_argc);
    }

    XtFree(savePath);
    XtFree(saveFile);
}
