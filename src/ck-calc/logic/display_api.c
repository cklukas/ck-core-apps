#include "display_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <Xm/Xm.h>
#include <Xm/Label.h>

#include "../app_state.h"
#include "formula_eval.h"

static void update_display(AppState *app)
{
    if (!app || !app->display_label) return;
    XmString xms = XmStringCreateLocalized(app->display);
    XtVaSetValues(app->display_label,
                  XmNlabelString, xms,
                  NULL);
    XmStringFree(xms);
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

static double current_input(AppState *app)
{
    if (!app) return 0.0;
    if (app->calc_state.error_state) return 0.0;

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

void ck_calc_set_display(AppState *app, const char *text)
{
    if (!app || !text) return;
    strncpy(app->display, text, sizeof(app->display) - 1);
    app->display[sizeof(app->display) - 1] = '\0';
    update_display(app);
}

void ck_calc_set_display_from_double(AppState *app, double value)
{
    if (!app) return;
    format_number(app, value, app->display, sizeof(app->display));
    update_display(app);
}

double ck_calc_current_input(AppState *app)
{
    return current_input(app);
}

void ck_calc_reset_state(AppState *app)
{
    if (!app) return;
    app->calc_state.stored_value      = 0.0;
    app->calc_state.last_operand      = 0.0;
    app->calc_state.pending_op        = 0;
    app->calc_state.last_op           = 0;
    app->calc_state.has_pending_value = false;
    app->calc_state.entering_new      = true;
    app->calc_state.error_state       = false;
    formula_clear(&app->formula_ctx);
    app->formula_showing_result = false;
    app->formula_last_result = 0.0;
    app->last_rand_len = 0;
    app->last_rand_token[0] = '\0';
    ck_calc_set_display(app, "0");
}

void ck_calc_ensure_keyboard_focus(AppState *app)
{
    ensure_keyboard_focus(app);
}

void ck_calc_reformat_display(AppState *app)
{
    if (!app || app->calc_state.error_state) return;

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
    ck_calc_set_display_from_double(app, val);
    app->calc_state.entering_new = false;
}
