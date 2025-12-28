#include "ck_table.h"

#include <X11/Xlib.h>
#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <Xm/ArrowBG.h>
#include <Xm/Frame.h>
#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/PushBG.h>
#include <Xm/ScrolledW.h>

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "../gridlayout/gridlayout.h"

#define CK_TABLE_VIRTUAL_DEFAULT_ROWS 32
#define CK_TABLE_VIRTUAL_DEFAULT_ROW_HEIGHT 24

typedef struct {
    Widget row_form;
    Widget *cells;
    char **cell_text;
    int row_id;
} CkTableVirtualRow;

struct CkTable {
    CkTableMode mode;
    TableColumnDef *columns;
    int column_count;

    TableWidget *table_widget;

    Widget scroll_window;
    GridLayout *grid;
    Widget header_form;
    Widget *header_buttons;
    Widget *header_indicators;
    CkTableVirtualRow *rows;
    int rows_alloc;
    int *row_order;
    int row_count;
    int row_start;
    int row_page_size;
    int row_height;
    TableSortDirection sort_direction;
    int sort_column;

    const void *entries;
    CkTableCellTextFn text_fn;
    CkTableCellNumberFn number_fn;
    CkTableSortCompareFn compare_fn;
    void *callback_context;

    CkTableViewportChangedFn viewport_callback;
    void *viewport_context;
};

static XmString ck_table_make_string(const char *text)
{
    return XmStringCreateLocalized((String)(text ? text : ""));
}

static void ck_table_set_label_text(Widget widget, const char *text)
{
    if (!widget) return;
    XmString label = ck_table_make_string(text);
    XtVaSetValues(widget, XmNlabelString, label, NULL);
    XmStringFree(label);
}

static int ck_table_alignment_to_motif(TableAlignment alignment)
{
    switch (alignment) {
    case TABLE_ALIGN_CENTER:
        return XmALIGNMENT_CENTER;
    case TABLE_ALIGN_RIGHT:
        return XmALIGNMENT_END;
    case TABLE_ALIGN_LEFT:
    default:
        return XmALIGNMENT_BEGINNING;
    }
}

static void ck_table_virtual_update_viewport_metrics(CkTable *table)
{
    if (!table || !table->scroll_window) return;
    Dimension scroll_height = 0;
    XtVaGetValues(table->scroll_window, XmNheight, &scroll_height, NULL);
    int header_height = 0;
    if (table->header_form) {
        Dimension header_value = 0;
        XtVaGetValues(table->header_form, XmNheight, &header_value, NULL);
        header_height = (int)header_value;
    }
    if (table->rows_alloc > 0 && table->rows[0].row_form) {
        Dimension row_height = 0;
        XtVaGetValues(table->rows[0].row_form, XmNheight, &row_height, NULL);
        if (row_height > 0) {
            table->row_height = (int)row_height;
        }
    }
    int available_height = (int)scroll_height - header_height;
    if (available_height <= 0) {
        available_height = table->row_height;
    }
    int row_height = table->row_height > 0 ? table->row_height : CK_TABLE_VIRTUAL_DEFAULT_ROW_HEIGHT;
    int row_spacing = 0;
    if (table->grid) {
        row_spacing = gridlayout_get_row_spacing(table->grid);
    }
    int effective_row_height = row_height + row_spacing;
    if (effective_row_height <= 0) {
        effective_row_height = row_height;
    }
    int rows = available_height / effective_row_height;
    if (rows <= 0) rows = 1;
    table->row_page_size = rows;
}

static void ck_table_virtual_scroll_resize_cb(Widget widget,
                                              XtPointer client,
                                              XEvent *event,
                                              Boolean *continue_to_dispatch)
{
    (void)widget;
    (void)continue_to_dispatch;
    if (!event || event->type != ConfigureNotify) return;
    CkTable *table = (CkTable *)client;
    if (!table) return;
    ck_table_virtual_update_viewport_metrics(table);
    if (table->viewport_callback) {
        table->viewport_callback(table->viewport_context);
    }
}

static void ck_table_virtual_refresh_header(CkTable *table)
{
    if (!table || !table->header_buttons || !table->header_indicators) return;
    for (int col = 0; col < table->column_count; ++col) {
        Widget button = table->header_buttons[col];
        if (!button) continue;
        const char *label = table->columns[col].label ? table->columns[col].label : "";
        ck_table_set_label_text(button, label);

        Widget indicator = table->header_indicators[col];
        if (indicator) {
            Boolean active = (table->sort_column == col && table->sort_direction != TABLE_SORT_NONE);
            if (active) {
                Cardinal direction = (table->sort_direction == TABLE_SORT_DESCENDING)
                                         ? XmARROW_DOWN
                                         : XmARROW_UP;
                XtVaSetValues(indicator, XmNarrowDirection, direction, NULL);
                XtManageChild(indicator);
            } else {
                XtUnmanageChild(indicator);
            }
        }
    }
}

static const char *ck_table_virtual_get_text(CkTable *table, int row, int column,
                                            char *buffer, size_t buffer_len)
{
    if (!table || !table->text_fn) return "";
    return table->text_fn(table->callback_context, table->entries, row, column, buffer, buffer_len);
}

static double ck_table_virtual_get_number(CkTable *table, int row, int column, Boolean *has_value)
{
    if (!table || !table->number_fn) {
        if (has_value) *has_value = False;
        return 0.0;
    }
    return table->number_fn(table->callback_context, table->entries, row, column, has_value);
}

static CkTable *g_ck_table_sort_context = NULL;

static int ck_table_virtual_compare_rows(const void *a, const void *b)
{
    CkTable *table = g_ck_table_sort_context;
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    if (!table || table->sort_direction == TABLE_SORT_NONE || table->sort_column < 0) {
        return ia - ib;
    }
    int column = table->sort_column;
    int cmp = 0;
    if (table->compare_fn) {
        cmp = table->compare_fn(table->callback_context, table->entries, ia, ib,
                                column, table->sort_direction);
    } else {
        if (column >= 0 && column < table->column_count && table->columns[column].numeric) {
            Boolean has_a = False;
            Boolean has_b = False;
            double da = ck_table_virtual_get_number(table, ia, column, &has_a);
            double db = ck_table_virtual_get_number(table, ib, column, &has_b);
            if (!has_a && !has_b) {
                cmp = 0;
            } else if (!has_a) {
                cmp = -1;
            } else if (!has_b) {
                cmp = 1;
            } else if (da < db) {
                cmp = -1;
            } else if (da > db) {
                cmp = 1;
            } else {
                cmp = 0;
            }
        } else {
            char va[128] = {0};
            char vb[128] = {0};
            const char *sa = ck_table_virtual_get_text(table, ia, column, va, sizeof(va));
            const char *sb = ck_table_virtual_get_text(table, ib, column, vb, sizeof(vb));
            if (!sa) sa = "";
            if (!sb) sb = "";
            cmp = strcoll(sa, sb);
        }
    }
    if (table->sort_direction == TABLE_SORT_DESCENDING) cmp = -cmp;
    if (cmp == 0) cmp = ia - ib;
    return cmp;
}

static void ck_table_virtual_apply_sort(CkTable *table)
{
    if (!table) return;
    if (!table->row_order || table->row_count <= 0) {
        ck_table_virtual_refresh_header(table);
        return;
    }
    for (int i = 0; i < table->row_count; ++i) {
        table->row_order[i] = i;
    }
    if (table->sort_direction != TABLE_SORT_NONE) {
        g_ck_table_sort_context = table;
        qsort(table->row_order, table->row_count, sizeof(int), ck_table_virtual_compare_rows);
        g_ck_table_sort_context = NULL;
    }
    ck_table_virtual_refresh_header(table);
}

static void ck_table_virtual_update_row(CkTable *table, CkTableVirtualRow *row, int entry_index)
{
    if (!table || !row || !table->entries) return;
    for (int col = 0; col < table->column_count; ++col) {
        char buffer[128];
        const char *value = ck_table_virtual_get_text(table, entry_index, col, buffer, sizeof(buffer));
        if (!value) value = "";
        const char *current = row->cell_text[col] ? row->cell_text[col] : "";
        if (strcmp(current, value) == 0) continue;
        free(row->cell_text[col]);
        row->cell_text[col] = strdup(value);
        ck_table_set_label_text(row->cells[col], value);
    }
}

static void ck_table_virtual_refresh_rows(CkTable *table)
{
    if (!table || !table->grid) return;
    ck_table_virtual_update_viewport_metrics(table);
    int total_rows = table->row_count;
    int page_size = table->row_page_size;
    if (page_size <= 0) page_size = CK_TABLE_VIRTUAL_DEFAULT_ROWS;
    int max_start = total_rows > page_size ? total_rows - page_size : 0;
    if (table->row_start < 0) table->row_start = 0;
    if (table->row_start > max_start) table->row_start = max_start;
    int visible_rows = page_size;
    if (visible_rows <= 0) visible_rows = CK_TABLE_VIRTUAL_DEFAULT_ROWS;
    if (table->rows_alloc < visible_rows) {
        int needed = visible_rows;
        CkTableVirtualRow *expanded = (CkTableVirtualRow *)realloc(table->rows,
                                                                    sizeof(CkTableVirtualRow) * needed);
        if (!expanded) return;
        table->rows = expanded;
        for (int i = table->rows_alloc; i < needed; ++i) {
            CkTableVirtualRow *row = &table->rows[i];
            memset(row, 0, sizeof(*row));
            row->row_id = gridlayout_add_row(table->grid);
            row->row_form = gridlayout_get_row_form(table->grid, row->row_id);
            if (!row->row_form) continue;
            row->cells = (Widget *)calloc(table->column_count, sizeof(Widget));
            row->cell_text = (char **)calloc(table->column_count, sizeof(char *));
            if (!row->cells || !row->cell_text) continue;
            for (int col = 0; col < table->column_count; ++col) {
                Widget cell = XmCreateLabelGadget(row->row_form, "ckTableCell", NULL, 0);
                XtVaSetValues(cell,
                              XmNalignment, ck_table_alignment_to_motif(table->columns[col].alignment),
                              XmNrecomputeSize, False,
                              XmNmarginWidth, 6,
                              XmNmarginHeight, 3,
                              XmNborderWidth, 1,
                              XmNshadowThickness, 1,
                              XmNshadowType, XmSHADOW_OUT,
                              NULL);
                gridlayout_add_cell(table->grid, row->row_id, col, cell, 1);
                row->cells[col] = cell;
            }
            XtUnmanageChild(row->row_form);
        }
        table->rows_alloc = needed;
    }

    int available_rows = total_rows - table->row_start;
    if (available_rows < 0) available_rows = 0;
    if (visible_rows > available_rows) {
        visible_rows = available_rows;
    }

    for (int i = 0; i < visible_rows; ++i) {
        CkTableVirtualRow *row = &table->rows[i];
        if (!row || !row->row_form) continue;
        int dataset_index = table->row_start + i;
        if (dataset_index >= total_rows) break;
        int entry_index = table->row_order ? table->row_order[dataset_index] : dataset_index;
        ck_table_virtual_update_row(table, row, entry_index);
        if (!XtIsManaged(row->row_form)) {
            XtManageChild(row->row_form);
        }
    }
    for (int i = visible_rows; i < table->rows_alloc; ++i) {
        if (table->rows[i].row_form && XtIsManaged(table->rows[i].row_form)) {
            XtUnmanageChild(table->rows[i].row_form);
        }
    }
}

static void ck_table_virtual_release_rows(CkTable *table)
{
    if (!table || !table->rows) return;
    for (int i = 0; i < table->rows_alloc; ++i) {
        for (int col = 0; col < table->column_count; ++col) {
            free(table->rows[i].cell_text ? table->rows[i].cell_text[col] : NULL);
        }
        free(table->rows[i].cell_text);
        free(table->rows[i].cells);
        if (table->rows[i].row_form && XtIsManaged(table->rows[i].row_form)) {
            XtUnmanageChild(table->rows[i].row_form);
        }
    }
    free(table->rows);
    table->rows = NULL;
    table->rows_alloc = 0;
}

static void ck_table_virtual_toggle_sort(CkTable *table, int column)
{
    if (!table || column < 0 || column >= table->column_count) return;
    if (table->sort_column != column) {
        table->sort_column = column;
        table->sort_direction = TABLE_SORT_ASCENDING;
    } else if (table->sort_direction == TABLE_SORT_ASCENDING) {
        table->sort_direction = TABLE_SORT_DESCENDING;
    } else if (table->sort_direction == TABLE_SORT_DESCENDING) {
        table->sort_direction = TABLE_SORT_NONE;
    } else {
        table->sort_direction = TABLE_SORT_ASCENDING;
    }
    ck_table_virtual_apply_sort(table);
    ck_table_virtual_refresh_rows(table);
}

static void ck_table_virtual_on_header_activate(Widget widget, XtPointer client, XtPointer call)
{
    (void)widget;
    (void)call;
    CkTable *table = (CkTable *)client;
    if (!table) return;
    XtPointer data = NULL;
    XtVaGetValues(widget, XmNuserData, &data, NULL);
    int column = (int)(intptr_t)data;
    if (column < 0 || column >= table->column_count) return;
    if (!table->columns[column].sortable) return;
    ck_table_virtual_toggle_sort(table, column);
}

static void ck_table_virtual_create_header(CkTable *table)
{
    if (!table || !table->grid) return;
    int header_row = gridlayout_add_row(table->grid);
    Widget header_form = gridlayout_get_row_form(table->grid, header_row);
    table->header_form = header_form;
    table->header_buttons = (Widget *)calloc(table->column_count, sizeof(Widget));
    table->header_indicators = (Widget *)calloc(table->column_count, sizeof(Widget));
    if (!header_form || !table->header_buttons || !table->header_indicators) return;

    for (int col = 0; col < table->column_count; ++col) {
        Widget header_frame = XmCreateFrame(header_form, "ckTableHeaderFrame", NULL, 0);
        XtVaSetValues(header_frame,
                      XmNshadowThickness, 2,
                      XmNshadowType, XmSHADOW_IN,
                      XmNmarginWidth, 0,
                      XmNmarginHeight, 2,
                      XmNborderWidth, 1,
                      XmNborderColor, BlackPixel(XtDisplay(header_frame),
                                                 DefaultScreen(XtDisplay(header_frame))),
                      NULL);

        Widget header_cell = XmCreateForm(header_frame, "ckTableHeaderCell", NULL, 0);
        XtVaSetValues(header_cell,
                      XmNfractionBase, 100,
                      XmNmarginWidth, 0,
                      XmNmarginHeight, 0,
                      XmNshadowThickness, 0,
                      XmNshadowType, XmSHADOW_OUT,
                      NULL);
        XtManageChild(header_cell);

        Widget cell = XmCreatePushButtonGadget(header_cell, "ckTableHeaderButton", NULL, 0);
        XmString label = ck_table_make_string(table->columns[col].label);
        XtVaSetValues(cell,
                      XmNlabelString, label,
                      XmNalignment, ck_table_alignment_to_motif(table->columns[col].alignment),
                      XmNrecomputeSize, False,
                      XmNmarginWidth, 6,
                      XmNmarginHeight, 4,
                      XmNborderWidth, 0,
                      XmNshadowThickness, 0,
                      XmNshadowType, XmSHADOW_OUT,
                      XmNleftAttachment, XmATTACH_FORM,
                      XmNrightAttachment, XmATTACH_POSITION,
                      XmNrightPosition, 80,
                      XmNuserData, (XtPointer)(intptr_t)col,
                      NULL);
        XmStringFree(label);
        XtAddCallback(cell, XmNactivateCallback, ck_table_virtual_on_header_activate, table);
        table->header_buttons[col] = cell;

        Widget indicator_form = XmCreateForm(header_cell, "ckTableHeaderIndicatorForm", NULL, 0);
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
            "ckTableHeaderSortIndicator",
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
        XtAddCallback(indicator, XmNactivateCallback, ck_table_virtual_on_header_activate, table);
        XtUnmanageChild(indicator);
        table->header_indicators[col] = indicator;

        XtManageChild(cell);
        gridlayout_add_cell(table->grid, header_row, col, header_frame, 1);
    }
}

static TableColumnDef *ck_table_copy_columns(const TableColumnDef *columns, int column_count)
{
    if (!columns || column_count <= 0) return NULL;
    TableColumnDef *copy = (TableColumnDef *)calloc((size_t)column_count, sizeof(TableColumnDef));
    if (!copy) return NULL;
    for (int i = 0; i < column_count; ++i) {
        copy[i] = columns[i];
    }
    return copy;
}

CkTable *ck_table_create_standard(Widget parent, const char *name,
                                 const TableColumnDef *columns,
                                 int column_count)
{
    if (!parent || !columns || column_count <= 0) return NULL;
    CkTable *table = (CkTable *)calloc(1, sizeof(CkTable));
    if (!table) return NULL;
    table->mode = CK_TABLE_MODE_STANDARD;
    table->columns = ck_table_copy_columns(columns, column_count);
    table->column_count = column_count;
    table->table_widget = table_widget_create(parent, name, columns, column_count);
    if (!table->table_widget) {
        ck_table_destroy(table);
        return NULL;
    }
    return table;
}

CkTable *ck_table_create_virtual(Widget parent, const char *name,
                                 const TableColumnDef *columns,
                                 int column_count)
{
    if (!parent || !columns || column_count <= 0) return NULL;
    CkTable *table = (CkTable *)calloc(1, sizeof(CkTable));
    if (!table) return NULL;
    table->mode = CK_TABLE_MODE_VIRTUAL;
    table->columns = ck_table_copy_columns(columns, column_count);
    table->column_count = column_count;
    table->row_height = CK_TABLE_VIRTUAL_DEFAULT_ROW_HEIGHT;
    table->row_page_size = CK_TABLE_VIRTUAL_DEFAULT_ROWS;
    table->sort_direction = TABLE_SORT_NONE;
    table->sort_column = -1;

    Arg scroll_args[8];
    int sn = 0;
    XtSetArg(scroll_args[sn], XmNscrollingPolicy, XmAPPLICATION_DEFINED); sn++;
    XtSetArg(scroll_args[sn], XmNvisualPolicy, XmVARIABLE); sn++;
    XtSetArg(scroll_args[sn], XmNshadowThickness, 0); sn++;
    Widget scroll = XmCreateScrolledWindow(parent, (String)(name ? name : "ckTableScroll"),
                                           scroll_args, sn);
    XtManageChild(scroll);
    table->scroll_window = scroll;

    GridLayout *grid = gridlayout_create(scroll, "ckTableGrid", column_count);
    if (grid) {
        table->grid = grid;
        gridlayout_set_row_spacing(grid, 4);
        Widget grid_widget = gridlayout_get_widget(grid);
        XtVaSetValues(scroll, XmNworkWindow, grid_widget, NULL);
        ck_table_virtual_create_header(table);
    }
    XtAddEventHandler(scroll, StructureNotifyMask, False,
                      ck_table_virtual_scroll_resize_cb, (XtPointer)table);

    return table;
}

void ck_table_destroy(CkTable *table)
{
    if (!table) return;
    if (table->table_widget) {
        table_widget_destroy(table->table_widget);
        table->table_widget = NULL;
    }
    ck_table_virtual_release_rows(table);
    if (table->row_order) {
        free(table->row_order);
        table->row_order = NULL;
    }
    if (table->grid) {
        gridlayout_destroy(table->grid);
        table->grid = NULL;
    }
    free(table->header_buttons);
    free(table->header_indicators);
    free(table->columns);
    free(table);
}

Widget ck_table_get_widget(CkTable *table)
{
    if (!table) return NULL;
    if (table->mode == CK_TABLE_MODE_STANDARD && table->table_widget) {
        return table_widget_get_widget(table->table_widget);
    }
    if (table->mode == CK_TABLE_MODE_VIRTUAL) {
        return table->scroll_window;
    }
    return NULL;
}

CkTableMode ck_table_get_mode(const CkTable *table)
{
    return table ? table->mode : CK_TABLE_MODE_STANDARD;
}

TableRow *ck_table_add_row(CkTable *table, const char *const values[])
{
    if (!table || table->mode != CK_TABLE_MODE_STANDARD) return NULL;
    return table_widget_add_row(table->table_widget, values);
}

TableRow *ck_table_add_row_with_sort_values(CkTable *table,
                                            const char *const values[],
                                            const char *const sort_values[])
{
    if (!table || table->mode != CK_TABLE_MODE_STANDARD) return NULL;
    return table_widget_add_row_with_sort_values(table->table_widget, values, sort_values);
}

void ck_table_update_row(TableRow *row, const char *const values[])
{
    if (!row) return;
    table_widget_update_row(row, values);
}

void ck_table_update_row_with_sort_values(TableRow *row,
                                          const char *const values[],
                                          const char *const sort_values[])
{
    if (!row) return;
    table_widget_update_row_with_sort_values(row, values, sort_values);
}

void ck_table_remove_row(CkTable *table, TableRow *row)
{
    if (!table || table->mode != CK_TABLE_MODE_STANDARD || !row) return;
    table_widget_remove_row(table->table_widget, row);
}

void ck_table_clear(CkTable *table)
{
    if (!table || table->mode != CK_TABLE_MODE_STANDARD) return;
    table_widget_clear(table->table_widget);
}

Widget ck_table_row_get_widget(TableRow *row)
{
    return table_row_get_widget(row);
}

void ck_table_set_grid(CkTable *table, Boolean enabled)
{
    if (!table || table->mode != CK_TABLE_MODE_STANDARD) return;
    table_widget_set_grid(table->table_widget, enabled);
}

void ck_table_set_header_font(CkTable *table, XmFontList font)
{
    if (!table || table->mode != CK_TABLE_MODE_STANDARD) return;
    table_widget_set_header_font(table->table_widget, font);
}

void ck_table_set_row_font(CkTable *table, XmFontList font)
{
    if (!table || table->mode != CK_TABLE_MODE_STANDARD) return;
    table_widget_set_row_font(table->table_widget, font);
}

void ck_table_set_row_colors(CkTable *table, Pixel even_row, Pixel odd_row)
{
    if (!table || table->mode != CK_TABLE_MODE_STANDARD) return;
    table_widget_set_row_colors(table->table_widget, even_row, odd_row);
}

void ck_table_set_column_color(CkTable *table, int column, Pixel color)
{
    if (!table || table->mode != CK_TABLE_MODE_STANDARD) return;
    table_widget_set_column_color(table->table_widget, column, color);
}

void ck_table_set_alternate_row_colors(CkTable *table, Boolean enabled)
{
    if (!table || table->mode != CK_TABLE_MODE_STANDARD) return;
    table_widget_set_alternate_row_colors(table->table_widget, enabled);
}

void ck_table_sort_by_column(CkTable *table, int column,
                             TableSortDirection direction)
{
    if (!table) return;
    if (table->mode == CK_TABLE_MODE_STANDARD) {
        table_widget_sort_by_column(table->table_widget, column, direction);
        return;
    }
    if (table->mode == CK_TABLE_MODE_VIRTUAL) {
        table->sort_column = column;
        table->sort_direction = direction;
        ck_table_virtual_apply_sort(table);
        ck_table_virtual_refresh_rows(table);
    }
}

void ck_table_toggle_sorting(CkTable *table, int column)
{
    if (!table) return;
    if (table->mode == CK_TABLE_MODE_STANDARD) {
        table_widget_toggle_sorting(table->table_widget, column);
        return;
    }
    if (table->mode == CK_TABLE_MODE_VIRTUAL) {
        ck_table_virtual_toggle_sort(table, column);
    }
}

Boolean ck_table_suspend_updates(CkTable *table)
{
    if (!table || table->mode != CK_TABLE_MODE_STANDARD) return False;
    return table_widget_suspend_updates(table->table_widget);
}

void ck_table_resume_updates(CkTable *table, Boolean suspended)
{
    if (!table || table->mode != CK_TABLE_MODE_STANDARD) return;
    table_widget_resume_updates(table->table_widget, suspended);
}

void ck_table_set_virtual_callbacks(CkTable *table,
                                    CkTableCellTextFn text_fn,
                                    CkTableCellNumberFn number_fn,
                                    CkTableSortCompareFn compare_fn,
                                    void *context)
{
    if (!table || table->mode != CK_TABLE_MODE_VIRTUAL) return;
    table->text_fn = text_fn;
    table->number_fn = number_fn;
    table->compare_fn = compare_fn;
    table->callback_context = context;
}

void ck_table_set_virtual_data(CkTable *table, const void *entries, int count)
{
    if (!table || table->mode != CK_TABLE_MODE_VIRTUAL) return;
    table->entries = entries;
    table->row_count = (entries && count > 0) ? count : 0;
    if (table->row_order) {
        free(table->row_order);
        table->row_order = NULL;
    }
    if (table->row_count > 0) {
        table->row_order = (int *)calloc((size_t)table->row_count, sizeof(int));
        if (!table->row_order) {
            table->row_count = 0;
            return;
        }
    }
    ck_table_virtual_apply_sort(table);
    ck_table_virtual_refresh_rows(table);
}

void ck_table_set_virtual_row_window(CkTable *table, int start)
{
    if (!table || table->mode != CK_TABLE_MODE_VIRTUAL) return;
    int total = table->row_count;
    int page = table->row_page_size;
    if (page <= 0) page = CK_TABLE_VIRTUAL_DEFAULT_ROWS;
    int max_start = total > page ? total - page : 0;
    if (max_start < 0) max_start = 0;
    if (start < 0) start = 0;
    if (start > max_start) start = max_start;
    table->row_start = start;
    ck_table_virtual_refresh_rows(table);
}

int ck_table_get_virtual_row_page_size(const CkTable *table)
{
    if (!table || table->mode != CK_TABLE_MODE_VIRTUAL) return 0;
    return table->row_page_size;
}

void ck_table_set_virtual_row_spacing(CkTable *table, int pixels)
{
    if (!table || table->mode != CK_TABLE_MODE_VIRTUAL || !table->grid) return;
    gridlayout_set_row_spacing(table->grid, pixels);
}

void ck_table_set_virtual_viewport_changed_callback(CkTable *table,
                                                    CkTableViewportChangedFn callback,
                                                    void *context)
{
    if (!table || table->mode != CK_TABLE_MODE_VIRTUAL) return;
    table->viewport_callback = callback;
    table->viewport_context = context;
}
