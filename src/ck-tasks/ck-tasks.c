/*
 * TODO: Recreate Windows 95 Task Manager layout and behavior in Motif/CDE
 * 1. Build the window structure with chrome, menu bar, notebook, and placeholders. (done)
 * 2. Process tab: process list with filtering, kill/refresh controls, user checkbox, dtsession hooks.
 * 3. Performance tab: overall/divided CPU history, memory details, summary meters.
 * 4. Networking tab: adapters, connections, throughput, flow charts.
 * 5. Menu wiring: connect every action to controller callbacks / underlying model.
 */

#include "ck-tasks-ui.h"
#include "ck-tasks-ctrl.h"
#include "ck-tasks-model.h"

#include "../shared/session_utils.h"

#include <Dt/Dt.h>
#include <Dt/Session.h>
#include <Xm/Protocols.h>
#include <Xm/Form.h>
#include <Xm/Xm.h>
#include <X11/Xatom.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_exec_path[PATH_MAX] = "ck-tasks";
static SessionData *g_session_data = NULL;
static TasksUi *g_ui = NULL;
static TasksController *g_controller = NULL;

static void init_exec_path(const char *argv0)
{
    ssize_t len = readlink("/proc/self/exe", g_exec_path, sizeof(g_exec_path) - 1);
    if (len > 0) {
        g_exec_path[len] = '\0';
        return;
    }

    if (argv0 && argv0[0]) {
        if (argv0[0] == '/') {
            strncpy(g_exec_path, argv0, sizeof(g_exec_path) - 1);
            g_exec_path[sizeof(g_exec_path) - 1] = '\0';
            return;
        }

        if (strchr(argv0, '/')) {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd))) {
                size_t cwd_len = strlen(cwd);
                size_t argv_len = strlen(argv0);
                size_t needed = cwd_len + 1 + argv_len + 1;
                if (needed <= sizeof(g_exec_path)) {
                    memcpy(g_exec_path, cwd, cwd_len);
                    g_exec_path[cwd_len] = '/';
                    memcpy(g_exec_path + cwd_len + 1, argv0, argv_len);
                    g_exec_path[cwd_len + 1 + argv_len] = '\0';
                    return;
                }
            }
        }

        strncpy(g_exec_path, argv0, sizeof(g_exec_path) - 1);
        g_exec_path[sizeof(g_exec_path) - 1] = '\0';
    }
}

static void session_save_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    (void)call_data;
    if (!g_session_data) return;
    if (!g_ui) return;
    session_capture_geometry(w, g_session_data, "x", "y", "w", "h");
    int current_tab = tasks_ui_get_current_tab(g_ui);
    if (current_tab > 0) {
        session_data_set_int(g_session_data, "current_tab", current_tab);
    }
    session_save(w, g_session_data, g_exec_path);
}

static void wm_delete_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    XtAppContext app = (XtAppContext)client_data;
    XtAppSetExitFlag(app);
}

int main(int argc, char *argv[])
{
    XtAppContext app;
    Widget toplevel;
    XtSetLanguageProc(NULL, NULL, NULL);

    char *session_id = session_parse_argument(&argc, argv);
    g_session_data = session_data_create(session_id);
    free(session_id);
    init_exec_path(argv[0]);

    toplevel = XtVaAppInitialize(&app, "TasksManager", NULL, 0, &argc, argv, NULL, NULL);
    DtInitialize(XtDisplay(toplevel), toplevel, "TasksManager", "TasksManager");
    XtVaSetValues(toplevel,
                  XmNtitle, "Task Manager",
                  XmNiconName, "Task Manager",
                  NULL);

    g_ui = tasks_ui_create(app, toplevel);
    if (!g_ui) {
        return 1;
    }

    tasks_model_initialize();
    g_controller = tasks_ctrl_create(g_ui, g_session_data);

    if (g_session_data && session_load(toplevel, g_session_data)) {
        session_apply_geometry(toplevel, g_session_data, "x", "y", "w", "h");
        int tab = session_data_get_int(g_session_data, "current_tab", TASKS_TAB_PROCESSES);
        tasks_ui_set_current_tab(g_ui, (TasksTab)tab);
    } else {
        tasks_ui_center_on_screen(g_ui);
    }

    Atom wm_delete = XmInternAtom(XtDisplay(toplevel), "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(toplevel, wm_delete, wm_delete_cb, (XtPointer)app);
    XmActivateWMProtocol(toplevel, wm_delete);

    Atom wm_save = XmInternAtom(XtDisplay(toplevel), "WM_SAVE_YOURSELF", False);
    XmAddWMProtocolCallback(toplevel, wm_save, session_save_cb, NULL);
    XmActivateWMProtocol(toplevel, wm_save);

    XtRealizeWidget(toplevel);
    XtAppMainLoop(app);

    if (g_controller) {
        tasks_ctrl_destroy(g_controller);
    }
    tasks_model_shutdown();
    session_data_free(g_session_data);
    return 0;
}
