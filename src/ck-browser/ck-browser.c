#include <Xm/Xm.h>
#include <Xm/CascadeBG.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/LabelG.h>
#include <Xm/MessageB.h>
#include <Xm/MenuShell.h>
#include <Xm/Protocols.h>
#include <Xm/PushBG.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/TabStack.h>
#include <Xm/TextF.h>
#include <X11/Intrinsic.h>
#include <X11/Xlib.h>
#include <Dt/Dt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../shared/about_dialog.h"

static XtAppContext g_app = NULL;
static Widget g_toplevel = NULL;
static Widget g_tab_stack = NULL;
static Widget g_about_shell = NULL;

static void wm_delete_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    XtAppContext app = (XtAppContext)client_data;
    XtAppSetExitFlag(app);
}

static XmString make_string(const char *text)
{
    return XmStringCreateLocalized((String)(text ? text : ""));
}

static Widget create_menu_item(Widget parent, const char *name, const char *label)
{
    XmString xm_label = make_string(label);
    Widget item = XtVaCreateManagedWidget(
        name,
        xmPushButtonGadgetClass, parent,
        XmNlabelString, xm_label,
        NULL);
    XmStringFree(xm_label);
    return item;
}

static Widget create_cascade_menu(Widget menu_bar, const char *label, const char *name)
{
    Widget menu = XmCreatePulldownMenu(menu_bar, (String)name, NULL, 0);
    XmString xm_label = make_string(label);
    XtVaCreateManagedWidget(
        name,
        xmCascadeButtonGadgetClass, menu_bar,
        XmNlabelString, xm_label,
        XmNsubMenuId, menu,
        NULL);
    XmStringFree(xm_label);
    return menu;
}

static void destroy_base_title(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    char *base = (char *)client_data;
    free(base);
}

static void build_time_suffix(char *buffer, size_t buffer_len)
{
    if (!buffer || buffer_len == 0) return;
    time_t now = time(NULL);
    struct tm local_tm;
    localtime_r(&now, &local_tm);
    strftime(buffer, buffer_len, "%H:%M:%S", &local_tm);
}

static void build_page_message(char *buffer, size_t buffer_len, const char *prefix)
{
    if (!buffer || buffer_len == 0) return;
    char time_buf[32] = {0};
    build_time_suffix(time_buf, sizeof(time_buf));
    snprintf(buffer, buffer_len, "%s (created at %s).", prefix, time_buf);
}

static int count_tabs_with_base_title(Widget tab_stack, const char *base_title)
{
    if (!tab_stack || !base_title) return 0;
    WidgetList children = NULL;
    Cardinal count = 0;
    XtVaGetValues(tab_stack, XmNchildren, &children, XmNnumChildren, &count, NULL);
    int matches = 0;
    for (Cardinal i = 0; i < count; ++i) {
        char *base = NULL;
        XtVaGetValues(children[i], XmNuserData, &base, NULL);
        if (base && strcmp(base, base_title) == 0) {
            matches++;
        }
    }
    return matches;
}

static Widget create_tab_page(Widget tab_stack,
                              const char *name,
                              const char *title,
                              const char *message,
                              const char *base_title)
{
    XmString tab_label = make_string(title);
    Widget page = XmCreateForm(tab_stack, (String)name, NULL, 0);
    XtVaSetValues(page,
                  XmNtabLabelString, tab_label,
                  XmNfractionBase, 100,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  NULL);
    XmStringFree(tab_label);

    if (base_title) {
        char *stored = strdup(base_title);
        XtVaSetValues(page, XmNuserData, stored, NULL);
        XtAddCallback(page, XmNdestroyCallback, destroy_base_title, stored);
    }

    XmString msg = make_string(message ? message : "");
    XtVaCreateManagedWidget(
        "pageLabel",
        xmLabelGadgetClass, page,
        XmNlabelString, msg,
        XmNalignment, XmALIGNMENT_CENTER,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL);
    XmStringFree(msg);

    XtManageChild(page);
    return page;
}

static void on_new_tab(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    if (!g_tab_stack) return;

    const char *base = "New Tab";
    int count = count_tabs_with_base_title(g_tab_stack, base);
    char name[32];
    snprintf(name, sizeof(name), "tabNew%d", count + 1);
    char tab_title[64];
    snprintf(tab_title, sizeof(tab_title), "%s (%d)", base, count + 1);

    char msg[128];
    build_page_message(msg, sizeof(msg), "No page loaded yet");
    Widget page = create_tab_page(g_tab_stack, name, tab_title, msg, base);
    XmTabStackSelectTab(page, True);
}

static void on_close_tab(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    if (!g_tab_stack) return;
    Widget selected = XmTabStackGetSelectedTab(g_tab_stack);
    if (!selected) return;
    XtDestroyWidget(selected);
}

static void on_about_destroy(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    g_about_shell = NULL;
}

static void on_help_about(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    if (!g_toplevel) return;
    if (g_about_shell && XtIsWidget(g_about_shell)) {
        XtPopup(g_about_shell, XtGrabNone);
        return;
    }

    Widget shell = NULL;
    Widget notebook = about_dialog_build(g_toplevel, "about_browser", "About Internet Browser", &shell);
    if (!notebook || !shell) return;

    about_add_standard_pages(notebook, 1,
                             "Internet Browser",
                             "Internet Browser",
                             "A placeholder browser shell for CK-Core.\n"
                             "Tabs, navigation controls, and status display are ready.",
                             True);
    XtAddCallback(shell, XmNdestroyCallback, on_about_destroy, NULL);
    g_about_shell = shell;
    XtPopup(shell, XtGrabNone);
}

static void on_help_view(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    if (!g_toplevel) return;
    Widget dialog = XmCreateInformationDialog(g_toplevel, "browserHelpDialog", NULL, 0);
    XmString msg = make_string("Help will be added once browsing features are implemented.");
    XtVaSetValues(dialog, XmNmessageString, msg, NULL);
    XmStringFree(msg);
    XtAddCallback(dialog, XmNokCallback, (XtCallbackProc)XtDestroyWidget, dialog);
    XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));
    XtManageChild(dialog);
}

static void on_menu_exit(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    XtAppContext app = (XtAppContext)client_data;
    XtAppSetExitFlag(app);
}

static Widget create_menu_bar(Widget parent)
{
    Widget menu_bar = XmCreateMenuBar(parent, "browserMenuBar", NULL, 0);
    XtVaSetValues(menu_bar,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  NULL);
    XtManageChild(menu_bar);

    Widget file_menu = create_cascade_menu(menu_bar, "File", "fileMenu");
    Widget file_new_tab = create_menu_item(file_menu, "fileNewTab", "New Tab");
    Widget file_close_tab = create_menu_item(file_menu, "fileCloseTab", "Close Tab");
    Widget file_exit = create_menu_item(file_menu, "fileExit", "Exit");

    Widget view_menu = create_cascade_menu(menu_bar, "View", "viewMenu");
    create_menu_item(view_menu, "viewZoomIn", "Zoom In");
    create_menu_item(view_menu, "viewZoomOut", "Zoom Out");
    create_menu_item(view_menu, "viewZoomReset", "Reset Zoom");

    Widget help_menu = XmCreatePulldownMenu(menu_bar, "helpMenu", NULL, 0);
    XmString help_label = make_string("Help");
    Widget help_cascade = XtVaCreateManagedWidget(
        "helpCascade",
        xmCascadeButtonGadgetClass, menu_bar,
        XmNlabelString, help_label,
        XmNmnemonic, 'H',
        XmNsubMenuId, help_menu,
        NULL);
    XmStringFree(help_label);
    XtVaSetValues(menu_bar, XmNmenuHelpWidget, help_cascade, NULL);

    Widget help_view = create_menu_item(help_menu, "helpView", "View Help");
    Widget help_about = create_menu_item(help_menu, "helpAbout", "About");

    XtAddCallback(file_new_tab, XmNactivateCallback, on_new_tab, NULL);
    XtAddCallback(file_close_tab, XmNactivateCallback, on_close_tab, NULL);
    XtAddCallback(file_exit, XmNactivateCallback, on_menu_exit, g_app);
    XtAddCallback(help_view, XmNactivateCallback, on_help_view, NULL);
    XtAddCallback(help_about, XmNactivateCallback, on_help_about, NULL);

    return menu_bar;
}

static Widget create_toolbar(Widget parent, Widget attach_top)
{
    Widget toolbar = XmCreateForm(parent, "browserToolbar", NULL, 0);
    XtVaSetValues(toolbar,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, attach_top,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNtopOffset, 6,
                  XmNleftOffset, 10,
                  XmNrightOffset, 10,
                  NULL);
    XtManageChild(toolbar);

    Widget button_row = XmCreateRowColumn(toolbar, "toolbarButtons", NULL, 0);
    XtVaSetValues(button_row,
                  XmNorientation, XmHORIZONTAL,
                  XmNpacking, XmPACK_TIGHT,
                  XmNspacing, 6,
                  XmNmarginWidth, 0,
                  XmNmarginHeight, 0,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  NULL);
    XtManageChild(button_row);

    XmString back_label = make_string("Back");
    XtVaCreateManagedWidget("backButton", xmPushButtonWidgetClass, button_row,
                            XmNlabelString, back_label, NULL);
    XmStringFree(back_label);

    XmString forward_label = make_string("Forward");
    XtVaCreateManagedWidget("forwardButton", xmPushButtonWidgetClass, button_row,
                            XmNlabelString, forward_label, NULL);
    XmStringFree(forward_label);

    XmString reload_label = make_string("Reload");
    XtVaCreateManagedWidget("reloadButton", xmPushButtonWidgetClass, button_row,
                            XmNlabelString, reload_label, NULL);
    XmStringFree(reload_label);

    XmString home_label = make_string("Home");
    XtVaCreateManagedWidget("homeButton", xmPushButtonWidgetClass, button_row,
                            XmNlabelString, home_label, NULL);
    XmStringFree(home_label);

    Widget url_field = XtVaCreateManagedWidget(
        "urlField",
        xmTextFieldWidgetClass, toolbar,
        XmNvalue, "https://",
        XmNleftAttachment, XmATTACH_WIDGET,
        XmNleftWidget, button_row,
        XmNleftOffset, 10,
        XmNrightAttachment, XmATTACH_FORM,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        NULL);

    XtVaSetValues(url_field, XmNcursorPositionVisible, True, NULL);

    return toolbar;
}

static Widget create_status_segment(Widget parent, const char *name, const char *text)
{
    Widget frame = XmCreateFrame(parent, (String)name, NULL, 0);
    XtVaSetValues(frame,
                  XmNshadowType, XmSHADOW_IN,
                  XmNmarginWidth, 4,
                  XmNmarginHeight, 2,
                  NULL);
    XtManageChild(frame);

    XmString xm_text = make_string(text);
    XtVaCreateManagedWidget(
        "statusLabel",
        xmLabelGadgetClass, frame,
        XmNlabelString, xm_text,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNmarginLeft, 4,
        XmNmarginRight, 4,
        NULL);
    XmStringFree(xm_text);

    return frame;
}

static Widget create_status_bar(Widget parent)
{
    Widget status_form = XmCreateForm(parent, "browserStatusBar", NULL, 0);
    XtVaSetValues(status_form,
                  XmNfractionBase, 100,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNbottomOffset, 6,
                  XmNleftOffset, 10,
                  XmNrightOffset, 10,
                  NULL);
    XtManageChild(status_form);

    Widget status_left = create_status_segment(status_form, "statusMain", "Ready");
    Widget status_center = create_status_segment(status_form, "statusSecurity", "Security: None");
    Widget status_right = create_status_segment(status_form, "statusZoom", "Zoom: 100%");

    XtVaSetValues(status_left,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_POSITION,
                  XmNrightPosition, 60,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  NULL);
    XtVaSetValues(status_center,
                  XmNleftAttachment, XmATTACH_POSITION,
                  XmNleftPosition, 60,
                  XmNrightAttachment, XmATTACH_POSITION,
                  XmNrightPosition, 85,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftOffset, 6,
                  NULL);
    XtVaSetValues(status_right,
                  XmNleftAttachment, XmATTACH_POSITION,
                  XmNleftPosition, 85,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftOffset, 6,
                  NULL);

    return status_form;
}

int main(int argc, char *argv[])
{
    XtAppContext app;
    Widget toplevel = XtVaAppInitialize(&app, "CkBrowser", NULL, 0,
                                        &argc, argv, NULL, NULL);
    g_app = app;
    g_toplevel = toplevel;
    DtInitialize(XtDisplay(toplevel), toplevel, "CkBrowser", "CkBrowser");
    XtVaSetValues(toplevel,
                  XmNtitle, "Internet Browser",
                  XmNiconName, "Internet Browser",
                  NULL);

    Widget main_form = XmCreateForm(toplevel, "browserMainForm", NULL, 0);
    XtVaSetValues(main_form,
                  XmNmarginWidth, 0,
                  XmNmarginHeight, 0,
                  XmNfractionBase, 100,
                  NULL);
    XtManageChild(main_form);

    Widget menu_bar = create_menu_bar(main_form);
    Widget toolbar = create_toolbar(main_form, menu_bar);
    Widget status_bar = create_status_bar(main_form);

    Widget tab_stack = XmCreateTabStack(main_form, "browserTabStack", NULL, 0);
    XtVaSetValues(tab_stack,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, toolbar,
                  XmNbottomAttachment, XmATTACH_WIDGET,
                  XmNbottomWidget, status_bar,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNleftOffset, 12,
                  XmNrightOffset, 12,
                  XmNtopOffset, 6,
                  XmNbottomOffset, 6,
                  NULL);
    XtManageChild(tab_stack);
    g_tab_stack = tab_stack;

    char msg_home[128];
    build_page_message(msg_home, sizeof(msg_home), "Welcome. No page loaded");
    Widget tab_home = create_tab_page(tab_stack, "tabWelcome", "Welcome", msg_home, "Welcome");

    char msg_blank[128];
    build_page_message(msg_blank, sizeof(msg_blank), "Nothing to show yet");
    Widget tab_blank = create_tab_page(tab_stack, "tabBlank", "New Tab (1)", msg_blank, "New Tab");
    XmTabStackSelectTab(tab_home, False);
    (void)tab_blank;

    Atom wm_delete = XmInternAtom(XtDisplay(toplevel), "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(toplevel, wm_delete, wm_delete_cb, (XtPointer)app);
    XmActivateWMProtocol(toplevel, wm_delete);

    XtRealizeWidget(toplevel);
    XtAppMainLoop(app);
    return 0;
}
