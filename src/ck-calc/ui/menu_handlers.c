#include "menu_handlers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/RowColumn.h>
#include <Xm/DialogS.h>
#include <Xm/Notebook.h>
#include <Xm/CascadeB.h>
#include <Xm/PushB.h>
#include <Xm/ToggleB.h>
#include <Xm/Protocols.h>

#include "../../shared/session_utils.h"
#include "../../shared/about_dialog.h"
#include "../logic/display_api.h"
#include "../app_state_utils.h"

static Widget g_about_shell = NULL;

static void capture_and_save_session(AppState *app)
{
    if (!app || !app->session_data) return;
    session_capture_geometry(app->shell, app->session_data, "x", "y", "w", "h");
    session_data_set(app->session_data, "display", app->display);
    session_data_set_int(app->session_data, "show_thousands", app->show_thousands ? 1 : 0);
    session_data_set_int(app->session_data, "mode", app->mode);
    session_save(app->shell, app->session_data, app->exec_path);
    ck_calc_save_view_state(app);
}

static void about_close_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    Widget shell = (Widget)client_data;
    if (shell && XtIsWidget(shell)) {
        XtDestroyWidget(shell);
    }
}

static void about_destroy_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    g_about_shell = NULL;
}

static void about_wm_protocol_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    Widget shell = (Widget)client_data;
    if (shell && XtIsWidget(shell)) {
        XtDestroyWidget(shell);
    }
}

static void show_about_dialog(AppState *app)
{
    if (!app) return;

    if (g_about_shell && XtIsWidget(g_about_shell)) {
        XtPopup(g_about_shell, XtGrabNone);
        return;
    }

    Arg shell_args[8];
    int sn = 0;
    XtSetArg(shell_args[sn], XmNtitle, "About Calculator"); sn++;
    XtSetArg(shell_args[sn], XmNallowShellResize, True); sn++;
    XtSetArg(shell_args[sn], XmNtransientFor, app->shell); sn++;
    g_about_shell = XmCreateDialogShell(app->shell, "aboutCalcShell", shell_args, sn);
    if (!g_about_shell) return;
    XtAddCallback(g_about_shell, XmNdestroyCallback, about_destroy_cb, NULL);

    Widget form = XmCreateForm(g_about_shell, "aboutForm", NULL, 0);
    XtManageChild(form);

    Arg args[8];
    int n = 0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
    Widget notebook = XmCreateNotebook(form, "aboutNotebook", args, n);
    XtVaSetValues(notebook, XmNmarginWidth, 12, XmNmarginHeight, 12, NULL);
    XtManageChild(notebook);

    Widget ok = XtVaCreateManagedWidget(
        "aboutOk",
        xmPushButtonWidgetClass, form,
        XmNlabelString, XmStringCreateLocalized("OK"),
        XmNbottomAttachment, XmATTACH_FORM,
        XmNbottomOffset,    8,
        XmNleftAttachment,  XmATTACH_POSITION,
        XmNrightAttachment, XmATTACH_POSITION,
        XmNleftPosition,    40,
        XmNrightPosition,   60,
        NULL
    );

    XtVaSetValues(notebook,
                  XmNbottomAttachment, XmATTACH_WIDGET,
                  XmNbottomWidget,     ok,
                  XmNbottomOffset,     8,
                  NULL);

    XtAddCallback(ok, XmNactivateCallback, about_close_cb, (XtPointer)g_about_shell);

    about_add_standard_pages(notebook, 1,
                             "About",
                             "CK-Core Calculator",
                             "(c) 2026 by Dr. C. Klukas",
                             True);

    XtRealizeWidget(g_about_shell);
    Atom wm_delete = XmInternAtom(XtDisplay(g_about_shell), "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(g_about_shell, wm_delete, about_wm_protocol_cb, (XtPointer)g_about_shell);
    XmActivateWMProtocol(g_about_shell, wm_delete);

    XtPopup(g_about_shell, XtGrabNone);
}

void menu_handlers_cb_toggle_thousands(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)call_data;
    AppState *app = (AppState *)client_data;
    if (!app) return;
    Boolean state = XmToggleButtonGetState(w);
    app->show_thousands = (state != False);
    ck_calc_save_view_state(app);
    if (app->session_data) {
        session_data_set_int(app->session_data, "show_thousands", app->show_thousands ? 1 : 0);
    }
    ck_calc_reformat_display(app);
    ck_calc_ensure_keyboard_focus(app);
}

void menu_handlers_cb_menu_new(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = (AppState *)client_data;
    if (!app) return;

    pid_t pid = fork();
    if (pid == 0) {
        execl(app->exec_path, app->exec_path, (char *)NULL);
        _exit(1);
    }
}

void menu_handlers_cb_menu_close(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = (AppState *)client_data;
    if (!app) return;
    capture_and_save_session(app);
    XtAppSetExitFlag(app->app_context);
}

void menu_handlers_cb_wm_delete(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = (AppState *)client_data;
    if (!app) return;
    capture_and_save_session(app);
    XtAppSetExitFlag(app->app_context);
}

void menu_handlers_cb_wm_save(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = (AppState *)client_data;
    if (!app) return;
    capture_and_save_session(app);
}

void menu_handlers_cb_about(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = (AppState *)client_data;
    if (!app) return;
    show_about_dialog(app);
}
