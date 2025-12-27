#include "ck-tasks-tabs.h"
#include "ck-tasks-ctrl.h"
#include "ck-tasks-ui-helpers.h"

#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/LabelG.h>
#include <Xm/PanedW.h>
#include <Xm/ToggleBG.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static const TableColumnDef services_columns[] = {
    {"serviceOrder", "Order", TABLE_ALIGN_RIGHT, True, True, 0},
    {"serviceName", "Service", TABLE_ALIGN_LEFT, False, True, 0},
    {"serviceState", "State", TABLE_ALIGN_LEFT, False, True, 0},
};

#define SERVICE_COLUMN_COUNT (sizeof(services_columns) / sizeof(services_columns[0]))

static const TableColumnDef services_info_columns[] = {
    {"serviceInfoField", "Field", TABLE_ALIGN_LEFT, False, False, 0},
    {"serviceInfoValue", "Value", TABLE_ALIGN_LEFT, False, False, 0},
};

#define SERVICE_INFO_COLUMN_COUNT (sizeof(services_info_columns) / sizeof(services_info_columns[0]))

static void services_set_info_visible(TasksUi *ui, Boolean visible);
static void services_update_info_panel(TasksUi *ui, const TasksServiceEntry *entry);
static void services_set_selection(TasksUi *ui, Widget row_widget, int index);
static void services_clear_selection(TasksUi *ui);
static void on_services_row_press(Widget widget, XtPointer client, XEvent *event, Boolean *continue_to_dispatch);

static void services_update_info_pane_height(TasksUi *ui, Widget row_widget)
{
    if (!ui || !ui->services_info_frame || !row_widget) return;
    Dimension row_height = 0;
    XtVaGetValues(row_widget, XmNheight, &row_height, NULL);
    if (row_height < 12) {
        row_height = 24;
    }
    int min_height = (int)row_height * 2;
    int target_height = (int)row_height * 10;
    if (target_height < min_height) {
        target_height = min_height;
    }
    XtVaSetValues(ui->services_info_frame,
                  XmNpaneMinimum, min_height,
                  XmNpaneMaximum, target_height * 4,
                  XmNheight, target_height,
                  XmNallowResize, True,
                  NULL);
}

static void on_services_show_disabled_changed(Widget widget, XtPointer client, XtPointer call)
{
    (void)call;
    TasksUi *ui = (TasksUi *)client;
    if (!ui || !widget) return;
    Boolean state = XmToggleButtonGadgetGetState(widget);
    if (ui->services_show_disabled == state) return;
    ui->services_show_disabled = state;
    if (ui->controller) {
        tasks_ctrl_set_show_disabled_services(ui->controller, state);
    }
}

static void ensure_services_row_capacity(TasksUi *ui, int needed)
{
    if (!ui || needed <= ui->services_row_capacity) return;
    int new_cap = ui->services_row_capacity ? ui->services_row_capacity : 32;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    TableRow **expanded = (TableRow **)realloc(ui->services_rows, sizeof(TableRow *) * new_cap);
    if (!expanded) return;
    if (new_cap > ui->services_row_capacity) {
        memset(expanded + ui->services_row_capacity, 0, sizeof(TableRow *) * (new_cap - ui->services_row_capacity));
    }
    ui->services_rows = expanded;
    ui->services_row_capacity = new_cap;
}

static void services_set_info_visible(TasksUi *ui, Boolean visible)
{
    if (!ui || !ui->services_table || !ui->services_info_frame) return;
    if (visible) {
        XtManageChild(ui->services_info_frame);
    } else {
        XtUnmanageChild(ui->services_info_frame);
    }
}

static void services_update_info_panel(TasksUi *ui, const TasksServiceEntry *entry)
{
    if (!ui || !ui->services_info_table || !entry) return;
    table_widget_clear(ui->services_info_table);

    const char *file_path = entry->filename_path[0] ? entry->filename_path : "(none)";
    const char *link_path = entry->symlink_path[0] ? entry->symlink_path : "(none)";
    const char *values_path[] = {"File path", file_path};
    table_widget_add_row(ui->services_info_table, values_path);
    const char *values_link[] = {"Symlink path", link_path};
    table_widget_add_row(ui->services_info_table, values_link);

    for (int i = 0; i < entry->info_count; ++i) {
        const char *field_values[] = {
            entry->info_fields[i].key,
            entry->info_fields[i].value,
        };
        table_widget_add_row(ui->services_info_table, field_values);
    }
}

static void services_set_selection(TasksUi *ui, Widget row_widget, int index)
{
    if (!ui || !row_widget || index < 0) return;
    if (ui->services_selected_row == row_widget && ui->services_selected_index == index) return;
    services_clear_selection(ui);
    ui->services_selected_row = row_widget;
    ui->services_selected_index = index;
    ui->services_updates_paused = True;
    XtVaSetValues(row_widget,
                  XmNshadowThickness, 2,
                  XmNshadowType, XmSHADOW_ETCHED_IN,
                  NULL);
    if (ui->services_entries && index < ui->services_entries_count) {
        const TasksServiceEntry *entry = &ui->services_entries[index];
        if (ui->services_info_title) {
            char title[192];
            snprintf(title, sizeof(title), "Service details: %s", entry->name);
            tasks_ui_set_label_text(ui->services_info_title, title);
        }
        services_update_info_pane_height(ui, row_widget);
        services_update_info_panel(ui, entry);
        services_set_info_visible(ui, True);
    }
}

static void services_clear_selection(TasksUi *ui)
{
    if (!ui) return;
    if (ui->services_selected_row) {
        XtVaSetValues(ui->services_selected_row,
                      XmNshadowThickness, 1,
                      XmNshadowType, XmSHADOW_ETCHED_IN,
                      NULL);
    }
    ui->services_selected_row = NULL;
    ui->services_selected_index = -1;
    ui->services_updates_paused = False;
    services_set_info_visible(ui, False);
}

static void on_services_row_press(Widget widget, XtPointer client, XEvent *event, Boolean *continue_to_dispatch)
{
    (void)continue_to_dispatch;
    if (!widget || !client || !event) return;
    if (event->type != ButtonPress) return;
    TasksUi *ui = (TasksUi *)client;
    if (!ui) return;
    XtPointer user_data = NULL;
    XtVaGetValues(widget, XmNuserData, &user_data, NULL);
    int index = (int)(intptr_t)user_data - 1;
    if (index < 0) return;
    if (ui->services_selected_row == widget && ui->services_selected_index == index) {
        services_clear_selection(ui);
        return;
    }
    services_set_selection(ui, widget, index);
}

Widget tasks_ui_create_services_tab(TasksUi *ui)
{
    if (!ui) return NULL;
    Widget page = tasks_ui_create_page(ui, "servicesPage", TASKS_TAB_SERVICES,
                                       "Services", "Service status, start/stop controls, and dependencies.");
    if (!page) return NULL;

    ui->services_controls_form = XmCreateForm(page, "servicesControlsForm", NULL, 0);
    XtVaSetValues(ui->services_controls_form,
                  XmNfractionBase, 100,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNtopOffset, 4,
                  XmNleftOffset, 8,
                  XmNrightOffset, 8,
                  NULL);
    XtManageChild(ui->services_controls_form);

    XmString show_disabled_label = tasks_ui_make_string("Show disabled services");
    ui->services_show_disabled_toggle = XtVaCreateManagedWidget(
        "servicesShowDisabledToggle",
        xmToggleButtonGadgetClass, ui->services_controls_form,
        XmNlabelString, show_disabled_label,
        XmNrightAttachment, XmATTACH_FORM,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNset, ui->services_show_disabled,
        NULL);
    XmStringFree(show_disabled_label);
    XtAddCallback(ui->services_show_disabled_toggle, XmNvalueChangedCallback,
                  on_services_show_disabled_changed, (XtPointer)ui);

    ui->services_info_label = XtVaCreateManagedWidget(
        "servicesInitInfoLabel",
        xmLabelGadgetClass, ui->services_controls_form,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_WIDGET,
        XmNrightWidget, ui->services_show_disabled_toggle,
        XmNleftOffset, 0,
        XmNrightOffset, 12,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        NULL);
    ui->services_info_text[0] = '\0';
    tasks_ui_status_set_label_text(ui->services_info_label, ui->services_info_text,
                                   sizeof(ui->services_info_text),
                                   "Init system: detecting...");

    ui->services_pane = XmCreatePanedWindow(page, "servicesPane", NULL, 0);
    XtVaSetValues(ui->services_pane,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, ui->services_controls_form,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNtopOffset, 8,
                  XmNbottomOffset, 8,
                  XmNleftOffset, 6,
                  XmNrightOffset, 6,
                  XmNspacing, 6,
                  NULL);
    XtManageChild(ui->services_pane);

    ui->services_table = table_widget_create(ui->services_pane, "servicesTable",
                                             services_columns, SERVICE_COLUMN_COUNT);
    if (!ui->services_table) {
        XmString message = tasks_ui_make_string("Unable to display services.");
        XtVaCreateManagedWidget(
            "servicesFallbackLabel",
            xmLabelGadgetClass, page,
            XmNlabelString, message,
            XmNalignment, XmALIGNMENT_CENTER,
            XmNtopAttachment, XmATTACH_WIDGET,
            XmNtopWidget, ui->services_controls_form,
            XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM,
            XmNmarginWidth, 8,
            XmNmarginHeight, 8,
            XmNtopOffset, 10,
            NULL);
        XmStringFree(message);
        return page;
    }

    Widget table_widget = table_widget_get_widget(ui->services_table);
    if (table_widget) {
        XtVaSetValues(table_widget,
                      XmNpaneMinimum, 120,
                      XmNallowResize, True,
                      NULL);
    }
    table_widget_set_grid(ui->services_table, True);
    table_widget_set_alternate_row_colors(ui->services_table, True);

    ui->services_info_frame = XmCreateFrame(ui->services_pane, "servicesInfoFrame", NULL, 0);
    XtVaSetValues(ui->services_info_frame,
                  XmNshadowType, XmSHADOW_ETCHED_IN,
                  XmNheight, 180,
                  XmNpaneMinimum, 96,
                  XmNallowResize, True,
                  NULL);

    Widget info_form = XmCreateForm(ui->services_info_frame, "servicesInfoForm", NULL, 0);
    XtVaSetValues(info_form,
                  XmNfractionBase, 100,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNmarginWidth, 6,
                  XmNmarginHeight, 6,
                  NULL);
    XtManageChild(info_form);

    ui->services_info_title = XtVaCreateManagedWidget(
        "servicesInfoTitle",
        xmLabelGadgetClass, info_form,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNtopAttachment, XmATTACH_FORM,
        NULL);
    tasks_ui_set_label_text(ui->services_info_title, "Service details");

    ui->services_info_table = table_widget_create(info_form, "servicesInfoTable",
                                                  services_info_columns, SERVICE_INFO_COLUMN_COUNT);
    if (ui->services_info_table) {
        Widget info_table_widget = table_widget_get_widget(ui->services_info_table);
        if (info_table_widget) {
            XtVaSetValues(info_table_widget,
                          XmNtopAttachment, XmATTACH_WIDGET,
                          XmNtopWidget, ui->services_info_title,
                          XmNleftAttachment, XmATTACH_FORM,
                          XmNrightAttachment, XmATTACH_FORM,
                          XmNbottomAttachment, XmATTACH_FORM,
                          XmNtopOffset, 6,
                          NULL);
        }
        table_widget_set_grid(ui->services_info_table, True);
        table_widget_set_alternate_row_colors(ui->services_info_table, True);
    }

    ui->services_row_count = 0;
    ui->services_row_capacity = 0;
    ui->services_entries = NULL;
    ui->services_entries_count = 0;
    ui->services_selected_row = NULL;
    ui->services_selected_index = -1;
    ui->services_updates_paused = False;
    services_set_info_visible(ui, False);
    return page;
}

void tasks_ui_destroy_services_tab(TasksUi *ui)
{
    if (!ui) return;
    if (ui->services_table) {
        table_widget_destroy(ui->services_table);
        ui->services_table = NULL;
    }
    if (ui->services_info_table) {
        table_widget_destroy(ui->services_info_table);
        ui->services_info_table = NULL;
    }
    free(ui->services_rows);
    ui->services_rows = NULL;
    ui->services_row_count = 0;
    ui->services_row_capacity = 0;
    ui->services_entries = NULL;
    ui->services_entries_count = 0;
    ui->services_controls_form = NULL;
    ui->services_show_disabled_toggle = NULL;
    ui->services_info_label = NULL;
    ui->services_pane = NULL;
    ui->services_info_frame = NULL;
    ui->services_info_title = NULL;
    ui->services_selected_row = NULL;
    ui->services_selected_index = -1;
    ui->services_updates_paused = False;
}

void tasks_ui_set_services_table(TasksUi *ui, const TasksServiceEntry *entries, int count,
                                 const TasksInitInfo *init_info)
{
    if (!ui || !ui->services_table) return;
    if (ui->services_updates_paused) return;
    ui->services_entries = entries;
    ui->services_entries_count = (entries && count > 0) ? count : 0;
    int desired = (entries && count > 0) ? count : 0;
    ensure_services_row_capacity(ui, desired);

    if (ui->services_info_label) {
        char info[160];
        const char *name = (init_info && init_info->init_name[0]) ? init_info->init_name : "unknown";
        const char *detail = (init_info && init_info->init_detail[0]) ? init_info->init_detail : NULL;
        if (detail && detail[0]) {
            snprintf(info, sizeof(info), "Init system: %s (%s)", name, detail);
        } else {
            snprintf(info, sizeof(info), "Init system: %s", name);
        }
        tasks_ui_status_set_label_text(ui->services_info_label, ui->services_info_text,
                                       sizeof(ui->services_info_text), info);
    }

    int old_count = ui->services_row_count;
    int common = (old_count < desired) ? old_count : desired;

    for (int i = 0; i < common; ++i) {
        const char *values[] = {
            entries[i].order,
            entries[i].name,
            entries[i].state,
        };
        const char *sort_values[] = {
            entries[i].order,
            NULL,
            NULL,
        };
        if (ui->services_rows && ui->services_rows[i]) {
            table_widget_update_row_with_sort_values(ui->services_rows[i], values, sort_values);
            Widget row_widget = table_row_get_widget(ui->services_rows[i]);
            if (row_widget) {
                XtVaSetValues(row_widget, XmNuserData, (XtPointer)(intptr_t)(i + 1), NULL);
            }
        }
    }

    for (int i = common; i < desired; ++i) {
        const char *values[] = {
            entries[i].order,
            entries[i].name,
            entries[i].state,
        };
        const char *sort_values[] = {
            entries[i].order,
            NULL,
            NULL,
        };
        TableRow *row = table_widget_add_row_with_sort_values(ui->services_table, values, sort_values);
        if (ui->services_rows) {
            ui->services_rows[i] = row;
        }
        Widget row_widget = table_row_get_widget(row);
        if (row_widget) {
            XtVaSetValues(row_widget, XmNuserData, (XtPointer)(intptr_t)(i + 1), NULL);
            XtAddEventHandler(row_widget, ButtonPressMask, False, on_services_row_press, (XtPointer)ui);
        }
    }

    for (int i = desired; i < old_count; ++i) {
        if (ui->services_rows && ui->services_rows[i]) {
            table_widget_remove_row(ui->services_table, ui->services_rows[i]);
            ui->services_rows[i] = NULL;
        }
    }
    ui->services_row_count = desired;
}
