#include "input_handler.h"

#include <string.h>

#include "display_api.h"
#include "formula_eval.h"
#include "formula_mode.h"
#include "calc_state.h"

static void ensure_reset_on_error(AppState *app)
{
    if (!app) return;
    if (app->calc_state.error_state) {
        ck_calc_reset_state(app);
    }
}

void input_handler_handle_digit(AppState *app, char digit)
{
    if (!app) return;
    CalcState *state = &app->calc_state;
    if (app->mode == 1) {
        ensure_reset_on_error(app);
        formula_mode_prepare_for_edit(app);
        if (formula_append_char(&app->formula_ctx, digit)) {
            formula_mode_update_display(app);
        }
        ck_calc_ensure_keyboard_focus(app);
        return;
    }
    if (state->error_state) {
        ck_calc_reset_state(app);
    }
    size_t len = strlen(app->display);
    if (state->entering_new) {
        if (state->pending_op == 0) {
            state->has_pending_value = false;
            state->last_op = 0;
        }
        app->display[0] = digit;
        app->display[1] = '\0';
        state->entering_new = false;
    } else if (len == 1 && app->display[0] == '0') {
        app->display[0] = digit;
        app->display[1] = '\0';
    } else if (len + 1 < sizeof(app->display)) {
        app->display[len] = digit;
        app->display[len + 1] = '\0';
    }
    ck_calc_reformat_display(app);
    ck_calc_ensure_keyboard_focus(app);
}

void input_handler_handle_decimal(AppState *app)
{
    if (!app) return;
    CalcState *state = &app->calc_state;
    if (app->mode == 1) {
        ensure_reset_on_error(app);
        formula_mode_prepare_for_edit(app);
        if (formula_append_char(&app->formula_ctx, '.')) {
            formula_mode_update_display(app);
        }
        ck_calc_ensure_keyboard_focus(app);
        return;
    }
    if (state->error_state) {
        ck_calc_reset_state(app);
    }
    if (state->entering_new) {
        char buf[3] = { '0', app->decimal_char, '\0' };
        strncpy(app->display, buf, sizeof(app->display));
        state->entering_new = false;
        ck_calc_reformat_display(app);
        ck_calc_ensure_keyboard_focus(app);
        return;
    }
    if (!strchr(app->display, app->decimal_char)) {
        size_t len = strlen(app->display);
        if (len + 1 < sizeof(app->display)) {
            app->display[len] = app->decimal_char;
            app->display[len + 1] = '\0';
            ck_calc_reformat_display(app);
        }
    }
    ck_calc_ensure_keyboard_focus(app);
}

void input_handler_handle_backspace(AppState *app)
{
    if (!app) return;
    CalcState *state = &app->calc_state;
    if (app->mode == 1) {
        ensure_reset_on_error(app);
        formula_mode_prepare_for_edit(app);
        formula_backspace(&app->formula_ctx);
        formula_mode_update_display(app);
        ck_calc_ensure_keyboard_focus(app);
        return;
    }
    if (state->error_state) {
        ck_calc_reset_state(app);
        return;
    }
    size_t len = strlen(app->display);
    if (len <= 1 || (len == 2 && app->display[0] == '-')) {
        ck_calc_set_display(app, "0");
        state->entering_new = true;
    } else {
        app->display[len - 1] = '\0';
        ck_calc_reformat_display(app);
    }
    ck_calc_ensure_keyboard_focus(app);
}

void input_handler_handle_clear(AppState *app)
{
    if (!app) return;
    ck_calc_reset_state(app);
    ck_calc_ensure_keyboard_focus(app);
}

void input_handler_handle_toggle_sign(AppState *app)
{
    if (!app) return;
    CalcState *state = &app->calc_state;
    if (app->mode == 1) {
        ck_calc_ensure_keyboard_focus(app);
        return;
    }
    if (state->error_state) {
        ck_calc_reset_state(app);
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
    ck_calc_ensure_keyboard_focus(app);
}

void input_handler_handle_percent(AppState *app)
{
    if (!app) return;
    CalcState *state = &app->calc_state;
    if (app->mode == 1) {
        ck_calc_ensure_keyboard_focus(app);
        return;
    }
    if (state->error_state) {
        ck_calc_reset_state(app);
        return;
    }
    double value = ck_calc_current_input(app);
    if (state->has_pending_value) {
        value = state->stored_value * value / 100.0;
    } else {
        value = value / 100.0;
    }
    ck_calc_set_display_from_double(app, value);
    state->entering_new = true;
    state->last_op = 0;
    ck_calc_ensure_keyboard_focus(app);
}

static void apply_pending_operation(AppState *app, CalcState *state, double value)
{
    if (state->pending_op && state->has_pending_value && !state->entering_new) {
        double result = 0.0;
        if (calc_state_apply_operation(state, state->pending_op, value, &result)) {
            state->stored_value = result;
            ck_calc_set_display_from_double(app, result);
        } else {
            state->error_state = true;
            state->pending_op = 0;
            state->has_pending_value = false;
            state->last_op = 0;
            state->entering_new = true;
            ck_calc_set_display(app, "Error");
        }
    } else if (!state->has_pending_value) {
        state->stored_value = value;
        state->has_pending_value = true;
    }
}

void input_handler_handle_operator(AppState *app, char op)
{
    if (!app) return;
    CalcState *state = &app->calc_state;
    if (app->mode == 1) {
        ensure_reset_on_error(app);
        if (app->formula_showing_result) {
            formula_mode_seed_with_last_result(app);
        }
        if (formula_append_char(&app->formula_ctx, op)) {
            formula_mode_update_display(app);
        }
        ck_calc_ensure_keyboard_focus(app);
        return;
    }
    if (state->error_state) {
        ck_calc_reset_state(app);
    }
    double value = ck_calc_current_input(app);
    apply_pending_operation(app, state, value);
    state->pending_op = op;
    state->entering_new = true;
    state->last_op = 0;
    ck_calc_ensure_keyboard_focus(app);
}

void input_handler_handle_equals(AppState *app)
{
    if (!app) return;
    CalcState *state = &app->calc_state;
    if (app->mode == 1) {
        if (state->error_state) {
            ck_calc_reset_state(app);
            return;
        }
        ck_calc_ensure_keyboard_focus(app);
        return;
    }
    if (state->error_state) {
        ck_calc_reset_state(app);
        return;
    }
    char op = state->pending_op;
    double rhs = ck_calc_current_input(app);
    if (!op) {
        if (!state->last_op) return;
        op = state->last_op;
        rhs = state->last_operand;
    } else {
        state->last_op = op;
        state->last_operand = rhs;
    }
    if (!state->has_pending_value) {
        state->stored_value = ck_calc_current_input(app);
        state->has_pending_value = true;
    }
    double result = 0.0;
    if (!calc_state_apply_operation(state, op, rhs, &result)) {
        ck_calc_set_display(app, "Error");
        state->error_state = true;
        return;
    }
    state->stored_value = result;
    ck_calc_set_display_from_double(app, result);
    state->pending_op = 0;
    state->entering_new = true;
    ck_calc_ensure_keyboard_focus(app);
}
