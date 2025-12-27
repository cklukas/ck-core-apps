#include "ck-tasks-ui.h"
#include "ck-tasks-tabs.h"
#include "ck-tasks-ui-helpers.h"

#include <Xm/CascadeBG.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/LabelG.h>
#include <Xm/MenuShell.h>
#include <Xm/Protocols.h>
#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/TabStack.h>
#include <Xm/ToggleBG.h>
#include <Xm/Xm.h>
#include <X11/Intrinsic.h>
#include <X11/Xlib.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static Widget tasks_ui_get_tab_widget(TasksUi *ui, TasksTab tab);

static Dimension measure_text_width(Widget label, const char *text)
{
    if (!label || !text) return 0;
    static int debug_enabled = -1;
    if (debug_enabled < 0) {
        const char *env = getenv("CK_TASKS_DEBUG_MEASURE_TEXT_WIDTH");
        debug_enabled = (env && env[0] && strcmp(env, "0") != 0) ? 1 : 0;
    }

    XmFontList font_list = NULL;
    XtVaGetValues(label, XmNfontList, &font_list, NULL);
    if (debug_enabled) {
        fprintf(stderr,
                "[ck-tasks] measure_text_width label=%s text=\"%s\" fontList(label)=%p\n",
                XtName(label) ? XtName(label) : "(unnamed)",
                text,
                (void *)font_list);
    }
    if (!font_list) {
        Widget parent = XtParent(label);
        if (parent) {
            XtVaGetValues(parent, XmNfontList, &font_list, NULL);
        }
        if (debug_enabled) {
            fprintf(stderr,
                    "[ck-tasks] measure_text_width label=%s fontList(parent=%s)=%p\n",
                    XtName(label) ? XtName(label) : "(unnamed)",
                    (parent && XtName(parent)) ? XtName(parent) : "(none)",
                    (void *)font_list);
        }
    }
    if (!font_list) {
        size_t len = strlen(text);
        Dimension fallback = (Dimension)(len * 8);
        if (debug_enabled) {
            fprintf(stderr,
                    "[ck-tasks] measure_text_width label=%s fallback len=%zu width=%u\n",
                    XtName(label) ? XtName(label) : "(unnamed)",
                    len,
                    (unsigned)fallback);
        }
        return fallback;
    }
    XmString xm = XmStringCreateLocalized((String)text);
    Dimension w = 0;
    Dimension h = 0;
    XmStringExtent(font_list, xm, &w, &h);
    XmStringFree(xm);
    if (debug_enabled) {
        fprintf(stderr,
                "[ck-tasks] measure_text_width label=%s extent width=%u height=%u\n",
                XtName(label) ? XtName(label) : "(unnamed)",
                (unsigned)w,
                (unsigned)h);
    }
    return w;
}

static Dimension compute_status_segment_width(Widget frame, Widget label, const char *text)
{
    Dimension text_w = measure_text_width(label, text);
    Dimension frame_margin = 0;
    Dimension shadow = 0;
    Dimension frame_border = 0;
    Dimension frame_highlight = 0;
    Dimension ml = 0;
    Dimension mr = 0;
    Dimension label_margin = 0;
    Dimension label_highlight = 0;
    XtVaGetValues(frame,
                  XmNmarginWidth, &frame_margin,
                  XmNshadowThickness, &shadow,
                  XmNborderWidth, &frame_border,
                  XmNhighlightThickness, &frame_highlight,
                  NULL);
    XtVaGetValues(label,
                  XmNmarginLeft, &ml,
                  XmNmarginRight, &mr,
                  XmNmarginWidth, &label_margin,
                  XmNhighlightThickness, &label_highlight,
                  NULL);
    Dimension padding = 24;
    return (Dimension)(text_w
                       + (2 * frame_margin)
                       + (2 * shadow)
                       + (2 * frame_border)
                       + (2 * frame_highlight)
                       + ml + mr
                       + (2 * label_margin)
                       + (2 * label_highlight)
                       + padding);
}

static void autosize_status_bar(TasksUi *ui)
{
    if (!ui) return;
    if (!ui->status_frame_processes || !ui->status_frame_cpu || !ui->status_frame_memory ||
        !ui->status_frame_message) return;
    if (!ui->status_processes_label || !ui->status_cpu_label || !ui->status_memory_label) return;

    if (!ui->status_bar_layout_initialized) {
        XtVaSetValues(ui->status_frame_processes,
                      XmNrecomputeSize, False,
                      XmNleftAttachment, XmATTACH_FORM,
                      XmNrightAttachment, XmATTACH_NONE,
                      XmNtopAttachment, XmATTACH_FORM,
                      XmNbottomAttachment, XmATTACH_FORM,
                      NULL);
        XtVaSetValues(ui->status_frame_cpu,
                      XmNrecomputeSize, False,
                      XmNleftAttachment, XmATTACH_WIDGET,
                      XmNleftWidget, ui->status_frame_processes,
                      XmNleftOffset, 6,
                      XmNrightAttachment, XmATTACH_NONE,
                      XmNtopAttachment, XmATTACH_FORM,
                      XmNbottomAttachment, XmATTACH_FORM,
                      NULL);
        XtVaSetValues(ui->status_frame_memory,
                      XmNrecomputeSize, False,
                      XmNleftAttachment, XmATTACH_WIDGET,
                      XmNleftWidget, ui->status_frame_cpu,
                      XmNleftOffset, 6,
                      XmNrightAttachment, XmATTACH_NONE,
                      XmNtopAttachment, XmATTACH_FORM,
                      XmNbottomAttachment, XmATTACH_FORM,
                      NULL);
        XtVaSetValues(ui->status_frame_message,
                      XmNleftAttachment, XmATTACH_WIDGET,
                      XmNleftWidget, ui->status_frame_memory,
                      XmNleftOffset, 6,
                      XmNrightAttachment, XmATTACH_FORM,
                      XmNtopAttachment, XmATTACH_FORM,
                      XmNbottomAttachment, XmATTACH_FORM,
                      NULL);
        ui->status_bar_layout_initialized = True;
    }

    Dimension w_processes = compute_status_segment_width(ui->status_frame_processes,
                                                         ui->status_processes_label,
                                                         ui->status_processes_text);
    Dimension w_cpu = compute_status_segment_width(ui->status_frame_cpu,
                                                   ui->status_cpu_label,
                                                   ui->status_cpu_text);
    Dimension w_mem = compute_status_segment_width(ui->status_frame_memory,
                                                   ui->status_memory_label,
                                                   ui->status_memory_text);

    Boolean changed = False;
    if (w_processes > ui->status_bar_width_processes) {
        ui->status_bar_width_processes = w_processes;
        XtVaSetValues(ui->status_frame_processes, XmNwidth, w_processes, NULL);
        changed = True;
    }
    if (w_cpu > ui->status_bar_width_cpu) {
        ui->status_bar_width_cpu = w_cpu;
        XtVaSetValues(ui->status_frame_cpu, XmNwidth, w_cpu, NULL);
        changed = True;
    }
    if (w_mem > ui->status_bar_width_memory) {
        ui->status_bar_width_memory = w_mem;
        XtVaSetValues(ui->status_frame_memory, XmNwidth, w_mem, NULL);
        changed = True;
    }

    (void)changed;
}

static Widget create_status_segment(Widget parent, const char *name, Widget *out_label)
{
    Widget frame = XmCreateFrame(parent, (String)name, NULL, 0);
    XtVaSetValues(frame,
                  XmNshadowType, XmSHADOW_IN,
                  XmNmarginWidth, 4,
                  XmNmarginHeight, 2,
                  XmNrecomputeSize, False,
                  NULL);
    XtManageChild(frame);

    Widget label = XtVaCreateManagedWidget(
        "statusSegmentLabel",
        xmLabelGadgetClass, frame,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNmarginLeft, 4,
        XmNmarginRight, 4,
        XmNrecomputeSize, False,
        NULL);
    if (out_label) *out_label = label;
    return frame;
}

static Widget create_status_bar(TasksUi *ui, Widget parent)
{
    Widget status_form = XmCreateForm(parent, "tasksStatusBar", NULL, 0);
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

    ui->status_frame_processes = create_status_segment(status_form, "statusProcessesFrame",
                                                       &ui->status_processes_label);
    ui->status_frame_cpu = create_status_segment(status_form, "statusCpuFrame",
                                                 &ui->status_cpu_label);
    ui->status_frame_memory = create_status_segment(status_form, "statusMemoryFrame",
                                                    &ui->status_memory_label);
    ui->status_frame_message = create_status_segment(status_form, "statusMessageFrame",
                                                     &ui->status_message_label);

    XtVaSetValues(ui->status_frame_processes,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_POSITION,
                  XmNrightPosition, 25,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  NULL);
    XtVaSetValues(ui->status_frame_cpu,
                  XmNleftAttachment, XmATTACH_POSITION,
                  XmNleftPosition, 25,
                  XmNrightAttachment, XmATTACH_POSITION,
                  XmNrightPosition, 50,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftOffset, 6,
                  NULL);
    XtVaSetValues(ui->status_frame_memory,
                  XmNleftAttachment, XmATTACH_POSITION,
                  XmNleftPosition, 50,
                  XmNrightAttachment, XmATTACH_POSITION,
                  XmNrightPosition, 75,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftOffset, 6,
                  NULL);
    XtVaSetValues(ui->status_frame_message,
                  XmNleftAttachment, XmATTACH_POSITION,
                  XmNleftPosition, 75,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftOffset, 6,
                  NULL);

    tasks_ui_status_set_label_text(ui->status_processes_label, ui->status_processes_text,
                                   sizeof(ui->status_processes_text), "Processes: 0");
    tasks_ui_status_set_label_text(ui->status_cpu_label, ui->status_cpu_text,
                                   sizeof(ui->status_cpu_text), "CPU Usage: 0%");
    tasks_ui_status_set_label_text(ui->status_memory_label, ui->status_memory_text,
                                   sizeof(ui->status_memory_text), "Physical Memory: 0% (0 GB/0 GB)");
    tasks_ui_status_set_label_text(ui->status_message_label, ui->status_message_text,
                                   sizeof(ui->status_message_text), "Status: idle");

    autosize_status_bar(ui);
    return status_form;
}

void tasks_ui_statusbar_maybe_resize(TasksUi *ui)
{
    autosize_status_bar(ui);
}

static Widget create_menu_item(Widget parent, const char *name, const char *label)
{
    XmString xm_label = tasks_ui_make_string(label);
    Widget item = XtVaCreateManagedWidget(
        name,
        xmPushButtonGadgetClass, parent,
        XmNlabelString, xm_label,
        NULL);
    XmStringFree(xm_label);
    return item;
}

static Widget create_toggle_item(Widget parent, const char *name, const char *label)
{
    XmString xm_label = tasks_ui_make_string(label);
    Widget item = XtVaCreateManagedWidget(
        name,
        xmToggleButtonGadgetClass, parent,
        XmNlabelString, xm_label,
        NULL);
    XmStringFree(xm_label);
    return item;
}

static Widget create_checkbox_item(Widget parent, const char *name, const char *label)
{
    XmString xm_label = tasks_ui_make_string(label);
    Widget toggle = XtVaCreateManagedWidget(
        name,
        xmToggleButtonGadgetClass, parent,
        XmNlabelString, xm_label,
        NULL);
    XmStringFree(xm_label);
    return toggle;
}

static Widget create_cascade_menu(Widget menu_bar, const char *label,
                                  const char *name, Widget *out_menu)
{
    Widget pulldown = XmCreatePulldownMenu(menu_bar, (String)name, NULL, 0);
    XmString xm_label = tasks_ui_make_string(label);
    XtVaCreateManagedWidget(
        (String)name,
        xmCascadeButtonGadgetClass, menu_bar,
        XmNsubMenuId, pulldown,
        XmNlabelString, xm_label,
        NULL);
    XmStringFree(xm_label);
    if (out_menu) *out_menu = pulldown;
    return pulldown;
}

static void attach_menu_bar(Widget menu_bar, Widget parent)
{
    if (!menu_bar) return;
    XtVaSetValues(menu_bar,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  NULL);
}

TasksUi *tasks_ui_create(XtAppContext app, Widget toplevel)
{
    if (!app || !toplevel) return NULL;
    TasksUi *ui = (TasksUi *)calloc(1, sizeof(TasksUi));
    if (!ui) return NULL;
    ui->app = app;
    ui->toplevel = toplevel;

    Widget form = XmCreateForm(toplevel, "tasksMainForm", NULL, 0);
    XtVaSetValues(form,
                  XmNfractionBase, 100,
                  XmNmarginWidth, 0,
                  XmNmarginHeight, 0,
                  XmNnavigationType, XmTAB_GROUP,
                  NULL);
    XtManageChild(form);
    ui->main_form = form;

    Widget menu_bar = XmCreateMenuBar(form, "tasksMenuBar", NULL, 0);
    attach_menu_bar(menu_bar, form);
    ui->menu_bar = menu_bar;
    XtManageChild(menu_bar);

    Widget file_menu = create_cascade_menu(menu_bar, "File", "fileMenu", NULL);
    ui->menu_file_connect = create_menu_item(file_menu, "fileConnect", "Connect to Remote...");
    ui->menu_file_new_window = create_menu_item(file_menu, "fileNewWindow", "New Window");
    ui->menu_file_exit = create_menu_item(file_menu, "fileExit", "Exit");

    Widget options_menu = create_cascade_menu(menu_bar, "Options", "optionsMenu", NULL);
    ui->menu_options_always_on_top = create_toggle_item(options_menu, "optionsAlwaysOnTop", "Always on Top");
    Widget update_menu = XmCreatePulldownMenu(menu_bar, "updateFrequencyMenu", NULL, 0);
    XmString update_label = tasks_ui_make_string("Update Frequency");
    XtVaCreateManagedWidget(
        (String)"updateFrequency",
        xmCascadeButtonGadgetClass, options_menu,
        XmNlabelString, update_label,
        XmNsubMenuId, update_menu,
        NULL);
    XmStringFree(update_label);
    ui->menu_options_update_1s = create_menu_item(update_menu, "update1s", "1s");
    ui->menu_options_update_2s = create_menu_item(update_menu, "update2s", "2s");
    ui->menu_options_update_5s = create_menu_item(update_menu, "update5s", "5s");
    ui->menu_options_filter_by_user = create_checkbox_item(options_menu, "filterByUser", "Filter by User");

    Widget view_menu = create_cascade_menu(menu_bar, "View", "viewMenu", NULL);
    ui->menu_view_refresh = create_menu_item(view_menu, "viewRefresh", "Refresh");
    ui->menu_view_processes = create_menu_item(view_menu, "viewProcesses", "Show Processes");
    ui->menu_view_performance = create_menu_item(view_menu, "viewPerformance", "Show Performance");
    ui->menu_view_networking = create_menu_item(view_menu, "viewNetworking", "Show Networking");

    Widget help_menu = XmCreatePulldownMenu(menu_bar, "helpMenu", NULL, 0);
    XmString help_label = tasks_ui_make_string("Help");
    Widget help_cascade = XtVaCreateManagedWidget(
        "helpCascade",
        xmCascadeButtonGadgetClass, menu_bar,
        XmNlabelString, help_label,
        XmNmnemonic, 'H',
        XmNsubMenuId, help_menu,
        NULL);
    XmStringFree(help_label);
    XtVaSetValues(menu_bar, XmNmenuHelpWidget, help_cascade, NULL);

    ui->menu_help_help = create_menu_item(help_menu, "helpView", "View Help");
    ui->menu_help_about = create_menu_item(help_menu, "helpAbout", "About");

    Widget status = create_status_bar(ui, form);

    Widget tab_stack = XmCreateTabStack(form, "tasksTabStack", NULL, 0);
    XtVaSetValues(tab_stack,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, menu_bar,
                  XmNbottomAttachment, XmATTACH_WIDGET,
                  XmNbottomWidget, status,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNleftOffset, 12,
                  XmNrightOffset, 12,
                  XmNtopOffset, 6,
                  XmNbottomOffset, 6,
                  NULL);
    ui->tab_stack = tab_stack;
    XtManageChild(tab_stack);

    ui->tab_applications = tasks_ui_create_applications_tab(ui);
    ui->tab_processes = tasks_ui_create_process_tab(ui);
    ui->tab_services = tasks_ui_create_simple_tab(ui, TASKS_TAB_SERVICES, "servicesPage",
                                                  "Services", "Service status, start/stop controls, and dependencies.");
    ui->tab_performance = tasks_ui_create_performance_tab(ui);
    ui->tab_networking = tasks_ui_create_networking_tab(ui);
    ui->tab_users = tasks_ui_create_users_tab(ui);

    Widget initial_tab = tasks_ui_get_tab_widget(ui, TASKS_TAB_PROCESSES);
    if (initial_tab) {
        XmTabStackSelectTab(initial_tab, False);
    }

    return ui;
}

void tasks_ui_destroy(TasksUi *ui)
{
    tasks_ui_destroy_applications_tab(ui);
    tasks_ui_destroy_process_tab(ui);
    tasks_ui_destroy_users_tab(ui);
    free(ui);
}

XtAppContext tasks_ui_get_app_context(TasksUi *ui)
{
    return ui ? ui->app : NULL;
}

Widget tasks_ui_get_toplevel(TasksUi *ui)
{
    return ui ? ui->toplevel : NULL;
}

int tasks_ui_get_current_tab(TasksUi *ui)
{
    if (!ui || !ui->tab_stack) return 0;
    Widget selected = XmTabStackGetSelectedTab(ui->tab_stack);
    if (!selected) return 0;
    if (selected == ui->tab_processes) return TASKS_TAB_PROCESSES;
    if (selected == ui->tab_performance) return TASKS_TAB_PERFORMANCE;
    if (selected == ui->tab_networking) return TASKS_TAB_NETWORKING;
    if (selected == ui->tab_applications) return TASKS_TAB_APPLICATIONS;
    if (selected == ui->tab_services) return TASKS_TAB_SERVICES;
    if (selected == ui->tab_users) return TASKS_TAB_USERS;
    return TASKS_TAB_PROCESSES;
}

void tasks_ui_set_current_tab(TasksUi *ui, TasksTab tab)
{
    if (!ui || !ui->tab_stack) return;
    Widget page = tasks_ui_get_tab_widget(ui, tab);
    if (!page) return;
    XmTabStackSelectTab(page, True);
}

void tasks_ui_update_status(TasksUi *ui, const char *text)
{
    if (!ui || !ui->status_message_label) return;
    tasks_ui_status_set_label_text(ui->status_message_label, ui->status_message_text,
                                   sizeof(ui->status_message_text), text ? text : "Status: idle");
}

void tasks_ui_center_on_screen(TasksUi *ui)
{
    if (!ui || !ui->toplevel) return;
    Dimension width = 0, height = 0;
    XtVaGetValues(ui->toplevel,
                  XmNwidth, &width,
                  XmNheight, &height,
                  NULL);
    Display *dpy = XtDisplay(ui->toplevel);
    int screen = DefaultScreen(dpy);
    int sw = DisplayWidth(dpy, screen);
    int sh = DisplayHeight(dpy, screen);
    Position x = (Position)((sw - (int)width) / 2);
    Position y = (Position)((sh - (int)height) / 2);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    XtVaSetValues(ui->toplevel,
                  XmNx, x,
                  XmNy, y,
                  NULL);
}

static Widget tasks_ui_get_tab_widget(TasksUi *ui, TasksTab tab)
{
    if (!ui) return NULL;
    switch (tab) {
    case TASKS_TAB_PROCESSES: return ui->tab_processes;
    case TASKS_TAB_PERFORMANCE: return ui->tab_performance;
    case TASKS_TAB_NETWORKING: return ui->tab_networking;
    case TASKS_TAB_APPLICATIONS: return ui->tab_applications;
    case TASKS_TAB_SERVICES: return ui->tab_services;
    case TASKS_TAB_USERS: return ui->tab_users;
    default: return NULL;
    }
}

void tasks_ui_update_process_count(TasksUi *ui, int total_processes)
{
    if (!ui || !ui->status_processes_label) return;
    char buffer[64];
    if (total_processes < 0) total_processes = 0;
    snprintf(buffer, sizeof(buffer), "Processes: %d", total_processes);
    Boolean changed = tasks_ui_status_set_label_text(ui->status_processes_label, ui->status_processes_text,
                                                     sizeof(ui->status_processes_text), buffer);
    if (changed) {
        tasks_ui_statusbar_maybe_resize(ui);
    }
}
