#ifndef CK_TASKS_CTRL_H
#define CK_TASKS_CTRL_H

#include "../shared/session_utils.h"
#include "ck-tasks-ui.h"

typedef struct TasksController TasksController;

TasksController *tasks_ctrl_create(TasksUi *ui, SessionData *session_data);
void tasks_ctrl_destroy(TasksController *ctrl);
void tasks_ctrl_handle_viewport_change(TasksController *ctrl);
void tasks_ctrl_set_selected_application(TasksController *ctrl, int index);
void tasks_ctrl_set_show_disabled_services(TasksController *ctrl, Boolean show_disabled);

#endif /* CK_TASKS_CTRL_H */
