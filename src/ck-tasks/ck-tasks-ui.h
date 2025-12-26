#ifndef CK_TASKS_UI_H
#define CK_TASKS_UI_H

#include <Xm/Xm.h>

#include "../shared/gridlayout/gridlayout.h"
#include "ck-tasks-model.h"

typedef enum {
    TASKS_TAB_PROCESSES = 1,
    TASKS_TAB_PERFORMANCE,
    TASKS_TAB_NETWORKING,
    TASKS_TAB_APPLICATIONS,
    TASKS_TAB_SERVICES,
} TasksTab;

typedef struct {
    XtAppContext app;
    Widget toplevel;
    Widget main_form;
    Widget menu_bar;
    Widget tab_stack;
    Widget tab_processes;
    Widget tab_performance;
    Widget tab_networking;
    Widget tab_applications;
    Widget tab_services;
    Widget status_label;
    Widget process_filter_toggle;
    GridLayout *process_grid;
    Widget apps_list;
    Widget apps_close_button;
    Widget perf_cpu_meter;
    Widget perf_mem_meter;
    Widget perf_load1_meter;
    Widget perf_load5_meter;
    Widget perf_load15_meter;
    Widget perf_cpu_value_label;
    Widget perf_mem_value_label;
    Widget perf_load1_value_label;
    Widget perf_load5_value_label;
    Widget perf_load15_value_label;
    Widget menu_file_connect;
    Widget menu_file_new_window;
    Widget menu_file_exit;
    Widget menu_view_refresh;
    Widget menu_view_processes;
    Widget menu_view_performance;
    Widget menu_view_networking;
    Widget menu_options_always_on_top;
    Widget menu_options_update_1s;
    Widget menu_options_update_2s;
    Widget menu_options_update_5s;
    Widget menu_options_filter_by_user;
    Widget menu_help_help;
    Widget menu_help_about;
} TasksUi;

TasksUi *tasks_ui_create(XtAppContext app, Widget toplevel);
void tasks_ui_destroy(TasksUi *ui);
XtAppContext tasks_ui_get_app_context(TasksUi *ui);
Widget tasks_ui_get_toplevel(TasksUi *ui);
int tasks_ui_get_current_tab(TasksUi *ui);
void tasks_ui_set_current_tab(TasksUi *ui, TasksTab tab);
void tasks_ui_update_status(TasksUi *ui, const char *text);
void tasks_ui_center_on_screen(TasksUi *ui);
void tasks_ui_set_processes(TasksUi *ui, const TasksProcessEntry *entries, int count);
void tasks_ui_set_applications(TasksUi *ui, const XmString *items, int count);
void tasks_ui_update_system_stats(TasksUi *ui, const TasksSystemStats *stats);

#endif /* CK_TASKS_UI_H */
