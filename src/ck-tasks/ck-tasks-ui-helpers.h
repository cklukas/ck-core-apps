#ifndef CK_TASKS_UI_HELPERS_H
#define CK_TASKS_UI_HELPERS_H

#include <Xm/Xm.h>

#include <stddef.h>

#include "ck-tasks-ui.h"

XmString tasks_ui_make_string(const char *text);
XmString *tasks_ui_make_string_array(const char *const strings[], int count);
void tasks_ui_set_label_text(Widget widget, const char *text);
Boolean tasks_ui_status_set_label_text(Widget label, char *cache, size_t cache_len, const char *text);
Widget tasks_ui_create_page(TasksUi *ui, const char *name, TasksTab tab_number,
                            const char *title, const char *subtitle);

#endif /* CK_TASKS_UI_HELPERS_H */
