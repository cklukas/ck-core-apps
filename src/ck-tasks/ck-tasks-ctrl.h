#ifndef CK_TASKS_CTRL_H
#define CK_TASKS_CTRL_H

#include "../shared/session_utils.h"
#include "ck-tasks-ui.h"

typedef struct TasksController TasksController;

TasksController *tasks_ctrl_create(TasksUi *ui, SessionData *session_data);
void tasks_ctrl_destroy(TasksController *ctrl);

#endif /* CK_TASKS_CTRL_H */
