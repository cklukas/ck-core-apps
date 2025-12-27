#include "ck-tasks-ctrl.h"

#include "../shared/about_dialog.h"
#include <X11/Intrinsic.h>
#include <X11/Xatom.h>
#include <Xm/Xm.h>
#include <Xm/MessageB.h>
#include <Xm/List.h>
#include <Xm/ScrollBar.h>
#include <Xm/TextF.h>
#include <Xm/ToggleBG.h>

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <pwd.h>
#include <ctype.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

struct TasksController {
    TasksUi *ui;
    SessionData *session;
    Widget about_shell;
    TasksProcessEntry *process_entries;
    int process_count;
    TasksApplicationEntry *applications;
    int applications_count;
    int selected_application;
    TasksUserEntry *user_sessions;
    int user_session_count;
    int process_total_count;
    XtIntervalId refresh_timer;
    int refresh_interval_ms;
    Boolean filter_by_user;
    char search_text[128];
    char apps_search_text[128];
    int virtual_row_start;
};

typedef struct {
    Window *items;
    unsigned long count;
    unsigned long capacity;
} WindowCollection;

static void window_collection_init(WindowCollection *collection);
static int window_collection_add_unique(WindowCollection *collection, Window window);
static void window_collection_add_from_atom(Display *dpy, Window root, const char *atom_name, WindowCollection *collection);
static void window_collection_add_recursive(Display *dpy, Window window, WindowCollection *collection);
static int query_client_list(Display *dpy, Window root, Window **out_list, unsigned long *out_count);

static void tasks_ctrl_schedule_refresh(TasksController *ctrl);
static void tasks_ctrl_refresh_applications(TasksController *ctrl);
static void tasks_ctrl_refresh_users(TasksController *ctrl);
static void on_apps_close(Widget widget, XtPointer client, XtPointer call);
static int tasks_ctrl_filter_processes(TasksController *ctrl, TasksProcessEntry *entries, int count);
static void tasks_ctrl_apply_filter_state(TasksController *ctrl, Boolean state);
static void tasks_ctrl_set_search_text(TasksController *ctrl, const char *text);
static int tasks_ctrl_entry_matches_search(TasksController *ctrl, const TasksProcessEntry *entry);
static void on_process_search_changed(Widget widget, XtPointer client, XtPointer call);
static int contains_ignore_case(const char *text, const char *pattern);
static void tasks_ctrl_set_virtual_window(TasksController *ctrl, int start);
static void tasks_ctrl_update_virtual_scrollbar(TasksController *ctrl);
static void on_process_scroll(Widget widget, XtPointer client, XtPointer call);
static int get_window_command(Display *dpy, Window window, char *out, size_t out_len);
static pid_t tasks_ctrl_find_pid_by_command(TasksController *ctrl, const char *command);
static void get_window_wm_class(Display *dpy, Window window, char *out, size_t out_len);
static int tasks_ctrl_application_matches_search(TasksController *ctrl, const TasksApplicationEntry *entry);
static void on_apps_search_changed(Widget widget, XtPointer client, XtPointer call);

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
    if (tasks_model_list_processes(&entries, &count) == 0) {
        ctrl->process_total_count = count;
        count = tasks_ctrl_filter_processes(ctrl, entries, count);
        tasks_model_free_processes(ctrl->process_entries, ctrl->process_count);
        ctrl->process_entries = entries;
        ctrl->process_count = count;
        tasks_ui_set_processes(ctrl->ui, entries, count);
        tasks_ctrl_set_virtual_window(ctrl, ctrl->virtual_row_start);
        tasks_ui_update_process_count(ctrl->ui, ctrl->process_total_count);
        TasksSystemStats stats;
        if (tasks_model_get_system_stats(&stats) == 0) {
            tasks_ui_update_system_stats(ctrl->ui, &stats);
        }
        tasks_ctrl_refresh_applications(ctrl);
        tasks_ctrl_refresh_users(ctrl);
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

static void on_process_search_changed(Widget widget, XtPointer client, XtPointer call)
{
    (void)call;
    TasksController *ctrl = client;
    if (!ctrl || !widget) return;
    char *value = XmTextFieldGetString(widget);
    tasks_ctrl_set_search_text(ctrl, value);
    XtFree(value);
    on_view_refresh(NULL, ctrl, NULL);
    tasks_ui_update_status(ctrl->ui, "Process search filter applied.");
}

static void on_apps_search_changed(Widget widget, XtPointer client, XtPointer call)
{
    (void)call;
    TasksController *ctrl = client;
    if (!ctrl || !widget) return;
    char *value = XmTextFieldGetString(widget);
    if (!value) return;
    size_t len = strlen(value);
    if (len >= sizeof(ctrl->apps_search_text)) {
        len = sizeof(ctrl->apps_search_text) - 1;
    }
    memcpy(ctrl->apps_search_text, value, len);
    ctrl->apps_search_text[len] = '\0';
    XtFree(value);
    tasks_ctrl_refresh_applications(ctrl);
    tasks_ui_update_status(ctrl->ui, "Application filter applied.");
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
                                         "about_tasks", "About Task Manager", &shell);
    if (!notebook || !shell) return;
    about_add_standard_pages(notebook, 1,
                             "Task Manager",
                             "Task Manager",
                             "Task Manager for CK-Core",
                             True);
    XtAddCallback(shell, XmNdestroyCallback, on_about_destroy, ctrl);
    ctrl->about_shell = shell;
    XtPopup(shell, XtGrabNone);
}

static int get_window_pid(Display *dpy, Window window, pid_t *out_pid)
{
    if (!dpy || !out_pid) return -1;
    const char *pid_atoms[] = {
        "_NET_WM_PID",
        "_DT_WM_PID",
        "DtWM_PID",
        NULL
    };
    for (const char **atom_name = pid_atoms; atom_name && *atom_name; ++atom_name) {
        Atom pid_atom = XInternAtom(dpy, *atom_name, True);
        if (pid_atom == None) continue;
        Atom type = None;
        int format = 0;
        unsigned long nitems = 0;
        unsigned long bytes_after = 0;
        unsigned char *data = NULL;
        int status = XGetWindowProperty(dpy, window, pid_atom, 0, 1, False, XA_CARDINAL,
                                        &type, &format, &nitems, &bytes_after, &data);
        if (status == Success && data && nitems > 0) {
            pid_t pid = (pid_t)(*(unsigned long *)data);
            XFree(data);
            *out_pid = pid;
            return 0;
        }
        if (data) XFree(data);
    }
    return -1;
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

static void get_window_wm_class(Display *dpy, Window window, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';
    XClassHint hint;
    if (XGetClassHint(dpy, window, &hint) == 0) return;
    const char *res_name = hint.res_name ? hint.res_name : "";
    const char *res_class = hint.res_class ? hint.res_class : "";
    if (res_name[0] && res_class[0]) {
        snprintf(out, out_len, "%s/%s", res_name, res_class);
    } else if (res_class[0]) {
        snprintf(out, out_len, "%s", res_class);
    } else if (res_name[0]) {
        snprintf(out, out_len, "%s", res_name);
    }
    if (hint.res_name) XFree(hint.res_name);
    if (hint.res_class) XFree(hint.res_class);
}

static int get_window_command(Display *dpy, Window window, char *out, size_t out_len)
{
    if (!dpy || !out || out_len == 0) return -1;
    char **argv = NULL;
    int argc = 0;
    Status status = XGetCommand(dpy, window, &argv, &argc);
    if (status == 0 || !argv || argc <= 0) {
        if (argv) XFreeStringList(argv);
        return -1;
    }
    size_t pos = 0;
    for (int i = 0; i < argc && argv && argv[i]; ++i) {
        size_t chunk = strlen(argv[i]);
        if (pos + chunk >= out_len) {
            chunk = out_len - pos - 1;
        }
        if (chunk > 0) {
            memcpy(out + pos, argv[i], chunk);
            pos += chunk;
        }
        if (i < argc - 1 && pos < out_len - 1) {
            out[pos++] = ' ';
        }
        if (pos >= out_len - 1) break;
    }
    if (pos >= out_len) pos = out_len - 1;
    out[pos] = '\0';
    if (argv) XFreeStringList(argv);
    return pos > 0 ? 0 : -1;
}

static int tasks_ctrl_application_matches_search(TasksController *ctrl, const TasksApplicationEntry *entry)
{
    if (!ctrl || !entry) return 1;
    if (!ctrl->apps_search_text[0]) return 1;
    char pid_buffer[16];
    pid_buffer[0] = '\0';
    if (entry->pid_known) {
        snprintf(pid_buffer, sizeof(pid_buffer), "%d", (int)entry->pid);
    }
    return contains_ignore_case(entry->title, ctrl->apps_search_text) ||
           contains_ignore_case(entry->command, ctrl->apps_search_text) ||
           contains_ignore_case(entry->wm_class, ctrl->apps_search_text) ||
           contains_ignore_case(pid_buffer, ctrl->apps_search_text);
}

static pid_t tasks_ctrl_find_pid_by_command(TasksController *ctrl, const char *command)
{
    if (!ctrl || !command || !command[0] || ctrl->process_count <= 0 || !ctrl->process_entries) return -1;
    const char *p = command;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (!*p) return -1;
    char token[PATH_MAX];
    memset(token, 0, sizeof(token));
    size_t len = 0;
    if (*p == '"' || *p == '\'') {
        char quote = *p++;
        while (*p && *p != quote && len + 1 < sizeof(token)) {
            token[len++] = *p++;
        }
        if (*p == quote) ++p;
    } else {
        while (*p && !isspace((unsigned char)*p) && len + 1 < sizeof(token)) {
            token[len++] = *p++;
        }
    }
    token[len] = '\0';
    if (!token[0]) return -1;
    const char *slash = strrchr(token, '/');
    const char *base = slash ? slash + 1 : token;
    for (int i = 0; i < ctrl->process_count; ++i) {
        const TasksProcessEntry *entry = &ctrl->process_entries[i];
        if (!entry) continue;
        if (entry->command[0]) {
            if (strcasecmp(entry->command, token) == 0 ||
                (base && strcasecmp(entry->command, base) == 0) ||
                strcasestr(entry->command, token) ||
                (base && strcasestr(entry->command, base))) {
                return entry->pid;
            }
        }
        if (entry->name[0]) {
            if (strcasecmp(entry->name, base) == 0 ||
                (base && strcasestr(base, entry->name))) {
                return entry->pid;
            }
            if (command && strcasestr(command, entry->name)) {
                return entry->pid;
            }
        }
    }
    return -1;
}

static void window_collection_init(WindowCollection *collection)
{
    if (!collection) return;
    collection->items = NULL;
    collection->count = 0;
    collection->capacity = 0;
}

static int window_collection_add_unique(WindowCollection *collection, Window window)
{
    if (!collection) return -1;
    for (unsigned long i = 0; i < collection->count; ++i) {
        if (collection->items[i] == window) return 0;
    }
    if (collection->count >= collection->capacity) {
        unsigned long new_capacity = collection->capacity > 0 ? collection->capacity * 2 : 32;
        Window *resized = (Window *)realloc(collection->items, new_capacity * sizeof(Window));
        if (!resized) return -1;
        collection->items = resized;
        collection->capacity = new_capacity;
    }
    collection->items[collection->count++] = window;
    return 0;
}

static void window_collection_add_from_atom(Display *dpy, Window root, const char *atom_name, WindowCollection *collection)
{
    if (!dpy || !collection || !atom_name) return;
    Atom atom = XInternAtom(dpy, atom_name, True);
    if (atom == None) return;
    Atom actual_type = None;
    int format = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;
    int status = XGetWindowProperty(dpy, root, atom, 0, LONG_MAX, False, XA_WINDOW,
                                    &actual_type, &format, &nitems, &bytes_after, &data);
    if (status == Success && data && nitems > 0) {
        Window *windows = (Window *)data;
        for (unsigned long i = 0; i < nitems; ++i) {
            window_collection_add_unique(collection, windows[i]);
        }
    }
    if (data) XFree(data);
}

static void window_collection_add_recursive(Display *dpy, Window window, WindowCollection *collection)
{
    if (!dpy || !collection) return;
    Window root_return = 0;
    Window parent_return = 0;
    Window *children = NULL;
    unsigned int nchildren = 0;
    if (!XQueryTree(dpy, window, &root_return, &parent_return, &children, &nchildren)) return;
    for (unsigned int i = 0; i < nchildren; ++i) {
        Window child = children[i];
        XWindowAttributes attrs;
        if (XGetWindowAttributes(dpy, child, &attrs) == 0) {
            window_collection_add_recursive(dpy, child, collection);
            continue;
        }
        if (attrs.class != InputOutput) {
            window_collection_add_recursive(dpy, child, collection);
            continue;
        }
        if (!attrs.override_redirect) {
            window_collection_add_unique(collection, child);
        }
        window_collection_add_recursive(dpy, child, collection);
    }
    if (children) XFree(children);
}

static int query_client_list(Display *dpy, Window root, Window **out_list, unsigned long *out_count)
{
    if (!dpy || !out_list || !out_count) return -1;
    WindowCollection collection;
    window_collection_init(&collection);

    window_collection_add_from_atom(dpy, root, "_NET_CLIENT_LIST_STACKING", &collection);
    window_collection_add_from_atom(dpy, root, "_NET_CLIENT_LIST", &collection);
    window_collection_add_recursive(dpy, root, &collection);

    if (collection.count == 0) {
        Window root_ret = 0;
        Window parent_ret = 0;
        Window *children = NULL;
        unsigned int count = 0;
        if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &count) && children) {
            for (unsigned int i = 0; i < count; ++i) {
                window_collection_add_unique(&collection, children[i]);
            }
        }
        if (children) XFree(children);
    }

    *out_list = collection.items;
    *out_count = collection.count;
    return 0;
}

static void tasks_ctrl_refresh_applications(TasksController *ctrl)
{
    if (!ctrl || !ctrl->ui || !ctrl->ui->apps_table) return;
    Display *dpy = XtDisplay(tasks_ui_get_toplevel(ctrl->ui));
    Window root = DefaultRootWindow(dpy);
    Window *windows = NULL;
    unsigned long count = 0;
    if (query_client_list(dpy, root, &windows, &count) != 0) return;

    free(ctrl->applications);
    ctrl->applications = NULL;
    ctrl->applications_count = 0;
    ctrl->selected_application = -1;

    TasksApplicationEntry *entries = NULL;
    int entry_count = 0;
    int entry_capacity = 0;

    for (unsigned long i = 0; i < count; ++i) {
        char title[128] = {0};
        char command[256] = {0};
        char wm_class[128] = {0};
        get_window_title(dpy, windows[i], title, sizeof(title));
        get_window_command(dpy, windows[i], command, sizeof(command));
        get_window_wm_class(dpy, windows[i], wm_class, sizeof(wm_class));

        pid_t pid = 0;
        int pid_known = (get_window_pid(dpy, windows[i], &pid) == 0) ? 1 : 0;
        if (!pid_known && command[0]) {
            pid_t matched = tasks_ctrl_find_pid_by_command(ctrl, command);
            if (matched > 0) {
                pid = matched;
                pid_known = 1;
            }
        }

        pid_t key = pid_known ? pid : (pid_t)(-((int)i + 1));
        int idx = -1;
        for (int j = 0; j < entry_count; ++j) {
            if (entries[j].pid == key) {
                idx = j;
                break;
            }
        }

        if (idx >= 0) {
            entries[idx].window_count += 1;
            continue;
        }

        if (entry_count >= entry_capacity) {
            int new_capacity = entry_capacity ? entry_capacity * 2 : 32;
            TasksApplicationEntry *resized = (TasksApplicationEntry *)realloc(entries, sizeof(TasksApplicationEntry) * new_capacity);
            if (!resized) break;
            entries = resized;
            entry_capacity = new_capacity;
        }

        TasksApplicationEntry *entry = &entries[entry_count];
        memset(entry, 0, sizeof(*entry));
        entry->window = windows[i];
        entry->pid = key;
        entry->pid_known = pid_known;
        entry->window_count = 1;
        snprintf(entry->title, sizeof(entry->title), "%s", title[0] ? title : "(unnamed)");
        snprintf(entry->command, sizeof(entry->command), "%s", command);
        snprintf(entry->wm_class, sizeof(entry->wm_class), "%s", wm_class);
        entry_count++;
    }

    free(windows);

    int write_index = 0;
    for (int i = 0; i < entry_count; ++i) {
        if (!tasks_ctrl_application_matches_search(ctrl, &entries[i])) continue;
        if (write_index != i) {
            entries[write_index] = entries[i];
        }
        write_index++;
    }
    entry_count = write_index;

    ctrl->applications = entries;
    ctrl->applications_count = entry_count;
    tasks_ui_set_applications_table(ctrl->ui, ctrl->applications, ctrl->applications_count);
}

static void tasks_ctrl_refresh_users(TasksController *ctrl)
{
    if (!ctrl || !ctrl->ui) return;
    if (ctrl->ui->users_updates_paused) return;
    TasksUserEntry *entries = NULL;
    int count = 0;
    if (tasks_model_list_users(&entries, &count) != 0) {
        return;
    }
    tasks_model_free_users(ctrl->user_sessions, ctrl->user_session_count);
    ctrl->user_sessions = entries;
    ctrl->user_session_count = count;
    tasks_ui_set_users_table(ctrl->ui, ctrl->user_sessions, ctrl->user_session_count);
}

static void on_apps_close(Widget widget, XtPointer client, XtPointer call)
{
    (void)widget;
    (void)call;
    TasksController *ctrl = client;
    if (!ctrl || !ctrl->ui) return;
    int index = ctrl->selected_application;
    if (index < 0 || index >= ctrl->applications_count || !ctrl->applications) {
        tasks_ui_update_status(ctrl->ui, "Select an application row to close.");
        return;
    }
    Window window = ctrl->applications[index].window;
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

static void tasks_ctrl_update_virtual_scrollbar(TasksController *ctrl)
{
    if (!ctrl || !ctrl->ui || !ctrl->ui->process_scrollbar) return;
    Widget scrollbar = ctrl->ui->process_scrollbar;
    int total = ctrl->process_count;
    int page = tasks_ui_get_process_row_page_size();
    if (page <= 0) page = 1;
    int slider_size = total > 0 ? (total < page ? total : page) : 1;
    int max_start = total > page ? total - page : 0;
    if (max_start < 0) max_start = 0;
    int maximum = max_start + slider_size;
    int max_value = maximum - slider_size;
    if (max_value < 0) max_value = 0;
    int value = ctrl->virtual_row_start;
    if (value < 0) value = 0;
    if (value > max_value) value = max_value;
    XtVaSetValues(scrollbar,
                  XmNminimum, 0,
                  XmNmaximum, maximum,
                  NULL);
    XmScrollBarSetValues(scrollbar, value, slider_size, 1, page, False);
}

static void tasks_ctrl_set_virtual_window(TasksController *ctrl, int start)
{
    if (!ctrl) return;
    int page = tasks_ui_get_process_row_page_size();
    if (page <= 0) page = 1;
    int total = ctrl->process_count;
    int max_start = total > page ? total - page : 0;
    if (max_start < 0) max_start = 0;
    if (start < 0) start = 0;
    if (start > max_start) start = max_start;
    ctrl->virtual_row_start = start;
    tasks_ui_set_process_row_window(start);
    tasks_ctrl_update_virtual_scrollbar(ctrl);
}

void tasks_ctrl_handle_viewport_change(TasksController *ctrl)
{
    if (!ctrl) return;
    tasks_ctrl_set_virtual_window(ctrl, ctrl->virtual_row_start);
}

void tasks_ctrl_set_selected_application(TasksController *ctrl, int index)
{
    if (!ctrl) return;
    if (index < 0 || index >= ctrl->applications_count) {
        ctrl->selected_application = -1;
        return;
    }
    ctrl->selected_application = index;
}

static void on_process_scroll(Widget widget, XtPointer client, XtPointer call)
{
    (void)widget;
    TasksController *ctrl = client;
    if (!ctrl || !call) return;
    XmScrollBarCallbackStruct *cb = (XmScrollBarCallbackStruct *)call;
    tasks_ctrl_set_virtual_window(ctrl, cb->value);
}

static int tasks_ctrl_filter_processes(TasksController *ctrl, TasksProcessEntry *entries, int count)
{
    if (!ctrl || !entries || count <= 0) return 0;
    uid_t uid = getuid();
    struct passwd *pw = getpwuid(uid);
    char uid_buffer[16];
    snprintf(uid_buffer, sizeof(uid_buffer), "%d", (int)uid);

    const char *user_name = (pw && pw->pw_name) ? pw->pw_name : "";
    int write_index = 0;
    for (int i = 0; i < count; ++i) {
        const char *entry_user = entries[i].user;
        if (ctrl->filter_by_user) {
            if (!entry_user || entry_user[0] == '\0') continue;
            if (!((user_name[0] && strcmp(entry_user, user_name) == 0) ||
                  strcmp(entry_user, uid_buffer) == 0)) {
                continue;
            }
        }
        if (!tasks_ctrl_entry_matches_search(ctrl, &entries[i])) {
            continue;
        }
        if (write_index != i) {
            entries[write_index] = entries[i];
        }
        write_index++;
    }
    return write_index;
}

static int contains_ignore_case(const char *text, const char *pattern)
{
    if (!text || !pattern || !pattern[0]) return 0;
    size_t pattern_len = strlen(pattern);
    for (const char *base = text; *base; ++base) {
        size_t i = 0;
        for (; i < pattern_len; ++i) {
            char tc = base[i];
            char pc = pattern[i];
            if (tc == '\0') break;
            if (tolower((unsigned char)tc) != tolower((unsigned char)pc)) break;
        }
        if (i == pattern_len) return 1;
    }
    return 0;
}

static int tasks_ctrl_entry_matches_search(TasksController *ctrl, const TasksProcessEntry *entry)
{
    if (!ctrl || !entry) return 1;
    if (!ctrl->search_text[0]) return 1;
    char pid_string[16];
    snprintf(pid_string, sizeof(pid_string), "%d", (int)entry->pid);
    return contains_ignore_case(entry->name, ctrl->search_text) ||
           contains_ignore_case(entry->user, ctrl->search_text) ||
           contains_ignore_case(pid_string, ctrl->search_text);
}

static void tasks_ctrl_set_search_text(TasksController *ctrl, const char *text)
{
    if (!ctrl) return;
    if (!text) text = "";
    size_t len = strlen(text);
    if (len >= sizeof(ctrl->search_text)) {
        len = sizeof(ctrl->search_text) - 1;
    }
    memcpy(ctrl->search_text, text, len);
    ctrl->search_text[len] = '\0';
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
    ui->controller = ctrl;
    ctrl->session = session_data;
    ctrl->refresh_interval_ms = 2000;
    ctrl->virtual_row_start = 0;
    ctrl->selected_application = -1;
    ctrl->process_total_count = 0;
    ctrl->user_sessions = NULL;
    ctrl->user_session_count = 0;

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
    if (ui->process_search_field) {
        XtAddCallback(ui->process_search_field, XmNvalueChangedCallback, on_process_search_changed, ctrl);
    }
    if (ui->process_scrollbar) {
        XtAddCallback(ui->process_scrollbar, XmNvalueChangedCallback, on_process_scroll, ctrl);
        XtAddCallback(ui->process_scrollbar, XmNdragCallback, on_process_scroll, ctrl);
    }

    XtAddCallback(ui->menu_help_help, XmNactivateCallback, on_help_view, ctrl);
    XtAddCallback(ui->menu_help_about, XmNactivateCallback, on_about, ctrl);
    if (ui->apps_search_field) {
        XtAddCallback(ui->apps_search_field, XmNvalueChangedCallback, on_apps_search_changed, ctrl);
    }
    if (ui->apps_close_button) {
        XtAddCallback(ui->apps_close_button, XmNactivateCallback, on_apps_close, ctrl);
    }

    XtVaSetValues(ui->menu_view_processes, XmNuserData, (XtPointer)(intptr_t)TASKS_TAB_PROCESSES, NULL);
    XtVaSetValues(ui->menu_view_performance, XmNuserData, (XtPointer)(intptr_t)TASKS_TAB_PERFORMANCE, NULL);
    XtVaSetValues(ui->menu_view_networking, XmNuserData, (XtPointer)(intptr_t)TASKS_TAB_NETWORKING, NULL);
    XtVaSetValues(ui->menu_options_update_1s, XmNuserData, (XtPointer)(intptr_t)1000, NULL);
    XtVaSetValues(ui->menu_options_update_2s, XmNuserData, (XtPointer)(intptr_t)2000, NULL);
    XtVaSetValues(ui->menu_options_update_5s, XmNuserData, (XtPointer)(intptr_t)5000, NULL);

    tasks_ctrl_apply_filter_state(ctrl, True);
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
    tasks_model_free_users(ctrl->user_sessions, ctrl->user_session_count);
    free(ctrl->applications);
    if (ctrl->ui) {
        ctrl->ui->controller = NULL;
    }
    free(ctrl);
}
