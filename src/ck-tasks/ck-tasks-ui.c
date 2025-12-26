#include "ck-tasks-ui.h"
#include "ck-tasks-ctrl.h"

#include <Xm/RowColumn.h>
#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/ToggleB.h>
#include <Xm/ToggleBG.h>
#include <Xm/PushBG.h>
#include <Xm/CascadeBG.h>
#include <Xm/Label.h>
#include <Xm/ArrowBG.h>
#include <Xm/ScrollBar.h>
#include <Xm/TextF.h>
#include <Xm/TabStack.h>
#include <X11/Xlib.h>
#include <X11/Intrinsic.h>
#include <Xm/MenuShell.h>
#include <Xm/Protocols.h>
#include <Xm/DialogS.h>
#include <Xm/ScrolledW.h>
#include <Xm/List.h>
#include <Xm/Frame.h>

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../ck-load/vertical_meter.h"

static const char *process_headers[] = {
    "Image Name",
    "PID",
    "CPU (%)",
    "Memory (MB)",
    "Threads",
    "User/Session",
};

#define PROCESS_COLUMN_COUNT (sizeof(process_headers) / sizeof(process_headers[0]))
#define PROCESS_VIRTUAL_ROW_COUNT 32
#define PROCESS_VIRTUAL_ROW_HEIGHT_DEFAULT 24
#define PROCESS_VIRTUAL_ROW_COUNT 32


static XmString make_string(const char *text)
{
    return XmStringCreateLocalized((String)(text ? text : ""));
}

static XmString *make_string_array(const char *const strings[], int count)
{
    if (!strings || count <= 0) return NULL;
    XmString *result = (XmString *)calloc(count, sizeof(XmString));
    if (!result) return NULL;
    for (int i = 0; i < count; ++i) {
        result[i] = make_string(strings[i]);
    }
    return result;
}

typedef enum {
    SORT_DIR_NONE = 0,
    SORT_DIR_ASC = 1,
    SORT_DIR_DESC = -1,
} SortDirection;

static const Boolean process_numeric_columns[PROCESS_COLUMN_COUNT] = {
    False, True, True, True, True, False
};

static SortDirection g_process_sort_direction = SORT_DIR_NONE;
static int g_process_sort_column = -1;
static Widget g_process_header_buttons[PROCESS_COLUMN_COUNT];
static Widget g_process_header_indicators[PROCESS_COLUMN_COUNT];
static GridLayout *g_process_grid = NULL;
static int *g_process_row_order = NULL;
static int g_process_row_count = 0;
static Widget g_process_scroll_window = NULL;
static Widget g_process_header_form = NULL;
static int g_process_row_height = PROCESS_VIRTUAL_ROW_HEIGHT_DEFAULT;
static int g_process_row_start = 0;
static int g_process_row_page_size = PROCESS_VIRTUAL_ROW_COUNT;

typedef struct {
    Widget row_form;
    Widget cells[PROCESS_COLUMN_COUNT];
    int row_id;
} ProcessRow;

static ProcessRow *g_process_rows = NULL;
static int g_process_rows_alloc = 0;

static int process_row_compare(const void *a, const void *b);
static void process_refresh_rows(void);
static void process_refresh_header_labels(void);
static void process_toggle_sort(int column);
static void process_apply_sort(void);
static void process_set_cell_text(Widget cell, const char *text);
static void process_update_row(ProcessRow *row, const TasksProcessEntry *entry);
static void ensure_process_rows(GridLayout *grid, int needed);
static void release_process_rows(void);
static Boolean process_suspend_updates(void);
static void process_resume_updates(Boolean suspended);
static void on_process_header_activate(Widget widget, XtPointer client, XtPointer call);
static Widget tasks_ui_get_tab_widget(TasksUi *ui, TasksTab tab);
static void process_update_viewport_metrics(void);
static void on_process_scroll_window_resize(Widget widget, XtPointer client, XEvent *event, Boolean *continue_to_dispatch);
static const TasksProcessEntry *g_process_entries = NULL;

static void on_apps_row_press(Widget widget, XtPointer client, XEvent *event, Boolean *continue_to_dispatch);

static const TableColumnDef apps_columns[] = {
    {"appTitle", "Application", TABLE_ALIGN_LEFT, False, True, 0},
    {"appPid", "PID", TABLE_ALIGN_RIGHT, True, True, 0},
    {"appWindows", "Windows", TABLE_ALIGN_RIGHT, True, True, 0},
    {"appCommand", "Command", TABLE_ALIGN_LEFT, False, True, 0},
    {"appClass", "Class", TABLE_ALIGN_LEFT, False, True, 0},
};

#define APPS_COLUMN_COUNT (sizeof(apps_columns) / sizeof(apps_columns[0]))

static void process_refresh_rows(void)
{
    if (!g_process_grid) return;
    process_update_viewport_metrics();
    Boolean suspended = process_suspend_updates();
    int total_rows = g_process_row_count;
    int page_size = g_process_row_page_size;
    if (page_size <= 0) page_size = PROCESS_VIRTUAL_ROW_COUNT;
    int max_start = total_rows > page_size ? total_rows - page_size : 0;
    if (g_process_row_start < 0) g_process_row_start = 0;
    if (g_process_row_start > max_start) g_process_row_start = max_start;
    int visible_rows = page_size;
    ensure_process_rows(g_process_grid, visible_rows);
    if (g_process_rows_alloc < visible_rows) {
        visible_rows = g_process_rows_alloc;
    }
    int available_rows = total_rows - g_process_row_start;
    if (available_rows < 0) available_rows = 0;
    if (visible_rows > available_rows) {
        visible_rows = available_rows;
    }
    for (int i = 0; i < visible_rows; ++i) {
        ProcessRow *row = &g_process_rows[i];
        if (!row || !row->row_form) continue;
        int dataset_index = g_process_row_start + i;
        if (dataset_index >= total_rows) break;
        int entry_index = g_process_row_order ? g_process_row_order[dataset_index] : dataset_index;
        const TasksProcessEntry *entry = &g_process_entries[entry_index];
        process_update_row(row, entry);
        if (!XtIsManaged(row->row_form)) {
            XtManageChild(row->row_form);
        }
    }
    for (int i = visible_rows; i < g_process_rows_alloc; ++i) {
        if (g_process_rows[i].row_form && XtIsManaged(g_process_rows[i].row_form)) {
            XtUnmanageChild(g_process_rows[i].row_form);
        }
    }
    process_resume_updates(suspended);
}

static void process_set_cell_text(Widget cell, const char *text)
{
    if (!cell) return;
    XmString label = make_string(text);
    XtVaSetValues(cell, XmNlabelString, label, NULL);
    XmStringFree(label);
}

static void process_update_row(ProcessRow *row, const TasksProcessEntry *entry)
{
    if (!row || !entry) return;
    char buffer[64];
    process_set_cell_text(row->cells[0], entry->name);
    if (row->cells[1]) {
        snprintf(buffer, sizeof(buffer), "%d", (int)entry->pid);
        process_set_cell_text(row->cells[1], buffer);
    }
    if (row->cells[2]) {
        snprintf(buffer, sizeof(buffer), "%.1f", entry->cpu_percent);
        process_set_cell_text(row->cells[2], buffer);
    }
    if (row->cells[3]) {
        snprintf(buffer, sizeof(buffer), "%.0f", entry->memory_mb);
        process_set_cell_text(row->cells[3], buffer);
    }
    if (row->cells[4]) {
        snprintf(buffer, sizeof(buffer), "%d", entry->threads);
        process_set_cell_text(row->cells[4], buffer);
    }
    process_set_cell_text(row->cells[5], entry->user);
}

static void ensure_process_rows(GridLayout *grid, int needed)
{
    if (!grid || needed <= g_process_rows_alloc) return;
    ProcessRow *expanded = (ProcessRow *)realloc(g_process_rows, sizeof(ProcessRow) * needed);
    if (!expanded) return;
    g_process_rows = expanded;
    for (int i = g_process_rows_alloc; i < needed; ++i) {
        ProcessRow *row = &g_process_rows[i];
        memset(row, 0, sizeof(*row));
        row->row_id = gridlayout_add_row(grid);
        row->row_form = gridlayout_get_row_form(grid, row->row_id);
        if (!row->row_form) continue;
        for (int col = 0; col < PROCESS_COLUMN_COUNT; ++col) {
            Widget cell = XmCreateLabelGadget(row->row_form, "processCell", NULL, 0);
            XtVaSetValues(cell,
                          XmNalignment, (col == 1 || col == 2 || col == 3 || col == 4)
                                        ? XmALIGNMENT_END : XmALIGNMENT_BEGINNING,
                          XmNrecomputeSize, False,
                          XmNmarginWidth, 6,
                          XmNmarginHeight, 3,
                          XmNborderWidth, 1,
                          XmNshadowThickness, 1,
                          XmNshadowType, XmSHADOW_OUT,
                          NULL);
            gridlayout_add_cell(grid, row->row_id, col, cell, 1);
            row->cells[col] = cell;
        }
        XtUnmanageChild(row->row_form);
    }
    g_process_rows_alloc = needed;
}

static void release_process_rows(void)
{
    if (!g_process_rows) return;
    for (int i = 0; i < g_process_rows_alloc; ++i) {
        if (g_process_rows[i].row_form && XtIsManaged(g_process_rows[i].row_form)) {
            XtUnmanageChild(g_process_rows[i].row_form);
        }
    }
    free(g_process_rows);
    g_process_rows = NULL;
    g_process_rows_alloc = 0;
}

static Boolean process_suspend_updates(void)
{
    if (!g_process_grid) return False;
    Widget grid_widget = gridlayout_get_widget(g_process_grid);
    if (!grid_widget || !XtIsManaged(grid_widget)) return False;
    XtUnmanageChild(grid_widget);
    return True;
}

static void process_resume_updates(Boolean suspended)
{
    if (!suspended || !g_process_grid) return;
    Widget grid_widget = gridlayout_get_widget(g_process_grid);
    if (!grid_widget) return;
    XtManageChild(grid_widget);
    XmUpdateDisplay(grid_widget);
}

static void process_update_viewport_metrics(void)
{
    if (!g_process_scroll_window) return;
    Dimension scroll_height = 0;
    XtVaGetValues(g_process_scroll_window, XmNheight, &scroll_height, NULL);
    int header_height = 0;
    if (g_process_header_form) {
        Dimension header_value = 0;
        XtVaGetValues(g_process_header_form, XmNheight, &header_value, NULL);
        header_height = (int)header_value;
    }
    if (g_process_rows_alloc > 0 && g_process_rows[0].row_form) {
        Dimension row_height = 0;
        XtVaGetValues(g_process_rows[0].row_form, XmNheight, &row_height, NULL);
        if (row_height > 0) {
            g_process_row_height = (int)row_height;
        }
    }
    int available_height = (int)scroll_height - header_height;
    if (available_height <= 0) {
        available_height = g_process_row_height;
    }
    int row_height = g_process_row_height > 0 ? g_process_row_height : PROCESS_VIRTUAL_ROW_HEIGHT_DEFAULT;
    int row_spacing = 0;
    if (g_process_grid) {
        row_spacing = gridlayout_get_row_spacing(g_process_grid);
    }
    int effective_row_height = row_height + row_spacing;
    if (effective_row_height <= 0) {
        effective_row_height = row_height;
    }
    int rows = available_height / effective_row_height;
    if (rows <= 0) rows = 1;
    g_process_row_page_size = rows;
}

static void on_process_scroll_window_resize(Widget widget, XtPointer client, XEvent *event, Boolean *continue_to_dispatch)
{
    (void)widget;
    (void)continue_to_dispatch;
    if (!event || event->type != ConfigureNotify) return;
    process_update_viewport_metrics();
    TasksUi *ui = (TasksUi *)client;
    if (ui && ui->controller) {
        tasks_ctrl_handle_viewport_change(ui->controller);
    }
}

static void process_refresh_header_labels(void)
{
    if (!g_process_grid) return;
    for (int col = 0; col < PROCESS_COLUMN_COUNT; ++col) {
        Widget button = g_process_header_buttons[col];
        if (!button) continue;
        char buffer[64];
        const char *base = process_headers[col];
        snprintf(buffer, sizeof(buffer), "%s", base ? base : "");
        XmString label = make_string(buffer);
        XtVaSetValues(button, XmNlabelString, label, NULL);
        XmStringFree(label);

        Widget indicator = g_process_header_indicators[col];
        if (indicator) {
            Boolean active = (g_process_sort_column == col && g_process_sort_direction != SORT_DIR_NONE);
            if (active) {
                Cardinal direction = (g_process_sort_direction == SORT_DIR_DESC) ? XmARROW_DOWN : XmARROW_UP;
                XtVaSetValues(indicator,
                              XmNarrowDirection, direction,
                              NULL);
                XtManageChild(indicator);
            } else {
                XtUnmanageChild(indicator);
            }
        }
    }
}

static int process_row_compare(const void *a, const void *b)
{
    if (g_process_sort_column < 0 || g_process_sort_direction == SORT_DIR_NONE) {
        int ia = *(const int *)a;
        int ib = *(const int *)b;
        return ia - ib;
    }
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    const TasksProcessEntry *ea = &g_process_entries[ia];
    const TasksProcessEntry *eb = &g_process_entries[ib];
    char va[64] = {0};
    char vb[64] = {0};
    int cmp = 0;
    if (process_numeric_columns[g_process_sort_column]) {
        double da = 0.0;
        double db = 0.0;
        switch (g_process_sort_column) {
        case 1:
            da = (double)ea->pid;
            db = (double)eb->pid;
            break;
        case 2:
            da = ea->cpu_percent;
            db = eb->cpu_percent;
            break;
        case 3:
            da = ea->memory_mb;
            db = eb->memory_mb;
            break;
        case 4:
            da = (double)ea->threads;
            db = (double)eb->threads;
            break;
        default:
            break;
        }
        if (da < db) cmp = -1;
        else if (da > db) cmp = 1;
        else cmp = 0;
    } else {
        const char *sa = "";
        const char *sb = "";
        if (g_process_sort_column == 0) {
            sa = ea->name;
            sb = eb->name;
        } else if (g_process_sort_column == 5) {
            sa = ea->user;
            sb = eb->user;
        }
        snprintf(va, sizeof(va), "%s", sa ? sa : "");
        snprintf(vb, sizeof(vb), "%s", sb ? sb : "");
        cmp = strcoll(va, vb);
    }
    if (g_process_sort_direction == SORT_DIR_DESC) cmp = -cmp;
    if (cmp == 0) cmp = ia - ib;
    return cmp;
}

static void process_apply_sort(void)
{
    if (!g_process_row_order || g_process_row_count <= 0) {
        process_refresh_rows();
        process_refresh_header_labels();
        return;
    }
    for (int i = 0; i < g_process_row_count; ++i) {
        g_process_row_order[i] = i;
    }
    if (g_process_sort_direction != SORT_DIR_NONE) {
        qsort(g_process_row_order, g_process_row_count, sizeof(int), process_row_compare);
    }
    process_refresh_rows();
    process_refresh_header_labels();
}

static void process_toggle_sort(int column)
{
    if (column < 0 || column >= PROCESS_COLUMN_COUNT) return;
    if (g_process_sort_column != column) {
        g_process_sort_column = column;
        g_process_sort_direction = SORT_DIR_ASC;
    } else if (g_process_sort_direction == SORT_DIR_ASC) {
        g_process_sort_direction = SORT_DIR_DESC;
    } else if (g_process_sort_direction == SORT_DIR_DESC) {
        g_process_sort_direction = SORT_DIR_NONE;
    } else {
        g_process_sort_direction = SORT_DIR_ASC;
    }
    process_apply_sort();
}

static void on_process_header_activate(Widget widget, XtPointer client, XtPointer call)
{
    (void)widget;
    (void)call;
    int column = (int)(intptr_t)client;
    process_toggle_sort(column);
}


static void status_set_label_text(Widget label, char *cache, size_t cache_len, const char *text)
{
    if (!label || !cache || cache_len == 0) return;
    const char *value = text ? text : "";
    if (strncmp(cache, value, cache_len) == 0) return;
    strncpy(cache, value, cache_len - 1);
    cache[cache_len - 1] = '\0';
    XmString label_text = make_string(value);
    XtVaSetValues(label, XmNlabelString, label_text, NULL);
    XmStringFree(label_text);
}

static Widget create_status_segment(Widget parent, const char *name, Widget *out_label)
{
    Widget frame = XmCreateFrame(parent, (String)name, NULL, 0);
    XtVaSetValues(frame,
                  XmNshadowType, XmSHADOW_IN,
                  XmNmarginWidth, 4,
                  XmNmarginHeight, 2,
                  NULL);
    XtManageChild(frame);

    Widget label = XtVaCreateManagedWidget(
        "statusSegmentLabel",
        xmLabelGadgetClass, frame,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNmarginLeft, 4,
        XmNmarginRight, 4,
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

    status_set_label_text(ui->status_processes_label, ui->status_processes_text,
                          sizeof(ui->status_processes_text), "Processes: 0");
    status_set_label_text(ui->status_cpu_label, ui->status_cpu_text,
                          sizeof(ui->status_cpu_text), "CPU Usage: 0%");
    status_set_label_text(ui->status_memory_label, ui->status_memory_text,
                          sizeof(ui->status_memory_text), "Physical Memory: 0% (0 GB/0 GB)");
    status_set_label_text(ui->status_message_label, ui->status_message_text,
                          sizeof(ui->status_message_text), "Status: idle");

    return status_form;
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

static Widget create_toggle_item(Widget parent, const char *name, const char *label)
{
    XmString xm_label = make_string(label);
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
    XmString xm_label = make_string(label);
    Widget toggle = XtVaCreateManagedWidget(
        name,
        xmToggleButtonGadgetClass, parent,
        XmNlabelString, xm_label,
        NULL);
    XmStringFree(xm_label);
    return toggle;
}

static Widget create_meter_column(Widget parent, const char *label_text,
                                  Widget *out_meter, Widget *out_value_label)
{
    Widget col_form = XmCreateForm(parent, "meterColumn", NULL, 0);
    XtVaSetValues(col_form,
                  XmNfractionBase, 100,
                  XmNmarginWidth, 4,
                  XmNmarginHeight, 4,
                  NULL);
    XtManageChild(col_form);

    XmString label = make_string(label_text);
    Widget label_widget = XtVaCreateManagedWidget(
        "meterLabel",
        xmLabelGadgetClass, col_form,
        XmNlabelString, label,
        XmNalignment, XmALIGNMENT_CENTER,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNtopOffset, 2,
        NULL);
    XmStringFree(label);

    XmString initial = make_string("-");
    Widget value_label = XtVaCreateManagedWidget(
        "meterValueLabel",
        xmLabelGadgetClass, col_form,
        XmNlabelString, initial,
        XmNalignment, XmALIGNMENT_CENTER,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomOffset, 2,
        NULL);
    XmStringFree(initial);

    Arg args[8];
    int n = 0;
    XtSetArg(args[n], XmNwidth, 40); n++;
    XtSetArg(args[n], XmNheight, 150); n++;
    Widget meter = VerticalMeterCreate(col_form, "verticalMeter", args, n);
    XtVaSetValues(meter,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, label_widget,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_WIDGET,
                  XmNbottomWidget, value_label,
                  XmNbottomOffset, 4,
                  NULL);
    VerticalMeterSetMaximum(meter, 100);
    VerticalMeterSetDefaultMaximum(meter, 100);
    VerticalMeterSetCellHeight(meter, 4);
    if (out_meter) *out_meter = meter;
    if (out_value_label) *out_value_label = value_label;
    XtManageChild(meter);
    return col_form;
}

static void add_performance_tab_content(TasksUi *ui, Widget page)
{
    Widget mode_box = XmCreateRowColumn(page, "cpuModeBox", NULL, 0);
    XtVaSetValues(mode_box,
                  XmNorientation, XmHORIZONTAL,
                  XmNpacking, XmPACK_COLUMN,
                  XmNspacing, 12,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNtopOffset, 12,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNleftOffset, 12,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNrightOffset, 12,
                  XmNradioBehavior, True,
                  NULL);
    XmString overall = make_string("Overall chart");
    XtVaCreateManagedWidget(
        "cpuOverallMode",
        xmToggleButtonGadgetClass, mode_box,
        XmNlabelString, overall,
        XmNset, True,
        XmNindicatorType, XmONE_OF_MANY,
        NULL);
    XmStringFree(overall);

    XmString divided = make_string("Divided per-core chart");
    XtVaCreateManagedWidget(
        "cpuPerCoreMode",
        xmToggleButtonGadgetClass, mode_box,
        XmNlabelString, divided,
        XmNindicatorType, XmONE_OF_MANY,
        NULL);
    XmStringFree(divided);
    XtManageChild(mode_box);

    Widget meter_row = XmCreateForm(page, "performanceMeters", NULL, 0);
    XtVaSetValues(meter_row,
                  XmNfractionBase, 100,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, mode_box,
                  XmNtopOffset, 10,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNleftOffset, 12,
                  XmNrightOffset, 12,
                  NULL);
    XtManageChild(meter_row);

    Widget columns[5];
    columns[0] = create_meter_column(meter_row, "CPU", &ui->perf_cpu_meter, &ui->perf_cpu_value_label);
    columns[1] = create_meter_column(meter_row, "Memory", &ui->perf_mem_meter, &ui->perf_mem_value_label);
    columns[2] = create_meter_column(meter_row, "Load 1m", &ui->perf_load1_meter, &ui->perf_load1_value_label);
    columns[3] = create_meter_column(meter_row, "Load 5m", &ui->perf_load5_meter, &ui->perf_load5_value_label);
    columns[4] = create_meter_column(meter_row, "Load 15m", &ui->perf_load15_meter, &ui->perf_load15_value_label);

    for (int i = 0; i < 5; ++i) {
        if (!columns[i]) continue;
        XtVaSetValues(columns[i],
                      XmNtopAttachment, XmATTACH_FORM,
                      XmNbottomAttachment, XmATTACH_FORM,
                      XmNleftAttachment, XmATTACH_POSITION,
                      XmNleftPosition, i * 20,
                      XmNrightAttachment, XmATTACH_POSITION,
                      XmNrightPosition, (i + 1) * 20,
                      NULL);
    }

    Widget history_frame = XmCreateFrame(page, "historyFrame", NULL, 0);
    XtVaSetValues(history_frame,
                  XmNshadowType, XmSHADOW_ETCHED_IN,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, meter_row,
                  XmNtopOffset, 12,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNleftOffset, 10,
                  XmNrightOffset, 10,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNbottomOffset, 10,
                  NULL);
    XtManageChild(history_frame);

    XmString history_text = make_string("CPU history chart will alternate between overall and per-core views when the user toggles a mode or double-clicks the plot area.");
    XtVaCreateManagedWidget(
        "historyLabel",
        xmLabelGadgetClass, history_frame,
        XmNlabelString, history_text,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNmarginHeight, 8,
        XmNmarginWidth, 8,
        NULL);
    XmStringFree(history_text);
}

static const char *network_samples[] = {
    "eth0: 192.168.1.12 -> 10.0.0.5 (UDP)  10 MB/s",
    "eth0: 192.168.1.12 -> 172.16.0.3 (TCP)  2 MB/s",
    "wlan0: 10.0.0.52 -> 52.15.5.1 (HTTPS) 1.2 MB/s",
    "wlan0: adapter idle",
};

static void add_networking_tab_content(TasksUi *ui, Widget page)
{
    Widget summary_box = XmCreateRowColumn(page, "networkSummary", NULL, 0);
    XtVaSetValues(summary_box,
                  XmNorientation, XmHORIZONTAL,
                  XmNspacing, 14,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNtopOffset, 12,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNleftOffset, 12,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNrightOffset, 12,
                  XmNpacking, XmPACK_TIGHT,
                  NULL);
    XtManageChild(summary_box);

    XmString tx = make_string("Tx: 12 MB/s");
    XtVaCreateManagedWidget(
        "netTxLabel",
        xmLabelGadgetClass, summary_box,
        XmNlabelString, tx,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL);
    XmStringFree(tx);

    XmString rx = make_string("Rx: 3.6 MB/s");
    XtVaCreateManagedWidget(
        "netRxLabel",
        xmLabelGadgetClass, summary_box,
        XmNlabelString, rx,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL);
    XmStringFree(rx);

        Arg scroll_args[10];
        int sn = 0;
        XtSetArg(scroll_args[sn], XmNtopAttachment, XmATTACH_WIDGET); sn++;
        XtSetArg(scroll_args[sn], XmNtopWidget, summary_box); sn++;
        XtSetArg(scroll_args[sn], XmNtopOffset, 10); sn++;
        XtSetArg(scroll_args[sn], XmNbottomAttachment, XmATTACH_FORM); sn++;
        XtSetArg(scroll_args[sn], XmNbottomOffset, 10); sn++;
        XtSetArg(scroll_args[sn], XmNleftAttachment, XmATTACH_FORM); sn++;
        XtSetArg(scroll_args[sn], XmNrightAttachment, XmATTACH_FORM); sn++;
        XtSetArg(scroll_args[sn], XmNleftOffset, 12); sn++;
        XtSetArg(scroll_args[sn], XmNrightOffset, 12); sn++;
        XtSetArg(scroll_args[sn], XmNscrollingPolicy, XmAUTOMATIC); sn++;
        Widget scroll = XmCreateScrolledWindow(page, "networkListScroll", scroll_args, sn);
    XtManageChild(scroll);

    Widget list = XmCreateScrolledList(scroll, "networkEntries", NULL, 0);
    XtVaSetValues(list,
                  XmNvisibleItemCount, 5,
                  XmNselectionPolicy, XmSINGLE_SELECT,
                  XmNscrollBarDisplayPolicy, XmAS_NEEDED,
                  NULL);

    size_t sample_count = sizeof(network_samples) / sizeof(network_samples[0]);
    XmString *items = make_string_array(network_samples, sample_count);
    if (items) {
        XmListAddItems(list, items, (Cardinal)sample_count, 0);
        for (size_t i = 0; i < sample_count; ++i) {
            XmStringFree(items[i]);
        }
        free(items);
    }

    XtManageChild(list);
}

static Widget create_cascade_menu(Widget menu_bar, const char *label,
                                  const char *name, Widget *out_menu)
{
    Widget pulldown = XmCreatePulldownMenu(menu_bar, (String)name, NULL, 0);
    XmString xm_label = make_string(label);
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

static Widget create_page(TasksUi *ui, const char *name, TasksTab tab_number,
                          const char *title, const char *subtitle)
{
    (void)tab_number;
    XmString tab_label = make_string(title);
    Widget page = XmCreateForm(ui->tab_stack, (String)(name ? name : "tasksPage"), NULL, 0);
    if (!page) {
        XmStringFree(tab_label);
        return NULL;
    }
    XtVaSetValues(page,
                  XmNtabLabelString, tab_label,
                  XmNfractionBase, 100,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  NULL);
    XmStringFree(tab_label);

    XtManageChild(page);
    return page;
}

static Widget create_process_tab(TasksUi *ui)
{
    Widget page = create_page(ui, "processesPage", TASKS_TAB_PROCESSES,
                              "Processes", "");
    XmString toggle_label = make_string("Show only my processes");
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

    XmString filter_label_text = make_string("Filter:");
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

    Arg scroll_args[16];
    int sn = 0;
    XtSetArg(scroll_args[sn], XmNtopAttachment, XmATTACH_FORM); sn++;
    XtSetArg(scroll_args[sn], XmNtopOffset, 0); sn++;
    XtSetArg(scroll_args[sn], XmNleftAttachment, XmATTACH_FORM); sn++;
    XtSetArg(scroll_args[sn], XmNleftOffset, 0); sn++;
    XtSetArg(scroll_args[sn], XmNrightAttachment, XmATTACH_WIDGET); sn++;
    XtSetArg(scroll_args[sn], XmNrightWidget, scrollbar); sn++;
    XtSetArg(scroll_args[sn], XmNrightOffset, 4); sn++;
    XtSetArg(scroll_args[sn], XmNbottomAttachment, XmATTACH_FORM); sn++;
    XtSetArg(scroll_args[sn], XmNscrollingPolicy, XmAPPLICATION_DEFINED); sn++;
    XtSetArg(scroll_args[sn], XmNvisualPolicy, XmVARIABLE); sn++;
    XtSetArg(scroll_args[sn], XmNshadowThickness, 0); sn++;
    Widget scroll = XmCreateScrolledWindow(process_area, "processListScroll", scroll_args, sn);
    XtManageChild(scroll);
    g_process_scroll_window = scroll;
    XtAddEventHandler(scroll, StructureNotifyMask, False, on_process_scroll_window_resize, (XtPointer)ui);

    GridLayout *grid = gridlayout_create(scroll, "processGrid", PROCESS_COLUMN_COUNT);
    if (grid) {
        gridlayout_set_row_spacing(grid, 4);
        Widget grid_widget = gridlayout_get_widget(grid);
        XtVaSetValues(scroll, XmNworkWindow, grid_widget, NULL);

        int header_row = gridlayout_add_row(grid);
        Widget header_form = gridlayout_get_row_form(grid, header_row);
        g_process_header_form = header_form;
        for (int col = 0; col < PROCESS_COLUMN_COUNT; ++col) {
            Widget header_frame = XmCreateFrame(header_form, "headerFrame", NULL, 0);
            XtVaSetValues(header_frame,
                          XmNshadowThickness, 2,
                          XmNshadowType, XmSHADOW_IN,
                          XmNmarginWidth, 0,
                          XmNmarginHeight, 2,
                          XmNborderWidth, 1,
                          XmNborderColor, BlackPixel(XtDisplay(header_frame),
                                                     DefaultScreen(XtDisplay(header_frame))),
                          NULL);

            Widget header_cell = XmCreateForm(header_frame, "headerCell", NULL, 0);
            XtVaSetValues(header_cell,
                          XmNfractionBase, 100,
                          XmNmarginWidth, 0,
                          XmNmarginHeight, 0,
                          XmNshadowThickness, 0,
                          XmNshadowType, XmSHADOW_OUT,
                          NULL);
            XtManageChild(header_cell);

            Widget cell = XmCreatePushButtonGadget(header_cell, "headerButton", NULL, 0);
            XmString label = make_string(process_headers[col]);
            XtVaSetValues(cell,
                          XmNlabelString, label,
                          XmNalignment, col == 1 || col == 2 || col == 3 || col == 4
                                        ? XmALIGNMENT_END : XmALIGNMENT_BEGINNING,
                          XmNrecomputeSize, False,
                          XmNmarginWidth, 6,
                          XmNmarginHeight, 4,
                          XmNborderWidth, 0,
                          XmNshadowThickness, 0,
                          XmNshadowType, XmSHADOW_OUT,
                          XmNleftAttachment, XmATTACH_FORM,
                          XmNrightAttachment, XmATTACH_POSITION,
                          XmNrightPosition, 80,
                          NULL);
            XmStringFree(label);
            XtAddCallback(cell, XmNactivateCallback, on_process_header_activate, (XtPointer)(intptr_t)col);
            g_process_header_buttons[col] = cell;

            Widget indicator_form = XmCreateForm(header_cell, "headerIndicatorForm", NULL, 0);
            XtVaSetValues(indicator_form,
                          XmNleftAttachment, XmATTACH_POSITION,
                          XmNleftPosition, 80,
                          XmNrightAttachment, XmATTACH_FORM,
                          XmNtopAttachment, XmATTACH_FORM,
                          XmNbottomAttachment, XmATTACH_FORM,
                          XmNborderWidth, 0,
                          XmNmarginWidth, 0,
                          XmNmarginHeight, 0,
                          XmNfractionBase, 100,
                          XmNshadowThickness, 0,
                          NULL);
            XtManageChild(indicator_form);

            Widget indicator = XtVaCreateManagedWidget(
                "processHeaderSortIndicator",
                xmArrowButtonGadgetClass,
                indicator_form,
                XmNleftAttachment, XmATTACH_FORM,
                XmNrightAttachment, XmATTACH_FORM,
                XmNtopAttachment, XmATTACH_POSITION,
                XmNtopPosition, 25,
                XmNbottomAttachment, XmATTACH_POSITION,
                XmNbottomPosition, 75,
                XmNborderWidth, 0,
                XmNmarginWidth, 0,
                XmNmarginHeight, 0,
                XmNshadowThickness, 0,
                XmNtraversalOn, False,
                XmNuserData, (XtPointer)(intptr_t)col,
                NULL);
            XtAddCallback(indicator, XmNactivateCallback, on_process_header_activate, (XtPointer)(intptr_t)col);
            XtUnmanageChild(indicator);

            g_process_header_indicators[col] = indicator;

            XtManageChild(cell);
            gridlayout_add_cell(grid, header_row, col, header_frame, 1);
        }

        g_process_grid = grid;
        process_apply_sort();
        ui->process_grid = grid;
    }

    return page;
}

static Widget create_simple_tab(TasksUi *ui, TasksTab tab, const char *name,
                                const char *title, const char *description)
{
    Widget page = create_page(ui, name, tab, title, description);
    XmString placeholder = make_string("Content placeholder for this tab.");
    XtVaCreateManagedWidget(
        "tabPlaceholder",
        xmLabelGadgetClass, page,
        XmNlabelString, placeholder,
        XmNalignment, XmALIGNMENT_CENTER,
        XmNtopAttachment, XmATTACH_FORM,
        XmNtopOffset, 14,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftOffset, 8,
        XmNrightOffset, 8,
        XmNbottomOffset, 8,
        NULL);
    XmStringFree(placeholder);
    return page;
}

static Widget create_performance_tab(TasksUi *ui)
{
    Widget page = create_page(ui, "performancePage", TASKS_TAB_PERFORMANCE,
                              "Performance", "CPU / memory meters and graphs will live here.");
    add_performance_tab_content(ui, page);
    return page;
}

static Widget create_networking_tab(TasksUi *ui)
{
    Widget page = create_page(ui, "networkingPage", TASKS_TAB_NETWORKING,
                              "Networking", "Adapter status, throughput logs, and connection list.");
    add_networking_tab_content(ui, page);
    return page;
}

static Widget create_applications_tab(TasksUi *ui)
{
    Widget page = create_page(ui, "applicationsPage", TASKS_TAB_APPLICATIONS,
                              "Applications", "Windows grouped by process.");

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

    XmString filter_label_text = make_string("Filter:");
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

    XmString close_label = make_string("Close Window");
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

    TableWidget *table = table_widget_create(page, "appsTable", apps_columns, (int)APPS_COLUMN_COUNT);
    if (table) {
        Widget table_widget = table_widget_get_widget(table);
        XtVaSetValues(table_widget,
                      XmNtopAttachment, XmATTACH_FORM,
                      XmNtopOffset, 10,
                      XmNleftAttachment, XmATTACH_FORM,
                      XmNrightAttachment, XmATTACH_FORM,
                      XmNleftOffset, 8,
                      XmNrightOffset, 8,
                      XmNbottomAttachment, XmATTACH_WIDGET,
                      XmNbottomWidget, controls,
                      XmNbottomOffset, 8,
                      NULL);
        table_widget_set_grid(table, True);
        table_widget_set_alternate_row_colors(table, True);
        ui->apps_table = table;
    }

    return page;
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
    XmString update_label = make_string("Update Frequency");
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

    Widget help_menu = create_cascade_menu(menu_bar, "Help", "helpMenu", NULL);
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

    ui->tab_applications = create_applications_tab(ui);
    ui->tab_processes = create_process_tab(ui);
    ui->tab_services = create_simple_tab(ui, TASKS_TAB_SERVICES, "servicesPage",
                                         "Services", "Service status, start/stop controls, and dependencies.");
    ui->tab_performance = create_performance_tab(ui);
    ui->tab_networking = create_networking_tab(ui);
    ui->tab_users = create_simple_tab(ui, TASKS_TAB_USERS, "usersPage",
                                      "Users", "User sessions and resource consumption.");

    Widget initial_tab = tasks_ui_get_tab_widget(ui, TASKS_TAB_PROCESSES);
    if (initial_tab) {
        XmTabStackSelectTab(initial_tab, False);
    }

    return ui;
}

void tasks_ui_destroy(TasksUi *ui)
{
    if (ui && ui->process_grid) {
        gridlayout_destroy(ui->process_grid);
    }
    if (ui && ui->apps_table) {
        table_widget_destroy(ui->apps_table);
        ui->apps_table = NULL;
    }
    release_process_rows();
    if (g_process_row_order) {
        free(g_process_row_order);
        g_process_row_order = NULL;
    }
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
    status_set_label_text(ui->status_message_label, ui->status_message_text,
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

void tasks_ui_set_processes(TasksUi *ui, const TasksProcessEntry *entries, int count)
{
    if (!ui || !ui->process_grid) return;
    if (!entries || count <= 0) {
        g_process_entries = NULL;
        g_process_row_count = 0;
        if (g_process_row_order) {
            free(g_process_row_order);
            g_process_row_order = NULL;
        }
        process_refresh_rows();
        process_refresh_header_labels();
        return;
    }
    g_process_entries = entries;
    if (g_process_row_order) {
        free(g_process_row_order);
        g_process_row_order = NULL;
    }
    g_process_row_order = (int *)calloc(count, sizeof(int));
    if (!g_process_row_order) return;
    g_process_row_count = count;
    process_apply_sort();
}

void tasks_ui_set_process_row_window(int start)
{
    int total = g_process_row_count;
    int page_size = g_process_row_page_size;
    if (page_size <= 0) page_size = PROCESS_VIRTUAL_ROW_COUNT;
    int max_start = total > page_size ? total - page_size : 0;
    if (max_start < 0) max_start = 0;
    if (start < 0) start = 0;
    if (start > max_start) start = max_start;
    g_process_row_start = start;
    process_refresh_rows();
}

int tasks_ui_get_process_row_page_size(void)
{
    return g_process_row_page_size;
}

void tasks_ui_set_applications_table(TasksUi *ui, const TasksApplicationEntry *entries, int count)
{
    if (!ui || !ui->apps_table) return;
    table_widget_clear(ui->apps_table);
    ui->apps_selected_row = NULL;
    if (!entries || count <= 0) return;

    for (int i = 0; i < count; ++i) {
        const TasksApplicationEntry *entry = &entries[i];
        char pid_buffer[16] = {0};
        char windows_buffer[16] = {0};
        if (entry->pid_known) {
            snprintf(pid_buffer, sizeof(pid_buffer), "%d", (int)entry->pid);
        } else {
            pid_buffer[0] = '\0';
        }
        snprintf(windows_buffer, sizeof(windows_buffer), "%d", entry->window_count);
        const char *values[] = {
            entry->title,
            pid_buffer,
            windows_buffer,
            entry->command,
            entry->wm_class,
        };
        TableRow *row = table_widget_add_row(ui->apps_table, values);
        Widget row_widget = table_row_get_widget(row);
        if (row_widget) {
            XtVaSetValues(row_widget, XmNuserData, (XtPointer)(intptr_t)i, NULL);
            XtAddEventHandler(row_widget, ButtonPressMask, False, on_apps_row_press, (XtPointer)ui);
        }
    }
}

void tasks_ui_update_process_count(TasksUi *ui, int total_processes)
{
    if (!ui || !ui->status_processes_label) return;
    char buffer[64];
    if (total_processes < 0) total_processes = 0;
    snprintf(buffer, sizeof(buffer), "Processes: %d", total_processes);
    status_set_label_text(ui->status_processes_label, ui->status_processes_text,
                          sizeof(ui->status_processes_text), buffer);
}

void tasks_ui_update_system_stats(TasksUi *ui, const TasksSystemStats *stats)
{
    if (!ui || !stats) return;
    char buffer[32];
    if (ui->perf_cpu_meter) {
        VerticalMeterSetValue(ui->perf_cpu_meter, stats->cpu_percent);
    }
    if (ui->perf_cpu_value_label) {
        snprintf(buffer, sizeof(buffer), "%d%%", stats->cpu_percent);
        process_set_cell_text(ui->perf_cpu_value_label, buffer);
    }
    if (ui->perf_mem_meter) {
        VerticalMeterSetValue(ui->perf_mem_meter, stats->memory_percent);
    }
    if (ui->perf_mem_value_label) {
        snprintf(buffer, sizeof(buffer), "%d%%", stats->memory_percent);
        process_set_cell_text(ui->perf_mem_value_label, buffer);
    }
    if (ui->status_cpu_label) {
        snprintf(buffer, sizeof(buffer), "CPU Usage: %d%%", stats->cpu_percent);
        status_set_label_text(ui->status_cpu_label, ui->status_cpu_text,
                              sizeof(ui->status_cpu_text), buffer);
    }
    if (ui->status_memory_label) {
        double used_gb = (double)stats->mem_used_kb / (1024.0 * 1024.0);
        double total_gb = (double)stats->mem_total_kb / (1024.0 * 1024.0);
        char mem_buffer[128];
        snprintf(mem_buffer, sizeof(mem_buffer),
                 "Physical Memory: %d%% (%.1f GB/%.1f GB)",
                 stats->memory_percent, used_gb, total_gb);
        status_set_label_text(ui->status_memory_label, ui->status_memory_text,
                              sizeof(ui->status_memory_text), mem_buffer);
    }
    int load_max = 100;
    if (stats->load1_percent > load_max) load_max = stats->load1_percent;
    if (stats->load5_percent > load_max) load_max = stats->load5_percent;
    if (stats->load15_percent > load_max) load_max = stats->load15_percent;
    if (ui->perf_load1_meter) {
        VerticalMeterSetMaximum(ui->perf_load1_meter, load_max);
        VerticalMeterSetValue(ui->perf_load1_meter, stats->load1_percent);
    }
    if (ui->perf_load1_value_label) {
        snprintf(buffer, sizeof(buffer), "%d%%", stats->load1_percent);
        process_set_cell_text(ui->perf_load1_value_label, buffer);
    }
    if (ui->perf_load5_meter) {
        VerticalMeterSetMaximum(ui->perf_load5_meter, load_max);
        VerticalMeterSetValue(ui->perf_load5_meter, stats->load5_percent);
    }
    if (ui->perf_load5_value_label) {
        snprintf(buffer, sizeof(buffer), "%d%%", stats->load5_percent);
        process_set_cell_text(ui->perf_load5_value_label, buffer);
    }
    if (ui->perf_load15_meter) {
        VerticalMeterSetMaximum(ui->perf_load15_meter, load_max);
        VerticalMeterSetValue(ui->perf_load15_meter, stats->load15_percent);
    }
    if (ui->perf_load15_value_label) {
        snprintf(buffer, sizeof(buffer), "%d%%", stats->load15_percent);
        process_set_cell_text(ui->perf_load15_value_label, buffer);
    }
}
