#ifndef CK_TASKS_TABS_H
#define CK_TASKS_TABS_H

#include "ck-tasks-ui.h"

Widget tasks_ui_create_process_tab(TasksUi *ui);
Widget tasks_ui_create_applications_tab(TasksUi *ui);
Widget tasks_ui_create_performance_tab(TasksUi *ui);
Widget tasks_ui_create_networking_tab(TasksUi *ui);
Widget tasks_ui_create_simple_tab(TasksUi *ui, TasksTab tab, const char *name,
                                  const char *title, const char *description);

void tasks_ui_destroy_process_tab(TasksUi *ui);
void tasks_ui_destroy_applications_tab(TasksUi *ui);

#endif /* CK_TASKS_TABS_H */
