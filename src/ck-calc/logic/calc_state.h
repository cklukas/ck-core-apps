#ifndef CK_CALC_STATE_H
#define CK_CALC_STATE_H

#include <stdbool.h>

typedef struct {
    double stored_value;
    double last_operand;
    char pending_op;
    char last_op;
    bool has_pending_value;
    bool entering_new;
    bool error_state;
} CalcState;

void calc_state_init(CalcState *state);
void calc_state_reset(CalcState *state);
double calc_state_current_input(const CalcState *state,
                                const char *display,
                                char decimal_char,
                                char thousands_char,
                                bool show_thousands);
bool calc_state_apply_operation(CalcState *state, char op, double rhs, double *out);

#endif /* CK_CALC_STATE_H */
