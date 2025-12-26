#include "ck-tasks-ctrl.h"

#include "../shared/about_dialog.h"
#include <X11/Intrinsic.h>
#include <Xm/Xm.h>
#include <Xm/MessageB.h>
#include <Xm/ToggleBG.h>

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

struct TasksController {
    TasksUi *ui;
    SessionData *session;
    Widget about_shell;
};

static void destroy_dialog(Widget widget, XtPointer client, XtPointer call)
{
    (void)call;
    if (widget) XtDestroyWidget(widget);
    (void)client;
}

static void on_file_exit(Widget widget, XtPointer client, XtPointer call)
{
    (void)widget;
    (void)call;
    TasksController *ctrl = client;
    if (!ctrl) return;
    XtAppContext app = tasks_ui_get_app_context(ctrl->ui);
    XtAppSetExitFlag(app);
}

static void on_file_connect(Widget widget, XtPointer client, XtPointer call)
{
    (void)widget;
    (void)call;
    TasksController *ctrl = client;
    if (!ctrl) return;
    tasks_ui_update_status(ctrl->ui, "Connect to Remote placeholder.");
}

static void on_file_new_window(Widget widget, XtPointer client, XtPointer call)
{
    (void)widget;
    (void)call;
    TasksController *ctrl = client;
    if (!ctrl) return;
    char path[PATH_MAX] = {0};
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len <= 0) {
        tasks_ui_update_status(ctrl->ui, "Cannot determine executable path.");
        return;
    }
    path[len] = '\0';
    pid_t pid = fork();
    if (pid == 0) {
        execl(path, path, (char *)NULL);
        _exit(1);
    }
    if (pid < 0) {
        tasks_ui_update_status(ctrl->ui, "Failed to launch another Task Manager.");
    } else {
        tasks_ui_update_status(ctrl->ui, "Launched new Task Manager window.");
    }
}

static void on_view_refresh(Widget widget, XtPointer client, XtPointer call)
{
    (void)widget;
    (void)call;
    TasksController *ctrl = client;
    if (!ctrl) return;
    tasks_ui_update_status(ctrl->ui, "Manual refresh requested.");
}

static void on_view_show_tab(Widget widget, XtPointer client, XtPointer call)
{
    (void)call;
    TasksController *ctrl = client;
    if (!ctrl || !widget) return;
    int tab_idx = 0;
    XtVaGetValues(widget, XmNuserData, &tab_idx, NULL);
    tasks_ui_set_current_tab(ctrl->ui, (TasksTab)tab_idx);
    tasks_ui_update_status(ctrl->ui, "Switched tab.");
}

static void on_options_always_on_top(Widget widget, XtPointer client, XtPointer call)
{
    (void)call;
    TasksController *ctrl = client;
    if (!ctrl) return;
    Boolean state = XmToggleButtonGadgetGetState(widget);
    tasks_ui_update_status(ctrl->ui, state ? "Always on Top enabled." : "Always on Top disabled.");
}

static void on_options_update(Widget widget, XtPointer client, XtPointer call)
{
    (void)widget;
    (void)call;
    TasksController *ctrl = client;
    if (!ctrl) return;
    char label[32] = {0};
    XmString xs = NULL;
    XtVaGetValues(widget, XmNlabelString, &xs, NULL);
    char *text = XmStringUnparse(xs, NULL, XmCHARSET_TEXT, XmCHARSET_TEXT, NULL, 0, XmOUTPUT_ALL);
    if (text) {
        snprintf(label, sizeof(label), "Update interval %s selected.", text);
        XtFree(text);
    } else {
        snprintf(label, sizeof(label), "Update interval set.");
    }
    tasks_ui_update_status(ctrl->ui, label);
}

static void on_options_filter_by_user(Widget widget, XtPointer client, XtPointer call)
{
    (void)call;
    TasksController *ctrl = client;
    if (!ctrl) return;
    Boolean state = XmToggleButtonGadgetGetState(widget);
    if (ctrl->ui->process_filter_toggle) {
        XmToggleButtonGadgetSetState(ctrl->ui->process_filter_toggle, state, False);
    }
    tasks_ui_update_status(ctrl->ui, state ? "Filtering to current user." : "Showing all users.");
}

static void on_help_view(Widget widget, XtPointer client, XtPointer call)
{
    (void)widget;
    (void)call;
    TasksController *ctrl = client;
    if (!ctrl) return;
    Widget dialog = XmCreateInformationDialog(tasks_ui_get_toplevel(ctrl->ui),
                                              "tasksHelpDialog", NULL, 0);
    XmString msg = XmStringCreateLocalized(
        "Help documentation will be added once features are implemented.");
    XtVaSetValues(dialog, XmNmessageString, msg, NULL);
    XmStringFree(msg);
    XtAddCallback(dialog, XmNokCallback, destroy_dialog, dialog);
    XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));
    XtManageChild(dialog);
}

static void on_about_destroy(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)call;
    TasksController *ctrl = client;
    if (!ctrl) return;
    ctrl->about_shell = NULL;
}

static void on_about(Widget widget, XtPointer client, XtPointer call)
{
    (void)widget;
    (void)call;
    TasksController *ctrl = client;
    if (!ctrl || !ctrl->ui) return;
    if (ctrl->about_shell && XtIsWidget(ctrl->about_shell)) {
        XtPopup(ctrl->about_shell, XtGrabNone);
        return;
    }
    Widget shell = NULL;
    Widget notebook = about_dialog_build(tasks_ui_get_toplevel(ctrl->ui),
                                         "about_tasks", "About CK Task Manager", &shell);
    if (!notebook || !shell) return;
    about_add_standard_pages(notebook, 1,
                             "Task Manager",
                             "CK Task Manager",
                             "Motif/CDE recreation of the classic Windows 95 Task Manager.",
                             True);
    XtAddCallback(shell, XmNdestroyCallback, on_about_destroy, ctrl);
    ctrl->about_shell = shell;
    XtPopup(shell, XtGrabNone);
}

TasksController *tasks_ctrl_create(TasksUi *ui, SessionData *session_data)
{
    if (!ui) return NULL;
    TasksController *ctrl = (TasksController *)calloc(1, sizeof(TasksController));
    if (!ctrl) return NULL;
    ctrl->ui = ui;
    ctrl->session = session_data;

    XtAddCallback(ui->menu_file_exit, XmNactivateCallback, on_file_exit, ctrl);
    XtAddCallback(ui->menu_file_connect, XmNactivateCallback, on_file_connect, ctrl);
    XtAddCallback(ui->menu_file_new_window, XmNactivateCallback, on_file_new_window, ctrl);
    XtAddCallback(ui->menu_view_refresh, XmNactivateCallback, on_view_refresh, ctrl);
    XtAddCallback(ui->menu_view_processes, XmNactivateCallback, on_view_show_tab, ctrl);
    XtAddCallback(ui->menu_view_performance, XmNactivateCallback, on_view_show_tab, ctrl);
    XtAddCallback(ui->menu_view_networking, XmNactivateCallback, on_view_show_tab, ctrl);

    XtAddCallback(ui->menu_options_always_on_top, XmNvalueChangedCallback, on_options_always_on_top, ctrl);
    XtAddCallback(ui->menu_options_update_1s, XmNactivateCallback, on_options_update, ctrl);
    XtAddCallback(ui->menu_options_update_2s, XmNactivateCallback, on_options_update, ctrl);
    XtAddCallback(ui->menu_options_update_5s, XmNactivateCallback, on_options_update, ctrl);
    XtAddCallback(ui->menu_options_filter_by_user, XmNvalueChangedCallback, on_options_filter_by_user, ctrl);

    XtAddCallback(ui->menu_help_help, XmNactivateCallback, on_help_view, ctrl);
    XtAddCallback(ui->menu_help_about, XmNactivateCallback, on_about, ctrl);

    XtVaSetValues(ui->menu_view_processes, XmNuserData, (XtPointer)(intptr_t)TASKS_TAB_PROCESSES, NULL);
    XtVaSetValues(ui->menu_view_performance, XmNuserData, (XtPointer)(intptr_t)TASKS_TAB_PERFORMANCE, NULL);
    XtVaSetValues(ui->menu_view_networking, XmNuserData, (XtPointer)(intptr_t)TASKS_TAB_NETWORKING, NULL);

    return ctrl;
}

void tasks_ctrl_destroy(TasksController *ctrl)
{
    if (!ctrl) return;
    if (ctrl->about_shell && XtIsWidget(ctrl->about_shell)) {
        XtDestroyWidget(ctrl->about_shell);
    }
    free(ctrl);
}
