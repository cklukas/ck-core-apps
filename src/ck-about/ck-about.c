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
#include <string.h>
#include <limits.h>
#include <unistd.h>

/* ---------- Globals for session handling ---------- */

static char  *g_session_id  = NULL;  /* from -session <id> */
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

/* helper: run a command and capture a single-line result */
static void run_cmd_single_line(const char *cmd, char *buf, size_t size)
{
    buf[0] = '\0';
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        snprintf(buf, size, "n/a");
        return;
    }

    if (!fgets(buf, size, fp)) {
        snprintf(buf, size, "n/a");
        pclose(fp);
        return;
    }
    pclose(fp);

    /* trim newline and trailing spaces/tabs */
    size_t len = strlen(buf);
    while (len > 0 &&
           (buf[len-1] == '\n' || buf[len-1] == ' ' || buf[len-1] == '\t')) {
        buf[--len] = '\0';
    }
}

/* struct to hold fields for simple 2-column tables */
typedef struct {
    const char *label;
    char value[256];
} LsbField;

/* fill 4 fields: Distributor ID, Description, Release, Codename */
static int get_lsb_fields(LsbField fields[], int max_fields)
{
    if (max_fields < 4) return 0;

    fields[0].label = "Distributor ID";
    run_cmd_single_line("lsb_release -i -s 2>/dev/null",
                        fields[0].value, sizeof(fields[0].value));

    fields[1].label = "Description";
    run_cmd_single_line("lsb_release -d -s 2>/dev/null",
                        fields[1].value, sizeof(fields[1].value));

    fields[2].label = "Release";
    run_cmd_single_line("lsb_release -r -s 2>/dev/null",
                        fields[2].value, sizeof(fields[2].value));

    fields[3].label = "Codename";
    run_cmd_single_line("lsb_release -c -s 2>/dev/null",
                        fields[3].value, sizeof(fields[3].value));

    return 4;
}

/* fill CDE fields using Dt/Dt.h macros */
static int get_cde_fields(LsbField fields[], int max_fields)
{
    if (max_fields < 4) return 0;

    fields[0].label = "Version";
    snprintf(fields[0].value, sizeof(fields[0].value), "%s", DtVERSION_STRING);

    fields[1].label = "Major / Revision / Update";
    snprintf(fields[1].value, sizeof(fields[1].value),
             "%d / %d / %d", DtVERSION, DtREVISION, DtUPDATE_LEVEL);

    fields[2].label = "Version Number";
    snprintf(fields[2].value, sizeof(fields[2].value), "%d", DtVERSION_NUMBER);

    fields[3].label = "DtVersion (runtime)";
    /* If you link against the full Dt library, DtVersion may be a runtime int.
       Here we just show the macro version again. */
    snprintf(fields[3].value, sizeof(fields[3].value), "%d", DtVERSION_NUMBER);

    return 4;
}

/* ---------- Session handling helpers ---------- */

/* Parse -session <id> from argv, store in g_session_id */
static void parse_session_arg(int *argc, char **argv)
{
    for (int i = 1; i < *argc - 1; ++i) {
        if (strcmp(argv[i], "-session") == 0) {
            g_session_id = argv[i+1];
            break;
        }
    }
}

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
                snprintf(g_exec_path, sizeof(g_exec_path), "%s/%s", cwd, argv0);
                return;
            }
        }

        strncpy(g_exec_path, argv0, sizeof(g_exec_path) - 1);
        g_exec_path[sizeof(g_exec_path) - 1] = '\0';
    }
}

/* Restore window geometry + current tab from CDE session state */
static Boolean restore_session_state(Widget toplevel, Widget notebook)
{
    if (!g_session_id)
        return False;

    char *path = NULL;

    if (!DtSessionRestorePath(toplevel, &path, g_session_id)) {
        return False;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        XtFree(path);
        return False;
    }

    int x = 0, y = 0, w = 0, h = 0, page = 1;
    char key[32];
    Boolean geometry_restored = False;

    while (fscanf(fp, "%31s", key) == 1) {
        if (strcmp(key, "x") == 0)      fscanf(fp, "%d", &x);
        else if (strcmp(key, "y") == 0) fscanf(fp, "%d", &y);
        else if (strcmp(key, "w") == 0) fscanf(fp, "%d", &w);
        else if (strcmp(key, "h") == 0) fscanf(fp, "%d", &h);
        else if (strcmp(key, "page") == 0) fscanf(fp, "%d", &page);
        else {
            /* unknown token: skip remainder of line */
            char buf[256];
            fgets(buf, sizeof(buf), fp);
        }
    }
    fclose(fp);

    if (w > 0 && h > 0) {
        XtVaSetValues(toplevel,
                      XmNx,      (Position)x,
                      XmNy,      (Position)y,
                      XmNwidth,  (Dimension)w,
                      XmNheight, (Dimension)h,
                      NULL);
        geometry_restored = True;
    }

    if (page >= 1 && notebook) {
        XtVaSetValues(notebook,
                      XmNcurrentPageNumber, page,
                      NULL);
    }

    XtFree(path);
    return geometry_restored;
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
    char *savePath = NULL;
    char *saveFile = NULL;

    /* Ask CDE where to save our session file */
    if (!DtSessionSavePath(w, &savePath, &saveFile)) {
        return;
    }

    FILE *fp = fopen(savePath, "w");
    if (!fp) {
        XtFree(savePath);
        XtFree(saveFile);
        return;
    }

    /* 1) get window geometry */
    Position x, y;
    Dimension width, height;
    XtVaGetValues(w,
                  XmNx,     &x,
                  XmNy,     &y,
                  XmNwidth, &width,
                  XmNheight,&height,
                  NULL);

    /* 2) get current notebook page */
    int page = 1;
    if (g_notebook) {
        XtVaGetValues(g_notebook,
                      XmNcurrentPageNumber, &page,
                      NULL);
    }

    /* 3) write state file (simple text format) */
    fprintf(fp, "x %d\n", (int)x);
    fprintf(fp, "y %d\n", (int)y);
    fprintf(fp, "w %u\n", (unsigned)width);
    fprintf(fp, "h %u\n", (unsigned)height);
    fprintf(fp, "page %d\n", page);
    fclose(fp);

    /* 4) tell the session manager how to restart us: ck-about -session <id> */
    char *cmd_argv[3];
    int   cmd_argc = 0;

    cmd_argv[cmd_argc++] = g_exec_path;
    cmd_argv[cmd_argc++] = "-session";
    cmd_argv[cmd_argc++] = saveFile;   /* session id */

    XSetCommand(XtDisplay(w), XtWindow(w), cmd_argv, cmd_argc);

    XtFree(savePath);
    XtFree(saveFile);
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
    parse_session_arg(&argc, argv);

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

    /* ---- Page 1: ABOUT ---- */

    Widget page1;
    n = 0;
    XtSetArg(args[n], XmNfractionBase, 1); n++;
    page1 = XmCreateForm(notebook, "page1", args, n);
    XtManageChild(page1);

    /* Fonts: default for normal text, try to get a bold one for title */
    Display *dpy = XtDisplay(toplevel);
    XFontStruct *bold_fs = XLoadQueryFont(dpy, "-*-helvetica-bold-r-normal-*-24-*");
    XmFontList bold_fontlist = NULL;
    if (bold_fs) {
        bold_fontlist = XmFontListCreate(bold_fs, XmFONTLIST_DEFAULT_TAG);
    }

    /* Top label: "CK-Core" (bold, centered) */
    Widget label_title;
    n = 0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNalignment,        XmALIGNMENT_CENTER); n++;
    XtSetArg(args[n], XmNtopOffset,        16); n++;
    label_title = XmCreateLabel(page1, "label_title", args, n);

    XtVaSetValues(label_title,
                  XmNlabelString,
                  XmStringCreateLocalized("CK-Core"),
                  NULL);
    if (bold_fontlist) {
        XtVaSetValues(label_title,
                      XmNfontList, bold_fontlist,
                      NULL);
    }
    XtManageChild(label_title);

    /* Bottom label: "(c) 2025-2026 by Dr. C. Klukas" (normal font) */
    Widget label_copy;
    n = 0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget,        label_title); n++;
    XtSetArg(args[n], XmNtopOffset,        8); n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNalignment,        XmALIGNMENT_CENTER); n++;
    label_copy = XmCreateLabel(page1, "label_copy", args, n);

    XtVaSetValues(label_copy,
                  XmNlabelString,
                  XmStringCreateLocalized("(c) 2025-2026 by Dr. C. Klukas"),
                  NULL);
    XtManageChild(label_copy);

    /* Tab for page 1 */
    Widget tab1;
    tab1 = XmCreatePushButton(notebook, "tab1", NULL, 0);
    XtVaSetValues(tab1,
                  XmNlabelString, XmStringCreateLocalized("About"),
                  NULL);
    XtManageChild(tab1);

    /* Link page + tab to notebook as page #1 */
    XtVaSetValues(page1,
                  XmNnotebookChildType, XmPAGE,
                  XmNpageNumber,        1,
                  NULL);

    XtVaSetValues(tab1,
                  XmNnotebookChildType, XmMAJOR_TAB,
                  XmNpageNumber,        1,
                  NULL);

    /* ---- Page 2: CDE (table layout using Dt/Dt.h) ---- */

    Widget page_cde;
    n = 0;
    XtSetArg(args[n], XmNfractionBase, 100); n++;
    page_cde = XmCreateForm(notebook, "page_cde", args, n);
    XtManageChild(page_cde);

    LsbField cde_fields[4];
    int cde_count = get_cde_fields(cde_fields, 4);

    Widget prev_row_cde = NULL;
    for (int i = 0; i < cde_count; ++i) {
        Widget key_label, value_label;

        /* Left label: field name */
        n = 0;
        if (prev_row_cde == NULL) {
            XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
            XtSetArg(args[n], XmNtopOffset,     8); n++;
        } else {
            XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
            XtSetArg(args[n], XmNtopWidget,     prev_row_cde); n++;
            XtSetArg(args[n], XmNtopOffset,     4); n++;
        }
        XtSetArg(args[n], XmNleftAttachment,  XmATTACH_POSITION); n++;
        XtSetArg(args[n], XmNleftPosition,    5); n++;
        XtSetArg(args[n], XmNrightAttachment, XmATTACH_POSITION); n++;
        XtSetArg(args[n], XmNrightPosition,   40); n++;
        XtSetArg(args[n], XmNalignment,       XmALIGNMENT_END); n++;
        key_label = XmCreateLabel(page_cde, "cde_key", args, n);

        XmString key_xms = XmStringCreateLocalized((char*)cde_fields[i].label);
        XtVaSetValues(key_label,
                      XmNlabelString, key_xms,
                      NULL);
        XmStringFree(key_xms);
        XtManageChild(key_label);

        /* Right label: value */
        n = 0;
        XtSetArg(args[n], XmNtopAttachment,   XmATTACH_OPPOSITE_WIDGET); n++;
        XtSetArg(args[n], XmNtopWidget,       key_label); n++;
        XtSetArg(args[n], XmNleftAttachment,  XmATTACH_POSITION); n++;
        XtSetArg(args[n], XmNleftPosition,    45); n++;
        XtSetArg(args[n], XmNrightAttachment, XmATTACH_POSITION); n++;
        XtSetArg(args[n], XmNrightPosition,   95); n++;
        XtSetArg(args[n], XmNalignment,       XmALIGNMENT_BEGINNING); n++;
        value_label = XmCreateLabel(page_cde, "cde_value", args, n);

        XmString value_xms = XmStringCreateLocalized(cde_fields[i].value);
        XtVaSetValues(value_label,
                      XmNlabelString, value_xms,
                      NULL);
        XmStringFree(value_xms);
        XtManageChild(value_label);

        prev_row_cde = key_label;
    }

    /* Tab for page 2 (CDE) */
    Widget tab_cde;
    tab_cde = XmCreatePushButton(notebook, "tab_cde", NULL, 0);
    XtVaSetValues(tab_cde,
                  XmNlabelString, XmStringCreateLocalized("CDE"),
                  NULL);
    XtManageChild(tab_cde);

    /* Link page + tab to notebook as page #2 */
    XtVaSetValues(page_cde,
                  XmNnotebookChildType, XmPAGE,
                  XmNpageNumber,        2,
                  NULL);

    XtVaSetValues(tab_cde,
                  XmNnotebookChildType, XmMAJOR_TAB,
                  XmNpageNumber,        2,
                  NULL);

    /* ---- Page 3: OS (table layout) ---- */

    Widget page_os;
    n = 0;
    XtSetArg(args[n], XmNfractionBase, 100); n++;  /* use positions for columns */
    page_os = XmCreateForm(notebook, "page_os", args, n);
    XtManageChild(page_os);

    LsbField fields[4];
    int field_count = get_lsb_fields(fields, 4);

    Widget prev_row_os = NULL;
    for (int i = 0; i < field_count; ++i) {
        Widget key_label, value_label;

        n = 0;
        if (prev_row_os == NULL) {
            XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
            XtSetArg(args[n], XmNtopOffset,     8); n++;
        } else {
            XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
            XtSetArg(args[n], XmNtopWidget,     prev_row_os); n++;
            XtSetArg(args[n], XmNtopOffset,     4); n++;
        }
        XtSetArg(args[n], XmNleftAttachment,  XmATTACH_POSITION); n++;
        XtSetArg(args[n], XmNleftPosition,    5); n++;
        XtSetArg(args[n], XmNrightAttachment, XmATTACH_POSITION); n++;
        XtSetArg(args[n], XmNrightPosition,   40); n++;
        XtSetArg(args[n], XmNalignment,       XmALIGNMENT_END); n++;
        key_label = XmCreateLabel(page_os, "os_key", args, n);

        XmString key_xms = XmStringCreateLocalized((char*)fields[i].label);
        XtVaSetValues(key_label,
                      XmNlabelString, key_xms,
                      NULL);
        XmStringFree(key_xms);
        XtManageChild(key_label);

        n = 0;
        XtSetArg(args[n], XmNtopAttachment,   XmATTACH_OPPOSITE_WIDGET); n++;
        XtSetArg(args[n], XmNtopWidget,       key_label); n++;
        XtSetArg(args[n], XmNleftAttachment,  XmATTACH_POSITION); n++;
        XtSetArg(args[n], XmNleftPosition,    45); n++;
        XtSetArg(args[n], XmNrightAttachment, XmATTACH_POSITION); n++;
        XtSetArg(args[n], XmNrightPosition,   95); n++;
        XtSetArg(args[n], XmNalignment,       XmALIGNMENT_BEGINNING); n++;
        value_label = XmCreateLabel(page_os, "os_value", args, n);

        XmString value_xms = XmStringCreateLocalized(fields[i].value);
        XtVaSetValues(value_label,
                      XmNlabelString, value_xms,
                      NULL);
        XmStringFree(value_xms);
        XtManageChild(value_label);

        prev_row_os = key_label;
    }

    /* Tab for page 3 */
    Widget tab_os;
    tab_os = XmCreatePushButton(notebook, "tab_os", NULL, 0);
    XtVaSetValues(tab_os,
                  XmNlabelString, XmStringCreateLocalized("OS"),
                  NULL);
    XtManageChild(tab_os);

    /* Link page + tab to notebook as page #3 */
    XtVaSetValues(page_os,
                  XmNnotebookChildType, XmPAGE,
                  XmNpageNumber,        3,
                  NULL);

    XtVaSetValues(tab_os,
                  XmNnotebookChildType, XmMAJOR_TAB,
                  XmNpageNumber,        3,
                  NULL);

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
    restored_geometry = restore_session_state(toplevel, notebook);

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
