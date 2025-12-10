#include "keypad_layout.h"

#include <Xm/Form.h>
#include <Xm/PushB.h>
#include <Xm/Xm.h>
#include <X11/Intrinsic.h>
#include <X11/Xlib.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../shared/cde_palette.h"
#include "sci_visuals.h"

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
        char *seg = malloc(len + 1);
        if (!seg) { success = false; break; }
        memcpy(seg, start, len);
        seg[len] = '\0';
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

void ck_calc_cleanup_scientific_font(AppState *app)
{
    cleanup_scientific_font(app);
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

extern void ck_calc_cb_backspace(Widget, XtPointer, XtPointer);
extern void ck_calc_cb_clear(Widget, XtPointer, XtPointer);
extern void ck_calc_cb_percent(Widget, XtPointer, XtPointer);
extern void ck_calc_cb_operator(Widget, XtPointer, XtPointer);
extern void ck_calc_cb_digit(Widget, XtPointer, XtPointer);
extern void ck_calc_cb_decimal(Widget, XtPointer, XtPointer);
extern void ck_calc_cb_toggle_sign(Widget, XtPointer, XtPointer);
extern void ck_calc_cb_equals(Widget, XtPointer, XtPointer);

void ck_calc_rebuild_keypad(AppState *app)
{
    if (!app || !app->content_form || !app->display_label) return;

    if (app->keypad && XtIsWidget(app->keypad)) {
        XtDestroyWidget(app->keypad);
        app->keypad = NULL;
    }

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

    if (offset > 0) {
        for (int i = 0; i < offset; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "sciR1C%d", i);
            Widget sci_button = create_key_button(keypad, name, get_sci_label(0, i), row_anchor, False, i, 1, col_step, NULL, NULL);
            register_scientific_button(app, sci_button, sci_extra_buttons, &sci_extra_count, sci_extra_capacity, sci_extra_height, sci_extra_flushed);
            sci_visuals_register_button(app, name, sci_button);
        }
    }

    /* Row 1 */
    row_anchor = create_key_button(keypad, "backBtn", "◀", NULL, False, offset + 0, 1, col_step, ck_calc_cb_backspace, NULL);
    app->btn_back = row_anchor;
    row_top = row_anchor;
    if (offset > 0) {
        for (int i = 0; i < offset; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "sciR1bC%d", i);
            Widget sci_button = create_key_button(keypad, name, get_sci_label(0, i), row_top, True, i, 1, col_step, NULL, NULL);
            register_scientific_button(app, sci_button, sci_extra_buttons, &sci_extra_count, sci_extra_capacity, sci_extra_height, sci_extra_flushed);
            sci_visuals_register_button(app, name, sci_button);
        }
    }
    app->btn_ac = create_key_button(keypad, "acBtn",   "AC",   row_top, True, offset + 1, 1, col_step, ck_calc_cb_clear, NULL);
    app->btn_percent = create_key_button(keypad, "percentBtn", "%", row_top, True, offset + 2, 1, col_step, ck_calc_cb_percent, NULL);
    app->btn_div = create_key_button(keypad, "divBtn", "/", row_top, True, offset + 3, 1, col_step, ck_calc_cb_operator, (XtPointer)(uintptr_t)'/');
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
    row_anchor = create_key_button(keypad, "sevenBtn", "7", row_anchor, False, offset + 0, 1, col_step, ck_calc_cb_digit, (XtPointer)(uintptr_t)'7');
    app->btn_digits[7] = row_anchor;
    row_top = row_anchor;
    if (offset > 0) {
        for (int i = 0; i < offset; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "sciR2C%d", i);
            Widget sci_button = create_key_button(keypad, name, get_sci_label(1, i), row_top, True, i, 1, col_step, (i == 0) ? sci_visuals_toggle_button : NULL, app);
            register_scientific_button(app, sci_button, sci_extra_buttons, &sci_extra_count, sci_extra_capacity, sci_extra_height, sci_extra_flushed);
            sci_visuals_register_button(app, name, sci_button);
            if (i == 0) {
                app->btn_second = sci_button;
                XtAddEventHandler(sci_button, ButtonPressMask, False, sci_visuals_second_button_event, app);
                XtAddEventHandler(sci_button, ButtonReleaseMask, False, sci_visuals_second_button_event, app);
                XtAddCallback(sci_button, XmNarmCallback, sci_visuals_arm_button, app);
            }
        }
    }
    app->btn_digits[8] = create_key_button(keypad, "eightBtn", "8", row_top, True, offset + 1, 1, col_step, ck_calc_cb_digit, (XtPointer)(uintptr_t)'8');
    app->btn_digits[9] = create_key_button(keypad, "nineBtn",  "9", row_top, True, offset + 2, 1, col_step, ck_calc_cb_digit, (XtPointer)(uintptr_t)'9');
    app->btn_mul = create_key_button(keypad, "mulBtn",   "*", row_top, True, offset + 3, 1, col_step, ck_calc_cb_operator, (XtPointer)(uintptr_t)'*');

    /* Row 3 */
    row_anchor = create_key_button(keypad, "fourBtn", "4", row_anchor, False, offset + 0, 1, col_step, ck_calc_cb_digit, (XtPointer)(uintptr_t)'4');
    app->btn_digits[4] = row_anchor;
    row_top = row_anchor;
    if (offset > 0) {
        for (int i = 0; i < offset; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "sciR3C%d", i);
            Widget sci_button = create_key_button(keypad, name, get_sci_label(2, i), row_top, True, i, 1, col_step, NULL, NULL);
            register_scientific_button(app, sci_button, sci_extra_buttons, &sci_extra_count, sci_extra_capacity, sci_extra_height, sci_extra_flushed);
            sci_visuals_register_button(app, name, sci_button);
        }
    }
    app->btn_digits[5] = create_key_button(keypad, "fiveBtn", "5", row_top, True, offset + 1, 1, col_step, ck_calc_cb_digit, (XtPointer)(uintptr_t)'5');
    app->btn_digits[6] = create_key_button(keypad, "sixBtn",  "6", row_top, True, offset + 2, 1, col_step, ck_calc_cb_digit, (XtPointer)(uintptr_t)'6');
    app->btn_minus = create_key_button(keypad, "minusBtn","-", row_top, True, offset + 3, 1, col_step, ck_calc_cb_operator, (XtPointer)(uintptr_t)'-');

    /* Row 4 */
    row_anchor = create_key_button(keypad, "oneBtn", "1", row_anchor, False, offset + 0, 1, col_step, ck_calc_cb_digit, (XtPointer)(uintptr_t)'1');
    app->btn_digits[1] = row_anchor;
    row_top = row_anchor;
    if (offset > 0) {
        for (int i = 0; i < offset; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "sciR4C%d", i);
            Widget sci_button = create_key_button(keypad, name, get_sci_label(3, i), row_top, True, i, 1, col_step, NULL, NULL);
            register_scientific_button(app, sci_button, sci_extra_buttons, &sci_extra_count, sci_extra_capacity, sci_extra_height, sci_extra_flushed);
            sci_visuals_register_button(app, name, sci_button);
        }
    }
    app->btn_digits[2] = create_key_button(keypad, "twoBtn", "2", row_top, True, offset + 1, 1, col_step, ck_calc_cb_digit, (XtPointer)(uintptr_t)'2');
    app->btn_digits[3] = create_key_button(keypad, "threeBtn", "3", row_top, True, offset + 2, 1, col_step, ck_calc_cb_digit, (XtPointer)(uintptr_t)'3');
    app->btn_plus = create_key_button(keypad, "plusBtn", "+", row_top, True, offset + 3, 1, col_step, ck_calc_cb_operator, (XtPointer)(uintptr_t)'+');

    /* Row 5 */
    char decimal_label[2] = {app->decimal_char, '\0'};
    row_anchor = create_key_button(keypad, "signBtn", "+/-", row_anchor, False, offset + 0, 1, col_step, ck_calc_cb_toggle_sign, NULL);
    app->btn_sign = row_anchor;
    row_top = row_anchor;
    if (offset > 0) {
        for (int i = 0; i < offset; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "sciR5C%d", i);
            Widget sci_button = create_key_button(keypad, name, get_sci_label(4, i), row_top, True, i, 1, col_step, NULL, NULL);
            register_scientific_button(app, sci_button, sci_extra_buttons, &sci_extra_count, sci_extra_capacity, sci_extra_height, sci_extra_flushed);
            sci_visuals_register_button(app, name, sci_button);
        }
    }
    app->btn_digits[0] = create_key_button(keypad, "zeroBtn", "0", row_top, True, offset + 1, 1, col_step, ck_calc_cb_digit, (XtPointer)(uintptr_t)'0');
    app->btn_decimal = create_key_button(keypad, "decimalBtn", decimal_label, row_top, True, offset + 2, 1, col_step, ck_calc_cb_decimal, NULL);
    Widget eq_btn = create_key_button(keypad, "eqBtn", "=", row_top, True, offset + 3, 1, col_step, ck_calc_cb_equals, NULL);
    app->btn_eq = eq_btn;

    /* Apply color accents to right column using CDE select color */
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

    sci_visuals_update(app);
}
