#include "ck-tasks-tabs.h"
#include "ck-tasks-ctrl.h"
#include "ck-tasks-ui-helpers.h"

#include <Xm/Form.h>
#include <Xm/ScrolledW.h>
#include <Xm/ScrollBar.h>
#include <Xm/TextF.h>
#include <Xm/ToggleBG.h>
#include <Xm/LabelG.h>
#include <X11/Intrinsic.h>
#include <X11/Xlib.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const TableColumnDef process_columns[] = {
    {"processName", "Image Name", TABLE_ALIGN_LEFT, False, True, 0},
    {"processPid", "PID", TABLE_ALIGN_RIGHT, True, True, 0},
    {"processCpu", "CPU (%)", TABLE_ALIGN_RIGHT, True, True, 0},
    {"processMemory", "Memory (MB)", TABLE_ALIGN_RIGHT, True, True, 0},
    {"processThreads", "Threads", TABLE_ALIGN_RIGHT, True, True, 0},
    {"processUser", "User/Session", TABLE_ALIGN_LEFT, False, True, 0},
};

#define PROCESS_COLUMN_COUNT (sizeof(process_columns) / sizeof(process_columns[0]))

static CkTable *g_process_table = NULL;

static const char *process_table_get_text(void *context,
                                          const void *entries,
                                          int row,
                                          int column,
                                          char *buffer,
                                          size_t buffer_len)
{
    (void)context;
    const TasksProcessEntry *list = (const TasksProcessEntry *)entries;
    if (!list || row < 0) return "";
    const TasksProcessEntry *entry = &list[row];
    switch (column) {
    case 0:
        return entry->name;
    case 1:
        snprintf(buffer, buffer_len, "%d", (int)entry->pid);
        return buffer;
    case 2:
        snprintf(buffer, buffer_len, "%.1f", entry->cpu_percent);
        return buffer;
    case 3:
        snprintf(buffer, buffer_len, "%.0f", entry->memory_mb);
        return buffer;
    case 4:
        snprintf(buffer, buffer_len, "%d", entry->threads);
        return buffer;
    case 5:
        return entry->user;
    default:
        return "";
    }
}

static double process_table_get_number(void *context,
                                       const void *entries,
                                       int row,
                                       int column,
                                       Boolean *has_value)
{
    (void)context;
    const TasksProcessEntry *list = (const TasksProcessEntry *)entries;
    if (!list || row < 0) {
        if (has_value) *has_value = False;
        return 0.0;
    }
    const TasksProcessEntry *entry = &list[row];
    if (has_value) *has_value = True;
    switch (column) {
    case 1:
        return (double)entry->pid;
    case 2:
        return entry->cpu_percent;
    case 3:
        return entry->memory_mb;
    case 4:
        return (double)entry->threads;
    default:
        if (has_value) *has_value = False;
        return 0.0;
    }
}

static void process_table_viewport_changed(void *context)
{
    TasksUi *ui = (TasksUi *)context;
    if (ui && ui->controller) {
        tasks_ctrl_handle_viewport_change(ui->controller);
    }
}

Widget tasks_ui_create_process_tab(TasksUi *ui)
{
    Widget page = tasks_ui_create_page(ui, "processesPage", TASKS_TAB_PROCESSES,
                                       "Processes", "");
    XmString toggle_label = tasks_ui_make_string("Show only my processes");
    Widget toggle = XtVaCreateManagedWidget(
        "processFilterToggle",
        xmToggleButtonGadgetClass, page,
        XmNlabelString, toggle_label,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_FORM,
        XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNleftOffset, 8,
        XmNrightOffset, 8,
        NULL);
    XmStringFree(toggle_label);
    ui->process_filter_toggle = toggle;

    Widget filter_form = XmCreateForm(page, "processFilterForm", NULL, 0);
    XtVaSetValues(filter_form,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNleftOffset, 8,
                  XmNrightOffset, 8,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNbottomOffset, 8,
                  XmNheight, 48,
                  NULL);
    XtManageChild(filter_form);

    XmString filter_label_text = tasks_ui_make_string("Filter:");
    Widget filter_label = XtVaCreateManagedWidget(
        "processFilterLabel",
        xmLabelGadgetClass, filter_form,
        XmNlabelString, filter_label_text,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNbottomOffset, 4,
        NULL);
    XmStringFree(filter_label_text);

    Widget filter_field = XmCreateTextField(filter_form, "processFilterField", NULL, 0);
    XtVaSetValues(filter_field,
                  XmNcolumns, 32,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_WIDGET,
                  XmNleftWidget, filter_label,
                  XmNleftOffset, 10,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNrightOffset, 2,
                  XmNbottomOffset, 4,
                  XmNmarginHeight, 2,
                  NULL);
    XtManageChild(filter_field);
    ui->process_search_field = filter_field;

    Widget process_area = XmCreateForm(page, "processListArea", NULL, 0);
    XtVaSetValues(process_area,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, toggle,
                  XmNtopOffset, 8,
                  XmNbottomAttachment, XmATTACH_WIDGET,
                  XmNbottomWidget, filter_form,
                  XmNbottomOffset, 8,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNleftOffset, 8,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNrightOffset, 8,
                  NULL);
    XtManageChild(process_area);

    Widget scrollbar = XmCreateScrollBar(process_area, "processScrollBar", NULL, 0);
    XtVaSetValues(scrollbar,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNwidth, 20,
                  NULL);
    XtManageChild(scrollbar);
    ui->process_scrollbar = scrollbar;

    ui->process_table = ck_table_create_virtual(process_area, "processListScroll",
                                                process_columns, PROCESS_COLUMN_COUNT);
    g_process_table = ui->process_table;
    if (ui->process_table) {
        Widget scroll = ck_table_get_widget(ui->process_table);
        XtVaSetValues(scroll,
                      XmNtopAttachment, XmATTACH_FORM,
                      XmNtopOffset, 0,
                      XmNleftAttachment, XmATTACH_FORM,
                      XmNleftOffset, 0,
                      XmNrightAttachment, XmATTACH_WIDGET,
                      XmNrightWidget, scrollbar,
                      XmNrightOffset, 4,
                      XmNbottomAttachment, XmATTACH_FORM,
                      NULL);
        ck_table_set_virtual_row_spacing(ui->process_table, 4);
        ck_table_set_virtual_callbacks(ui->process_table,
                                       process_table_get_text,
                                       process_table_get_number,
                                       NULL,
                                       ui);
        ck_table_set_virtual_viewport_changed_callback(ui->process_table,
                                                       process_table_viewport_changed,
                                                       ui);
    }

    return page;
}

void tasks_ui_destroy_process_tab(TasksUi *ui)
{
    if (ui && ui->process_table) {
        if (g_process_table == ui->process_table) {
            g_process_table = NULL;
        }
        ck_table_destroy(ui->process_table);
        ui->process_table = NULL;
    }
}

void tasks_ui_set_processes(TasksUi *ui, const TasksProcessEntry *entries, int count)
{
    if (!ui || !ui->process_table) return;
    ck_table_set_virtual_data(ui->process_table, entries, count);
}

void tasks_ui_set_process_row_window(int start)
{
    if (!g_process_table) return;
    ck_table_set_virtual_row_window(g_process_table, start);
}

int tasks_ui_get_process_row_page_size(void)
{
    if (!g_process_table) return 0;
    return ck_table_get_virtual_row_page_size(g_process_table);
}
