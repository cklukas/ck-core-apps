#ifndef CK_CALC_APP_STATE_UTILS_H
#define CK_CALC_APP_STATE_UTILS_H

#include "app_state.h"

void ck_calc_init_locale_settings(AppState *app);
void ck_calc_load_view_state(AppState *app);
void ck_calc_save_view_state(const AppState *app);

#endif /* CK_CALC_APP_STATE_UTILS_H */
