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
#include "../shared/config_utils.h"
#include "../shared/cde_palette.h"
#include "formula_eval.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_DISPLAY_LEN 64
#define VIEW_STATE_FILENAME "ck-calc.view"

typedef struct {
    XtAppContext app_context;
    Widget       shell;
    Widget       main_form;
    Widget       content_form;
    Widget       key_focus_proxy;
    Widget       display_label;
    Widget       display_menu;
    Widget       keypad;
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
    FormulaCtx   formula_ctx;
    bool         formula_showing_result;
    double       formula_last_result;

    bool         show_thousands;
    char         decimal_char;
    char         thousands_char;
    int          mode; /* 0=basic, 1=scientific */

    bool         shift_left_down;
    bool         shift_right_down;
    bool         second_mouse_pressed;
    Widget       btn_second;
    Pixel        second_shadow_top;
    Pixel        second_shadow_bottom;
    Boolean      second_shadow_cached;
    short        second_shadow_thickness;
    Boolean      second_thickness_cached;
    Pixel        second_bg_normal;
    Pixel        second_bg_active;
    Boolean      second_color_cached;
    Boolean      second_border_prev_active;
    XmFontList   sci_font_list;
    XFontStruct *sci_font_struct;

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
    Widget       view_mode_basic_btn;
    Widget       view_mode_sci_btn;
    Widget       btn_sci_exp;
    Widget       btn_sci_10x;
    Widget       btn_sci_ln;
    Widget       btn_sci_log10;
    Widget       btn_sci_sin;
    Widget       btn_sci_cos;
    Widget       btn_sci_tan;
    Widget       btn_sci_sinh;
    Widget       btn_sci_cosh;
    Widget       btn_sci_tanh;

    XtIntervalId copy_flash_id;
    char         copy_flash_backup[MAX_DISPLAY_LEN];
    XtIntervalId paste_flash_id;
    char         paste_flash_backup[MAX_DISPLAY_LEN];

    Dimension    chrome_dy;
    Boolean      chrome_inited;

    CdePalette   palette;
    Boolean      palette_ok;

} AppState;

static AppState *g_app = NULL;
static Widget g_about_shell = NULL;

static const char *SCI_LABELS[5][6] = {
    { "(", ")", "mc",  "m+",   "m-",  "mr" },
    { "2nd", "x^2", "x^3", "x^y",  "e^x","10^x" },
    { "1/x", "sqrt(x)", "3rd root(x)", "y root x", "ln", "log10" },
    { "x!",  "sqrt", "3rd_root", "y root x",  "e",   "EE" },
    { "Rand", "sinh", "cosh", "tanh", "pi",  "Rad" }
};

static const char *get_sci_label(int row, int col)
{
    if (row < 0 || row >= 5 || col < 0 || col >= 6) return "?";
    return SCI_LABELS[row][col];
}

static void format_number(AppState *app, double value, char *out, size_t out_len);
static void key_press_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont);
static void key_release_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont);
static void ensure_keyboard_focus(AppState *app);
static void focus_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont);
static void reformat_display(AppState *app);
static void clipboard_copy(AppState *app);
static void clipboard_paste(AppState *app);
static void display_button_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont);
static void cb_display_copy(Widget w, XtPointer client_data, XtPointer call_data);
static void cb_display_paste(Widget w, XtPointer client_data, XtPointer call_data);
static void cb_second_toggle(Widget w, XtPointer client_data, XtPointer call_data);
static void cb_second_arm(Widget w, XtPointer client_data, XtPointer call_data);
static void second_button_mouse_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont);
static void assign_sci_button_ref(AppState *app, const char *name, Widget button);
static void copy_flash_reset(XtPointer client_data, XtIntervalId *id);
static void paste_flash_reset(XtPointer client_data, XtIntervalId *id);
static void rebuild_keypad(AppState *app);
static void set_mode(AppState *app, int mode, Boolean from_menu);
static void cb_mode_toggle(Widget w, XtPointer client_data, XtPointer call_data);
static void clear_button_refs(AppState *app);
static Boolean is_second_active(const AppState *app);
static void update_second_button_state(AppState *app);
static void set_shift_state(AppState *app, KeySym sym, Boolean down);
static void ensure_scientific_font(AppState *app);
static void apply_scientific_button_font(AppState *app, Widget button);
static void cleanup_scientific_font(AppState *app);
static void register_scientific_button(AppState *app,
                                       Widget button,
                                       Widget stash[],
                                       size_t *count,
                                       size_t capacity,
                                       Dimension height,
                                       Boolean flushed);
static void flush_scientific_extra_buttons(AppState *app,
                                           Widget stash[],
                                           size_t *count,
                                           Dimension height);
static bool build_smaller_xlfd(const char *base,
                                int pixel_step,
                                int point_step,
                                char *out,
                                size_t out_size);
static void formula_mode_update_display(AppState *app);
static void formula_mode_prepare_for_edit(AppState *app);
static void formula_mode_seed_with_last_result(AppState *app);
static Widget create_key_button(Widget parent, const char *name, const char *label,
                                Widget top_widget, Boolean align_top,
                                int col, int col_span, int col_step,
                                XtCallbackProc cb, XtPointer data);
static Dimension get_desired_width(const AppState *app);
static void log_mode_width(AppState *app, const char *context);
static void apply_current_mode_width(AppState *app);
static void apply_wm_hints(AppState *app);
static void lock_shell_dimensions(AppState *app);

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

static void ensure_scientific_font(AppState *app)
{
    if (!app || app->sci_font_list) return;
    Widget ref = app->btn_back ? app->btn_back : app->display_label;
    if (!ref) return;

    XmFontList font_list = NULL;
    XtVaGetValues(ref, XmNfontList, &font_list, NULL);
    if (!font_list) return;

    Display *dpy = app->shell ? XtDisplay(app->shell) : NULL;
    if (!dpy) return;

    XmFontContext ctx = NULL;
    if (!XmFontListInitFontContext(&ctx, font_list)) return;

    XmStringCharSet charset = NULL;
    XFontStruct *font = NULL;
    char base_font[512] = {0};
    if (XmFontListGetNextFont(ctx, &charset, &font) && font) {
        unsigned long value = 0;
        if (XGetFontProperty(font, XA_FONT, &value)) {
            char *name = XGetAtomName(dpy, (Atom)value);
            if (name) {
                strncpy(base_font, name, sizeof(base_font) - 1);
                XFree(name);
            }
        }
    }
    XmFontListFreeFontContext(ctx);

    if (!base_font[0]) return;

    const struct {
        int pixel_step;
        int point_step;
    } adjustments[] = {
        { 2, 20 },
        { 1, 10 },
        { 1, 1 }
    };

    char smaller[512];
    for (size_t i = 0; i < sizeof(adjustments)/sizeof(adjustments[0]); ++i) {
        if (!build_smaller_xlfd(base_font, adjustments[i].pixel_step, adjustments[i].point_step,
                                smaller, sizeof(smaller))) {
            continue;
        }
        XFontStruct *font_struct = XLoadQueryFont(dpy, smaller);
        if (!font_struct) continue;
        XmFontList new_list = XmFontListCreate(font_struct, XmSTRING_DEFAULT_CHARSET);
        if (!new_list) {
            XFreeFont(dpy, font_struct);
            continue;
        }
        app->sci_font_list = new_list;
        app->sci_font_struct = font_struct;
        return;
    }
}

static void apply_scientific_button_font(AppState *app, Widget button)
{
    if (!app || !button || !app->sci_font_list) return;
    XtVaSetValues(button, XmNfontList, app->sci_font_list, NULL);
}

static void cleanup_scientific_font(AppState *app)
{
    if (!app) return;
    if (app->sci_font_list) {
        XmFontListFree(app->sci_font_list);
        app->sci_font_list = NULL;
    }
    if (app->sci_font_struct) {
        Display *dpy = app->shell ? XtDisplay(app->shell) : NULL;
        if (dpy) {
            XFreeFont(dpy, app->sci_font_struct);
        }
        app->sci_font_struct = NULL;
    }
}

static void register_scientific_button(AppState *app,
                                       Widget button,
                                       Widget stash[],
                                       size_t *count,
                                       size_t capacity,
                                       Dimension height,
                                       Boolean flushed)
{
    if (!button || !stash || !count || capacity == 0) return;
    if (flushed) {
        if (height > 0) {
            XtVaSetValues(button, XmNheight, height, NULL);
        }
        if (app && app->sci_font_list) {
            apply_scientific_button_font(app, button);
        }
        return;
    }
    if (*count < capacity) {
        stash[(*count)++] = button;
    }
}

static void flush_scientific_extra_buttons(AppState *app,
                                           Widget stash[],
                                           size_t *count,
                                           Dimension height)
{
    if (!app || !stash || !count || *count == 0) return;
    for (size_t i = 0; i < *count; ++i) {
        Widget button = stash[i];
        if (!button) continue;
        if (height > 0) {
            XtVaSetValues(button, XmNheight, height, NULL);
        }
        if (app->sci_font_list) {
            apply_scientific_button_font(app, button);
        }
    }
    *count = 0;
}

static char *duplicate_segment(const char *start, size_t len)
{
    char *res = malloc(len + 1);
    if (!res) return NULL;
    memcpy(res, start, len);
    res[len] = '\0';
    return res;
}

static bool build_smaller_xlfd(const char *base,
                                int pixel_step,
                                int point_step,
                                char *out,
                                size_t out_size)
{
    if (!base || !out || out_size == 0) return false;
    const size_t MAX_TOKENS = 64;
    char *tokens[MAX_TOKENS];
    size_t count = 0;
    const char *cur = base;
    bool success = true;
    while (*cur && count < MAX_TOKENS) {
        if (*cur == '-') {
            char *seg = strdup("");
            if (!seg) { success = false; break; }
            tokens[count++] = seg;
            cur++;
            continue;
        }
        const char *start = cur;
        while (*cur && *cur != '-') cur++;
        size_t len = cur - start;
        char *seg = duplicate_segment(start, len);
        if (!seg) { success = false; break; }
        tokens[count++] = seg;
        if (*cur == '-') cur++;
    }
    if (!success) {
        for (size_t i = 0; i < count; ++i) free(tokens[i]);
        return false;
    }
    if (count <= 8) {
        for (size_t i = 0; i < count; ++i) free(tokens[i]);
        return false;
    }
    int pixel = atoi(tokens[7]);
    int point = atoi(tokens[8]);
    int new_pixel = pixel - pixel_step;
    int new_point = point - point_step;
    if (new_pixel < 1) new_pixel = 1;
    if (new_point < 1) new_point = 1;
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%d", new_pixel);
    free(tokens[7]);
    tokens[7] = strdup(buffer);
    snprintf(buffer, sizeof(buffer), "%d", new_point);
    free(tokens[8]);
    tokens[8] = strdup(buffer);
    size_t pos = 0;
    char *ptr = out;
    for (size_t i = 0; i < count; ++i) {
        if (pos + 1 >= out_size) {
            for (size_t j = 0; j < count; ++j) free(tokens[j]);
            return false;
        }
        ptr[pos++] = '-';
        size_t len = strlen(tokens[i]);
        if (pos + len >= out_size) {
            for (size_t j = 0; j < count; ++j) free(tokens[j]);
            return false;
        }
        memcpy(ptr + pos, tokens[i], len);
        pos += len;
    }
    ptr[pos] = '\0';
    for (size_t i = 0; i < count; ++i) {
        free(tokens[i]);
    }
    return true;
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
    formula_clear(&app->formula_ctx);
    app->formula_showing_result = false;
    app->formula_last_result = 0.0;
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
    app->mode = config_read_int_map(VIEW_STATE_FILENAME, "mode", app->mode);
}

static void save_view_state(const AppState *app)
{
    if (!app) return;
    config_write_int_map(VIEW_STATE_FILENAME, "show_thousands", app->show_thousands ? 1 : 0);
    config_write_int_map(VIEW_STATE_FILENAME, "mode", app->mode);
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
    if (app->mode == 1) {
        if (app->error_state) reset_state(app);
        formula_mode_prepare_for_edit(app);
        char digit = (char)(uintptr_t)client_data;
        if (formula_append_char(&app->formula_ctx, digit)) {
            formula_mode_update_display(app);
        }
        ensure_keyboard_focus(app);
        return;
    }
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
    if (app->mode == 1) {
        if (app->error_state) reset_state(app);
        formula_mode_prepare_for_edit(app);
        if (formula_append_char(&app->formula_ctx, '.')) {
            formula_mode_update_display(app);
        }
        ensure_keyboard_focus(app);
        return;
    }
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
    if (app->mode == 1) {
        if (app->error_state) {
            reset_state(app);
            return;
        }
        formula_mode_prepare_for_edit(app);
        formula_backspace(&app->formula_ctx);
        formula_mode_update_display(app);
        ensure_keyboard_focus(app);
        return;
    }
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
    if (app->mode == 1) {
        ensure_keyboard_focus(app);
        return;
    }
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
    if (app->mode == 1) {
        ensure_keyboard_focus(app);
        return;
    }
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

static void cb_second_toggle(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app || !w) return;
    app->second_mouse_pressed = !app->second_mouse_pressed;
    fprintf(stderr, "[ck-calc] 2nd mouse toggle (%s) -> active=%d\n",
            app->second_mouse_pressed ? "press" : "release",
            is_second_active(app));
    update_second_button_state(app);
}

static void cb_second_arm(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    if (is_second_active(app)) {
        fprintf(stderr, "[ck-calc] 2nd arm: keeping border pressed\n");
        update_second_button_state(app);
    }
}

static void second_button_mouse_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont)
{
    (void)w;
    (void)client_data;
    (void)cont;
    if (!event) return;
    AppState *app = g_app;
    switch (event->type) {
        case ButtonPress:
            fprintf(stderr, "[ck-calc] 2nd mouse down\n");
            if (is_second_active(app)) {
                fprintf(stderr, "[ck-calc] 2nd already active; keeping border pressed\n");
                update_second_button_state(app);
            }
            break;
        case ButtonRelease:
            fprintf(stderr, "[ck-calc] 2nd mouse up\n");
            break;
        default:
            break;
    }
}

static void cb_operator(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = g_app;
    if (!app) return;
    if (app->mode == 1) {
        if (app->error_state) reset_state(app);
        if (app->formula_showing_result) {
            formula_mode_seed_with_last_result(app);
        }
        char op = (char)(uintptr_t)client_data;
        if (formula_append_char(&app->formula_ctx, op)) {
            formula_mode_update_display(app);
        }
        ensure_keyboard_focus(app);
        return;
    }
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
    if (app->mode == 1) {
        if (app->error_state) {
            reset_state(app);
            return;
        }
        if (formula_is_empty(&app->formula_ctx)) {
            ensure_keyboard_focus(app);
            return;
        }
        double result = 0.0;
        if (formula_evaluate(&app->formula_ctx, &result)) {
            formula_clear(&app->formula_ctx);
            app->formula_last_result = result;
            app->formula_showing_result = true;
            set_display_from_double(app, result);
        } else {
            formula_clear(&app->formula_ctx);
            set_error(app);
        }
        ensure_keyboard_focus(app);
        return;
    }
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

static void formula_mode_update_display(AppState *app)
{
    if (!app) return;
    const char *formula = formula_text(&app->formula_ctx);
    if (!formula || !*formula) {
        set_display(app, "0");
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
    set_display(app, buf);
}

static void formula_mode_prepare_for_edit(AppState *app)
{
    if (!app) return;
    if (app->formula_showing_result) {
        formula_clear(&app->formula_ctx);
        app->formula_showing_result = false;
    }
}

static void formula_mode_seed_with_last_result(AppState *app)
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

    save_view_state(app);
    if (app->session_data) {
        session_data_set_int(app->session_data, "mode", mode);
    }

    rebuild_keypad(app);

    apply_current_mode_width(app);
    apply_wm_hints(app);
    lock_shell_dimensions(app);
    log_mode_width(app, "set_mode");
}

static void rebuild_keypad(AppState *app)
{
    if (!app || !app->content_form || !app->display_label) return;

    if (app->keypad && XtIsWidget(app->keypad)) {
        XtDestroyWidget(app->keypad);
        app->keypad = NULL;
    }
    clear_button_refs(app);

    Arg pad_args[4];
    int pn = 0;
    int base_cols = 4 + ((app->mode == 1) ? 6 : 0);
    int col_step = 25;
    XtSetArg(pad_args[pn], XmNfractionBase, base_cols * col_step); pn++;
    Widget keypad = XmCreateForm(app->content_form, "keypadForm", pad_args, pn);
    XtVaSetValues(keypad,
                  XmNtopAttachment,    XmATTACH_WIDGET,
                  XmNtopWidget,        app->display_label,
                  XmNtopOffset,        8,
                  XmNleftAttachment,   XmATTACH_FORM,
                  XmNrightAttachment,  XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  NULL);
    XtManageChild(keypad);
    app->keypad = keypad;

    Widget row_anchor = NULL;
    Widget row_top = NULL;
    int offset = (app->mode == 1) ? 6 : 0;
    Widget sci_extra_buttons[64] = { 0 };
    const size_t sci_extra_capacity = sizeof(sci_extra_buttons) / sizeof(sci_extra_buttons[0]);
    size_t sci_extra_count = 0;
    Dimension sci_extra_height = 0;
    Boolean sci_extra_flushed = False;

    /* extra scientific columns (6) */
    if (offset > 0) {
        for (int i = 0; i < offset; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "sciR1C%d", i);
            Widget sci_button = create_key_button(keypad, name, get_sci_label(0, i), row_anchor, False, i, 1, col_step, NULL, NULL);
            register_scientific_button(app, sci_button, sci_extra_buttons, &sci_extra_count, sci_extra_capacity, sci_extra_height, sci_extra_flushed);
            assign_sci_button_ref(app, name, sci_button);
        }
    }

    /* Row 1 */
    row_anchor = create_key_button(keypad, "backBtn", "◀", NULL, False, offset + 0, 1, col_step, cb_backspace, NULL);
    app->btn_back = row_anchor;
    row_top = row_anchor;
    if (offset > 0) {
        for (int i = 0; i < offset; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "sciR1bC%d", i);
            Widget sci_button = create_key_button(keypad, name, get_sci_label(0, i), row_top, True, i, 1, col_step, NULL, NULL);
            register_scientific_button(app, sci_button, sci_extra_buttons, &sci_extra_count, sci_extra_capacity, sci_extra_height, sci_extra_flushed);
            assign_sci_button_ref(app, name, sci_button);
        }
    }
    app->btn_ac = create_key_button(keypad, "acBtn",   "AC",   row_top, True, offset + 1, 1, col_step, cb_clear, NULL);
    app->btn_percent = create_key_button(keypad, "percentBtn", "%", row_top, True, offset + 2, 1, col_step, cb_percent, NULL);
    app->btn_div = create_key_button(keypad, "divBtn", "/", row_top, True, offset + 3, 1, col_step, cb_operator, (XtPointer)(uintptr_t)'/');
    if (app->btn_back && app->btn_ac) {
        Dimension ac_h = 0;
        XtVaGetValues(app->btn_ac, XmNheight, &ac_h, NULL);
        if (ac_h > 0) {
            XtVaSetValues(app->btn_back, XmNheight, ac_h, NULL);
        }
    }
    if (sci_extra_height == 0 && app->btn_back) {
        Dimension back_h = 0;
        XtVaGetValues(app->btn_back, XmNheight, &back_h, NULL);
        if (back_h > 0) {
            sci_extra_height = back_h;
            ensure_scientific_font(app);
            flush_scientific_extra_buttons(app, sci_extra_buttons, &sci_extra_count, sci_extra_height);
            sci_extra_flushed = True;
        }
    }

    /* Row 2 */
    row_anchor = create_key_button(keypad, "sevenBtn", "7", row_anchor, False, offset + 0, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'7');
    app->btn_digits[7] = row_anchor;
    row_top = row_anchor;
    if (offset > 0) {
        for (int i = 0; i < offset; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "sciR2C%d", i);
            Widget sci_button = create_key_button(keypad, name, get_sci_label(1, i), row_top, True, i, 1, col_step, (i == 0) ? cb_second_toggle : NULL, NULL);
            register_scientific_button(app, sci_button, sci_extra_buttons, &sci_extra_count, sci_extra_capacity, sci_extra_height, sci_extra_flushed);
            assign_sci_button_ref(app, name, sci_button);
            if (i == 0) {
                app->btn_second = sci_button;
                XtAddEventHandler(sci_button, ButtonPressMask, False, second_button_mouse_handler, NULL);
                XtAddEventHandler(sci_button, ButtonReleaseMask, False, second_button_mouse_handler, NULL);
                XtAddCallback(sci_button, XmNarmCallback, cb_second_arm, NULL);
            }
        }
    }
    app->btn_digits[8] = create_key_button(keypad, "eightBtn", "8", row_top, True, offset + 1, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'8');
    app->btn_digits[9] = create_key_button(keypad, "nineBtn",  "9", row_top, True, offset + 2, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'9');
    app->btn_mul = create_key_button(keypad, "mulBtn",   "*", row_top, True, offset + 3, 1, col_step, cb_operator, (XtPointer)(uintptr_t)'*');

    /* Row 3 */
    row_anchor = create_key_button(keypad, "fourBtn", "4", row_anchor, False, offset + 0, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'4');
    app->btn_digits[4] = row_anchor;
    row_top = row_anchor;
    if (offset > 0) {
        for (int i = 0; i < offset; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "sciR3C%d", i);
            Widget sci_button = create_key_button(keypad, name, get_sci_label(2, i), row_top, True, i, 1, col_step, NULL, NULL);
            register_scientific_button(app, sci_button, sci_extra_buttons, &sci_extra_count, sci_extra_capacity, sci_extra_height, sci_extra_flushed);
            assign_sci_button_ref(app, name, sci_button);
        }
    }
    app->btn_digits[5] = create_key_button(keypad, "fiveBtn", "5", row_top, True, offset + 1, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'5');
    app->btn_digits[6] = create_key_button(keypad, "sixBtn",  "6", row_top, True, offset + 2, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'6');
    app->btn_minus = create_key_button(keypad, "minusBtn","-", row_top, True, offset + 3, 1, col_step, cb_operator, (XtPointer)(uintptr_t)'-');

    /* Row 4 */
    row_anchor = create_key_button(keypad, "oneBtn", "1", row_anchor, False, offset + 0, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'1');
    app->btn_digits[1] = row_anchor;
    row_top = row_anchor;
    if (offset > 0) {
        for (int i = 0; i < offset; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "sciR4C%d", i);
            Widget sci_button = create_key_button(keypad, name, get_sci_label(3, i), row_top, True, i, 1, col_step, NULL, NULL);
            register_scientific_button(app, sci_button, sci_extra_buttons, &sci_extra_count, sci_extra_capacity, sci_extra_height, sci_extra_flushed);
            assign_sci_button_ref(app, name, sci_button);
        }
    }
    app->btn_digits[2] = create_key_button(keypad, "twoBtn", "2", row_top, True, offset + 1, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'2');
    app->btn_digits[3] = create_key_button(keypad, "threeBtn", "3", row_top, True, offset + 2, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'3');
    app->btn_plus = create_key_button(keypad, "plusBtn", "+", row_top, True, offset + 3, 1, col_step, cb_operator, (XtPointer)(uintptr_t)'+');

    /* Row 5 */
    char decimal_label[2] = {app->decimal_char, '\0'};
    row_anchor = create_key_button(keypad, "signBtn", "+/-", row_anchor, False, offset + 0, 1, col_step, cb_toggle_sign, NULL);
    app->btn_sign = row_anchor;
    row_top = row_anchor;
    if (offset > 0) {
        for (int i = 0; i < offset; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "sciR5C%d", i);
            Widget sci_button = create_key_button(keypad, name, get_sci_label(4, i), row_top, True, i, 1, col_step, NULL, NULL);
            register_scientific_button(app, sci_button, sci_extra_buttons, &sci_extra_count, sci_extra_capacity, sci_extra_height, sci_extra_flushed);
            assign_sci_button_ref(app, name, sci_button);
        }
    }
    app->btn_digits[0] = create_key_button(keypad, "zeroBtn", "0", row_top, True, offset + 1, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'0');
    app->btn_decimal = create_key_button(keypad, "decimalBtn", decimal_label, row_top, True, offset + 2, 1, col_step, cb_decimal, NULL);
    Widget eq_btn = create_key_button(keypad, "eqBtn", "=", row_top, True, offset + 3, 1, col_step, cb_equals, NULL);
    app->btn_eq = eq_btn;

    ensure_keyboard_focus(app);

    /* Apply color accents to right column using CDE select color */
    /* Only colorize when full palette is available */
    int set_count = cde_palette_set_count(&app->palette);
    Boolean have_active   = (app->palette.active   >= 0 && app->palette.active   < set_count);
    Boolean have_inactive = (app->palette.inactive >= 0 && app->palette.inactive < set_count);
    Boolean allow_color = app->palette_ok && have_active && have_inactive;
    if (allow_color) {
        Pixel op_bg = 0, op_fg = 0;
        Pixel top_bg = 0, top_fg = 0;

        int active = app->palette.active;
        int inactive = app->palette.inactive;
        if (active >= 0 && active < app->palette.count) {
            op_bg = app->palette.set[active].bg.pixel;
            op_fg = app->palette.set[active].fg.pixel;
        }
        if (inactive >= 0 && inactive < app->palette.count) {
            top_bg = app->palette.set[inactive].bg.pixel;
            top_fg = app->palette.set[inactive].fg.pixel;
        }

        Widget ops[] = { app->btn_div, app->btn_mul, app->btn_minus, app->btn_plus, app->btn_eq };
        for (size_t i = 0; i < sizeof(ops)/sizeof(ops[0]); ++i) {
            if (ops[i]) {
                XtVaSetValues(ops[i],
                              XmNbackground, op_bg,
                              XmNforeground, op_fg,
                              NULL);
            }
        }

        Widget accents[] = { app->btn_back, app->btn_ac, app->btn_percent };
        for (size_t i = 0; i < sizeof(accents)/sizeof(accents[0]); ++i) {
            if (accents[i]) {
                XtVaSetValues(accents[i],
                              XmNbackground, top_bg,
                              XmNforeground, top_fg,
                              NULL);
            }
        }
    }

    update_second_button_state(app);
}

static Dimension get_desired_width(const AppState *app)
{
    if (!app) return 0;
    int cols = (app->mode == 1) ? 10 : 4;
    return (Dimension)(cols * 60 + 40);
}

static void log_mode_width(AppState *app, const char *context)
{
    if (!app || !context) return;
    Dimension desired = get_desired_width(app);
    Dimension current = 0;
    if (app->shell) {
        XtVaGetValues(app->shell, XmNwidth, &current, NULL);
    }
    fprintf(stderr,
            "[ck-calc] %s mode=%d desired_width=%u current_width=%u\n",
            context,
            app->mode,
            (unsigned int)desired,
            (unsigned int)current);
}

static void apply_current_mode_width(AppState *app)
{
    if (!app || !app->shell) return;
    Dimension desired_w = get_desired_width(app);
    XtVaSetValues(app->shell,
                  XmNwidth,     desired_w,
                  XmNminWidth,  desired_w,
                  XmNmaxWidth,  desired_w,
                  NULL);
    log_mode_width(app, "apply_current_mode_width");
}

static void apply_wm_hints(AppState *app)
{
    if (!app || !app->shell) return;
    unsigned int decor = MWM_DECOR_BORDER | MWM_DECOR_TITLE | MWM_DECOR_MENU | MWM_DECOR_MINIMIZE;
    unsigned int funcs = MWM_FUNC_MOVE | MWM_FUNC_CLOSE | MWM_FUNC_MINIMIZE;
    XtVaSetValues(app->shell,
                  XmNmwmDecorations, decor,
                  XmNmwmFunctions,   funcs,
                  XmNallowShellResize, False,
                  NULL);
}

static void copy_flash_reset(XtPointer client_data, XtIntervalId *id)
{
    (void)id;
    AppState *app = (AppState *)client_data;
    if (!app) return;
    if (app->copy_flash_id) {
        app->copy_flash_id = 0;
    }
    if (app->copy_flash_backup[0]) {
        set_display(app, app->copy_flash_backup);
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
        set_display(app, app->paste_flash_backup);
        app->paste_flash_backup[0] = '\0';
    }
}

static void lock_shell_dimensions(AppState *app)
{
    if (!app || !app->shell || !XtIsRealized(app->shell) || !app->main_form) return;

    Dimension shell_w = 0, shell_h = 0;
    Dimension form_w = 0, form_h = 0;
    XtVaGetValues(app->shell, XmNwidth, &shell_w, XmNheight, &shell_h, NULL);
    XtVaGetValues(app->main_form, XmNwidth, &form_w, XmNheight, &form_h, NULL);

    if (!app->chrome_inited && shell_h > form_h) {
        app->chrome_dy = shell_h - form_h;
        app->chrome_inited = True;
    }

    if (form_h == 0 || shell_w == 0) return;

    Dimension desired_h = form_h;
    if (app->chrome_inited) desired_h += app->chrome_dy;

    int cols = (app->mode == 1) ? 10 : 4;
    Dimension desired_w = (Dimension)(cols * 60 + 40);

    XtVaSetValues(app->shell,
                  XmNwidth,      desired_w,
                  XmNminWidth,   desired_w,
                  XmNmaxWidth,   desired_w,
                  XmNminHeight,  desired_h,
                  XmNmaxHeight,  desired_h,
                  NULL);
}

static void clear_button_refs(AppState *app)
{
    if (!app) return;
    memset(app->btn_digits, 0, sizeof(app->btn_digits));
    app->btn_decimal = NULL;
    app->btn_eq = NULL;
    app->btn_plus = NULL;
    app->btn_minus = NULL;
    app->btn_mul = NULL;
    app->btn_div = NULL;
    app->btn_percent = NULL;
    app->btn_sign = NULL;
    app->btn_back = NULL;
    app->btn_ac = NULL;
    app->btn_second = NULL;
    app->second_shadow_cached = False;
    app->second_color_cached = False;
    app->second_thickness_cached = False;
    app->second_shadow_thickness = 0;
    app->second_border_prev_active = False;
    app->btn_sci_exp = NULL;
    app->btn_sci_10x = NULL;
    app->btn_sci_ln = NULL;
    app->btn_sci_log10 = NULL;
    app->btn_sci_sin = NULL;
    app->btn_sci_cos = NULL;
    app->btn_sci_tan = NULL;
    app->btn_sci_sinh = NULL;
    app->btn_sci_cosh = NULL;
    app->btn_sci_tanh = NULL;
}

static Boolean is_second_active(const AppState *app)
{
    if (!app) return False;
    return (app->shift_left_down || app->shift_right_down || app->second_mouse_pressed) ? True : False;
}

static void assign_sci_button_ref(AppState *app, const char *name, Widget button);
static void set_button_label(Widget btn, const char *label)
{
    if (!btn || !label) return;
    XmString xms = XmStringCreateLocalized((char *)label);
    XtVaSetValues(btn, XmNlabelString, xms, NULL);
    XmStringFree(xms);
}

static void refresh_second_button_labels(AppState *app, Boolean active)
{
    if (!app) return;
    struct {
        Widget      btn;
        const char  *normal;
        const char  *alt;
    } swaps[] = {
        { app->btn_sci_exp,  "e^x",     "y^x"     },
        { app->btn_sci_10x,  "10^x",    "2^x"     },
        { app->btn_sci_ln,   "ln",      "log y"   },
        { app->btn_sci_log10,"log10",   "log 2"   },
        { app->btn_sci_sin,  "sin",     "sin^-1"  },
        { app->btn_sci_cos,  "cos",     "cos^-1"  },
        { app->btn_sci_tan,  "tan",     "tan^-1"  },
        { app->btn_sci_sinh, "sinh",    "sinh^-1" },
        { app->btn_sci_cosh, "cosh",    "cosh^-1" },
        { app->btn_sci_tanh, "tanh",    "tanh^-1" },
    };
    for (size_t i = 0; i < sizeof(swaps)/sizeof(swaps[0]); ++i) {
        Widget btn = swaps[i].btn;
        if (btn) {
            set_button_label(btn, active ? swaps[i].alt : swaps[i].normal);
        }
    }
}

static void assign_sci_button_ref(AppState *app, const char *name, Widget button)
{
    if (!app || !name || !button) return;
    if (strcmp(name, "sciR1bC4") == 0) {
        app->btn_sci_exp = button;
    } else if (strcmp(name, "sciR1bC5") == 0) {
        app->btn_sci_10x = button;
    } else if (strcmp(name, "sciR3C4") == 0) {
        app->btn_sci_ln = button;
    } else if (strcmp(name, "sciR3C5") == 0) {
        app->btn_sci_log10 = button;
    } else if (strcmp(name, "sciR4C1") == 0) {
        app->btn_sci_sin = button;
    } else if (strcmp(name, "sciR4C2") == 0) {
        app->btn_sci_cos = button;
    } else if (strcmp(name, "sciR4C3") == 0) {
        app->btn_sci_tan = button;
    } else if (strcmp(name, "sciR5C1") == 0) {
        app->btn_sci_sinh = button;
    } else if (strcmp(name, "sciR5C2") == 0) {
        app->btn_sci_cosh = button;
    } else if (strcmp(name, "sciR5C3") == 0) {
        app->btn_sci_tanh = button;
    }
}

static void update_second_button_state(AppState *app)
{
    if (!app || !app->btn_second) return;
    Boolean active = is_second_active(app);
    if (!app->second_shadow_cached) {
        Pixel top = 0;
        Pixel bottom = 0;
        short thickness = 0;
        XtVaGetValues(app->btn_second,
                      XmNtopShadowColor, &top,
                      XmNbottomShadowColor, &bottom,
                      XmNshadowThickness, &thickness,
                      NULL);
        app->second_shadow_top = top;
        app->second_shadow_bottom = bottom;
        app->second_shadow_thickness = thickness;
        app->second_shadow_cached = True;
        app->second_thickness_cached = True;
    }
    Pixel top = app->second_shadow_top;
    Pixel bottom = app->second_shadow_bottom;
    if (active) {
        Pixel tmp = top;
        top = bottom;
        bottom = tmp;
    }
    if (!app->second_color_cached) {
        Pixel base_bg = 0;
        XtVaGetValues(app->btn_second,
                      XmNbackground, &base_bg,
                      NULL);
        app->second_bg_normal = base_bg;
        Pixel highlight = base_bg;
        if (app->palette_ok) {
            int idx = (app->palette.active >= 0 && app->palette.active < app->palette.count)
                          ? app->palette.active
                          : app->palette.inactive;
            if (idx >= 0 && idx < app->palette.count) {
                highlight = app->palette.set[idx].bg.pixel;
            }
        }
        app->second_bg_active = highlight;
        app->second_color_cached = True;
    }
    Pixel bg = active ? app->second_bg_active : app->second_bg_normal;
    XtVaSetValues(app->btn_second,
                  XmNshadowType, active ? XmSHADOW_IN : XmSHADOW_OUT,
                  XmNtopShadowColor, top,
                  XmNbottomShadowColor, bottom,
                  XmNshadowThickness, app->second_shadow_thickness,
                  XmNbackground, bg,
                  NULL);
    refresh_second_button_labels(app, active);
    if (app->second_border_prev_active != active) {
        fprintf(stderr, "[ck-calc] 2nd border %s (top=%lu bottom=%lu bg=%lu)\n",
                active ? "pressed" : "released",
                (unsigned long)top,
                (unsigned long)bottom,
                (unsigned long)bg);
        app->second_border_prev_active = active;
    }
}

static void set_shift_state(AppState *app, KeySym sym, Boolean down)
{
    if (!app) return;
    if (sym == XK_Shift_L) {
        app->shift_left_down = (down != False);
    } else if (sym == XK_Shift_R) {
        app->shift_right_down = (down != False);
    } else {
        return;
    }
    fprintf(stderr, "[ck-calc] shift %s -> active=%d (L=%d R=%d)\n",
            down ? "down" : "up",
            is_second_active(app),
            app->shift_left_down,
            app->shift_right_down);
    update_second_button_state(app);
}

static void display_button_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont)
{
    (void)w;
    (void)cont;
    AppState *app = (AppState *)client_data;
    if (!app || !event) return;

    if (event->type == ButtonPress) {
        XButtonEvent *bev = &event->xbutton;
        if (bev->button == Button3) {
            /* Show context menu on right click */
            if (app->display_menu) {
                XmMenuPosition(app->display_menu, bev);
                XtManageChild(app->display_menu);
            }
        }
    }
}

static void cb_display_copy(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = (AppState *)client_data;
    clipboard_copy(app);
    ensure_keyboard_focus(app);
}

static void cb_display_paste(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = (AppState *)client_data;
    clipboard_paste(app);
    ensure_keyboard_focus(app);
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

/* -------------------------------------------------------------------------
 * Clipboard helpers (copy/paste current value)
 * ------------------------------------------------------------------------- */

static void clipboard_copy(AppState *app)
{
    if (!app || !app->shell) return;
    if (app->copy_flash_id) {
        XtRemoveTimeOut(app->copy_flash_id);
        app->copy_flash_id = 0;
    }
    double val = current_input(app);
    strncpy(app->copy_flash_backup, app->display, sizeof(app->copy_flash_backup) - 1);
    app->copy_flash_backup[sizeof(app->copy_flash_backup) - 1] = '\0';

    set_display(app, "COPIED");
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

static void clipboard_paste(AppState *app)
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
    set_display_from_double(app, val);
    strncpy(app->paste_flash_backup, app->display, sizeof(app->paste_flash_backup) - 1);
    app->paste_flash_backup[sizeof(app->paste_flash_backup) - 1] = '\0';
    app->paste_flash_id = XtAppAddTimeOut(app->app_context, 500, paste_flash_reset, (XtPointer)app);
    set_display(app, "PASTED");
    app->entering_new = false;
    ensure_keyboard_focus(app);
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
    session_data_set_int(app->session_data, "mode", app->mode);
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

    fprintf(stderr, "[ck-calc] key: code=%u sym=%lu state=0x%x buf='%c'\n",
            (unsigned int)kc,
            (unsigned long)sym,
            (unsigned int)event->xkey.state,
            (keybuf[0] >= 32 && keybuf[0] < 127) ? keybuf[0] : ' ');

    if (sym == XK_Shift_L || sym == XK_Shift_R) {
        set_shift_state(app, sym, True);
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
        set_shift_state(g_app, sym, False);
        KeyCode kc = event->xkey.keycode;
        fprintf(stderr, "[ck-calc] shift up (%s) code=%u state=0x%x\n",
                (sym == XK_Shift_L) ? "left" : "right",
                (unsigned int)kc,
                (unsigned int)event->xkey.state);
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
                                int col, int col_span, int col_step,
                                XtCallbackProc cb, XtPointer data)
{
    Arg args[12];
    int n = 0;
    XtSetArg(args[n], XmNleftAttachment,  XmATTACH_POSITION); n++;
    XtSetArg(args[n], XmNleftPosition,    col * col_step); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_POSITION); n++;
    XtSetArg(args[n], XmNrightPosition,   (col + col_span) * col_step); n++;
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
    XtAddCallback(about_item, XmNactivateCallback, about_menu_cb, NULL);

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
    update_display(app);

    XtSetSensitive(display_label, True);
    XtAddEventHandler(display_label, ButtonPressMask, True, display_button_handler, (XtPointer)app);

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
    Arg pad_args[4];
    int pn = 0;
    XtSetArg(pad_args[pn], XmNfractionBase, 100); pn++;
    int col_step = 25;
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
    app->keypad = keypad;

    Widget row_anchor = NULL;
    Widget row_top = NULL;
    /* Row 1 */
    row_anchor = create_key_button(keypad, "backBtn", "◀", NULL, False, 0, 1, col_step, cb_backspace, NULL);
    app->btn_back = row_anchor;
    row_top = row_anchor;
    app->btn_ac = create_key_button(keypad, "acBtn",   "AC",   row_top, True, 1, 1, col_step, cb_clear, NULL);
    app->btn_percent = create_key_button(keypad, "percentBtn", "%", row_top, True, 2, 1, col_step, cb_percent, NULL);
    app->btn_div = create_key_button(keypad, "divBtn", "/", row_top, True, 3, 1, col_step, cb_operator, (XtPointer)(uintptr_t)'/');
    /* Match Back height to AC height */
    if (app->btn_back && app->btn_ac) {
        Dimension ac_h = 0;
        XtVaGetValues(app->btn_ac, XmNheight, &ac_h, NULL);
        if (ac_h > 0) {
            XtVaSetValues(app->btn_back, XmNheight, ac_h, NULL);
        }
    }

    /* Row 2 */
    row_anchor = create_key_button(keypad, "sevenBtn", "7", row_anchor, False, 0, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'7');
    app->btn_digits[7] = row_anchor;
    row_top = row_anchor;
    app->btn_digits[8] = create_key_button(keypad, "eightBtn", "8", row_top, True, 1, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'8');
    app->btn_digits[9] = create_key_button(keypad, "nineBtn",  "9", row_top, True, 2, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'9');
    app->btn_mul = create_key_button(keypad, "mulBtn",   "*", row_top, True, 3, 1, col_step, cb_operator, (XtPointer)(uintptr_t)'*');

    /* Row 3 */
    row_anchor = create_key_button(keypad, "fourBtn", "4", row_anchor, False, 0, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'4');
    app->btn_digits[4] = row_anchor;
    row_top = row_anchor;
    app->btn_digits[5] = create_key_button(keypad, "fiveBtn", "5", row_top, True, 1, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'5');
    app->btn_digits[6] = create_key_button(keypad, "sixBtn",  "6", row_top, True, 2, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'6');
    app->btn_minus = create_key_button(keypad, "minusBtn","-", row_top, True, 3, 1, col_step, cb_operator, (XtPointer)(uintptr_t)'-');

    /* Row 4 */
    row_anchor = create_key_button(keypad, "oneBtn", "1", row_anchor, False, 0, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'1');
    app->btn_digits[1] = row_anchor;
    row_top = row_anchor;
    app->btn_digits[2] = create_key_button(keypad, "twoBtn", "2", row_top, True, 1, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'2');
    app->btn_digits[3] = create_key_button(keypad, "threeBtn", "3", row_top, True, 2, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'3');
    app->btn_plus = create_key_button(keypad, "plusBtn", "+", row_top, True, 3, 1, col_step, cb_operator, (XtPointer)(uintptr_t)'+');

    /* Row 5 */
    char decimal_label[2] = {app->decimal_char, '\0'};
    row_anchor = create_key_button(keypad, "signBtn", "+/-", row_anchor, False, 0, 1, col_step, cb_toggle_sign, NULL);
    app->btn_sign = row_anchor;
    row_top = row_anchor;
    app->btn_digits[0] = create_key_button(keypad, "zeroBtn", "0", row_top, True, 1, 1, col_step, cb_digit, (XtPointer)(uintptr_t)'0');
    app->btn_decimal = create_key_button(keypad, "decimalBtn", decimal_label, row_top, True, 2, 1, col_step, cb_decimal, NULL);
    Widget eq_btn = create_key_button(keypad, "eqBtn", "=", row_top, True, 3, 1, col_step, cb_equals, NULL);
    app->btn_eq = eq_btn;
    XtVaSetValues(eq_btn, XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 6, NULL);

    /* placeholder to allow future dynamic rebuilds */
    rebuild_keypad(app);
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
    app.mode = 0; /* basic by default */
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
                set_display(&app, saved_disp);
                app.entering_new = true;
            }
            if (session_data_has(app.session_data, "show_thousands")) {
                app.show_thousands = session_data_get_int(app.session_data, "show_thousands", app.show_thousands ? 1 : 0) != 0;
            }
            if (session_data_has(app.session_data, "mode")) {
                app.mode = session_data_get_int(app.session_data, "mode", app.mode);
            }
        }
    }

    build_ui(&app);
    set_mode(&app, app.mode, False);
    log_mode_width(&app, "startup");
    apply_wm_hints(&app);

    if (app.session_data) {
        if (!session_apply_geometry(app.shell, app.session_data, "x", "y", "w", "h")) {
            center_shell_on_screen(app.shell);
        }
    } else {
        center_shell_on_screen(app.shell);
    }

    apply_current_mode_width(&app);
    lock_shell_dimensions(&app);

    Dimension init_h = 0;
    XtVaGetValues(app.shell, XmNheight, &init_h, NULL);
    if (init_h < 200) {
        XtVaSetValues(app.shell,
                      XmNheight, (Dimension)360,
                      XmNminHeight, (Dimension)360,
                      NULL);
    }

    XtRealizeWidget(app.shell);
    apply_wm_hints(&app);
    lock_shell_dimensions(&app);

    /* Keyboard handler for shortcuts/digits */
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
    ensure_keyboard_focus(&app);

    Atom wm_delete = XmInternAtom(XtDisplay(app.shell), "WM_DELETE_WINDOW", False);
    Atom wm_save   = XmInternAtom(XtDisplay(app.shell), "WM_SAVE_YOURSELF", False);
    XmAddWMProtocolCallback(app.shell, wm_delete, cb_wm_delete, NULL);
    XmAddWMProtocolCallback(app.shell, wm_save,   cb_wm_save, NULL);
    XmActivateWMProtocol(app.shell, wm_delete);
    XmActivateWMProtocol(app.shell, wm_save);

    XtAppMainLoop(app.app_context);

    cleanup_scientific_font(&app);
    session_data_free(app.session_data);
    return 0;
}
