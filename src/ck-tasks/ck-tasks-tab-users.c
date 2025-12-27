#include "ck-tasks-tabs.h"
#include "ck-tasks-ui-helpers.h"

#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/MessageB.h>
#include <Xm/PushBG.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <termios.h>
#include <unistd.h>

static const TableColumnDef users_columns[] = {
    {"userName", "User", TABLE_ALIGN_LEFT, False, True, 0},
    {"userTty", "TTY", TABLE_ALIGN_LEFT, False, True, 0},
    {"loginTime", "Login Time", TABLE_ALIGN_CENTER, False, True, 0},
    {"idleTime", "Idle", TABLE_ALIGN_RIGHT, True, True, 0},
    {"hostName", "Host", TABLE_ALIGN_LEFT, False, True, 0},
    {"userPid", "PID", TABLE_ALIGN_RIGHT, True, True, 0},
};

#define USER_COLUMN_COUNT (sizeof(users_columns) / sizeof(users_columns[0]))

typedef struct {
    TasksUi *ui;
    char user[32];
    char tty[32];
    pid_t pid;
    pid_t pgrp;
    int affected_count;
    char affected_names[256];
} UsersLogoutDialogData;

static void destroy_dialog_cb(Widget widget, XtPointer client, XtPointer call)
{
    (void)widget;
    (void)call;
    Widget dialog = (Widget)client;
    if (dialog) XtDestroyWidget(dialog);
}

static void users_set_selection(TasksUi *ui, Widget row_widget, int index);
static void users_clear_selection(TasksUi *ui);
static void on_users_row_press(Widget widget, XtPointer client, XEvent *event, Boolean *continue_to_dispatch);
static void on_users_logout_activate(Widget widget, XtPointer client, XtPointer call);
static void on_users_logout_confirm(Widget widget, XtPointer client, XtPointer call);
static void on_users_logout_cancel(Widget widget, XtPointer client, XtPointer call);

static void ensure_users_row_capacity(TasksUi *ui, int needed)
{
    if (!ui || needed <= ui->users_row_capacity) return;
    int new_cap = ui->users_row_capacity ? ui->users_row_capacity : 16;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    TableRow **expanded = (TableRow **)realloc(ui->users_rows, sizeof(TableRow *) * new_cap);
    if (!expanded) return;
    if (new_cap > ui->users_row_capacity) {
        memset(expanded + ui->users_row_capacity, 0, sizeof(TableRow *) * (new_cap - ui->users_row_capacity));
    }
    ui->users_rows = expanded;
    ui->users_row_capacity = new_cap;
}

static void users_update_controls(TasksUi *ui)
{
    if (!ui) return;
    Boolean has_selection = (ui->users_selected_row != NULL && ui->users_selected_index >= 0);
    if (ui->users_logout_button) {
        XtVaSetValues(ui->users_logout_button, XmNsensitive, has_selection, NULL);
    }
    if (ui->users_logout_status_label) {
        if (has_selection) {
            tasks_ui_set_label_text(ui->users_logout_status_label, "User row selected; table updates are paused.");
        } else {
            tasks_ui_set_label_text(ui->users_logout_status_label, "");
        }
    }
}

static void users_set_selection(TasksUi *ui, Widget row_widget, int index)
{
    if (!ui || !row_widget || index < 0) return;
    if (ui->users_selected_row == row_widget && ui->users_selected_index == index) return;
    users_clear_selection(ui);
    ui->users_selected_row = row_widget;
    ui->users_selected_index = index;
    ui->users_updates_paused = True;
    XtVaSetValues(row_widget,
                  XmNshadowThickness, 2,
                  XmNshadowType, XmSHADOW_ETCHED_IN,
                  NULL);
    users_update_controls(ui);
}

static void users_clear_selection(TasksUi *ui)
{
    if (!ui) return;
    if (ui->users_selected_row) {
        XtVaSetValues(ui->users_selected_row,
                      XmNshadowThickness, 1,
                      XmNshadowType, XmSHADOW_ETCHED_IN,
                      NULL);
    }
    ui->users_selected_row = NULL;
    ui->users_selected_index = -1;
    ui->users_updates_paused = False;
    users_update_controls(ui);
}

static int users_open_tty(const char *tty, char *path_out, size_t path_len)
{
    if (!tty || tty[0] == '\0' || !path_out || path_len == 0) return -1;
    if (tty[0] == '/') {
        snprintf(path_out, path_len, "%s", tty);
    } else {
        snprintf(path_out, path_len, "/dev/%s", tty);
    }
    path_out[path_len - 1] = '\0';
    return open(path_out, O_RDONLY | O_NOCTTY);
}

static int users_parse_proc_stat(const char *path, long *out_pgrp, long *out_tty_nr)
{
    if (!path || !out_pgrp || !out_tty_nr) return -1;
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    char buf[4096];
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    char *end = strrchr(buf, ')');
    if (!end) return -1;
    const char *rest = end + 1;
    char state = 0;
    long ppid = 0;
    long pgrp = 0;
    long session = 0;
    long tty_nr = 0;
    long tpgid = 0;
    if (sscanf(rest, " %c %ld %ld %ld %ld %ld", &state, &ppid, &pgrp, &session, &tty_nr, &tpgid) < 6) {
        return -1;
    }
    *out_pgrp = pgrp;
    *out_tty_nr = tty_nr;
    return 0;
}

static long users_encode_dev(dev_t device)
{
    unsigned int major_id = (unsigned int)major(device);
    unsigned int minor_id = (unsigned int)minor(device);
    unsigned int encoded = ((major_id & 0xfffU) << 8)
                           | (minor_id & 0xffU)
                           | ((minor_id & ~0xffU) << 12);
    return (long)encoded;
}

static pid_t users_find_foreground_pgrp_for_tty(const char *tty)
{
    if (!tty || tty[0] == '\0') return -1;
    char path[128] = {0};
    if (tty[0] == '/') {
        snprintf(path, sizeof(path), "%s", tty);
    } else {
        snprintf(path, sizeof(path), "/dev/%s", tty);
    }
    path[sizeof(path) - 1] = '\0';

    struct stat st = {0};
    if (stat(path, &st) != 0) return -1;
    long target_tty_nr = users_encode_dev(st.st_rdev);

    DIR *dir = opendir("/proc");
    if (!dir) return -1;
    pid_t result = -1;
    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
        char stat_path[512];
        snprintf(stat_path, sizeof(stat_path), "/proc/%s/stat", ent->d_name);

        FILE *fp = fopen(stat_path, "r");
        if (!fp) continue;
        char buf[4096];
        if (!fgets(buf, sizeof(buf), fp)) {
            fclose(fp);
            continue;
        }
        fclose(fp);

        char *end = strrchr(buf, ')');
        if (!end) continue;
        const char *rest = end + 1;
        char state = 0;
        long ppid = 0;
        long pgrp = 0;
        long session = 0;
        long tty_nr = 0;
        long tpgid = 0;
        if (sscanf(rest, " %c %ld %ld %ld %ld %ld", &state, &ppid, &pgrp, &session, &tty_nr, &tpgid) < 6) {
            continue;
        }
        if (tty_nr != target_tty_nr) continue;
        if (tpgid > 0) {
            result = (pid_t)tpgid;
            break;
        }
    }
    closedir(dir);
    return result;
}

static int users_describe_processes_in_pgrp(pid_t pgrp, char *buffer, size_t buffer_len)
{
    if (!buffer || buffer_len == 0) return 0;
    buffer[0] = '\0';
    if (pgrp <= 0) return 0;

    DIR *dir = opendir("/proc");
    if (!dir) return 0;

    int count = 0;
    int names_added = 0;
    int truncated = 0;
    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
        char stat_path[512];
        snprintf(stat_path, sizeof(stat_path), "/proc/%s/stat", ent->d_name);
        long proc_pgrp = 0;
        long tty_nr = 0;
        if (users_parse_proc_stat(stat_path, &proc_pgrp, &tty_nr) != 0) continue;
        if ((pid_t)proc_pgrp != pgrp) continue;
        count++;
        if (names_added >= 5) {
            truncated = 1;
            continue;
        }
        char comm_path[512];
        snprintf(comm_path, sizeof(comm_path), "/proc/%s/comm", ent->d_name);
        FILE *fp = fopen(comm_path, "r");
        if (!fp) continue;
        char name[128] = {0};
        if (!fgets(name, sizeof(name), fp)) {
            fclose(fp);
            continue;
        }
        fclose(fp);
        size_t nlen = strlen(name);
        while (nlen > 0 && (name[nlen - 1] == '\n' || name[nlen - 1] == '\r')) {
            name[nlen - 1] = '\0';
            nlen--;
        }
        if (name[0] == '\0') continue;

        if (names_added == 0) {
            snprintf(buffer, buffer_len, "%s", name);
        } else {
            size_t used = strlen(buffer);
            if (used + 2 < buffer_len) {
                strncat(buffer, ", ", buffer_len - used - 1);
                used = strlen(buffer);
                strncat(buffer, name, buffer_len - used - 1);
            }
        }
        names_added++;
    }
    closedir(dir);

    if (truncated) {
        size_t used = strlen(buffer);
        if (used + 5 < buffer_len) {
            strncat(buffer, ", ...", buffer_len - used - 1);
        } else if (buffer_len >= 4) {
            buffer[buffer_len - 4] = '.';
            buffer[buffer_len - 3] = '.';
            buffer[buffer_len - 2] = '.';
            buffer[buffer_len - 1] = '\0';
        }
    }
    return count;
}

static void show_error_dialog(TasksUi *ui, const char *message)
{
    if (!ui || !message) return;
    Widget dialog = XmCreateErrorDialog(tasks_ui_get_toplevel(ui),
                                        "usersLogoutErrorDialog", NULL, 0);
    XmString msg = XmStringCreateLocalized((String)message);
    XtVaSetValues(dialog, XmNmessageString, msg, NULL);
    XmStringFree(msg);
    XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));
    XtAddCallback(dialog, XmNokCallback, destroy_dialog_cb, dialog);
    XtManageChild(dialog);
}

static void on_users_row_press(Widget widget, XtPointer client, XEvent *event, Boolean *continue_to_dispatch)
{
    (void)continue_to_dispatch;
    if (!event || event->type != ButtonPress) return;
    TasksUi *ui = (TasksUi *)client;
    if (!ui || !widget) return;

    XtPointer data = NULL;
    XtVaGetValues(widget, XmNuserData, &data, NULL);
    int encoded = data ? (int)(intptr_t)data : 0;
    int index = encoded > 0 ? (encoded - 1) : -1;
    if (index < 0) return;

    if (ui->users_selected_row == widget && ui->users_selected_index == index) {
        users_clear_selection(ui);
        return;
    }
    users_set_selection(ui, widget, index);
}

static void on_users_logout_activate(Widget widget, XtPointer client, XtPointer call)
{
    (void)widget;
    (void)call;
    TasksUi *ui = (TasksUi *)client;
    if (!ui) return;
    if (!ui->users_selected_row || ui->users_selected_index < 0) return;
    if (!ui->users_entries || ui->users_selected_index >= ui->users_entries_count) return;

    const TasksUserEntry *entry = &ui->users_entries[ui->users_selected_index];

    char tty_path[128] = {0};
    int fd = users_open_tty(entry->tty, tty_path, sizeof(tty_path));
    if (fd < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Unable to open %s: %s", tty_path[0] ? tty_path : "TTY", strerror(errno));
        show_error_dialog(ui, msg);
        return;
    }
    pid_t pgrp = tcgetpgrp(fd);
    int saved_errno = errno;
    close(fd);
    if (pgrp <= 0) {
        pgrp = users_find_foreground_pgrp_for_tty(entry->tty);
    }
    if (pgrp <= 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Unable to determine foreground process group for %s: %s", tty_path, strerror(saved_errno));
        show_error_dialog(ui, msg);
        return;
    }

    char affected_names[256] = {0};
    int affected = users_describe_processes_in_pgrp(pgrp, affected_names, sizeof(affected_names));

    UsersLogoutDialogData *data = (UsersLogoutDialogData *)calloc(1, sizeof(*data));
    if (!data) return;
    data->ui = ui;
    snprintf(data->user, sizeof(data->user), "%s", entry->user);
    snprintf(data->tty, sizeof(data->tty), "%s", entry->tty);
    data->pid = entry->pid;
    data->pgrp = pgrp;
    data->affected_count = affected;
    snprintf(data->affected_names, sizeof(data->affected_names), "%s", affected_names);

    char message[512];
    if (affected <= 0) {
        snprintf(message, sizeof(message),
                 "No running processes found for this user session.\n\n"
                 "Do you really want to send the HUP signal to the foreground process group %d on %s (user %s, PID %d)?",
                 (int)pgrp, entry->tty, entry->user, (int)entry->pid);
    } else {
        snprintf(message, sizeof(message),
                 "This will send the HUP signal to foreground process group %d on %s.\n"
                 "Affected processes (in that process group): %d (%s)\n\n"
                 "Do you really want to continue (user %s, PID %d)?",
                 (int)pgrp, entry->tty, affected, affected_names[0] ? affected_names : "unknown", entry->user, (int)entry->pid);
    }

    Widget dialog = XmCreateQuestionDialog(tasks_ui_get_toplevel(ui),
                                          "usersLogoutConfirmDialog", NULL, 0);
    XmString title = XmStringCreateLocalized((String)"Logout user (SIGHUP)");
    XtVaSetValues(dialog, XmNdialogTitle, title, NULL);
    XmStringFree(title);
    XmString msg = XmStringCreateLocalized((String)message);
    XtVaSetValues(dialog, XmNmessageString, msg, NULL);
    XmStringFree(msg);
    XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));
    XtAddCallback(dialog, XmNokCallback, on_users_logout_confirm, data);
    XtAddCallback(dialog, XmNcancelCallback, on_users_logout_cancel, data);
    XtManageChild(dialog);
}

static void on_users_logout_confirm(Widget widget, XtPointer client, XtPointer call)
{
    (void)call;
    UsersLogoutDialogData *data = (UsersLogoutDialogData *)client;
    if (!data || !data->ui) {
        if (widget) XtDestroyWidget(widget);
        free(data);
        return;
    }
    TasksUi *ui = data->ui;
    if (data->pgrp <= 0) {
        show_error_dialog(ui, "Invalid process group ID.");
        if (widget) XtDestroyWidget(widget);
        free(data);
        return;
    }
    if (kill(-data->pgrp, SIGHUP) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Failed to send HUP to process group %d on %s (user %s): %s",
                 (int)data->pgrp, data->tty, data->user, strerror(errno));
        show_error_dialog(ui, msg);
        if (widget) XtDestroyWidget(widget);
        free(data);
        return;
    }
    users_clear_selection(ui);
    if (widget) XtDestroyWidget(widget);
    free(data);
}

static void on_users_logout_cancel(Widget widget, XtPointer client, XtPointer call)
{
    (void)call;
    UsersLogoutDialogData *data = (UsersLogoutDialogData *)client;
    if (widget) XtDestroyWidget(widget);
    free(data);
}

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

    ui->users_controls_form = XmCreateForm(page, "usersControlsForm", NULL, 0);
    XtVaSetValues(ui->users_controls_form,
                  XmNfractionBase, 100,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftOffset, 8,
                  XmNrightOffset, 8,
                  XmNbottomOffset, 8,
                  XmNheight, 48,
                  NULL);
    XtManageChild(ui->users_controls_form);

    XmString logout_label = tasks_ui_make_string("Logout user (SIGHUP)");
    ui->users_logout_button = XtVaCreateManagedWidget(
        "usersLogoutButton",
        xmPushButtonGadgetClass, ui->users_controls_form,
        XmNlabelString, logout_label,
        XmNsensitive, False,
        XmNleftAttachment, XmATTACH_FORM,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNbottomOffset, 4,
        NULL);
    XmStringFree(logout_label);
    XtAddCallback(ui->users_logout_button, XmNactivateCallback, on_users_logout_activate, (XtPointer)ui);

    ui->users_logout_status_label = XtVaCreateManagedWidget(
        "usersLogoutStatusLabel",
        xmLabelGadgetClass, ui->users_controls_form,
        XmNlabelString, NULL,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNleftAttachment, XmATTACH_WIDGET,
        XmNleftWidget, ui->users_logout_button,
        XmNleftOffset, 12,
        XmNrightAttachment, XmATTACH_FORM,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNbottomOffset, 4,
        NULL);
    tasks_ui_set_label_text(ui->users_logout_status_label, "");

    Widget table_widget = table_widget_get_widget(ui->users_table);
    if (table_widget) {
        XtVaSetValues(table_widget,
                      XmNtopAttachment, XmATTACH_FORM,
                      XmNleftAttachment, XmATTACH_FORM,
                      XmNrightAttachment, XmATTACH_FORM,
                      XmNbottomAttachment, XmATTACH_WIDGET,
                      XmNbottomWidget, ui->users_controls_form,
                      XmNtopOffset, 6,
                      XmNbottomOffset, 8,
                      XmNleftOffset, 6,
                      XmNrightOffset, 6,
                      NULL);
    }
    table_widget_set_grid(ui->users_table, True);
    table_widget_set_alternate_row_colors(ui->users_table, True);
    ui->users_selected_index = -1;
    ui->users_updates_paused = False;
    return page;
}

void tasks_ui_destroy_users_tab(TasksUi *ui)
{
    if (!ui) return;
    if (ui->users_table) {
        table_widget_destroy(ui->users_table);
        ui->users_table = NULL;
    }
    free(ui->users_rows);
    ui->users_rows = NULL;
    ui->users_row_count = 0;
    ui->users_row_capacity = 0;
    ui->users_entries = NULL;
    ui->users_entries_count = 0;
    ui->users_controls_form = NULL;
    ui->users_logout_button = NULL;
    ui->users_logout_status_label = NULL;
    ui->users_selected_row = NULL;
    ui->users_selected_index = -1;
    ui->users_updates_paused = False;
}

void tasks_ui_set_users_table(TasksUi *ui, const TasksUserEntry *entries, int count)
{
    if (!ui || !ui->users_table) return;
    ui->users_entries = entries;
    ui->users_entries_count = (entries && count > 0) ? count : 0;
    int desired = (entries && count > 0) ? count : 0;
    ensure_users_row_capacity(ui, desired);

    int old_count = ui->users_row_count;
    int common = (old_count < desired) ? old_count : desired;

    for (int i = 0; i < common; ++i) {
        char pid_buffer[16] = {0};
        if (entries[i].pid > 0) {
            snprintf(pid_buffer, sizeof(pid_buffer), "%d", (int)entries[i].pid);
        }
        char idle_sort_buffer[32] = {0};
        if (entries[i].idle_seconds >= 0) {
            snprintf(idle_sort_buffer, sizeof(idle_sort_buffer), "%lld", entries[i].idle_seconds);
        }
        const char *values[] = {
            entries[i].user,
            entries[i].tty,
            entries[i].login_time,
            entries[i].idle_time,
            entries[i].host,
            pid_buffer,
        };
        const char *sort_values[] = {
            NULL,
            NULL,
            NULL,
            idle_sort_buffer,
            NULL,
            NULL,
        };
        if (ui->users_rows && ui->users_rows[i]) {
            table_widget_update_row_with_sort_values(ui->users_rows[i], values, sort_values);
            Widget row_widget = table_row_get_widget(ui->users_rows[i]);
            if (row_widget) {
                XtVaSetValues(row_widget, XmNuserData, (XtPointer)(intptr_t)(i + 1), NULL);
            }
        }
    }

    for (int i = common; i < desired; ++i) {
        char pid_buffer[16] = {0};
        if (entries[i].pid > 0) {
            snprintf(pid_buffer, sizeof(pid_buffer), "%d", (int)entries[i].pid);
        }
        char idle_sort_buffer[32] = {0};
        if (entries[i].idle_seconds >= 0) {
            snprintf(idle_sort_buffer, sizeof(idle_sort_buffer), "%lld", entries[i].idle_seconds);
        }
        const char *values[] = {
            entries[i].user,
            entries[i].tty,
            entries[i].login_time,
            entries[i].idle_time,
            entries[i].host,
            pid_buffer,
        };
        const char *sort_values[] = {
            NULL,
            NULL,
            NULL,
            idle_sort_buffer,
            NULL,
            NULL,
        };
        TableRow *row = table_widget_add_row_with_sort_values(ui->users_table, values, sort_values);
        if (ui->users_rows) {
            ui->users_rows[i] = row;
        }
        Widget row_widget = table_row_get_widget(row);
        if (row_widget) {
            XtVaSetValues(row_widget, XmNuserData, (XtPointer)(intptr_t)(i + 1), NULL);
            XtAddEventHandler(row_widget, ButtonPressMask, False, on_users_row_press, (XtPointer)ui);
        }
    }

    for (int i = desired; i < old_count; ++i) {
        if (ui->users_rows && ui->users_rows[i]) {
            table_widget_remove_row(ui->users_table, ui->users_rows[i]);
            ui->users_rows[i] = NULL;
        }
    }
    ui->users_row_count = desired;
}
