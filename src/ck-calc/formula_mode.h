#ifndef CK_CALC_FORMULA_MODE_H
#define CK_CALC_FORMULA_MODE_H

#include "app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void formula_mode_update_display(AppState *app);
void formula_mode_prepare_for_edit(AppState *app);
void formula_mode_seed_with_last_result(AppState *app);

#ifdef __cplusplus
}
#endif

#endif /* CK_CALC_FORMULA_MODE_H */
