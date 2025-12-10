#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/Notebook.h>
#include <Xm/PushB.h>
#include <Xm/Protocols.h>
#include <Xm/MwmUtil.h>
#include <X11/Xlib.h>
#include <Dt/Dt.h>        /* CDE version info */
#include <Dt/Session.h>   /* CDE session management */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

#include "../shared/session_utils.h"
#include "../shared/about_dialog.h"
/* ---------- Globals for session handling ---------- */

static SessionData *g_session_data = NULL;
static Widget g_notebook    = NULL;  /* set to the Notebook widget */
static char   g_exec_path[PATH_MAX] = "ck-about"; /* absolute path to executable */

/* ---------- Helpers ---------- */

/* Handle WM close (so window manager close button works) */
void wm_delete_callback(Widget w, XtPointer client_data, XtPointer call_data) {
    XtAppContext app = (XtAppContext)client_data;
    XtAppSetExitFlag(app);
}

/* generic helper: add padding around a widget that is child of an XmForm */
void
add_form_padding(Widget w, int padding)
{
    Arg args[8];
    int n = 0;

    XtSetArg(args[n], XmNtopOffset,    padding); n++;
    XtSetArg(args[n], XmNleftOffset,   padding); n++;
    XtSetArg(args[n], XmNrightOffset,  padding); n++;
    XtSetArg(args[n], XmNbottomOffset, 1); n++;  /* keep your choice here */

    XtSetValues(w, args, n);
}

/* ---------- Session handling helpers ---------- */

/* Resolve executable path (for session restart command) */
static void init_exec_path(const char *argv0)
{
    ssize_t len = readlink("/proc/self/exe", g_exec_path,
                           sizeof(g_exec_path) - 1);
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
                size_t needed = cwd_len + 1 + argv_len + 1; /* '/' + '\0' */

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

/* Center shell on screen (used when no session geometry is restored) */
static void center_shell_on_screen(Widget toplevel)
{
    Dimension w = 0, h = 0;
    XtVaGetValues(toplevel,
                  XmNwidth,  &w,
                  XmNheight, &h,
                  NULL);

    if (w == 0 || h == 0)
        return;

    Display *dpy = XtDisplay(toplevel);
    int screen   = DefaultScreen(dpy);
    int sw       = DisplayWidth(dpy, screen);
    int sh       = DisplayHeight(dpy, screen);

    Position x = (Position)((sw - (int)w) / 2);
    Position y = (Position)((sh - (int)h) / 2);

    if (x < 0) x = 0;
    if (y < 0) y = 0;

    XtVaSetValues(toplevel,
                  XmNx, x,
                  XmNy, y,
                  NULL);
}

/* Callback for WM_SAVE_YOURSELF: save geometry + current tab and set restart cmd */
void session_save_callback(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    (void)call_data;
    if (!g_session_data) return;

    session_capture_geometry(w, g_session_data, "x", "y", "w", "h");
    if (g_notebook) {
        int page = 1;
        XtVaGetValues(g_notebook, XmNcurrentPageNumber, &page, NULL);
        session_data_set_int(g_session_data, "page", page);
    }

    session_save(w, g_session_data, g_exec_path);
}

/* ---------- main ---------- */

int main(int argc, char *argv[])
{
    XtAppContext app;
    Widget toplevel, mainForm, notebook, okButton;
    Arg args[10];
    int n;
    Boolean restored_geometry = False;

    /* Parse -session argument, if any */
    char *session_id = session_parse_argument(&argc, argv);
    g_session_data = session_data_create(session_id);
    free(session_id);

    /* Find executable path for restart command */
    init_exec_path(argv[0]);

    /* Initialize top-level shell */
    toplevel = XtVaAppInitialize(&app, "CkAbout", NULL, 0,
                                 &argc, argv, NULL, NULL);

    /* Initialize CDE services (session manager, version info, etc.) */
    DtInitialize(XtDisplay(toplevel), toplevel, "CkAbout", "CkAbout");

    /* --------- Form as intermediate parent ---------- */
    n = 0;
    mainForm = XmCreateForm(toplevel, "mainForm", args, n);
    XtVaSetValues(mainForm,
                  XmNmarginWidth,  0,
                  XmNmarginHeight, 0,
                  XmNfractionBase, 100,
                  NULL);
    XtManageChild(mainForm);

    /* Create Notebook as child of mainForm, attached to all sides */
    n = 0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;

    notebook = XmCreateNotebook(mainForm, "notebook", args, n);

    /* INNER padding inside notebook (optional) */
    XtVaSetValues(notebook,
                  XmNmarginWidth,  12,
                  XmNmarginHeight, 12,
                  NULL);

    XtManageChild(notebook);

    /* Store global pointer for session handling */
    g_notebook = notebook;

    /* OUTER padding around notebook (space to window frame) */
    add_form_padding(notebook, 12);

    /* ---- OK button at bottom center (outside notebook) ---- */
    n = 0;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNbottomOffset,    8); n++;
    XtSetArg(args[n], XmNleftAttachment,  XmATTACH_POSITION); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_POSITION); n++;
    XtSetArg(args[n], XmNleftPosition,    40); n++;   /* centered: 40%..60% */
    XtSetArg(args[n], XmNrightPosition,   60); n++;
    okButton = XmCreatePushButton(mainForm, "okButton", args, n);
    XtVaSetValues(okButton,
                  XmNlabelString, XmStringCreateLocalized("OK"),
                  NULL);
    XtManageChild(okButton);

    /* Make notebook sit *above* the OK button */
    XtVaSetValues(notebook,
                  XmNbottomAttachment, XmATTACH_WIDGET,
                  XmNbottomWidget,     okButton,
                  XmNbottomOffset,     8,
                  NULL);

    /* OK closes like window close button */
    XtAddCallback(okButton, XmNactivateCallback,
                  wm_delete_callback, (XtPointer)app);

    /* Make OK the default button (Enter activates it) */
    XtVaSetValues(mainForm,
                  XmNdefaultButton, okButton,
                  NULL);

    about_add_standard_pages(notebook, 1,
                             "About",
                             "CK-Core",
                             "(c) 2025-2026 by Dr. C. Klukas",
                             False);

    /* Size + title */
    XtVaSetValues(toplevel,
                  XmNtitle,      "About CK-Core",
                  XmNminWidth,   400,
                  XmNminHeight,  200,
                  NULL);

    /* WM protocol handling: DELETE + SAVE_YOURSELF */

    Atom wm_delete = XmInternAtom(XtDisplay(toplevel),
                                  "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(toplevel, wm_delete,
                            wm_delete_callback, (XtPointer)app);
    XmActivateWMProtocol(toplevel, wm_delete);

    Atom wm_save = XmInternAtom(XtDisplay(toplevel),
                                "WM_SAVE_YOURSELF", False);
    XmAddWMProtocolCallback(toplevel, wm_save,
                            session_save_callback, NULL);
    XmActivateWMProtocol(toplevel, wm_save);

    /* Restore previous session state (geometry + current page), if any */
    if (g_session_data && session_load(toplevel, g_session_data)) {
        restored_geometry = session_apply_geometry(toplevel, g_session_data,
                                                   "x", "y", "w", "h");
        int page = session_data_get_int(g_session_data, "page", 1);
        if (page >= 1 && notebook) {
            XtVaSetValues(notebook,
                          XmNcurrentPageNumber, page,
                          NULL);
        }
    }

    /* Realize widgets */
    XtRealizeWidget(toplevel);

    /* Center window on first launch (avoid top-left placement) */
    if (!restored_geometry) {
        center_shell_on_screen(toplevel);
    }

    /* Main loop */
    XtAppMainLoop(app);

    return 0;
}
