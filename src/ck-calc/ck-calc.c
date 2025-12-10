/*
 * ck-calc.c - Simple CDE/Motif calculator (Mac OS style basic layout)
 *
 * Buttons:
 *   Back, AC, %, /
 *   7, 8, 9, *
 *   4, 5, 6, -
 *   1, 2, 3, +
 *   +/-, 0 (double width), =
 *
 * Features:
 *   - Multiple windows via Window -> New (spawns another instance)
 *   - Session handling (geometry + current display value)
 *   - Shared About dialog
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <limits.h>
#include <stdint.h>
#include <locale.h>
#include <X11/keysym.h>

#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/Notebook.h>
#include <Xm/CascadeB.h>
#include <Xm/ToggleB.h>
#include <Xm/DialogS.h>
#include <Xm/DrawingA.h>
#include <Xm/Protocols.h>
#include <Xm/MwmUtil.h>
#include <Dt/Session.h>
#include <Dt/Dt.h>

#include "../shared/session_utils.h"
#include "../shared/about_dialog.h"
#include "../shared/config_utils.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_DISPLAY_LEN 64
#define VIEW_STATE_FILENAME "ck-calc.view"

typedef struct {
    XtAppContext app_context;
    Widget       shell;
    Widget       main_form;
    Widget       key_focus_proxy;
    Widget       display_label;
    SessionData *session_data;

    char         exec_path[PATH_MAX];
    char         display[MAX_DISPLAY_LEN];

    double       stored_value;
    double       last_operand;
    char         pending_op;     /* '+', '-', '*', '/', or 0 */
    char         last_op;        /* repeat op for '=' */
    bool         has_pending_value;
    bool         entering_new;
    bool         error_state;

    bool         show_thousands;
    char         decimal_char;
    char         thousands_char;

    /* Widgets for keyboard activation */
    Widget       btn_digits[10];
    Widget       btn_decimal;
    Widget       btn_eq;
    Widget       btn_plus;
    Widget       btn_minus;
    Widget       btn_mul;
    Widget       btn_div;
    Widget       btn_percent;
    Widget       btn_sign;
    Widget       btn_back;
    Widget       btn_ac;
} AppState;

static AppState *g_app = NULL;
static Widget g_about_shell = NULL;

static void format_number(AppState *app, double value, char *out, size_t out_len);
static void key_press_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont);
static void ensure_keyboard_focus(AppState *app);
static void focus_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont);
static void reformat_display(AppState *app);

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static void update_display(AppState *app)
{
    if (!app || !app->display_label) return;
    XmString xms = XmStringCreateLocalized(app->display);
    XtVaSetValues(app->display_label,
                  XmNlabelString, xms,
                  NULL);
    XmStringFree(xms);
}

static void set_display(AppState *app, const char *text)
{
    if (!app || !text) return;
    strncpy(app->display, text, sizeof(app->display) - 1);
    app->display[sizeof(app->display) - 1] = '\0';
    update_display(app);
}

static void set_display_from_double(AppState *app, double value)
{
    if (!app) return;
    format_number(app, value, app->display, sizeof(app->display));
    update_display(app);
}

static void reset_state(AppState *app)
{
    if (!app) return;
    app->stored_value      = 0.0;
    app->last_operand      = 0.0;
    app->pending_op        = 0;
    app->last_op           = 0;
    app->has_pending_value = false;
    app->entering_new      = true;
    app->error_state       = false;
    set_display(app, "0");
}

static double current_input(AppState *app)
{
    if (!app) return 0.0;
    if (app->error_state) return 0.0;

    char buf[128];
    size_t bpos = 0;
    for (const char *p = app->display; *p && bpos + 1 < sizeof(buf); ++p) {
        if (app->show_thousands && *p == app->thousands_char) continue;
        if (*p == app->decimal_char && app->decimal_char != '.') {
            buf[bpos++] = '.';
            continue;
        }
        buf[bpos++] = *p;
    }
    buf[bpos] = '\0';
    return strtod(buf, NULL);
}

static void init_exec_path(AppState *app, const char *argv0)
{
    if (!app) return;

    ssize_t len = readlink("/proc/self/exe", app->exec_path,
                           sizeof(app->exec_path) - 1);
    if (len > 0) {
        app->exec_path[len] = '\0';
        return;
    }

    if (argv0 && argv0[0]) {
        if (argv0[0] == '/') {
            strncpy(app->exec_path, argv0, sizeof(app->exec_path) - 1);
            app->exec_path[sizeof(app->exec_path) - 1] = '\0';
            return;
        }

        if (strchr(argv0, '/')) {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd))) {
                size_t cwd_len  = strlen(cwd);
                size_t argv_len = strlen(argv0);
                size_t needed   = cwd_len + 1 + argv_len + 1;
                if (needed <= sizeof(app->exec_path)) {
                    memcpy(app->exec_path, cwd, cwd_len);
                    app->exec_path[cwd_len] = '/';
                    memcpy(app->exec_path + cwd_len + 1, argv0, argv_len);
                    app->exec_path[cwd_len + 1 + argv_len] = '\0';
                    return;
                }
            }
        }

        strncpy(app->exec_path, argv0, sizeof(app->exec_path) - 1);
        app->exec_path[sizeof(app->exec_path) - 1] = '\0';
    } else {
        strncpy(app->exec_path, "ck-calc", sizeof(app->exec_path) - 1);
        app->exec_path[sizeof(app->exec_path) - 1] = '\0';
    }
}

static void center_shell_on_screen(Widget shell)
{
    if (!shell) return;

    Dimension w = 0, h = 0;
    XtVaGetValues(shell,
                  XmNwidth,  &w,
                  XmNheight, &h,
                  NULL);

    if (w == 0 || h == 0) return;

    Display *dpy = XtDisplay(shell);
    int screen = DefaultScreen(dpy);
    int sw = DisplayWidth(dpy, screen);
    int sh = DisplayHeight(dpy, screen);

    Position x = (Position)((sw - (int)w) / 2);
    Position y = (Position)((sh - (int)h) / 2);
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    XtVaSetValues(shell,
                  XmNx, x,
                  XmNy, y,
                  NULL);
}

static void set_error(AppState *app)
{
    if (!app) return;
    app->error_state = true;
    app->pending_op = 0;
    app->has_pending_value = false;
    app->last_op = 0;
    set_display(app, "Error");
    app->entering_new = true;
}

static void init_locale_settings(AppState *app)
{
    if (!app) return;
    struct lconv *lc = localeconv();
    app->decimal_char = (lc && lc->decimal_point && lc->decimal_point[0])
                            ? lc->decimal_point[0]
                            : '.';
    app->thousands_char = (lc && lc->thousands_sep && lc->thousands_sep[0])
                              ? lc->thousands_sep[0]
                              : ',';
}

static void load_view_state(AppState *app)
{
    if (!app) return;
    int val = config_read_int_map(VIEW_STATE_FILENAME, "show_thousands", 1);
    app->show_thousands = (val != 0);
}

static void save_view_state(const AppState *app)
{
    if (!app) return;
    config_write_int_map(VIEW_STATE_FILENAME, "show_thousands", app->show_thousands ? 1 : 0);
}

static void format_number(AppState *app, double value, char *out, size_t out_len)
{
    if (!app || !out || out_len == 0) return;

    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%.12g", value);

    const char *exp_part = strchr(tmp, 'e');
    if (!exp_part) exp_part = strchr(tmp, 'E');

    char int_part[128] = {0};
    char frac_part[128] = {0};
    char sign = 0;

    const char *p = tmp;
    if (*p == '+' || *p == '-') {
        sign = *p;
        p++;
    }

    size_t int_len = 0;
    while (*p && *p != '.' && *p != 'e' && *p != 'E') {
        if (int_len + 1 < sizeof(int_part)) {
            int_part[int_len++] = *p;
        }
        p++;
    }
    int_part[int_len] = '\0';

    size_t frac_len = 0;
    if (*p == '.') {
        p++;
        while (*p && *p != 'e' && *p != 'E') {
            if (frac_len + 1 < sizeof(frac_part)) {
                frac_part[frac_len++] = *p;
            }
            p++;
        }
        frac_part[frac_len] = '\0';
    }

    char grouped[128] = {0};
    size_t gpos = 0;
    if (app->show_thousands && app->thousands_char && int_len > 3) {
        int count = 0;
        for (ssize_t i = (ssize_t)int_len - 1; i >= 0; --i) {
            if (gpos + 2 >= sizeof(grouped)) break;
            grouped[gpos++] = int_part[i];
            count++;
            if (count == 3 && i > 0) {
                grouped[gpos++] = app->thousands_char;
                count = 0;
            }
        }
        /* reverse */
        for (size_t i = 0; i < gpos / 2; ++i) {
            char tmpc = grouped[i];
            grouped[i] = grouped[gpos - 1 - i];
            grouped[gpos - 1 - i] = tmpc;
        }
        grouped[gpos] = '\0';
    } else {
        size_t copy_len = strlen(int_part);
        if (copy_len >= sizeof(grouped)) copy_len = sizeof(grouped) - 1;
        memcpy(grouped, int_part, copy_len);
        grouped[copy_len] = '\0';
    }

    /* Compose final string */
    out[0] = '\0';
    size_t pos = 0;
    if (sign) {
        out[pos++] = sign;
    }

    size_t grouped_len = strlen(grouped);
    if (pos + grouped_len < out_len) {
        memcpy(out + pos, grouped, grouped_len);
        pos += grouped_len;
    }

    if (frac_len > 0 && pos + 1 + frac_len < out_len) {
        out[pos++] = app->decimal_char;
        memcpy(out + pos, frac_part, frac_len);
        pos += frac_len;
    }

    if (exp_part && pos + strlen(exp_part) < out_len) {
        memcpy(out + pos, exp_part, strlen(exp_part));
        pos += strlen(exp_part);
    }

    if (pos >= out_len) pos = out_len - 1;
    out[pos] = '\0';
}

static bool apply_operation(AppState *app, char op, double rhs, double *out_value)
{
    if (!app || !out_value) return false;
    double lhs = app->stored_value;
    double result = lhs;

    switch (op) {
        case '+': result = lhs + rhs; break;
        case '-': result = lhs - rhs; break;
        case '*': result = lhs * rhs; break;
        case '/':
            if (rhs == 0.0) {
                set_error(app);
                return false;
            }
            result = lhs / rhs;
            break;
        default:
            return false;
    }

    *out_value = result;
    return true;
}

/* -------------------------------------------------------------------------
 * Callbacks
 * ------------------------------------------------------------------------- */

static void cb_digit(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    if (app->error_state) reset_state(app);

    char digit = (char)(uintptr_t)client_data;
    size_t len = strlen(app->display);

    if (app->entering_new) {
        if (app->pending_op == 0) {
            app->has_pending_value = false;
            app->last_op = 0;
        }
        app->display[0] = digit;
        app->display[1] = '\0';
        app->entering_new = false;
    } else if (len == 1 && app->display[0] == '0') {
        app->display[0] = digit;
        app->display[1] = '\0';
    } else if (len + 1 < sizeof(app->display)) {
        app->display[len] = digit;
        app->display[len + 1] = '\0';
    }

    reformat_display(app);
    ensure_keyboard_focus(app);
}

static void cb_decimal(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    if (app->error_state) reset_state(app);

    if (app->entering_new) {
        snprintf(app->display, sizeof(app->display), "0%c", app->decimal_char);
        app->entering_new = false;
        reformat_display(app);
        ensure_keyboard_focus(app);
        return;
    }

    if (!strchr(app->display, app->decimal_char)) {
        size_t len = strlen(app->display);
        if (len + 1 < sizeof(app->display)) {
            app->display[len] = app->decimal_char;
            app->display[len + 1] = '\0';
            reformat_display(app);
        }
    }
    ensure_keyboard_focus(app);
}

static void cb_backspace(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    if (app->error_state) {
        reset_state(app);
        return;
    }

    size_t len = strlen(app->display);
    if (len <= 1 || (len == 2 && app->display[0] == '-')) {
        set_display(app, "0");
        app->entering_new = true;
    } else {
        app->display[len - 1] = '\0';
        reformat_display(app);
    }
    ensure_keyboard_focus(app);
}

static void cb_clear(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    reset_state(app);
    ensure_keyboard_focus(app);
}

static void cb_toggle_sign(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    if (app->error_state) {
        reset_state(app);
        return;
    }

    if (strcmp(app->display, "0") == 0) {
        return;
    }

    if (app->display[0] == '-') {
        memmove(app->display, app->display + 1, strlen(app->display));
    } else {
        size_t len = strlen(app->display);
        if (len + 1 < sizeof(app->display)) {
            memmove(app->display + 1, app->display, len + 1);
            app->display[0] = '-';
        }
    }
    update_display(app);
    ensure_keyboard_focus(app);
}

static void cb_percent(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    if (app->error_state) {
        reset_state(app);
        return;
    }

    double value = current_input(app);
    if (app->has_pending_value) {
        value = app->stored_value * value / 100.0;
    } else {
        value = value / 100.0;
    }
    set_display_from_double(app, value);
    app->entering_new = true;
    app->last_op = 0;
    ensure_keyboard_focus(app);
}

static void cb_operator(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    if (app->error_state) reset_state(app);

    char op = (char)(uintptr_t)client_data;
    double value = current_input(app);

    if (app->pending_op && app->has_pending_value && !app->entering_new) {
        double result = 0.0;
        if (apply_operation(app, app->pending_op, value, &result)) {
            app->stored_value = result;
            set_display_from_double(app, result);
        } else {
            return;
        }
    } else if (!app->has_pending_value) {
        app->stored_value = value;
        app->has_pending_value = true;
    }

    app->pending_op = op;
    app->entering_new = true;
    app->last_op = 0;
    ensure_keyboard_focus(app);
}

static void cb_equals(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    if (app->error_state) {
        reset_state(app);
        return;
    }

    char op = app->pending_op;
    double rhs = current_input(app);

    if (!op) {
        if (!app->last_op) return;
        op = app->last_op;
        rhs = app->last_operand;
    } else {
        app->last_op = op;
        app->last_operand = rhs;
    }

    if (!app->has_pending_value) {
        app->stored_value = current_input(app);
        app->has_pending_value = true;
    }

    double result = 0.0;
    if (!apply_operation(app, op, rhs, &result)) {
        return;
    }

    app->stored_value = result;
    set_display_from_double(app, result);
    app->pending_op = 0;
    app->entering_new = true;
    ensure_keyboard_focus(app);
}

static void reformat_display(AppState *app)
{
    if (!app || app->error_state) return;

    size_t len = strlen(app->display);
    if (len > 0 && app->display[len - 1] == app->decimal_char) {
        double val = current_input(app);
        char tmp[MAX_DISPLAY_LEN];
        format_number(app, val, tmp, sizeof(tmp));
        size_t used = strlen(tmp);
        if (used + 1 < sizeof(app->display)) {
            memcpy(app->display, tmp, used);
            app->display[used] = app->decimal_char;
            app->display[used + 1] = '\0';
            update_display(app);
        }
        return;
    }

    double val = current_input(app);
    set_display_from_double(app, val);
    app->entering_new = false;
}

static void cb_toggle_thousands(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    Boolean state = XmToggleButtonGetState(w);
    app->show_thousands = (state != False);
    save_view_state(app);
    if (app->session_data) {
        session_data_set_int(app->session_data, "show_thousands", app->show_thousands ? 1 : 0);
    }
    reformat_display(app);
    ensure_keyboard_focus(app);
}

static void cb_menu_new(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;

    pid_t pid = fork();
    if (pid == 0) {
        execl(app->exec_path, app->exec_path, (char *)NULL);
        _exit(1);
    }
}

static void capture_and_save_session(AppState *app)
{
    if (!app || !app->session_data) return;
    session_capture_geometry(app->shell, app->session_data, "x", "y", "w", "h");
    session_data_set(app->session_data, "display", app->display);
    session_data_set_int(app->session_data, "show_thousands", app->show_thousands ? 1 : 0);
    session_save(app->shell, app->session_data, app->exec_path);
    save_view_state(app);
}

static void cb_menu_close(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    capture_and_save_session(app);
    XtAppSetExitFlag(app->app_context);
}

static void cb_wm_delete(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    capture_and_save_session(app);
    XtAppSetExitFlag(app->app_context);
}

static void cb_wm_save(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    capture_and_save_session(app);
}

/* -------------------------------------------------------------------------
 * About dialog
 * ------------------------------------------------------------------------- */

static void about_close_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    Widget shell = (Widget)client_data;
    if (shell && XtIsWidget(shell)) {
        XtDestroyWidget(shell);
    }
}

static void about_destroy_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    (void)client_data;
    g_about_shell = NULL;
}

static void show_about_dialog(AppState *app)
{
    if (!app) return;

    if (g_about_shell && XtIsWidget(g_about_shell)) {
        XtPopup(g_about_shell, XtGrabNone);
        return;
    }

    Arg shell_args[8];
    int sn = 0;
    XtSetArg(shell_args[sn], XmNtitle, "About Calculator"); sn++;
    XtSetArg(shell_args[sn], XmNallowShellResize, True); sn++;
    XtSetArg(shell_args[sn], XmNtransientFor, app->shell); sn++;
    g_about_shell = XmCreateDialogShell(app->shell, "aboutCalcShell", shell_args, sn);
    if (!g_about_shell) return;
    XtAddCallback(g_about_shell, XmNdestroyCallback, about_destroy_cb, NULL);

    Widget form = XmCreateForm(g_about_shell, "aboutForm", NULL, 0);
    XtManageChild(form);

    Arg args[8];
    int n = 0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
    Widget notebook = XmCreateNotebook(form, "aboutNotebook", args, n);
    XtVaSetValues(notebook, XmNmarginWidth, 12, XmNmarginHeight, 12, NULL);
    XtManageChild(notebook);

    Widget ok = XtVaCreateManagedWidget(
        "aboutOk",
        xmPushButtonWidgetClass, form,
        XmNlabelString, XmStringCreateLocalized("OK"),
        XmNbottomAttachment, XmATTACH_FORM,
        XmNbottomOffset,    8,
        XmNleftAttachment,  XmATTACH_POSITION,
        XmNrightAttachment, XmATTACH_POSITION,
        XmNleftPosition,    40,
        XmNrightPosition,   60,
        NULL
    );

    XtVaSetValues(notebook,
                  XmNbottomAttachment, XmATTACH_WIDGET,
                  XmNbottomWidget,     ok,
                  XmNbottomOffset,     8,
                  NULL);

    XtAddCallback(ok, XmNactivateCallback, about_close_cb, (XtPointer)g_about_shell);

    about_add_standard_pages(notebook, 1,
                             "About",
                             "CK-Core Calculator",
                             "(c) 2026 by Dr. C. Klukas",
                             True);

    XtRealizeWidget(g_about_shell);
    Atom wm_delete = XmInternAtom(XtDisplay(g_about_shell),
                                  "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(g_about_shell, wm_delete,
                            about_close_cb, (XtPointer)g_about_shell);
    XmActivateWMProtocol(g_about_shell, wm_delete);

    XtPopup(g_about_shell, XtGrabNone);
}

static void about_menu_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    show_about_dialog(g_app);
}

/* -------------------------------------------------------------------------
 * Keyboard handling
 * ------------------------------------------------------------------------- */

static void activate_button(Widget btn, XEvent *event)
{
    if (!btn) return;
    XtCallActionProc(btn, "ArmAndActivate", event, NULL, 0);
}

static void key_press_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont)
{
    (void)w;
    (void)client_data;
    (void)cont;
    if (!g_app || !event || event->type != KeyPress) return;

    AppState *app = g_app;
    char keybuf[8];
    KeySym sym = NoSymbol;
    XLookupString(&event->xkey, keybuf, sizeof(keybuf), &sym, NULL);
    if (sym == NoSymbol) {
        sym = XLookupKeysym(&event->xkey, 0);
    }
    if (sym == NoSymbol) {
        sym = XLookupKeysym(&event->xkey, 1);
    }
    Display *dpy = XtDisplay(app->shell);
    KeyCode kc = event->xkey.keycode;
    KeyCode kc_kp_enter = (dpy && app->shell) ? XKeysymToKeycode(dpy, XK_KP_Enter) : 0;
    KeyCode kc_return   = (dpy && app->shell) ? XKeysymToKeycode(dpy, XK_Return)   : 0;

    /* Numpad without NumLock (KP_End, KP_Down, ...) to digits */
    int kp_digit = -1;
    switch (sym) {
        case XK_KP_Insert: kp_digit = 0; break;
        case XK_KP_End:    kp_digit = 1; break;
        case XK_KP_Down:   kp_digit = 2; break;
        case XK_KP_Next:   kp_digit = 3; break;
        case XK_KP_Left:   kp_digit = 4; break;
        case XK_KP_Begin:  kp_digit = 5; break;
        case XK_KP_Right:  kp_digit = 6; break;
        case XK_KP_Home:   kp_digit = 7; break;
        case XK_KP_Up:     kp_digit = 8; break;
        case XK_KP_Prior:  kp_digit = 9; break;
        default: break;
    }
    if (kp_digit >= 0 && kp_digit <= 9 && app->btn_digits[kp_digit]) {
        activate_button(app->btn_digits[kp_digit], event);
        return;
    }

    /* ASCII digit via lookup string */
    if (keybuf[0] >= '0' && keybuf[0] <= '9') {
        int idx = keybuf[0] - '0';
        if (idx >= 0 && idx <= 9 && app->btn_digits[idx]) {
            activate_button(app->btn_digits[idx], event);
            return;
        }
    }

    /* Enter/Return via lookup string (helps some keypad mappings) */
    if (keybuf[0] == '\r' || keybuf[0] == '\n') {
        activate_button(app->btn_eq, event);
        return;
    }

    /* Enter/Return via keycode fallback (some layouts may not map keysym) */
    if ((kc_kp_enter && kc == kc_kp_enter) || (kc_return && kc == kc_return)) {
        activate_button(app->btn_eq, event);
        return;
    }

    /* Digits (main keyboard and keypad) */
    if ((sym >= XK_0 && sym <= XK_9) || (sym >= XK_KP_0 && sym <= XK_KP_9)) {
        int idx = (sym >= XK_KP_0 && sym <= XK_KP_9)
                    ? (int)(sym - XK_KP_0)
                    : (int)(sym - XK_0);
        if (idx >= 0 && idx <= 9 && app->btn_digits[idx]) {
            activate_button(app->btn_digits[idx], event);
        }
        return;
    }

    switch (sym) {
        case XK_BackSpace:
        case XK_Delete:
        case XK_KP_Delete:
            activate_button(app->btn_back, event);
            return;
        case XK_a:
        case XK_A:
        case XK_c:
        case XK_C:
            activate_button(app->btn_ac, event);
            return;
        case XK_plus:
        case XK_KP_Add:
            activate_button(app->btn_plus, event);
            return;
        case XK_minus:
        case XK_KP_Subtract:
            activate_button(app->btn_minus, event);
            return;
        case XK_asterisk:
        case XK_KP_Multiply:
            activate_button(app->btn_mul, event);
            return;
        case XK_slash:
        case XK_KP_Divide:
            activate_button(app->btn_div, event);
            return;
        case XK_percent:
            activate_button(app->btn_percent, event);
            return;
        case XK_equal:
        case XK_KP_Enter:
        case XK_Return:
        case XK_Linefeed:
            activate_button(app->btn_eq, event);
            return;
        case XK_period:
        case XK_comma:
        case XK_KP_Decimal:
        case XK_KP_Separator:
            /* Only react if key matches locale decimal or KP decimal */
            if ((sym == XK_period && app->decimal_char == '.') ||
                (sym == XK_comma  && app->decimal_char == ',') ||
                sym == XK_KP_Decimal) {
                activate_button(app->btn_decimal, event);
            }
            return;
        default:
            break;
    }
}

static void ensure_keyboard_focus(AppState *app)
{
    if (!app || !app->shell) return;
    Widget target = NULL;
    if (app->key_focus_proxy) {
        target = app->key_focus_proxy;
    } else if (app->btn_digits[0]) {
        target = app->btn_digits[0];
    } else if (app->main_form) {
        target = app->main_form;
    }
    if (target) {
        XtSetKeyboardFocus(app->shell, target);
        XmProcessTraversal(target, XmTRAVERSE_CURRENT);
    }
}

static void focus_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont)
{
    (void)w;
    (void)client_data;
    (void)cont;
    if (!event) return;
    if (event->type == FocusIn) {
        ensure_keyboard_focus(g_app);
    }
}

/* -------------------------------------------------------------------------
 * UI helpers
 * ------------------------------------------------------------------------- */

static Widget create_key_button(Widget parent, const char *name, const char *label,
                                Widget top_widget, Boolean align_top,
                                int col, int col_span,
                                XtCallbackProc cb, XtPointer data)
{
    Arg args[12];
    int n = 0;
    XtSetArg(args[n], XmNleftAttachment,  XmATTACH_POSITION); n++;
    XtSetArg(args[n], XmNleftPosition,    col * 25); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_POSITION); n++;
    XtSetArg(args[n], XmNrightPosition,   (col + col_span) * 25); n++;
    XtSetArg(args[n], XmNleftOffset,      4); n++;
    XtSetArg(args[n], XmNrightOffset,     4); n++;
    if (top_widget) {
        XtSetArg(args[n], XmNtopWidget, top_widget); n++;
        if (align_top) {
            XtSetArg(args[n], XmNtopAttachment, XmATTACH_OPPOSITE_WIDGET); n++;
            XtSetArg(args[n], XmNtopOffset,     0); n++;
        } else {
            XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
            XtSetArg(args[n], XmNtopOffset,     6); n++;
        }
    } else {
        XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
        XtSetArg(args[n], XmNtopOffset,     6); n++;
    }

    Widget btn = XmCreatePushButton(parent, (char *)name, args, n);
    XmString xms = XmStringCreateLocalized((char *)label);
    XtVaSetValues(btn, XmNlabelString, xms, NULL);
    XmStringFree(xms);
    XtManageChild(btn);
    if (cb) XtAddCallback(btn, XmNactivateCallback, cb, data);
    return btn;
}

static void build_ui(AppState *app)
{
    if (!app) return;

    Widget main_form = XmCreateForm(app->shell, "mainForm", NULL, 0);
    XtVaSetValues(main_form,
                  XmNmarginWidth,  0,
                  XmNmarginHeight, 0,
                  NULL);
    XtManageChild(main_form);
    app->main_form = main_form;

    /* Hidden focus proxy to capture keyboard events */
    Arg fargs[10];
    int fn = 0;
    XtSetArg(fargs[fn], XmNwidth, 1); fn++;
    XtSetArg(fargs[fn], XmNheight, 1); fn++;
    XtSetArg(fargs[fn], XmNhighlightThickness, 0); fn++;
    XtSetArg(fargs[fn], XmNshadowThickness, 0); fn++;
    XtSetArg(fargs[fn], XmNtraversalOn, True); fn++;
    XtSetArg(fargs[fn], XmNnavigationType, XmTAB_GROUP); fn++;
    XtSetArg(fargs[fn], XmNtopAttachment, XmATTACH_FORM); fn++;
    XtSetArg(fargs[fn], XmNleftAttachment, XmATTACH_FORM); fn++;
    app->key_focus_proxy = XmCreateDrawingArea(main_form, "focusProxy", fargs, fn);
    XtManageChild(app->key_focus_proxy);
    XmAddTabGroup(app->key_focus_proxy);
    XtAddEventHandler(app->key_focus_proxy, KeyPressMask, False, key_press_handler, NULL);
    XtAddEventHandler(app->key_focus_proxy, FocusChangeMask, False, focus_handler, NULL);
    XtVaSetValues(app->key_focus_proxy, XmNtraversalOn, True, NULL);

    /* Menubar (flush to edges) */
    Widget menubar = XmCreateMenuBar(main_form, "menubar", NULL, 0);
    XtManageChild(menubar);
    XtVaSetValues(menubar,
                  XmNtopAttachment,   XmATTACH_FORM,
                  XmNleftAttachment,  XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  NULL);

    /* Content area with padding for display + keypad */
    Widget content_form = XmCreateForm(main_form, "contentForm", NULL, 0);
    XtVaSetValues(content_form,
                  XmNmarginWidth,  8,
                  XmNmarginHeight, 8,
                  XmNtopAttachment,    XmATTACH_WIDGET,
                  XmNtopWidget,        menubar,
                  XmNtopOffset,        6,
                  XmNleftAttachment,   XmATTACH_FORM,
                  XmNrightAttachment,  XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  NULL);
    XtManageChild(content_form);

    /* Window menu */
    Widget window_pane = XmCreatePulldownMenu(menubar, "windowMenu", NULL, 0);
    Widget window_cascade = XtVaCreateManagedWidget(
        "windowCascade",
        xmCascadeButtonWidgetClass, menubar,
        XmNlabelString, XmStringCreateLocalized("Window"),
        XmNmnemonic, 'W',
        XmNsubMenuId, window_pane,
        NULL
    );
    (void)window_cascade;

    Widget new_item = XtVaCreateManagedWidget(
        "newItem",
        xmPushButtonWidgetClass, window_pane,
        XmNlabelString,    XmStringCreateLocalized("New"),
        XmNaccelerator,    "Ctrl<Key>N",
        XmNacceleratorText,XmStringCreateLocalized("Ctrl+N"),
        NULL
    );
    XtAddCallback(new_item, XmNactivateCallback, cb_menu_new, NULL);

    Widget close_item = XtVaCreateManagedWidget(
        "closeItem",
        xmPushButtonWidgetClass, window_pane,
        XmNlabelString,    XmStringCreateLocalized("Close"),
        XmNaccelerator,    "Alt<Key>F4",
        XmNacceleratorText,XmStringCreateLocalized("Alt+F4"),
        NULL
    );
    XtAddCallback(close_item, XmNactivateCallback, cb_menu_close, NULL);

    /* View menu */
    Widget view_pane = XmCreatePulldownMenu(menubar, "viewMenu", NULL, 0);
    Widget view_cascade = XtVaCreateManagedWidget(
        "viewCascade",
        xmCascadeButtonWidgetClass, menubar,
        XmNlabelString, XmStringCreateLocalized("View"),
        XmNmnemonic, 'V',
        XmNsubMenuId, view_pane,
        NULL
    );
    (void)view_cascade;

    Widget thousand_toggle = XtVaCreateManagedWidget(
        "thousandToggle",
        xmToggleButtonWidgetClass, view_pane,
        XmNlabelString, XmStringCreateLocalized("Show thousands separators"),
        XmNset, app->show_thousands ? True : False,
        NULL
    );
    XtAddCallback(thousand_toggle, XmNvalueChangedCallback, cb_toggle_thousands, NULL);

    /* Help menu */
    Widget help_pane = XmCreatePulldownMenu(menubar, "helpMenu", NULL, 0);
    Widget help_cascade = XtVaCreateManagedWidget(
        "helpCascade",
        xmCascadeButtonWidgetClass, menubar,
        XmNlabelString, XmStringCreateLocalized("Help"),
        XmNmnemonic, 'H',
        XmNsubMenuId, help_pane,
        XmNmenuHelpWidget, NULL,
        NULL
    );
    XtVaSetValues(menubar, XmNmenuHelpWidget, help_cascade, NULL);

    Widget about_item = XtVaCreateManagedWidget(
        "aboutCalc",
        xmPushButtonWidgetClass, help_pane,
        XmNlabelString, XmStringCreateLocalized("About"),
        NULL
    );
    XtAddCallback(about_item, XmNactivateCallback, about_menu_cb, NULL);

    /* Display label */
    Widget display_label = XtVaCreateManagedWidget(
        "displayLabel",
        xmLabelWidgetClass, content_form,
        XmNalignment,      XmALIGNMENT_END,
        XmNrecomputeSize,  False,
        XmNtopAttachment,  XmATTACH_FORM,
        XmNtopOffset,      0,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment,XmATTACH_FORM,
        XmNheight,         36,
        XmNmarginHeight,   8,
        XmNmarginWidth,    8,
        NULL
    );
    app->display_label = display_label;
    update_display(app);

    /* Keypad */
    Arg pad_args[4];
    int pn = 0;
    XtSetArg(pad_args[pn], XmNfractionBase, 100); pn++;
    Widget keypad = XmCreateForm(content_form, "keypadForm", pad_args, pn);
    XtVaSetValues(keypad,
                  XmNtopAttachment,    XmATTACH_WIDGET,
                  XmNtopWidget,        display_label,
                  XmNtopOffset,        8,
                  XmNleftAttachment,   XmATTACH_FORM,
                  XmNrightAttachment,  XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  NULL);
    XtManageChild(keypad);

    Widget row_anchor = NULL;
    Widget row_top = NULL;
    /* Row 1 */
    row_anchor = create_key_button(keypad, "backBtn", "◀", NULL, False, 0, 1, cb_backspace, NULL);
    app->btn_back = row_anchor;
    row_top = row_anchor;
    app->btn_ac = create_key_button(keypad, "acBtn",   "AC",   row_top, True, 1, 1, cb_clear, NULL);
    app->btn_percent = create_key_button(keypad, "percentBtn", "%", row_top, True, 2, 1, cb_percent, NULL);
    app->btn_div = create_key_button(keypad, "divBtn", "/", row_top, True, 3, 1, cb_operator, (XtPointer)(uintptr_t)'/');
    /* Match Back height to AC height */
    if (app->btn_back && app->btn_ac) {
        Dimension ac_h = 0;
        XtVaGetValues(app->btn_ac, XmNheight, &ac_h, NULL);
        if (ac_h > 0) {
            XtVaSetValues(app->btn_back, XmNheight, ac_h, NULL);
        }
    }

    /* Row 2 */
    row_anchor = create_key_button(keypad, "sevenBtn", "7", row_anchor, False, 0, 1, cb_digit, (XtPointer)(uintptr_t)'7');
    app->btn_digits[7] = row_anchor;
    row_top = row_anchor;
    app->btn_digits[8] = create_key_button(keypad, "eightBtn", "8", row_top, True, 1, 1, cb_digit, (XtPointer)(uintptr_t)'8');
    app->btn_digits[9] = create_key_button(keypad, "nineBtn",  "9", row_top, True, 2, 1, cb_digit, (XtPointer)(uintptr_t)'9');
    app->btn_mul = create_key_button(keypad, "mulBtn",   "*", row_top, True, 3, 1, cb_operator, (XtPointer)(uintptr_t)'*');

    /* Row 3 */
    row_anchor = create_key_button(keypad, "fourBtn", "4", row_anchor, False, 0, 1, cb_digit, (XtPointer)(uintptr_t)'4');
    app->btn_digits[4] = row_anchor;
    row_top = row_anchor;
    app->btn_digits[5] = create_key_button(keypad, "fiveBtn", "5", row_top, True, 1, 1, cb_digit, (XtPointer)(uintptr_t)'5');
    app->btn_digits[6] = create_key_button(keypad, "sixBtn",  "6", row_top, True, 2, 1, cb_digit, (XtPointer)(uintptr_t)'6');
    app->btn_minus = create_key_button(keypad, "minusBtn","-", row_top, True, 3, 1, cb_operator, (XtPointer)(uintptr_t)'-');

    /* Row 4 */
    row_anchor = create_key_button(keypad, "oneBtn", "1", row_anchor, False, 0, 1, cb_digit, (XtPointer)(uintptr_t)'1');
    app->btn_digits[1] = row_anchor;
    row_top = row_anchor;
    app->btn_digits[2] = create_key_button(keypad, "twoBtn", "2", row_top, True, 1, 1, cb_digit, (XtPointer)(uintptr_t)'2');
    app->btn_digits[3] = create_key_button(keypad, "threeBtn", "3", row_top, True, 2, 1, cb_digit, (XtPointer)(uintptr_t)'3');
    app->btn_plus = create_key_button(keypad, "plusBtn", "+", row_top, True, 3, 1, cb_operator, (XtPointer)(uintptr_t)'+');

    /* Row 5 */
    char decimal_label[2] = {app->decimal_char, '\0'};
    row_anchor = create_key_button(keypad, "signBtn", "+/-", row_anchor, False, 0, 1, cb_toggle_sign, NULL);
    app->btn_sign = row_anchor;
    row_top = row_anchor;
    app->btn_digits[0] = create_key_button(keypad, "zeroBtn", "0", row_top, True, 1, 1, cb_digit, (XtPointer)(uintptr_t)'0');
    app->btn_decimal = create_key_button(keypad, "decimalBtn", decimal_label, row_top, True, 2, 1, cb_decimal, NULL);
    Widget eq_btn = create_key_button(keypad, "eqBtn", "=", row_top, True, 3, 1, cb_equals, NULL);
    app->btn_eq = eq_btn;
    XtVaSetValues(eq_btn, XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 6, NULL);
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    AppState app;
    memset(&app, 0, sizeof(app));
    g_app = &app;
    XtSetLanguageProc(NULL, NULL, NULL);
    setlocale(LC_ALL, "");

    init_locale_settings(&app);
    app.show_thousands = true;
    load_view_state(&app);

    char *session_id = session_parse_argument(&argc, argv);
    app.session_data = session_data_create(session_id);
    free(session_id);
    init_exec_path(&app, argv[0]);
    reset_state(&app);

    app.shell = XtVaAppInitialize(&app.app_context,
                                  "CkCalc",
                                  NULL, 0,
                                  &argc, argv,
                                  NULL,
                                  XmNtitle, "Calculator",
                                  XmNminWidth, 220,
                                  XmNminHeight, 300,
                                  XmNkeyboardFocusPolicy, XmEXPLICIT,
                                  NULL);

    DtInitialize(XtDisplay(app.shell), app.shell, "CkCalc", "CkCalc");

    /* Load session data (geometry + display) */
    if (app.session_data) {
        if (session_load(app.shell, app.session_data)) {
            const char *saved_disp = session_data_get(app.session_data, "display");
            if (saved_disp && saved_disp[0]) {
                set_display(&app, saved_disp);
                app.entering_new = true;
            }
            if (session_data_has(app.session_data, "show_thousands")) {
                app.show_thousands = session_data_get_int(app.session_data, "show_thousands", app.show_thousands ? 1 : 0) != 0;
            }
        }
    }

    build_ui(&app);

    if (app.session_data) {
        if (!session_apply_geometry(app.shell, app.session_data, "x", "y", "w", "h")) {
            center_shell_on_screen(app.shell);
        }
    } else {
        center_shell_on_screen(app.shell);
    }

    XtRealizeWidget(app.shell);

    /* Keyboard handler for shortcuts/digits */
    XtAddEventHandler(app.shell, KeyPressMask, True, key_press_handler, NULL);
    XtAddEventHandler(app.shell, FocusChangeMask, False, focus_handler, NULL);
    if (app.main_form) {
        XtAddEventHandler(app.main_form, KeyPressMask, True, key_press_handler, NULL);
        XtAddEventHandler(app.main_form, FocusChangeMask, False, focus_handler, NULL);
    }
    if (app.key_focus_proxy) {
        XtSetKeyboardFocus(app.shell, app.key_focus_proxy);
        XmProcessTraversal(app.key_focus_proxy, XmTRAVERSE_CURRENT);
    } else if (app.main_form) {
        XtSetKeyboardFocus(app.shell, app.main_form);
    }
    ensure_keyboard_focus(&app);

    Atom wm_delete = XmInternAtom(XtDisplay(app.shell), "WM_DELETE_WINDOW", False);
    Atom wm_save   = XmInternAtom(XtDisplay(app.shell), "WM_SAVE_YOURSELF", False);
    XmAddWMProtocolCallback(app.shell, wm_delete, cb_wm_delete, NULL);
    XmAddWMProtocolCallback(app.shell, wm_save,   cb_wm_save, NULL);
    XmActivateWMProtocol(app.shell, wm_delete);
    XmActivateWMProtocol(app.shell, wm_save);

    XtAppMainLoop(app.app_context);

    session_data_free(app.session_data);
    return 0;
}
