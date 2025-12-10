#ifndef CK_CALC_KEYPAD_LAYOUT_H
#define CK_CALC_KEYPAD_LAYOUT_H

#include "../app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void ck_calc_rebuild_keypad(AppState *app);
void ck_calc_cleanup_scientific_font(AppState *app);

#ifdef __cplusplus
}
#endif

#endif /* CK_CALC_KEYPAD_LAYOUT_H */
