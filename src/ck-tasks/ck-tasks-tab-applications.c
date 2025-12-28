#include "ck-tasks-tabs.h"
#include "ck-tasks-ctrl.h"
#include "ck-tasks-ui-helpers.h"

#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/PushBG.h>
#include <Xm/TextF.h>
#include <X11/Xlib.h>

#include <stdint.h>
#include <stdio.h>

static void on_apps_row_press(Widget widget, XtPointer client, XEvent *event, Boolean *continue_to_dispatch);

static const TableColumnDef apps_columns[] = {
    {"appTitle", "Application", TABLE_ALIGN_LEFT, False, True, 0},
    {"appPid", "PID", TABLE_ALIGN_RIGHT, True, True, 0},
    {"appWindows", "Windows", TABLE_ALIGN_RIGHT, True, True, 0},
    {"appCommand", "Command", TABLE_ALIGN_LEFT, False, True, 0},
    {"appClass", "Class", TABLE_ALIGN_LEFT, False, True, 0},
};

#define APPS_COLUMN_COUNT (sizeof(apps_columns) / sizeof(apps_columns[0]))

Widget tasks_ui_create_applications_tab(TasksUi *ui)
{
    Widget page = tasks_ui_create_page(ui, "applicationsPage", TASKS_TAB_APPLICATIONS,
                                       "Applications", "Windows grouped by process.");

    ui->apps_table = ck_table_create_standard(page, "appsTable", apps_columns, APPS_COLUMN_COUNT);
    if (!ui->apps_table) {
        XmString message = tasks_ui_make_string("Unable to display applications.");
        XtVaCreateManagedWidget(
            "appsTableFallbackLabel",
            xmLabelGadgetClass, page,
            XmNlabelString, message,
            XmNalignment, XmALIGNMENT_CENTER,
            XmNtopAttachment, XmATTACH_FORM,
            XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM,
            XmNmarginWidth, 8,
            XmNmarginHeight, 8,
            NULL);
        XmStringFree(message);
        return page;
    }

    Widget controls = XmCreateForm(page, "appsControlsForm", NULL, 0);
    XtVaSetValues(controls,
                  XmNfractionBase, 100,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftOffset, 8,
                  XmNrightOffset, 8,
                  XmNbottomOffset, 8,
                  XmNheight, 48,
                  NULL);
    XtManageChild(controls);

    XmString filter_label_text = tasks_ui_make_string("Filter:");
    Widget filter_label = XtVaCreateManagedWidget(
        "appsFilterLabel",
        xmLabelGadgetClass, controls,
        XmNlabelString, filter_label_text,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNbottomOffset, 4,
        NULL);
    XmStringFree(filter_label_text);

    Widget filter_field = XmCreateTextField(controls, "appsFilterField", NULL, 0);
    XtVaSetValues(filter_field,
                  XmNcolumns, 28,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_WIDGET,
                  XmNleftWidget, filter_label,
                  XmNleftOffset, 10,
                  XmNrightAttachment, XmATTACH_POSITION,
                  XmNrightPosition, 70,
                  XmNbottomOffset, 4,
                  XmNmarginHeight, 2,
                  NULL);
    XtManageChild(filter_field);
    ui->apps_search_field = filter_field;

    XmString close_label = tasks_ui_make_string("Close Window");
    Widget close_button = XtVaCreateManagedWidget(
        "appsCloseButton",
        xmPushButtonGadgetClass, controls,
        XmNlabelString, close_label,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNrightOffset, 0,
        XmNleftAttachment, XmATTACH_WIDGET,
        XmNleftWidget, filter_field,
        XmNleftOffset, 10,
        XmNbottomOffset, 4,
        NULL);
    XmStringFree(close_label);
    ui->apps_close_button = close_button;

    Widget table_widget = ck_table_get_widget(ui->apps_table);
    if (table_widget) {
        XtVaSetValues(table_widget,
                      XmNtopAttachment, XmATTACH_FORM,
                      XmNleftAttachment, XmATTACH_FORM,
                      XmNrightAttachment, XmATTACH_FORM,
                      XmNbottomAttachment, XmATTACH_WIDGET,
                      XmNbottomWidget, controls,
                      XmNtopOffset, 6,
                      XmNbottomOffset, 8,
                      XmNleftOffset, 6,
                      XmNrightOffset, 6,
                      NULL);
    }
    ck_table_set_grid(ui->apps_table, True);
    ck_table_set_alternate_row_colors(ui->apps_table, True);

    return page;
}

void tasks_ui_destroy_applications_tab(TasksUi *ui)
{
    if (ui && ui->apps_table) {
        ck_table_destroy(ui->apps_table);
        ui->apps_table = NULL;
    }
    ui->apps_selected_row = NULL;
}

void tasks_ui_set_applications_table(TasksUi *ui, const TasksApplicationEntry *entries, int count)
{
    if (!ui || !ui->apps_table) return;
    ck_table_clear(ui->apps_table);
    ui->apps_selected_row = NULL;
    if (!entries || count <= 0) return;

    for (int i = 0; i < count; ++i) {
        const TasksApplicationEntry *entry = &entries[i];
        char pid_buffer[16] = {0};
        char windows_buffer[16] = {0};
        char pid_sort_buffer[16] = {0};
        char windows_sort_buffer[16] = {0};
        if (entry->pid_known) {
            snprintf(pid_buffer, sizeof(pid_buffer), "%d", (int)entry->pid);
            snprintf(pid_sort_buffer, sizeof(pid_sort_buffer), "%d", (int)entry->pid);
        } else {
            pid_buffer[0] = '\0';
            pid_sort_buffer[0] = '\0';
        }
        snprintf(windows_buffer, sizeof(windows_buffer), "%d", entry->window_count);
        snprintf(windows_sort_buffer, sizeof(windows_sort_buffer), "%d", entry->window_count);
        const char *values[] = {
            entry->title,
            pid_buffer,
            windows_buffer,
            entry->command,
            entry->wm_class,
        };
        const char *sort_values[] = {
            NULL,
            pid_sort_buffer,
            windows_sort_buffer,
            NULL,
            NULL,
        };
        TableRow *row = ck_table_add_row_with_sort_values(ui->apps_table, values, sort_values);
        Widget row_widget = ck_table_row_get_widget(row);
        if (row_widget) {
            XtVaSetValues(row_widget, XmNuserData, (XtPointer)(intptr_t)i, NULL);
            XtAddEventHandler(row_widget, ButtonPressMask, False, on_apps_row_press, (XtPointer)ui);
        }
    }
}

static void on_apps_row_press(Widget widget, XtPointer client, XEvent *event, Boolean *continue_to_dispatch)
{
    (void)continue_to_dispatch;
    if (!event || event->type != ButtonPress) return;
    TasksUi *ui = (TasksUi *)client;
    if (!ui || !ui->controller || !widget) return;
    XtPointer data = NULL;
    XtVaGetValues(widget, XmNuserData, &data, NULL);
    int index = data ? (int)(intptr_t)data : -1;
    if (index < 0) return;
    tasks_ctrl_set_selected_application(ui->controller, index);
    if (ui->apps_selected_row && ui->apps_selected_row != widget) {
        XtVaSetValues(ui->apps_selected_row, XmNshadowThickness, 1, XmNshadowType, XmSHADOW_OUT, NULL);
    }
    ui->apps_selected_row = widget;
    XtVaSetValues(widget, XmNshadowThickness, 2, XmNshadowType, XmSHADOW_ETCHED_IN, NULL);
}
