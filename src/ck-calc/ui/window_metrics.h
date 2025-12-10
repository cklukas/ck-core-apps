#ifndef CK_CALC_WINDOW_METRICS_H
#define CK_CALC_WINDOW_METRICS_H

#include <Xm/Xm.h>

#include "../app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void ck_calc_log_mode_width(AppState *app, const char *context);
void ck_calc_apply_current_mode_width(AppState *app);
void ck_calc_apply_wm_hints(AppState *app);
void ck_calc_lock_shell_dimensions(AppState *app);

#ifdef __cplusplus
}
#endif

#endif /* CK_CALC_WINDOW_METRICS_H */
