#ifndef CK_CALC_CLIPBOARD_H
#define CK_CALC_CLIPBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "app_state.h"

void ck_calc_clipboard_copy(AppState *app);
void ck_calc_clipboard_paste(AppState *app);

#ifdef __cplusplus
}
#endif

#endif /* CK_CALC_CLIPBOARD_H */
