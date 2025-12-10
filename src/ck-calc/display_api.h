#ifndef CK_CALC_DISPLAY_API_H
#define CK_CALC_DISPLAY_API_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AppState AppState;

void ck_calc_set_display(AppState *app, const char *text);
void ck_calc_set_display_from_double(AppState *app, double value);
double ck_calc_current_input(AppState *app);
void ck_calc_reset_state(AppState *app);
void ck_calc_ensure_keyboard_focus(AppState *app);
void ck_calc_reformat_display(AppState *app);

#ifdef __cplusplus
}
#endif

#endif /* CK_CALC_DISPLAY_API_H */
