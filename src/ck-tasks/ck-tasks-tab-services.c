#include "ck-tasks-tabs.h"
#include "ck-tasks-ctrl.h"
#include "ck-tasks-ui-helpers.h"

#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/ToggleBG.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const TableColumnDef services_columns[] = {
    {"serviceOrder", "Order", TABLE_ALIGN_RIGHT, True, True, 0},
    {"serviceName", "Service", TABLE_ALIGN_LEFT, False, True, 0},
    {"serviceState", "State", TABLE_ALIGN_LEFT, False, True, 0},
};

#define SERVICE_COLUMN_COUNT (sizeof(services_columns) / sizeof(services_columns[0]))

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

    ui->services_table = table_widget_create(page, "servicesTable", services_columns, SERVICE_COLUMN_COUNT);
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
                      XmNtopAttachment, XmATTACH_WIDGET,
                      XmNtopWidget, ui->services_controls_form,
                      XmNleftAttachment, XmATTACH_FORM,
                      XmNrightAttachment, XmATTACH_FORM,
                      XmNbottomAttachment, XmATTACH_FORM,
                      XmNtopOffset, 8,
                      XmNbottomOffset, 8,
                      XmNleftOffset, 6,
                      XmNrightOffset, 6,
                      NULL);
    }
    table_widget_set_grid(ui->services_table, True);
    table_widget_set_alternate_row_colors(ui->services_table, True);

    ui->services_row_count = 0;
    ui->services_row_capacity = 0;
    ui->services_entries = NULL;
    ui->services_entries_count = 0;
    return page;
}

void tasks_ui_destroy_services_tab(TasksUi *ui)
{
    if (!ui) return;
    if (ui->services_table) {
        table_widget_destroy(ui->services_table);
        ui->services_table = NULL;
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
}

void tasks_ui_set_services_table(TasksUi *ui, const TasksServiceEntry *entries, int count,
                                 const TasksInitInfo *init_info)
{
    if (!ui || !ui->services_table) return;
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
    }

    for (int i = desired; i < old_count; ++i) {
        if (ui->services_rows && ui->services_rows[i]) {
            table_widget_remove_row(ui->services_table, ui->services_rows[i]);
            ui->services_rows[i] = NULL;
        }
    }
    ui->services_row_count = desired;
}
