#include "clipboard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <Xm/CutPaste.h>

#include "logic/display_api.h"

static void copy_flash_reset(XtPointer client_data, XtIntervalId *id)
{
    (void)id;
    AppState *app = (AppState *)client_data;
    if (!app) return;
    if (app->copy_flash_id) {
        app->copy_flash_id = 0;
    }
    if (app->copy_flash_backup[0]) {
        ck_calc_set_display(app, app->copy_flash_backup);
        app->copy_flash_backup[0] = '\0';
    }
}

static void paste_flash_reset(XtPointer client_data, XtIntervalId *id)
{
    (void)id;
    AppState *app = (AppState *)client_data;
    if (!app) return;
    if (app->paste_flash_id) {
        app->paste_flash_id = 0;
    }
    if (app->paste_flash_backup[0]) {
        ck_calc_set_display(app, app->paste_flash_backup);
        app->paste_flash_backup[0] = '\0';
    }
}

void ck_calc_clipboard_copy(AppState *app)
{
    if (!app || !app->shell) return;
    if (app->copy_flash_id) {
        XtRemoveTimeOut(app->copy_flash_id);
        app->copy_flash_id = 0;
    }
    double val = ck_calc_current_input(app);
    strncpy(app->copy_flash_backup, app->display, sizeof(app->copy_flash_backup) - 1);
    app->copy_flash_backup[sizeof(app->copy_flash_backup) - 1] = '\0';

    ck_calc_set_display(app, "COPIED");
    app->copy_flash_id = XtAppAddTimeOut(app->app_context, 500, copy_flash_reset, (XtPointer)app);

    char buf[128];
    snprintf(buf, sizeof(buf), "%.12g", val);

    Display *dpy = XtDisplay(app->shell);
    if (!dpy || !XtIsRealized(app->shell)) return;
    Window win = XtWindow(app->shell);
    long item_id = 0;
    XmString label = XmStringCreateLocalized("ck-calc");
    int status = XmClipboardStartCopy(dpy, win, label, CurrentTime, NULL, NULL, &item_id);
    if (label) XmStringFree(label);
    if (status != ClipboardSuccess) return;
    status = XmClipboardCopy(dpy, win, item_id, "STRING", buf, (int)strlen(buf), 0, NULL);
    XmClipboardEndCopy(dpy, win, item_id);
    (void)status;
}

void ck_calc_clipboard_paste(AppState *app)
{
    if (!app || !app->shell) return;

    if (app->paste_flash_id) {
        XtRemoveTimeOut(app->paste_flash_id);
        app->paste_flash_id = 0;
    }

    Display *dpy = XtDisplay(app->shell);
    if (!dpy || !XtIsRealized(app->shell)) return;
    Window win = XtWindow(app->shell);
    Time ts = XtLastTimestampProcessed(dpy);

    fprintf(stderr, "[ck-calc] paste: starting\n");

    char data[512];
    unsigned long out_len = 0;
    long private_id = 0;
    int status = XmClipboardStartRetrieve(dpy, win, ts);
    if (status == ClipboardSuccess || status == ClipboardTruncate) {
        fprintf(stderr, "[ck-calc] paste: start retrieve ok (%d)\n", status);
        status = XmClipboardRetrieve(dpy, win, "STRING", data, sizeof(data)-1, (unsigned long *)&out_len, &private_id);
        if (status != ClipboardSuccess && status != ClipboardTruncate) {
            fprintf(stderr, "[ck-calc] paste: STRING failed, trying COMPOUND_TEXT (%d)\n", status);
            status = XmClipboardRetrieve(dpy, win, "COMPOUND_TEXT", data, sizeof(data)-1, (unsigned long *)&out_len, &private_id);
        }
        if (status != ClipboardSuccess && status != ClipboardTruncate) {
            fprintf(stderr, "[ck-calc] paste: COMPOUND_TEXT failed, trying UTF8_STRING (%d)\n", status);
            status = XmClipboardRetrieve(dpy, win, "UTF8_STRING", data, sizeof(data)-1, (unsigned long *)&out_len, &private_id);
        }
        XmClipboardEndRetrieve(dpy, win);
    }

    if (status != ClipboardSuccess && status != ClipboardTruncate) {
        /* Fallback to cut buffer 0 */
        int bytes = 0;
        char *buf = XFetchBuffer(dpy, &bytes, 0);
        if (!buf || bytes <= 0) {
            if (buf) XFree(buf);
            return;
        }
        size_t copy_len = (bytes < (int)sizeof(data)-1) ? (size_t)bytes : sizeof(data)-1;
        memcpy(data, buf, copy_len);
        data[copy_len] = '\0';
        XFree(buf);
        fprintf(stderr, "[ck-calc] paste: fallback cut buffer len=%zu data='%s'\n", copy_len, data);
    } else {
        data[(out_len < sizeof(data)-1) ? out_len : (sizeof(data)-1)] = '\0';
        fprintf(stderr, "[ck-calc] paste: retrieved len=%lu data='%s'\n", out_len, data);
    }

    char cleaned[128];
    size_t pos = 0;
    char decimal_seen = 0;
    for (size_t i = 0; data[i] && pos + 1 < sizeof(cleaned); ++i) {
        char c = data[i];
        if (c == '\0' || c == '\n' || c == '\r' || c == '\t' || c == ' ') continue;
        if (c == app->thousands_char) continue;
        if (c == ',' || c == '.') {
            if (decimal_seen) continue;
            cleaned[pos++] = '.';
            decimal_seen = 1;
            continue;
        }
        if (c == '+' || c == '-' || (c >= '0' && c <= '9') || c == 'e' || c == 'E') {
            cleaned[pos++] = c;
        }
    }
    cleaned[pos] = '\0';

    fprintf(stderr, "[ck-calc] paste: cleaned='%s'\n", cleaned);

    if (pos == 0) {
        fprintf(stderr, "[ck-calc] paste: nothing to parse\n");
        return;
    }

    char *endptr = NULL;
    double val = strtod(cleaned, &endptr);
    if (!endptr || endptr == cleaned) {
        fprintf(stderr, "[ck-calc] paste: strtod failed\n");
        return;
    }
    /* allow trailing garbage: ignore after parsed number */

    fprintf(stderr, "[ck-calc] paste: parsed=%f\n", val);
    ck_calc_set_display_from_double(app, val);
    strncpy(app->paste_flash_backup, app->display, sizeof(app->paste_flash_backup) - 1);
    app->paste_flash_backup[sizeof(app->paste_flash_backup) - 1] = '\0';
    app->paste_flash_id = XtAppAddTimeOut(app->app_context, 500, paste_flash_reset, (XtPointer)app);
    ck_calc_set_display(app, "PASTED");
    app->calc_state.entering_new = false;
}
