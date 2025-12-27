#include "ck-tasks-tabs.h"
#include "ck-tasks-ui-helpers.h"

#include <Xm/Form.h>
#include <Xm/LabelG.h>

#include <stdio.h>

static const TableColumnDef users_columns[] = {
    {"userName", "User", TABLE_ALIGN_LEFT, False, True, 0},
    {"userTty", "TTY", TABLE_ALIGN_LEFT, False, True, 0},
    {"loginTime", "Login Time", TABLE_ALIGN_CENTER, False, True, 0},
    {"idleTime", "Idle", TABLE_ALIGN_RIGHT, False, True, 0},
    {"hostName", "Host", TABLE_ALIGN_LEFT, False, True, 0},
    {"userPid", "PID", TABLE_ALIGN_RIGHT, True, True, 0},
};

#define USER_COLUMN_COUNT (sizeof(users_columns) / sizeof(users_columns[0]))

Widget tasks_ui_create_users_tab(TasksUi *ui)
{
    if (!ui) return NULL;
    Widget page = tasks_ui_create_page(ui, "usersPage", TASKS_TAB_USERS,
                                       "Users", "Logged-in user sessions and their resource footprints.");
    if (!page) return NULL;

    ui->users_table = table_widget_create(page, "usersTable", users_columns, USER_COLUMN_COUNT);
    if (!ui->users_table) {
        XmString message = tasks_ui_make_string("Unable to display user sessions.");
        XtVaCreateManagedWidget(
            "usersFallbackLabel",
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

    Widget table_widget = table_widget_get_widget(ui->users_table);
    if (table_widget) {
        XtVaSetValues(table_widget,
                      XmNtopAttachment, XmATTACH_FORM,
                      XmNleftAttachment, XmATTACH_FORM,
                      XmNrightAttachment, XmATTACH_FORM,
                      XmNbottomAttachment, XmATTACH_FORM,
                      XmNtopOffset, 6,
                      XmNbottomOffset, 6,
                      XmNleftOffset, 6,
                      XmNrightOffset, 6,
                      NULL);
    }
    table_widget_set_grid(ui->users_table, True);
    table_widget_set_alternate_row_colors(ui->users_table, True);
    return page;
}

void tasks_ui_destroy_users_tab(TasksUi *ui)
{
    if (!ui) return;
    if (ui->users_table) {
        table_widget_destroy(ui->users_table);
        ui->users_table = NULL;
    }
}

void tasks_ui_set_users_table(TasksUi *ui, const TasksUserEntry *entries, int count)
{
    if (!ui || !ui->users_table) return;
    table_widget_clear(ui->users_table);
    if (!entries || count <= 0) return;

    for (int i = 0; i < count; ++i) {
        char pid_buffer[16] = {0};
        if (entries[i].pid > 0) {
            snprintf(pid_buffer, sizeof(pid_buffer), "%d", (int)entries[i].pid);
        }
        const char *values[] = {
            entries[i].user,
            entries[i].tty,
            entries[i].login_time,
            entries[i].idle_time,
            entries[i].host,
            pid_buffer,
        };
        table_widget_add_row(ui->users_table, values);
    }
}
