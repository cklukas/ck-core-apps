#include "ck-tasks-ctrl.h"

#include "../shared/about_dialog.h"
#include <X11/Intrinsic.h>
#include <X11/Xatom.h>
#include <Xm/Xm.h>
#include <Xm/MessageB.h>
#include <Xm/List.h>
#include <Xm/ToggleBG.h>

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pwd.h>

struct TasksController {
    TasksUi *ui;
    SessionData *session;
    Widget about_shell;
    TasksProcessEntry *process_entries;
    int process_count;
    Window *app_windows;
    pid_t *app_pids;
    int *app_window_counts;
    int app_count;
    XtIntervalId refresh_timer;
    int refresh_interval_ms;
    Boolean filter_by_user;
};

static void tasks_ctrl_schedule_refresh(TasksController *ctrl);
static void tasks_ctrl_refresh_applications(TasksController *ctrl);
static void on_apps_close(Widget widget, XtPointer client, XtPointer call);
static int tasks_ctrl_filter_processes(TasksController *ctrl, TasksProcessEntry *entries, int count);
static void tasks_ctrl_apply_filter_state(TasksController *ctrl, Boolean state);

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
    TasksProcessEntry *entries = NULL;
    int count = 0;
    if (tasks_model_list_processes(&entries, &count, 64) == 0) {
        count = tasks_ctrl_filter_processes(ctrl, entries, count);
        tasks_model_free_processes(ctrl->process_entries, ctrl->process_count);
        ctrl->process_entries = entries;
        ctrl->process_count = count;
        tasks_ui_set_processes(ctrl->ui, entries, count);
        TasksSystemStats stats;
        if (tasks_model_get_system_stats(&stats) == 0) {
            tasks_ui_update_system_stats(ctrl->ui, &stats);
        }
        tasks_ctrl_refresh_applications(ctrl);
        tasks_ui_update_status(ctrl->ui, "Process list refreshed.");
    } else {
        tasks_ui_update_status(ctrl->ui, "Unable to refresh process list.");
    }
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
    int interval_ms = 0;
    XtPointer data = NULL;
    XtVaGetValues(widget, XmNuserData, &data, NULL);
    interval_ms = (int)(intptr_t)data;
    if (interval_ms <= 0) interval_ms = 2000;
    ctrl->refresh_interval_ms = interval_ms;
    tasks_ui_update_status(ctrl->ui, "Update interval changed.");
    tasks_ctrl_schedule_refresh(ctrl);
}

static void on_options_filter_by_user(Widget widget, XtPointer client, XtPointer call)
{
    (void)call;
    TasksController *ctrl = client;
    if (!ctrl) return;
    Boolean state = XmToggleButtonGadgetGetState(widget);
    tasks_ctrl_apply_filter_state(ctrl, state);
    on_view_refresh(NULL, ctrl, NULL);
    tasks_ui_update_status(ctrl->ui, state ? "Filtering to current user." : "Showing all users.");
}

static void on_process_filter_toggle(Widget widget, XtPointer client, XtPointer call)
{
    (void)call;
    TasksController *ctrl = client;
    if (!ctrl) return;
    Boolean state = XmToggleButtonGadgetGetState(widget);
    tasks_ctrl_apply_filter_state(ctrl, state);
    on_view_refresh(NULL, ctrl, NULL);
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

static int get_window_pid(Display *dpy, Window window, pid_t *out_pid)
{
    if (!dpy || !out_pid) return -1;
    Atom pid_atom = XInternAtom(dpy, "_NET_WM_PID", True);
    if (pid_atom == None) return -1;
    Atom type = None;
    int format = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;
    int status = XGetWindowProperty(dpy, window, pid_atom, 0, 1, False, XA_CARDINAL,
                                    &type, &format, &nitems, &bytes_after, &data);
    if (status != Success || !data) return -1;
    pid_t pid = (pid_t)(*(unsigned long *)data);
    XFree(data);
    *out_pid = pid;
    return 0;
}

static void get_window_title(Display *dpy, Window window, char *buffer, size_t len)
{
    if (!buffer || len == 0) return;
    buffer[0] = '\0';
    Atom net_name = XInternAtom(dpy, "_NET_WM_NAME", True);
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", True);
    if (net_name != None && utf8 != None) {
        Atom type = None;
        int format = 0;
        unsigned long nitems = 0;
        unsigned long bytes_after = 0;
        unsigned char *data = NULL;
        int status = XGetWindowProperty(dpy, window, net_name, 0, 128, False, utf8,
                                        &type, &format, &nitems, &bytes_after, &data);
        if (status == Success && data) {
            snprintf(buffer, len, "%s", (char *)data);
            XFree(data);
            return;
        }
    }
    char *name = NULL;
    if (XFetchName(dpy, window, &name) > 0 && name) {
        snprintf(buffer, len, "%s", name);
        XFree(name);
    }
}

static int query_client_list(Display *dpy, Window root, Window **out_list, unsigned long *out_count)
{
    if (!dpy || !out_list || !out_count) return -1;
    Atom client_list = XInternAtom(dpy, "_NET_CLIENT_LIST", True);
    if (client_list != None) {
        Atom type = None;
        int format = 0;
        unsigned long nitems = 0;
        unsigned long bytes_after = 0;
        unsigned char *data = NULL;
        int status = XGetWindowProperty(dpy, root, client_list, 0, 4096, False, XA_WINDOW,
                                        &type, &format, &nitems, &bytes_after, &data);
        if (status == Success && data) {
            *out_list = (Window *)data;
            *out_count = nitems;
            return 0;
        }
    }

    Window root_ret = 0;
    Window parent = 0;
    Window *children = NULL;
    unsigned int count = 0;
    if (!XQueryTree(dpy, root, &root_ret, &parent, &children, &count)) {
        return -1;
    }
    *out_list = children;
    *out_count = count;
    return 0;
}

static void tasks_ctrl_refresh_applications(TasksController *ctrl)
{
    if (!ctrl || !ctrl->ui || !ctrl->ui->apps_list) return;
    Display *dpy = XtDisplay(tasks_ui_get_toplevel(ctrl->ui));
    Window root = DefaultRootWindow(dpy);
    Window *windows = NULL;
    unsigned long count = 0;
    if (query_client_list(dpy, root, &windows, &count) != 0) return;

    if (ctrl->app_windows) {
        free(ctrl->app_windows);
        ctrl->app_windows = NULL;
    }
    if (ctrl->app_pids) {
        free(ctrl->app_pids);
        ctrl->app_pids = NULL;
    }
    if (ctrl->app_window_counts) {
        free(ctrl->app_window_counts);
        ctrl->app_window_counts = NULL;
    }
    ctrl->app_count = 0;

    XmString *items = NULL;
    int item_count = 0;

    for (unsigned long i = 0; i < count; ++i) {
        pid_t pid = 0;
        if (get_window_pid(dpy, windows[i], &pid) != 0) continue;

        int idx = -1;
        for (int j = 0; j < ctrl->app_count; ++j) {
            if (ctrl->app_pids[j] == pid) {
                idx = j;
                break;
            }
        }
        if (idx < 0) {
            Window *new_windows = (Window *)realloc(ctrl->app_windows, sizeof(Window) * (ctrl->app_count + 1));
            pid_t *new_pids = (pid_t *)realloc(ctrl->app_pids, sizeof(pid_t) * (ctrl->app_count + 1));
            int *new_counts = (int *)realloc(ctrl->app_window_counts, sizeof(int) * (ctrl->app_count + 1));
            XmString *new_items = (XmString *)realloc(items, sizeof(XmString) * (item_count + 1));
            if (!new_windows || !new_pids || !new_counts || !new_items) {
                free(new_windows);
                free(new_pids);
                free(new_counts);
                break;
            }
            ctrl->app_windows = new_windows;
            ctrl->app_pids = new_pids;
            ctrl->app_window_counts = new_counts;
            items = new_items;
            char title[128];
            get_window_title(dpy, windows[i], title, sizeof(title));
            if (title[0] == '\0') snprintf(title, sizeof(title), "PID %d", (int)pid);
            char label[160];
            snprintf(label, sizeof(label), "%s (PID %d)", title, (int)pid);
            items[item_count] = XmStringCreateLocalized(label);
            ctrl->app_windows[item_count] = windows[i];
            ctrl->app_pids[item_count] = pid;
            ctrl->app_window_counts[item_count] = 1;
            ctrl->app_count = item_count + 1;
            item_count++;
        } else {
            ctrl->app_window_counts[idx] += 1;
        }
    }

    if (windows) XFree(windows);
    for (int i = 0; i < item_count; ++i) {
        if (ctrl->app_window_counts[i] > 1) {
            char label[180];
            snprintf(label, sizeof(label), " (%d windows)", ctrl->app_window_counts[i]);
            XmString suffix = XmStringCreateLocalized(label);
            XmString combined = XmStringConcat(items[i], suffix);
            XmStringFree(items[i]);
            XmStringFree(suffix);
            items[i] = combined;
        }
    }
    tasks_ui_set_applications(ctrl->ui, items, item_count);
    for (int i = 0; i < item_count; ++i) {
        XmStringFree(items[i]);
    }
    free(items);
}

static void on_apps_close(Widget widget, XtPointer client, XtPointer call)
{
    (void)widget;
    (void)call;
    TasksController *ctrl = client;
    if (!ctrl || !ctrl->ui || !ctrl->ui->apps_list) return;
    int *positions = NULL;
    int count = 0;
    if (!XmListGetSelectedPos(ctrl->ui->apps_list, &positions, &count) || count <= 0) {
        tasks_ui_update_status(ctrl->ui, "Select an application to close.");
        return;
    }
    int index = positions[0] - 1;
    XtFree((char *)positions);
    if (index < 0 || index >= ctrl->app_count) return;
    Window window = ctrl->app_windows[index];
    Display *dpy = XtDisplay(tasks_ui_get_toplevel(ctrl->ui));
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    Atom wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    if (wm_delete != None) {
        XEvent event;
        memset(&event, 0, sizeof(event));
        event.xclient.type = ClientMessage;
        event.xclient.window = window;
        event.xclient.message_type = wm_protocols;
        event.xclient.format = 32;
        event.xclient.data.l[0] = (long)wm_delete;
        event.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, window, False, NoEventMask, &event);
        XFlush(dpy);
        tasks_ui_update_status(ctrl->ui, "Sent close request to window.");
    }
}

static void on_refresh_timer(XtPointer client_data, XtIntervalId *id)
{
    (void)id;
    TasksController *ctrl = client_data;
    if (!ctrl) return;
    ctrl->refresh_timer = 0;
    on_view_refresh(NULL, ctrl, NULL);
    tasks_ctrl_schedule_refresh(ctrl);
}

static void tasks_ctrl_schedule_refresh(TasksController *ctrl)
{
    if (!ctrl) return;
    XtAppContext app = tasks_ui_get_app_context(ctrl->ui);
    if (!app) return;
    if (ctrl->refresh_timer) {
        XtRemoveTimeOut(ctrl->refresh_timer);
        ctrl->refresh_timer = 0;
    }
    int interval = ctrl->refresh_interval_ms;
    if (interval <= 0) interval = 2000;
    ctrl->refresh_timer = XtAppAddTimeOut(app, (unsigned long)interval, on_refresh_timer, ctrl);
}

static int tasks_ctrl_filter_processes(TasksController *ctrl, TasksProcessEntry *entries, int count)
{
    if (!ctrl || !entries || count <= 0 || !ctrl->filter_by_user) return count;
    uid_t uid = getuid();
    struct passwd *pw = getpwuid(uid);
    char uid_buffer[16];
    snprintf(uid_buffer, sizeof(uid_buffer), "%d", (int)uid);

    const char *user_name = (pw && pw->pw_name) ? pw->pw_name : "";
    int write_index = 0;
    for (int i = 0; i < count; ++i) {
        const char *entry_user = entries[i].user;
        if (!entry_user || entry_user[0] == '\0') continue;
        if ((user_name[0] && strcmp(entry_user, user_name) == 0) ||
            strcmp(entry_user, uid_buffer) == 0) {
            if (write_index != i) {
                entries[write_index] = entries[i];
            }
            write_index++;
        }
    }
    return write_index;
}

static void tasks_ctrl_apply_filter_state(TasksController *ctrl, Boolean state)
{
    if (!ctrl) return;
    ctrl->filter_by_user = state;
    if (ctrl->ui->process_filter_toggle) {
        XmToggleButtonGadgetSetState(ctrl->ui->process_filter_toggle, state, False);
    }
    if (ctrl->ui->menu_options_filter_by_user) {
        XmToggleButtonGadgetSetState(ctrl->ui->menu_options_filter_by_user, state, False);
    }
}

TasksController *tasks_ctrl_create(TasksUi *ui, SessionData *session_data)
{
    if (!ui) return NULL;
    TasksController *ctrl = (TasksController *)calloc(1, sizeof(TasksController));
    if (!ctrl) return NULL;
    ctrl->ui = ui;
    ctrl->session = session_data;
    ctrl->refresh_interval_ms = 2000;

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
    if (ui->process_filter_toggle) {
        XtAddCallback(ui->process_filter_toggle, XmNvalueChangedCallback, on_process_filter_toggle, ctrl);
    }

    XtAddCallback(ui->menu_help_help, XmNactivateCallback, on_help_view, ctrl);
    XtAddCallback(ui->menu_help_about, XmNactivateCallback, on_about, ctrl);
    if (ui->apps_close_button) {
        XtAddCallback(ui->apps_close_button, XmNactivateCallback, on_apps_close, ctrl);
    }

    XtVaSetValues(ui->menu_view_processes, XmNuserData, (XtPointer)(intptr_t)TASKS_TAB_PROCESSES, NULL);
    XtVaSetValues(ui->menu_view_performance, XmNuserData, (XtPointer)(intptr_t)TASKS_TAB_PERFORMANCE, NULL);
    XtVaSetValues(ui->menu_view_networking, XmNuserData, (XtPointer)(intptr_t)TASKS_TAB_NETWORKING, NULL);
    XtVaSetValues(ui->menu_options_update_1s, XmNuserData, (XtPointer)(intptr_t)1000, NULL);
    XtVaSetValues(ui->menu_options_update_2s, XmNuserData, (XtPointer)(intptr_t)2000, NULL);
    XtVaSetValues(ui->menu_options_update_5s, XmNuserData, (XtPointer)(intptr_t)5000, NULL);

    on_view_refresh(NULL, ctrl, NULL);
    tasks_ctrl_schedule_refresh(ctrl);

    return ctrl;
}

void tasks_ctrl_destroy(TasksController *ctrl)
{
    if (!ctrl) return;
    if (ctrl->refresh_timer) {
        XtRemoveTimeOut(ctrl->refresh_timer);
        ctrl->refresh_timer = 0;
    }
    if (ctrl->about_shell && XtIsWidget(ctrl->about_shell)) {
        XtDestroyWidget(ctrl->about_shell);
    }
    tasks_model_free_processes(ctrl->process_entries, ctrl->process_count);
    free(ctrl->app_windows);
    free(ctrl->app_pids);
    free(ctrl->app_window_counts);
    free(ctrl);
}
