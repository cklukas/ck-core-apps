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
#include <math.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

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
#include <Xm/CutPaste.h>
#include <Xm/Protocols.h>
#include <Xm/MwmUtil.h>
#include <Xm/SeparatoG.h>
#include <Dt/Session.h>
#include <Dt/Dt.h>

#include "../shared/session_utils.h"
#include "../shared/about_dialog.h"
#include "../shared/cde_palette.h"
#include "app_state.h"
#include "app_state_utils.h"
#include "logic/formula_eval.h"
#include "logic/display_api.h"
#include "clipboard.h"
#include "ui/keypad_layout.h"
#include "ui/sci_visuals.h"
#include "ui/menu_handlers.h"
#include "ui/window_metrics.h"
#include "logic/formula_mode.h"
#include "logic/input_handler.h"

static AppState *g_app = NULL;

static void key_press_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont);
static void key_release_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont);
static void focus_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont);
static void display_button_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont);
static void cb_display_copy(Widget w, XtPointer client_data, XtPointer call_data);
static void cb_display_paste(Widget w, XtPointer client_data, XtPointer call_data);
static void cb_mode_toggle(Widget w, XtPointer client_data, XtPointer call_data);
static void set_mode(AppState *app, int mode, Boolean from_menu);

static void display_button_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont)
{
    (void)w;
    (void)cont;
    AppState *app = (AppState *)client_data;
    if (!app || !event) return;
    if (event->type == ButtonPress) {
        XButtonEvent *bev = &event->xbutton;
        if (bev->button == Button3 && app->display_menu) {
            XmMenuPosition(app->display_menu, bev);
            XtManageChild(app->display_menu);
        }
    }
}

static void cb_display_copy(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = (AppState *)client_data;
    ck_calc_clipboard_copy(app);
    ck_calc_ensure_keyboard_focus(app);
}

static void cb_display_paste(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = (AppState *)client_data;
    ck_calc_clipboard_paste(app);
    ck_calc_ensure_keyboard_focus(app);
}

static void cb_mode_toggle(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    Boolean set = False;
    XtVaGetValues(w, XmNset, &set, NULL);
    if (!set) return;
    int mode = (int)(intptr_t)client_data;
    set_mode(app, mode, True);
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
    app->calc_state.error_state = true;
    app->calc_state.pending_op = 0;
    app->calc_state.has_pending_value = false;
    app->calc_state.last_op = 0;
    ck_calc_set_display(app, "Error");
    app->calc_state.entering_new = true;
    app->last_rand_len = 0;
    app->last_rand_token[0] = '\0';
}

static Pixel get_dim_foreground(AppState *app, Pixel base)
{
    if (!app || !app->shell) return base;
    if (app->trig_dim_fg_set) return app->trig_dim_fg;
    Display *dpy = XtDisplay(app->shell);
    Colormap cmap = DefaultColormapOfScreen(XtScreen(app->shell));
    XColor screen, exact;
    if (XAllocNamedColor(dpy, cmap, "gray60", &screen, &exact)) {
        app->trig_dim_fg = screen.pixel;
    } else {
        app->trig_dim_fg = base;
    }
    app->trig_dim_fg_set = True;
    return app->trig_dim_fg;
}

static void update_trig_mode_display(AppState *app)
{
    if (!app) return;
    Pixel base_fg = BlackPixelOfScreen(XtScreen(app->shell));
    if (app->display_label) {
        XtVaGetValues(app->display_label, XmNforeground, &base_fg, NULL);
    }
    Pixel dim = get_dim_foreground(app, base_fg);
    for (int i = 0; i < TRIG_MODE_COUNT; ++i) {
        Widget lbl = app->trig_mode_labels[i];
        if (!lbl) continue;
        Pixel fg = (i == app->trig_mode) ? base_fg : dim;
        XtVaSetValues(lbl, XmNforeground, fg, NULL);
    }
}

static void cycle_trig_mode(AppState *app)
{
    if (!app) return;
    int next = ((int)app->trig_mode + 1) % TRIG_MODE_COUNT;
    app->trig_mode = (TrigMode)next;
    ck_calc_save_view_state(app);
    if (app->session_data) {
        session_data_set_int(app->session_data, "trig_mode", app->trig_mode);
    }
    update_trig_mode_display(app);
}

static void seed_formula_if_empty(AppState *app)
{
    if (!app) return;
    if (!formula_is_empty(&app->formula_ctx)) return;
    if (app->formula_showing_result) {
        formula_mode_seed_with_last_result(app);
        return;
    }
    double value = ck_calc_current_input(app);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.12g", value);
    formula_append_str(&app->formula_ctx, buf);
    app->formula_showing_result = false;
}

static bool get_current_value(AppState *app, double *out_value)
{
    if (!app || !out_value) return false;
    if (app->calc_state.error_state) return false;

    if (app->mode == 1) {
        if (app->formula_showing_result) {
            *out_value = app->formula_last_result;
            return true;
        }
        if (!formula_is_empty(&app->formula_ctx)) {
            double val = 0.0;
            if (!formula_evaluate(&app->formula_ctx, &val, app->trig_mode)) {
                return false;
            }
            *out_value = val;
            return true;
        }
    }

    *out_value = ck_calc_current_input(app);
    return true;
}

static bool append_formula_token(AppState *app, const char *text, Boolean seed_with_current)
{
    if (!app || !text || app->mode != 1) return false;
    if (app->calc_state.error_state) {
        ck_calc_reset_state(app);
    }
    if (app->formula_showing_result) {
        formula_mode_seed_with_last_result(app);
    } else {
        formula_mode_prepare_for_edit(app);
        if (seed_with_current) {
            seed_formula_if_empty(app);
        }
    }
    if (!formula_append_str(&app->formula_ctx, text)) {
        formula_clear(&app->formula_ctx);
        set_error(app);
        return false;
    }
    app->formula_showing_result = false;
    formula_mode_update_display(app);
    ck_calc_ensure_keyboard_focus(app);
    return true;
}

static bool formula_append_token(AppState *app, const char *text)
{
    return append_formula_token(app, text, False);
}

static void store_formula_result(AppState *app, double value)
{
    if (!app) return;
    formula_clear(&app->formula_ctx);
    app->last_rand_len = 0;
    app->last_rand_token[0] = '\0';
    app->formula_last_result = value;
    app->formula_showing_result = true;
    ck_calc_set_display_from_double(app, value);
    ck_calc_ensure_keyboard_focus(app);
}

static bool sci_compute_unary(const char *op, double input, double *out)
{
    if (!op || !out) return false;
    double result = 0.0;
    if (strcmp(op, "1/x") == 0) {
        if (input == 0.0) return false;
        result = 1.0 / input;
    } else if (strcmp(op, "x^2") == 0) {
        result = input * input;
    } else if (strcmp(op, "x^3") == 0) {
        result = input * input * input;
    } else if (strcmp(op, "sqrt(x)") == 0 || strcmp(op, "sqrt") == 0) {
        if (input < 0.0) return false;
        result = sqrt(input);
    } else if (strcmp(op, "3rd root(x)") == 0 || strcmp(op, "3rd_root") == 0) {
        result = cbrt(input);
    } else if (strcmp(op, "e^x") == 0) {
        result = exp(input);
    } else if (strcmp(op, "10^x") == 0) {
        result = pow(10.0, input);
    } else if (strcmp(op, "ln") == 0) {
        if (input <= 0.0) return false;
        result = log(input);
    } else if (strcmp(op, "log10") == 0) {
        if (input <= 0.0) return false;
        result = log10(input);
    } else if (strcmp(op, "x!") == 0) {
        if (input < 0.0) return false;
        double rounded = floor(input + 0.5);
        if (fabs(input - rounded) > 1e-9) return false;
        if (rounded > 20.0) return false;
        double fact = 1.0;
        for (int i = 2; i <= (int)rounded; ++i) {
            fact *= (double)i;
        }
        result = fact;
    } else if (strcmp(op, "sinh") == 0) {
        result = sinh(input);
    } else if (strcmp(op, "cosh") == 0) {
        result = cosh(input);
    } else if (strcmp(op, "tanh") == 0) {
        result = tanh(input);
    } else {
        return false;
    }

    if (!isfinite(result)) {
        return false;
    }
    *out = result;
    return true;
}

void ck_calc_cb_append_str(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = g_app;
    const char *text = (const char *)client_data;
    if (!app || !text || app->mode != 1) return;
    (void)formula_append_token(app, text);
}

void ck_calc_cb_insert_constant(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = g_app;
    const char *name = (const char *)client_data;
    if (!app || !name || app->mode != 1) return;
    double value = 0.0;
    if (strcmp(name, "pi") == 0) {
        value = 3.14159265358979323846;
    } else if (strcmp(name, "e") == 0) {
        value = 2.71828182845904523536;
    } else {
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.12g", value);
    (void)formula_append_token(app, buf);
}

void ck_calc_cb_insert_random(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app || app->mode != 1) return;
    if (app->calc_state.error_state) {
        ck_calc_reset_state(app);
    }
    formula_mode_prepare_for_edit(app);

    const char *formula = formula_text(&app->formula_ctx);
    size_t len = formula ? strlen(formula) : 0;
    if (app->last_rand_len > 0 && len >= app->last_rand_len && formula) {
        if (strcmp(formula + len - app->last_rand_len, app->last_rand_token) == 0) {
            if (app->formula_ctx.len >= app->last_rand_len) {
                app->formula_ctx.len -= app->last_rand_len;
                app->formula_ctx.buffer[app->formula_ctx.len] = '\0';
            }
        }
    }

    double value = (double)rand() / (double)RAND_MAX;
    char buf[32];
    snprintf(buf, sizeof(buf), "%.12g", value);
    if (!formula_append_str(&app->formula_ctx, buf)) {
        formula_clear(&app->formula_ctx);
        app->last_rand_len = 0;
        app->last_rand_token[0] = '\0';
        set_error(app);
        return;
    }
    app->last_rand_len = strlen(buf);
    strncpy(app->last_rand_token, buf, sizeof(app->last_rand_token) - 1);
    app->last_rand_token[sizeof(app->last_rand_token) - 1] = '\0';
    formula_mode_update_display(app);
    ck_calc_ensure_keyboard_focus(app);
}

void ck_calc_cb_toggle_trig_mode(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app || app->mode != 1) return;
    cycle_trig_mode(app);
}

void ck_calc_cb_unary_math(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = g_app;
    const char *op = (const char *)client_data;
    if (!app || !op) return;
    if (app->mode != 1) {
        ck_calc_ensure_keyboard_focus(app);
        return;
    }

    Boolean second = sci_visuals_is_second_active(app);
    const char *token = NULL;
    Boolean seed = False;

    if (strcmp(op, "x!") == 0) {
        token = "!";
        seed = True;
    } else if (strcmp(op, "x^2") == 0) {
        token = "^2";
        seed = True;
    } else if (strcmp(op, "x^3") == 0) {
        token = "^3";
        seed = True;
    } else if (strcmp(op, "x^y") == 0 || (second && strcmp(op, "e^x") == 0)) {
        token = "^";
        seed = True;
    } else if (strcmp(op, "1/x") == 0) {
        token = "^(-1)";
        seed = True;
    } else if (strcmp(op, "y root x") == 0) {
        token = "^(1/";
        seed = True;
    } else if (strcmp(op, "sqrt(x)") == 0 || strcmp(op, "sqrt") == 0) {
        token = second ? "asin(" : "sqrt(";
    } else if (strcmp(op, "3rd root(x)") == 0 || strcmp(op, "3rd_root") == 0) {
        token = second ? "acos(" : "cbrt(";
    } else if (strcmp(op, "e^x") == 0) {
        token = "e^";
    } else if (strcmp(op, "10^x") == 0) {
        token = second ? "2^" : "10^";
    } else if (strcmp(op, "ln") == 0) {
        token = "ln(";
    } else if (strcmp(op, "log10") == 0) {
        token = second ? "log2(" : "log10(";
    } else if (strcmp(op, "sin") == 0) {
        token = second ? "asin(" : "sin(";
    } else if (strcmp(op, "cos") == 0) {
        token = second ? "acos(" : "cos(";
    } else if (strcmp(op, "tan") == 0) {
        token = second ? "atan(" : "tan(";
    } else if (strcmp(op, "sinh") == 0) {
        token = second ? "asinh(" : "sinh(";
    } else if (strcmp(op, "cosh") == 0) {
        token = second ? "acosh(" : "cosh(";
    } else if (strcmp(op, "tanh") == 0) {
        token = second ? "atanh(" : "tanh(";
    }

    if (token) {
        append_formula_token(app, token, seed);
        return;
    }

    double value = 0.0;
    if (!get_current_value(app, &value)) {
        formula_clear(&app->formula_ctx);
        set_error(app);
        return;
    }
    double result = 0.0;
    if (!sci_compute_unary(op, value, &result)) {
        formula_clear(&app->formula_ctx);
        set_error(app);
        return;
    }
    if (app->mode == 1) {
        store_formula_result(app, result);
    } else {
        ck_calc_set_display_from_double(app, result);
        app->calc_state.entering_new = true;
        ck_calc_ensure_keyboard_focus(app);
    }
}

void ck_calc_cb_memory_clear(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    app->memory_value = 0.0;
    app->memory_set = false;
    ck_calc_ensure_keyboard_focus(app);
}

void ck_calc_cb_memory_add(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    double value = 0.0;
    if (!get_current_value(app, &value)) {
        formula_clear(&app->formula_ctx);
        set_error(app);
        return;
    }
    if (!app->memory_set) {
        app->memory_value = 0.0;
        app->memory_set = true;
    }
    app->memory_value += value;
    ck_calc_ensure_keyboard_focus(app);
}

void ck_calc_cb_memory_subtract(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    double value = 0.0;
    if (!get_current_value(app, &value)) {
        formula_clear(&app->formula_ctx);
        set_error(app);
        return;
    }
    if (!app->memory_set) {
        app->memory_value = 0.0;
        app->memory_set = true;
    }
    app->memory_value -= value;
    ck_calc_ensure_keyboard_focus(app);
}

void ck_calc_cb_memory_recall(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app || !app->memory_set) return;
    double value = app->memory_value;
    if (app->mode == 1) {
        formula_clear(&app->formula_ctx);
        app->formula_showing_result = false;
        char buf[32];
        snprintf(buf, sizeof(buf), "%.12g", value);
        formula_append_str(&app->formula_ctx, buf);
        formula_mode_update_display(app);
    } else {
        ck_calc_set_display_from_double(app, value);
        app->calc_state.entering_new = true;
    }
    ck_calc_ensure_keyboard_focus(app);
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
    char digit = (char)(uintptr_t)client_data;
    input_handler_handle_digit(app, digit);
}

void ck_calc_cb_digit(Widget w, XtPointer client_data, XtPointer call_data)
{
    cb_digit(w, client_data, call_data);
}

static void cb_decimal(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    input_handler_handle_decimal(app);
}

void ck_calc_cb_decimal(Widget w, XtPointer client_data, XtPointer call_data)
{
    cb_decimal(w, client_data, call_data);
}

static void cb_backspace(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    input_handler_handle_backspace(app);
}

void ck_calc_cb_backspace(Widget w, XtPointer client_data, XtPointer call_data)
{
    cb_backspace(w, client_data, call_data);
}

static void cb_clear(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    input_handler_handle_clear(app);
}

void ck_calc_cb_clear(Widget w, XtPointer client_data, XtPointer call_data)
{
    cb_clear(w, client_data, call_data);
}

static void cb_toggle_sign(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    input_handler_handle_toggle_sign(app);
}

void ck_calc_cb_toggle_sign(Widget w, XtPointer client_data, XtPointer call_data)
{
    cb_toggle_sign(w, client_data, call_data);
}

static void cb_percent(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    input_handler_handle_percent(app);
}

void ck_calc_cb_percent(Widget w, XtPointer client_data, XtPointer call_data)
{
    cb_percent(w, client_data, call_data);
}

static void cb_operator(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    char op = (char)(uintptr_t)client_data;
    input_handler_handle_operator(app, op);
}

void ck_calc_cb_operator(Widget w, XtPointer client_data, XtPointer call_data)
{
    cb_operator(w, client_data, call_data);
}

static void cb_equals(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    if (app->mode == 1) {
        if (app->calc_state.error_state) {
            ck_calc_reset_state(app);
            return;
        }
        if (formula_is_empty(&app->formula_ctx)) {
            ck_calc_ensure_keyboard_focus(app);
            return;
        }
        double result = 0.0;
        if (formula_evaluate(&app->formula_ctx, &result, app->trig_mode)) {
            formula_clear(&app->formula_ctx);
            app->formula_last_result = result;
            app->formula_showing_result = true;
            ck_calc_set_display_from_double(app, result);
        } else {
            formula_clear(&app->formula_ctx);
            set_error(app);
        }
        ck_calc_ensure_keyboard_focus(app);
        return;
    }
    input_handler_handle_equals(app);
}

void ck_calc_cb_equals(Widget w, XtPointer client_data, XtPointer call_data)
{
    cb_equals(w, client_data, call_data);
}

void formula_mode_update_display(AppState *app)
{
    if (!app) return;
    const char *formula = formula_text(&app->formula_ctx);
    if (!formula || !*formula) {
        ck_calc_set_display(app, "0");
        return;
    }

    char buf[MAX_DISPLAY_LEN];
    size_t pos = 0;
    for (const char *p = formula; *p && pos + 1 < sizeof(buf); ++p) {
        char ch = *p;
        if (ch == '.' && app->decimal_char != '.') {
            ch = app->decimal_char;
        }
        buf[pos++] = ch;
    }
    buf[pos] = '\0';
    ck_calc_set_display(app, buf);
}

void formula_mode_prepare_for_edit(AppState *app)
{
    if (!app) return;
    if (app->formula_showing_result) {
        formula_clear(&app->formula_ctx);
        app->formula_showing_result = false;
    }
}

void formula_mode_seed_with_last_result(AppState *app)
{
    if (!app) return;
    char tmp[64];
    int needed = snprintf(tmp, sizeof(tmp), "%.12g", app->formula_last_result);
    formula_clear(&app->formula_ctx);
    if (needed > 0) {
        formula_append_str(&app->formula_ctx, tmp);
    }
    app->formula_showing_result = false;
}

static void set_mode(AppState *app, int mode, Boolean from_menu)
{
    if (!app) return;
    int prev_mode = app->mode;
    if (mode != 0 && mode != 1) mode = 0;
    if (prev_mode == mode && from_menu) return;
    app->mode = mode;
    if (mode == 1 && prev_mode != 1) {
        formula_clear(&app->formula_ctx);
        app->formula_showing_result = false;
    }

    Boolean basic_set = (mode == 0) ? True : False;
    Boolean sci_set   = (mode == 1) ? True : False;
    if (app->view_mode_basic_btn) {
        XtVaSetValues(app->view_mode_basic_btn, XmNset, basic_set, NULL);
    }
    if (app->view_mode_sci_btn) {
        XtVaSetValues(app->view_mode_sci_btn, XmNset, sci_set, NULL);
    }
    if (app->trig_mode_row) {
        if (mode == 1) {
            XtManageChild(app->trig_mode_row);
        } else {
            XtUnmanageChild(app->trig_mode_row);
        }
    }
    update_trig_mode_display(app);

    ck_calc_save_view_state(app);
    if (app->session_data) {
        session_data_set_int(app->session_data, "mode", mode);
    }

    ck_calc_rebuild_keypad(app);

    ck_calc_apply_current_mode_width(app);
    ck_calc_apply_wm_hints(app);
    ck_calc_lock_shell_dimensions(app);
    ck_calc_log_mode_width(app, "set_mode");
}


/* -------------------------------------------------------------------------
 * About dialog
 * ------------------------------------------------------------------------- */

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

    fprintf(stderr, "[ck-calc] key: code=%u sym=%lu state=0x%x buf='%c'\n",
            (unsigned int)kc,
            (unsigned long)sym,
            (unsigned int)event->xkey.state,
            (keybuf[0] >= 32 && keybuf[0] < 127) ? keybuf[0] : ' ');

    if (sym == XK_Shift_L || sym == XK_Shift_R) {
        sci_visuals_handle_shift(app, sym, True);
        fprintf(stderr, "[ck-calc] shift down (%s) code=%u state=0x%x\n",
                (sym == XK_Shift_L) ? "left" : "right",
                (unsigned int)kc,
                (unsigned int)event->xkey.state);
    }

    /* Explicit keypad enter handling (some layouts map to keycode 108) */
    if (sym == XK_KP_Enter || (kc_kp_enter && kc == kc_kp_enter) || kc == 108) {
        activate_button(app->btn_eq, event);
        return;
    }

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
    if ((kc_return && kc == kc_return)) {
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

static void key_release_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont)
{
    (void)w;
    (void)client_data;
    (void)cont;
    if (!g_app || !event || event->type != KeyRelease) return;

    char keybuf[8];
    KeySym sym = NoSymbol;
    XLookupString(&event->xkey, keybuf, sizeof(keybuf), &sym, NULL);
    if (sym == NoSymbol) {
        sym = XLookupKeysym(&event->xkey, 0);
    }
    if (sym == NoSymbol) {
        sym = XLookupKeysym(&event->xkey, 1);
    }

    if (sym == XK_Shift_L || sym == XK_Shift_R) {
        sci_visuals_handle_shift(g_app, sym, False);
        KeyCode kc = event->xkey.keycode;
        fprintf(stderr, "[ck-calc] shift up (%s) code=%u state=0x%x\n",
                (sym == XK_Shift_L) ? "left" : "right",
                (unsigned int)kc,
                (unsigned int)event->xkey.state);
    }
}

static void focus_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont)
{
    (void)w;
    (void)client_data;
    (void)cont;
    if (!event) return;
    if (event->type == FocusIn) {
        ck_calc_ensure_keyboard_focus(g_app);
    }
}

static void shell_map_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont)
{
    (void)w;
    (void)cont;
    if (!event || event->type != MapNotify) return;
    AppState *app = (AppState *)client_data;
    if (!app) return;
    ck_calc_apply_current_mode_width(app);
    ck_calc_lock_shell_dimensions(app);
}

static void build_ui(AppState *app)
{
    if (!app) return;

    Widget main_form = XmCreateForm(app->shell, "mainForm", NULL, 0);
    XtVaSetValues(main_form,
                  XmNmarginWidth,  0,
                  XmNmarginHeight, 0,
                  XmNnoResize, True,
                  XmNresizePolicy, XmRESIZE_NONE,
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
    XtAddEventHandler(app->key_focus_proxy, KeyReleaseMask, False, key_release_handler, NULL);
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
    app->content_form = content_form;

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
    XtAddCallback(new_item, XmNactivateCallback, menu_handlers_cb_menu_new, (XtPointer)app);

    Widget close_item = XtVaCreateManagedWidget(
        "closeItem",
        xmPushButtonWidgetClass, window_pane,
        XmNlabelString,    XmStringCreateLocalized("Close"),
        XmNaccelerator,    "Alt<Key>F4",
        XmNacceleratorText,XmStringCreateLocalized("Alt+F4"),
        NULL
    );
    XtAddCallback(close_item, XmNactivateCallback, menu_handlers_cb_menu_close, (XtPointer)app);

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
    XtAddCallback(thousand_toggle, XmNvalueChangedCallback, menu_handlers_cb_toggle_thousands, (XtPointer)app);

    XtVaCreateManagedWidget(
        "viewSeparator",
        xmSeparatorGadgetClass, view_pane,
        NULL
    );

    Widget mode_basic = XtVaCreateManagedWidget(
        "modeBasic",
        xmToggleButtonWidgetClass, view_pane,
        XmNlabelString, XmStringCreateLocalized("Basic"),
        XmNindicatorType, XmONE_OF_MANY,
        XmNvisibleWhenOff, True,
        XmNset, (app->mode == 0) ? True : False,
        NULL
    );
    Widget mode_sci = XtVaCreateManagedWidget(
        "modeScientific",
        xmToggleButtonWidgetClass, view_pane,
        XmNlabelString, XmStringCreateLocalized("Scientific"),
        XmNindicatorType, XmONE_OF_MANY,
        XmNvisibleWhenOff, True,
        XmNset, (app->mode == 1) ? True : False,
        NULL
    );
    app->view_mode_basic_btn = mode_basic;
    app->view_mode_sci_btn   = mode_sci;
    XtAddCallback(mode_basic, XmNvalueChangedCallback, cb_mode_toggle, (XtPointer)(intptr_t)0);
    XtAddCallback(mode_sci,   XmNvalueChangedCallback, cb_mode_toggle, (XtPointer)(intptr_t)1);

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
    XtAddCallback(about_item, XmNactivateCallback, menu_handlers_cb_about, (XtPointer)app);

    /* Display label */
    Widget display_label = XtVaCreateManagedWidget(
        "displayLabel",
        xmLabelWidgetClass, content_form,
        XmNalignment,      XmALIGNMENT_END,
        XmNrecomputeSize,  False,
        XmNtopAttachment,  XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment,XmATTACH_FORM,
        XmNheight,         36,
        XmNmarginHeight,   8,
        XmNmarginWidth,    8,
        NULL
    );
    app->display_label = display_label;
    ck_calc_set_display(app, app->display);

    XtSetSensitive(display_label, True);
    XtAddEventHandler(display_label, ButtonPressMask, True, display_button_handler, (XtPointer)app);

    /* Angle mode row (small text under the display) */
    Widget angle_row = XmCreateRowColumn(content_form, "angleRow", NULL, 0);
    XtVaSetValues(angle_row,
                  XmNorientation,   XmHORIZONTAL,
                  XmNmarginHeight,  0,
                  XmNmarginWidth,   0,
                  XmNspacing,       6,
                  XmNpacking,       XmPACK_TIGHT,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget,     display_label,
                  XmNtopOffset,     0,
                  XmNleftAttachment,XmATTACH_FORM,
                  NULL);
    XtManageChild(angle_row);
    app->trig_mode_row = angle_row;

    const char *mode_labels[TRIG_MODE_COUNT] = { "Rad", "Deg", "Grad", "Turn" };
    for (int i = 0; i < TRIG_MODE_COUNT; ++i) {
        Widget lbl = XtVaCreateManagedWidget(
            "angleMode",
            xmLabelWidgetClass, angle_row,
            XmNlabelString, XmStringCreateLocalized((char *)mode_labels[i]),
            XmNmarginWidth, 2,
            XmNmarginHeight, 0,
            NULL
        );
        app->trig_mode_labels[i] = lbl;
    }
    update_trig_mode_display(app);
    if (app->mode != 1 && angle_row) {
        XtUnmanageChild(angle_row);
    }

    /* Context menu for display (Copy/Paste) */
    Widget menu = XmCreatePopupMenu(display_label, "displayPopup", NULL, 0);
    app->display_menu = menu;
    Widget copy_item = XtVaCreateManagedWidget(
        "copyItem",
        xmPushButtonWidgetClass, menu,
        XmNlabelString, XmStringCreateLocalized("Copy"),
        NULL
    );
    Widget paste_item = XtVaCreateManagedWidget(
        "pasteItem",
        xmPushButtonWidgetClass, menu,
        XmNlabelString, XmStringCreateLocalized("Paste"),
        NULL
    );
    XtAddCallback(copy_item, XmNactivateCallback, cb_display_copy, (XtPointer)app);
    XtAddCallback(paste_item, XmNactivateCallback, cb_display_paste, (XtPointer)app);

    /* Keypad */
    ck_calc_rebuild_keypad(app);
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

    ck_calc_init_locale_settings(&app);
    app.show_thousands = true;
    app.mode = 0; /* basic by default */
    app.trig_mode = TRIG_MODE_RAD;
    ck_calc_load_view_state(&app);

    char *session_id = session_parse_argument(&argc, argv);
    app.session_data = session_data_create(session_id);
    free(session_id);
    init_exec_path(&app, argv[0]);
    ck_calc_reset_state(&app);

    app.shell = XtVaAppInitialize(&app.app_context,
                                  "CkCalc",
                                  NULL, 0,
                                  &argc, argv,
                                  NULL,
                                  XmNtitle, "Calculator",
                                  XmNiconName, "Calculator",
                                  XmNkeyboardFocusPolicy, XmEXPLICIT,
                                  NULL);

    DtInitialize(XtDisplay(app.shell), app.shell, "CkCalc", "CkCalc");
    Display *dpy = XtDisplay(app.shell);
    Colormap cmap = DefaultColormapOfScreen(XtScreen(app.shell));
    app.palette_ok = cde_palette_read(dpy, DefaultScreen(dpy), cmap, &app.palette);

    /* Load session data (geometry + display) */
    if (app.session_data) {
        if (session_load(app.shell, app.session_data)) {
            const char *saved_disp = session_data_get(app.session_data, "display");
            if (saved_disp && saved_disp[0]) {
                ck_calc_set_display(&app, saved_disp);
                app.calc_state.entering_new = true;
            }
            if (session_data_has(app.session_data, "show_thousands")) {
                app.show_thousands = session_data_get_int(app.session_data, "show_thousands", app.show_thousands ? 1 : 0) != 0;
            }
            if (session_data_has(app.session_data, "mode")) {
                app.mode = session_data_get_int(app.session_data, "mode", app.mode);
            }
            if (session_data_has(app.session_data, "trig_mode")) {
                int t = session_data_get_int(app.session_data, "trig_mode", app.trig_mode);
                if (t < 0 || t >= TRIG_MODE_COUNT) t = TRIG_MODE_RAD;
                app.trig_mode = (TrigMode)t;
            }
        }
    }

    build_ui(&app);
    set_mode(&app, app.mode, False);
    ck_calc_log_mode_width(&app, "startup");
    ck_calc_apply_wm_hints(&app);

    if (app.session_data) {
        if (!session_apply_geometry(app.shell, app.session_data, "x", "y", "w", "h")) {
            center_shell_on_screen(app.shell);
        }
    } else {
        center_shell_on_screen(app.shell);
    }

    ck_calc_apply_current_mode_width(&app);
    ck_calc_lock_shell_dimensions(&app);

    XtRealizeWidget(app.shell);
    ck_calc_apply_wm_hints(&app);
    ck_calc_lock_shell_dimensions(&app);

    /* Keyboard handler for shortcuts/digits */
    XtAddEventHandler(app.shell, StructureNotifyMask, False, shell_map_handler, (XtPointer)&app);
    XtAddEventHandler(app.shell, KeyPressMask, True, key_press_handler, NULL);
    XtAddEventHandler(app.shell, KeyReleaseMask, True, key_release_handler, NULL);
    XtAddEventHandler(app.shell, FocusChangeMask, False, focus_handler, NULL);
    if (app.main_form) {
        XtAddEventHandler(app.main_form, KeyPressMask, True, key_press_handler, NULL);
        XtAddEventHandler(app.main_form, KeyReleaseMask, True, key_release_handler, NULL);
        XtAddEventHandler(app.main_form, FocusChangeMask, False, focus_handler, NULL);
    }
    if (app.key_focus_proxy) {
        XtSetKeyboardFocus(app.shell, app.key_focus_proxy);
        XmProcessTraversal(app.key_focus_proxy, XmTRAVERSE_CURRENT);
    } else if (app.main_form) {
        XtSetKeyboardFocus(app.shell, app.main_form);
    }
    ck_calc_ensure_keyboard_focus(&app);

    Atom wm_delete = XmInternAtom(XtDisplay(app.shell), "WM_DELETE_WINDOW", False);
    Atom wm_save   = XmInternAtom(XtDisplay(app.shell), "WM_SAVE_YOURSELF", False);
    XmAddWMProtocolCallback(app.shell, wm_delete, menu_handlers_cb_wm_delete, (XtPointer)&app);
    XmAddWMProtocolCallback(app.shell, wm_save,   menu_handlers_cb_wm_save, (XtPointer)&app);
    XmActivateWMProtocol(app.shell, wm_delete);
    XmActivateWMProtocol(app.shell, wm_save);

    XtAppMainLoop(app.app_context);

    ck_calc_cleanup_scientific_font(&app);
    session_data_free(app.session_data);
    return 0;
}
