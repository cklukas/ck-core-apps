#ifndef CK_CALC_SCI_VISUALS_H
#define CK_CALC_SCI_VISUALS_H

#include <Xm/Xm.h>
#include <X11/keysym.h>

#include "../app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void sci_visuals_register_button(AppState *app, const char *name, Widget button);
void sci_visuals_update(AppState *app);
void sci_visuals_handle_shift(AppState *app, KeySym sym, Boolean down);
void sci_visuals_toggle_button(Widget w, XtPointer client_data, XtPointer call_data);
void sci_visuals_arm_button(Widget w, XtPointer client_data, XtPointer call_data);
void sci_visuals_second_button_event(Widget w, XtPointer client_data, XEvent *event, Boolean *cont);
Boolean sci_visuals_is_second_active(const AppState *app);

#ifdef __cplusplus
}
#endif

#endif /* CK_CALC_SCI_VISUALS_H */
