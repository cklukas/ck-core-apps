#include "ck-tasks-tabs.h"
#include "ck-tasks-ctrl.h"
#include "ck-tasks-ui-helpers.h"

#include <Xm/ArrowBG.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/LabelG.h>
#include <Xm/PushBG.h>
#include <Xm/ScrolledW.h>
#include <Xm/ScrollBar.h>
#include <Xm/TextF.h>
#include <Xm/ToggleBG.h>
#include <X11/Intrinsic.h>
#include <X11/Xlib.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static int g_process_header_last_sort_column = -2;
static SortDirection g_process_header_last_sort_direction = (SortDirection)999;
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
    char *cell_text[PROCESS_COLUMN_COUNT];
    int row_id;
} ProcessRow;

static ProcessRow *g_process_rows = NULL;
static int g_process_rows_alloc = 0;
static const TasksProcessEntry *g_process_entries = NULL;

static int process_row_compare(const void *a, const void *b);
static void process_refresh_rows(void);
static void process_refresh_header_labels(void);
static void process_toggle_sort(int column);
static void process_apply_sort(void);
static void process_set_cell_text(Widget cell, const char *text);
static void process_update_row(ProcessRow *row, const TasksProcessEntry *entry);
static void ensure_process_rows(GridLayout *grid, int needed);
static void release_process_rows(void);
static void on_process_header_activate(Widget widget, XtPointer client, XtPointer call);
static void process_update_viewport_metrics(void);
static void on_process_scroll_window_resize(Widget widget, XtPointer client, XEvent *event, Boolean *continue_to_dispatch);

static void process_refresh_rows(void)
{
    if (!g_process_grid) return;
    process_update_viewport_metrics();
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
}

static void process_set_cell_text(Widget cell, const char *text)
{
    tasks_ui_set_label_text(cell, text);
}

static void process_set_row_cell_text(ProcessRow *row, int column, const char *text)
{
    if (!row || column < 0 || column >= PROCESS_COLUMN_COUNT) return;
    Widget cell = row->cells[column];
    if (!cell) return;
    const char *value = text ? text : "";
    const char *current = row->cell_text[column] ? row->cell_text[column] : "";
    if (strcmp(current, value) == 0) return;
    free(row->cell_text[column]);
    row->cell_text[column] = strdup(value);
    process_set_cell_text(cell, value);
}

static void process_update_row(ProcessRow *row, const TasksProcessEntry *entry)
{
    if (!row || !entry) return;
    char buffer[64];
    process_set_row_cell_text(row, 0, entry->name);
    if (row->cells[1]) {
        snprintf(buffer, sizeof(buffer), "%d", (int)entry->pid);
        process_set_row_cell_text(row, 1, buffer);
    }
    if (row->cells[2]) {
        snprintf(buffer, sizeof(buffer), "%.1f", entry->cpu_percent);
        process_set_row_cell_text(row, 2, buffer);
    }
    if (row->cells[3]) {
        snprintf(buffer, sizeof(buffer), "%.0f", entry->memory_mb);
        process_set_row_cell_text(row, 3, buffer);
    }
    if (row->cells[4]) {
        snprintf(buffer, sizeof(buffer), "%d", entry->threads);
        process_set_row_cell_text(row, 4, buffer);
    }
    process_set_row_cell_text(row, 5, entry->user);
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
        for (int col = 0; col < PROCESS_COLUMN_COUNT; ++col) {
            free(g_process_rows[i].cell_text[col]);
            g_process_rows[i].cell_text[col] = NULL;
        }
        if (g_process_rows[i].row_form && XtIsManaged(g_process_rows[i].row_form)) {
            XtUnmanageChild(g_process_rows[i].row_form);
        }
    }
    free(g_process_rows);
    g_process_rows = NULL;
    g_process_rows_alloc = 0;
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
    if (g_process_header_last_sort_column == g_process_sort_column &&
        g_process_header_last_sort_direction == g_process_sort_direction) {
        return;
    }
    for (int col = 0; col < PROCESS_COLUMN_COUNT; ++col) {
        Widget button = g_process_header_buttons[col];
        if (!button) continue;
        char buffer[64];
        const char *base = process_headers[col];
        snprintf(buffer, sizeof(buffer), "%s", base ? base : "");
        tasks_ui_set_label_text(button, buffer);

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
    g_process_header_last_sort_column = g_process_sort_column;
    g_process_header_last_sort_direction = g_process_sort_direction;
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
            XmString label = tasks_ui_make_string(process_headers[col]);
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

void tasks_ui_destroy_process_tab(TasksUi *ui)
{
    if (ui && ui->process_grid) {
        gridlayout_destroy(ui->process_grid);
        ui->process_grid = NULL;
    }
    release_process_rows();
    if (g_process_row_order) {
        free(g_process_row_order);
        g_process_row_order = NULL;
    }
    g_process_entries = NULL;
    g_process_row_count = 0;
    g_process_sort_direction = SORT_DIR_NONE;
    g_process_sort_column = -1;
    g_process_row_start = 0;
    g_process_row_page_size = PROCESS_VIRTUAL_ROW_COUNT;
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
