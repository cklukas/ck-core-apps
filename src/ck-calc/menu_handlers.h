#ifndef CK_CALC_MENU_HANDLERS_H
#define CK_CALC_MENU_HANDLERS_H

#include <Xm/Xm.h>

#include "app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void menu_handlers_cb_toggle_thousands(Widget w, XtPointer client_data, XtPointer call_data);
void menu_handlers_cb_menu_new(Widget w, XtPointer client_data, XtPointer call_data);
void menu_handlers_cb_menu_close(Widget w, XtPointer client_data, XtPointer call_data);
void menu_handlers_cb_wm_delete(Widget w, XtPointer client_data, XtPointer call_data);
void menu_handlers_cb_wm_save(Widget w, XtPointer client_data, XtPointer call_data);
void menu_handlers_cb_about(Widget w, XtPointer client_data, XtPointer call_data);

#ifdef __cplusplus
}
#endif

#endif /* CK_CALC_MENU_HANDLERS_H */
