#ifndef CK_CALC_INPUT_HANDLER_H
#define CK_CALC_INPUT_HANDLER_H

#include <Xm/Xm.h>

#include "../app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void input_handler_handle_digit(AppState *app, char digit);
void input_handler_handle_decimal(AppState *app);
void input_handler_handle_backspace(AppState *app);
void input_handler_handle_clear(AppState *app);
void input_handler_handle_toggle_sign(AppState *app);
void input_handler_handle_percent(AppState *app);
void input_handler_handle_operator(AppState *app, char op);
void input_handler_handle_equals(AppState *app);

#ifdef __cplusplus
}
#endif

#endif /* CK_CALC_INPUT_HANDLER_H */
