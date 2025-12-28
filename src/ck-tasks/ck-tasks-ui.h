#ifndef CK_TASKS_UI_H
#define CK_TASKS_UI_H

#include <Xm/Xm.h>
#include <X11/Xlib.h>

#include "../shared/ck-table/ck_table.h"
#include "ck-tasks-model.h"

typedef struct TasksController TasksController;

typedef struct TasksApplicationEntry {
    Window window;
    pid_t pid;
    int pid_known;
    int window_count;
    char title[128];
    char command[256];
    char wm_class[128];
} TasksApplicationEntry;

typedef enum {
    TASKS_TAB_PROCESSES = 1,
    TASKS_TAB_PERFORMANCE,
    TASKS_TAB_NETWORKING,
    TASKS_TAB_APPLICATIONS,
    TASKS_TAB_SERVICES,
    TASKS_TAB_USERS,
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
    Widget tab_users;
    Widget status_frame_processes;
    Widget status_frame_cpu;
    Widget status_frame_memory;
    Widget status_frame_message;
    Widget status_processes_label;
    Widget status_cpu_label;
    Widget status_memory_label;
    Widget status_message_label;
    char status_processes_text[128];
    char status_cpu_text[128];
    char status_memory_text[128];
    char status_message_text[256];
    Boolean status_bar_layout_initialized;
    Dimension status_bar_width_processes;
    Dimension status_bar_width_cpu;
    Dimension status_bar_width_memory;
    Widget process_filter_toggle;
    Widget process_search_field;
    Widget process_scrollbar;
    CkTable *process_table;
    CkTable *apps_table;
    CkTable *services_table;
    Widget services_controls_form;
    Widget services_show_disabled_toggle;
    Boolean services_show_disabled;
    Widget services_pane;
    TableRow **services_rows;
    int services_row_count;
    int services_row_capacity;
    const TasksServiceEntry *services_entries;
    int services_entries_count;
    Widget services_info_label;
    char services_info_text[160];
    Widget services_split_line;
    Widget services_info_frame;
    Widget services_info_title;
    CkTable *services_info_table;
    Dimension services_info_height;
    Boolean services_info_height_set;
    Boolean services_info_visible;
    int services_info_ignore_configure;
    Widget services_selected_row;
    int services_selected_index;
    Boolean services_updates_paused;
    CkTable *users_table;
    TableRow **users_rows;
    int users_row_count;
    int users_row_capacity;
    const TasksUserEntry *users_entries;
    int users_entries_count;
    Widget users_controls_form;
    Widget users_logout_button;
    Widget users_logout_status_label;
    Widget users_selected_row;
    int users_selected_index;
    Boolean users_updates_paused;
    Widget apps_search_field;
    Widget apps_selected_row;
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
    TasksController *controller;
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
void tasks_ui_set_process_row_window(int start);
int tasks_ui_get_process_row_page_size(void);
void tasks_ui_set_applications_table(TasksUi *ui, const TasksApplicationEntry *entries, int count);
void tasks_ui_set_users_table(TasksUi *ui, const TasksUserEntry *entries, int count);
void tasks_ui_set_services_table(TasksUi *ui, const TasksServiceEntry *entries, int count,
                                 const TasksInitInfo *init_info);
void tasks_ui_update_process_count(TasksUi *ui, int total_processes);
void tasks_ui_update_system_stats(TasksUi *ui, const TasksSystemStats *stats);
void tasks_ui_statusbar_maybe_resize(TasksUi *ui);

#endif /* CK_TASKS_UI_H */
