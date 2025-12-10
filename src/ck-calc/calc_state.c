#include "calc_state.h"

#include <stdlib.h>

void calc_state_init(CalcState *state)
{
    if (!state) return;
    state->stored_value = 0.0;
    state->last_operand = 0.0;
    state->pending_op = 0;
    state->last_op = 0;
    state->has_pending_value = false;
    state->entering_new = true;
    state->error_state = false;
}

void calc_state_reset(CalcState *state)
{
    calc_state_init(state);
}

double calc_state_current_input(const CalcState *state,
                                const char *display,
                                char decimal_char,
                                char thousands_char,
                                bool show_thousands)
{
    if (!state || !display) return 0.0;
    if (state->error_state) return 0.0;

    char buf[128];
    size_t bpos = 0;
    for (const char *p = display; *p && bpos + 1 < sizeof(buf); ++p) {
        if (show_thousands && *p == thousands_char) continue;
        if (*p == decimal_char && decimal_char != '.') {
            buf[bpos++] = '.';
            continue;
        }
        buf[bpos++] = *p;
    }
    buf[bpos] = '\0';
    return strtod(buf, NULL);
}

bool calc_state_apply_operation(CalcState *state, char op, double rhs, double *out)
{
    if (!state || !out) return false;
    double lhs = state->stored_value;
    double result = lhs;

    switch (op) {
        case '+': result = lhs + rhs; break;
        case '-': result = lhs - rhs; break;
        case '*': result = lhs * rhs; break;
        case '/':
            if (rhs == 0.0) {
                return false;
            }
            result = lhs / rhs;
            break;
        default:
            return false;
    }

    *out = result;
    return true;
}
