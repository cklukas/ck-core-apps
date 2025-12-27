#include "table_widget.h"

#include <X11/Xlib.h>
#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <Xm/ArrowBG.h>
#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/ScrolledW.h>
#include <Xm/Separator.h>
#include <Xm/Frame.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef XtNbackingStore
#define XtNbackingStore "backingStore"
#endif
#ifndef XtNsaveUnder
#define XtNsaveUnder "saveUnder"
#endif

typedef struct TableRow {
    TableWidget *table;
    Widget row_form;
    Widget *cells;
    char **values;
    char **sort_values;
    int original_index;
} TableRow;

struct TableWidget {
    Widget container;
    Widget header_row;
    Widget scroll_window;
    Widget rows_column;
    TableColumnDef *columns;
    int column_count;
    Widget *header_buttons;
    Widget *header_indicators;
    TableRow **rows;
    int row_count;
    int row_capacity;
    int next_index;
    TableSortDirection sort_direction;
    int sort_column;
    Boolean grid;
    Boolean alternate_rows;
    Pixel even_row_color;
    Pixel odd_row_color;
    XmFontList header_font;
    XmFontList row_font;
    Pixel *column_colors;
    Boolean updates_suspended;
    Boolean sort_pending;
};

static void table_widget_refresh_layout(TableWidget *table);

static void table_widget_enable_backing_store(Widget widget)
{
    if (!widget || !XtIsWidget(widget)) return;
    XtVaSetValues(widget,
                  XtNbackingStore, Always,
                  XtNsaveUnder, True,
                  NULL);
}

static const char *sort_direction_label(TableSortDirection direction)
{
    switch (direction) {
    case TABLE_SORT_ASCENDING:
        return "asc";
    case TABLE_SORT_DESCENDING:
        return "desc";
    case TABLE_SORT_NONE:
    default:
        return "none";
    }
}

static void log_table_event(TableWidget *table, const char *event, Widget widget, int column)
{
    if (!table || !event) return;
    const char *table_name = table->container ? XtName(table->container) : "(unknown)";
    const char *widget_name = widget ? XtName(widget) : "(unknown)";
    fprintf(stderr,
            "[table-widget] %s table=%s widget=%s column=%d sort_column=%d sort_dir=%s rows=%d\n",
            event,
            table_name,
            widget_name,
            column,
            table->sort_column,
            sort_direction_label(table->sort_direction),
            table->row_count);
}

static void table_widget_update_header_offset(TableWidget *table)
{
    if (!table || !table->header_row || !table->scroll_window) return;
    Widget vscroll = NULL;
    XtVaGetValues(table->scroll_window, XmNverticalScrollBar, &vscroll, NULL);
    Dimension offset = 0;
    if (vscroll && XtIsManaged(vscroll)) {
        XtVaGetValues(vscroll, XmNwidth, &offset, NULL);
    }
    XtVaSetValues(table->header_row, XmNrightOffset, (int)offset, NULL);
}

static void table_widget_update_row_width(TableWidget *table)
{
    if (!table || !table->rows_column || !table->scroll_window) return;
    Widget clip = NULL;
    XtVaGetValues(table->scroll_window, XmNclipWindow, &clip, NULL);
    Dimension width = 0;
    XtVaGetValues(table->scroll_window, XmNwidth, &width, NULL);

    Dimension reserved = 0;
    Widget vscroll = NULL;
    XtVaGetValues(table->scroll_window, XmNverticalScrollBar, &vscroll, NULL);
    if (vscroll && XtIsManaged(vscroll)) {
        XtVaGetValues(vscroll, XmNwidth, &reserved, NULL);
    }
    if (reserved > 0 && width > reserved) {
        width = (Dimension)(width - reserved);
    }

    if (clip) {
        Dimension clip_width = 0;
        XtVaGetValues(clip, XmNwidth, &clip_width, NULL);
        if (clip_width > 0 && clip_width < width) {
            width = clip_width;
        }
    }
    if (width <= 0) return;
    XtVaSetValues(table->rows_column, XmNwidth, width, NULL);
    for (int i = 0; i < table->row_count; ++i) {
        XtVaSetValues(table->rows[i]->row_form, XmNwidth, width, NULL);
    }
}

static void table_widget_scrollbar_structure_event(Widget widget, XtPointer client, XEvent *event, Boolean *continue_to_dispatch)
{
    (void)widget;
    (void)continue_to_dispatch;
    if (!event) return;
    if (event->type != MapNotify && event->type != UnmapNotify && event->type != ConfigureNotify) return;
    TableWidget *table = (TableWidget *)client;
    table_widget_refresh_layout(table);
}

static void table_widget_scroll_resize_cb(Widget widget, XtPointer client, XEvent *event, Boolean *continue_to_dispatch)
{
    (void)widget;
    (void)continue_to_dispatch;
    if (!event || event->type != ConfigureNotify) return;
    TableWidget *table = (TableWidget *)client;
    table_widget_update_header_offset(table);
    table_widget_update_row_width(table);
}

static void table_widget_refresh_layout(TableWidget *table)
{
    if (!table) return;
    table_widget_update_header_offset(table);
    table_widget_update_row_width(table);
    if (table->scroll_window) {
        Widget clip = NULL;
        Widget vscroll = NULL;
        XtVaGetValues(table->scroll_window,
                      XmNclipWindow, &clip,
                      XmNverticalScrollBar, &vscroll,
                      NULL);
        table_widget_enable_backing_store(clip);
        table_widget_enable_backing_store(vscroll);
    }
}

static XmString make_string(const char *text)
{
    return XmStringCreateLocalized((String)(text ? text : ""));
}

static const char *safe_value(const char *value)
{
    return value ? value : "";
}

static const char *row_sort_value(const TableRow *row, int column)
{
    if (!row || column < 0 || column >= row->table->column_count) return "";
    if (row->sort_values && row->sort_values[column]) return row->sort_values[column];
    if (row->values && row->values[column]) return row->values[column];
    return "";
}

static void apply_row_colors(TableWidget *table, TableRow *row, int row_index)
{
    if (!table || !row) return;
    Pixel bg = None;
    if (table->alternate_rows) {
        bg = (row_index % 2 == 0) ? table->even_row_color : table->odd_row_color;
    }
    if (bg != None) {
        XtVaSetValues(row->row_form, XmNbackground, bg, NULL);
    }
    for (int i = 0; i < table->column_count; ++i) {
        Pixel col = table->column_colors ? table->column_colors[i] : None;
        if (col != None) {
            XtVaSetValues(row->cells[i], XmNbackground, col, NULL);
        }
    }
}

static void update_cell_label(TableRow *row, int column, const char *value)
{
    if (!row || column < 0 || column >= row->table->column_count) return;
    XmString label = make_string(value);
    XtVaSetValues(row->cells[column], XmNlabelString, label, NULL);
    XmStringFree(label);
}

static void sort_rows(TableWidget *table);

static TableRow *table_row_create(TableWidget *table,
                                  const char *const values[],
                                  const char *const sort_values[])
{
    if (!table) return NULL;
    TableRow *row = (TableRow *)calloc(1, sizeof(TableRow));
    if (!row) return NULL;
    row->table = table;
    row->cells = (Widget *)calloc(table->column_count, sizeof(Widget));
    row->values = (char **)calloc(table->column_count, sizeof(char *));
    row->sort_values = (char **)calloc(table->column_count, sizeof(char *));
    if (!row->cells || !row->values || !row->sort_values) {
        free(row->cells);
        free(row->values);
        free(row->sort_values);
        free(row);
        return NULL;
    }
    row->row_form = XmCreateForm(table->rows_column, "tableRow", NULL, 0);
    XtVaSetValues(row->row_form,
                  XmNfractionBase, table->column_count * 10,
                  XmNmarginHeight, 2,
                  XmNmarginWidth, 2,
                  XmNrecomputeSize, False,
                  XmNshadowThickness, table->grid ? 1 : 0,
                  XmNshadowType, table->grid ? XmSHADOW_ETCHED_IN : XmSHADOW_OUT,
                  XmNnavigationType, XmTAB_GROUP,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  NULL);
    table_widget_enable_backing_store(row->row_form);

    for (int col = 0; col < table->column_count; ++col) {
        TableColumnDef *def = &table->columns[col];
        const char *content = safe_value(values ? values[col] : NULL);
        row->values[col] = strdup(content);
        const char *sort_value = safe_value(sort_values ? sort_values[col] : NULL);
        if (sort_values && sort_values[col] == NULL) {
            sort_value = content;
        }
        if (!sort_values) {
            sort_value = content;
        }
        row->sort_values[col] = strdup(sort_value);
        int left_pos = col * 10;
        int right_pos = (col + 1) * 10;
        if (col == table->column_count - 1) {
            right_pos = table->column_count * 10;
        }
        Widget cell = XtVaCreateManagedWidget(
            "tableCell",
            xmLabelGadgetClass, row->row_form,
            XmNlabelString, make_string(content),
            XmNalignment, def->alignment == TABLE_ALIGN_RIGHT ? XmALIGNMENT_END :
                         def->alignment == TABLE_ALIGN_CENTER ? XmALIGNMENT_CENTER :
                         XmALIGNMENT_BEGINNING,
            XmNrecomputeSize, False,
            XmNmarginWidth, 4,
            XmNmarginHeight, 2,
            XmNleftAttachment, XmATTACH_POSITION,
            XmNleftPosition, left_pos,
            XmNrightAttachment, XmATTACH_POSITION,
            XmNrightPosition, right_pos,
            XmNtopAttachment, XmATTACH_FORM,
            XmNbottomAttachment, XmATTACH_FORM,
            XmNborderWidth, 1,
            NULL);
        if (table->row_font) {
            XtVaSetValues(cell, XmNfontList, table->row_font, NULL);
        }
        row->cells[col] = cell;
    }

    row->original_index = table->next_index++;
    XtManageChild(row->row_form);
    apply_row_colors(table, row, table->row_count);
    return row;
}

static Boolean table_row_update_sort_values(TableRow *row,
                                            const char *const values[],
                                            const char *const sort_values[])
{
    if (!row) return False;
    Boolean changed = False;
    for (int i = 0; i < row->table->column_count; ++i) {
        const char *display = safe_value(values ? values[i] : NULL);
        const char *sort_value = safe_value(sort_values ? sort_values[i] : NULL);
        if (!sort_values || (sort_values && sort_values[i] == NULL)) {
            sort_value = display;
        }
        const char *current = row->sort_values[i] ? row->sort_values[i] : "";
        if (strcmp(current, sort_value) == 0) continue;
        free(row->sort_values[i]);
        row->sort_values[i] = strdup(sort_value);
        changed = True;
    }
    return changed;
}

static void destroy_row(TableRow *row)
{
    if (!row) return;
    if (row->row_form) {
        XtDestroyWidget(row->row_form);
    }
    if (row->cells) {
        free(row->cells);
    }
    if (row->values) {
        for (int i = 0; i < row->table->column_count; ++i) {
            free(row->values[i]);
        }
        free(row->values);
    }
    if (row->sort_values) {
        for (int i = 0; i < row->table->column_count; ++i) {
            free(row->sort_values[i]);
        }
        free(row->sort_values);
    }
    free(row);
}

static void ensure_row_capacity(TableWidget *table)
{
    if (table->row_count >= table->row_capacity) {
        int new_cap = table->row_capacity ? table->row_capacity * 2 : 16;
        TableRow **new_rows = (TableRow **)realloc(table->rows, new_cap * sizeof(TableRow *));
        if (new_rows) {
            table->rows = new_rows;
            table->row_capacity = new_cap;
        }
    }
}

static void reorder_row_children(TableWidget *table)
{
    if (!table) return;
    for (int i = 0; i < table->row_count; ++i) {
        if (table->rows[i] && table->rows[i]->row_form) {
            XtVaSetValues(table->rows[i]->row_form,
                          XmNpositionIndex, i,
                          NULL);
        }
        apply_row_colors(table, table->rows[i], i);
    }
    table_widget_update_row_width(table);
}

static int compare_rows(TableWidget *table, const TableRow *a, const TableRow *b)
{
    if (!table || !a || !b) return 0;
    if (table->sort_direction == TABLE_SORT_NONE) {
        return a->original_index - b->original_index;
    }
    int column = table->sort_column;
    if (column < 0 || column >= table->column_count) {
        return a->original_index - b->original_index;
    }
    const char *av = row_sort_value(a, column);
    const char *bv = row_sort_value(b, column);
    int cmp = 0;
    if (table->columns[column].numeric) {
        double da = 0.0;
        double db = 0.0;
        char *end_a = NULL;
        char *end_b = NULL;
        if (!av || av[0] == '\0') {
            da = 1e18;
        } else {
            da = strtod(av, &end_a);
            if (end_a == av) da = 1e18;
        }
        if (!bv || bv[0] == '\0') {
            db = 1e18;
        } else {
            db = strtod(bv, &end_b);
            if (end_b == bv) db = 1e18;
        }
        if (da < db) cmp = -1;
        else if (da > db) cmp = 1;
        else cmp = 0;
    } else {
        cmp = strcoll(av, bv);
    }
    if (table->sort_direction == TABLE_SORT_DESCENDING) {
        cmp = -cmp;
    }
    if (cmp == 0) {
        cmp = a->original_index - b->original_index;
    }
    return cmp;
}

static void sort_rows(TableWidget *table)
{
    if (!table || table->row_count < 2) return;
    for (int i = 1; i < table->row_count; ++i) {
        TableRow *key = table->rows[i];
        int j = i - 1;
        while (j >= 0 && compare_rows(table, key, table->rows[j]) < 0) {
            table->rows[j + 1] = table->rows[j];
            --j;
        }
        table->rows[j + 1] = key;
    }
    reorder_row_children(table);
}

static void update_header_labels(TableWidget *table)
{
    if (!table || !table->header_buttons) return;
    for (int i = 0; i < table->column_count; ++i) {
        const char *base = table->columns[i].label;
        char label_text[128];
        label_text[0] = '\0';
        if (!base) base = "";
        snprintf(label_text, sizeof(label_text), "%s", base);
        XmString label = make_string(label_text);
        XtVaSetValues(table->header_buttons[i], XmNlabelString, label, NULL);
        XmStringFree(label);

        Widget indicator = table->header_indicators ? table->header_indicators[i] : NULL;
        if (indicator) {
            Boolean active = (table->sort_column == i && table->sort_direction != TABLE_SORT_NONE);
            if (active) {
                Cardinal direction = (table->sort_direction == TABLE_SORT_DESCENDING)
                                     ? XmARROW_DOWN : XmARROW_UP;
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

static void header_activate_cb(Widget widget, XtPointer client, XtPointer call)
{
    (void)call;
    TableWidget *table = (TableWidget *)client;
    XtPointer index_ptr = NULL;
    XtVaGetValues(widget, XmNuserData, &index_ptr, NULL);
    int index = index_ptr ? (int)(intptr_t)index_ptr : -1;
    if (index < 0) return;
    log_table_event(table, "header_activate", widget, index);
    table_widget_toggle_sorting(table, index);
}

TableWidget *table_widget_create(Widget parent, const char *name,
                                 const TableColumnDef *columns, int column_count)
{
    if (!parent || !columns || column_count <= 0) return NULL;
    TableWidget *table = (TableWidget *)calloc(1, sizeof(TableWidget));
    if (!table) return NULL;
    table->columns = (TableColumnDef *)calloc(column_count, sizeof(TableColumnDef));
    table->column_count = column_count;
    table->header_buttons = (Widget *)calloc(column_count, sizeof(Widget));
    table->header_indicators = (Widget *)calloc(column_count, sizeof(Widget));
    table->column_colors = (Pixel *)calloc(column_count, sizeof(Pixel));
    if (!table->columns || !table->header_buttons || !table->column_colors ||
        !table->header_indicators) {
        table_widget_destroy(table);
        return NULL;
    }
    memcpy(table->columns, columns, column_count * sizeof(TableColumnDef));

    Widget form = XmCreateForm(parent, (String)(name ? name : "tableForm"), NULL, 0);
    table->container = form;
    table_widget_enable_backing_store(form);

    table->header_row = XmCreateForm(form, "tableHeader", NULL, 0);
    XtVaSetValues(table->header_row,
                  XmNfractionBase, column_count * 10,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNmarginHeight, 0,
                  XmNmarginWidth, 0,
                  XmNshadowThickness, 0,
                  NULL);
    table_widget_enable_backing_store(table->header_row);
    XtManageChild(table->header_row);

    for (int i = 0; i < column_count; ++i) {
        TableColumnDef *def = &table->columns[i];
        Widget header_frame = XmCreateFrame(table->header_row, "headerFrame", NULL, 0);
        XtVaSetValues(header_frame,
                      XmNshadowThickness, 2,
                      XmNshadowType, XmSHADOW_IN,
                      XmNmarginHeight, 0,
                      XmNmarginWidth, 0,
                      XmNborderWidth, 1,
                      XmNborderColor, BlackPixel(XtDisplay(table->header_row),
                                                 DefaultScreen(XtDisplay(table->header_row))),
                      XmNleftAttachment, XmATTACH_POSITION,
                      XmNrightAttachment, XmATTACH_POSITION,
                      XmNleftPosition, i * 10,
                      XmNrightPosition, (i + 1) * 10,
                      XmNtopAttachment, XmATTACH_FORM,
                      XmNbottomAttachment, XmATTACH_FORM,
                      NULL);

        Widget header_cell = XmCreateForm(header_frame, "headerCell", NULL, 0);
        XtVaSetValues(header_cell,
                      XmNfractionBase, 100,
                      XmNmarginHeight, 0,
                      XmNmarginWidth, 0,
                      XmNshadowThickness, 0,
                      XmNshadowType, XmSHADOW_OUT,
                      NULL);
        XtManageChild(header_cell);

        XmString title = make_string(def->label);
        Widget header = XtVaCreateManagedWidget(
            (String)(def->id ? def->id : "header"),
            xmPushButtonGadgetClass,
            header_cell,
            XmNlabelString, title,
            XmNrecomputeSize, False,
            XmNmarginWidth, 6,
            XmNmarginHeight, 2,
            XmNborderWidth, 0,
            XmNshadowThickness, 0,
            XmNalignment, def->alignment == TABLE_ALIGN_RIGHT ? XmALIGNMENT_END :
                         def->alignment == TABLE_ALIGN_CENTER ? XmALIGNMENT_CENTER :
                         XmALIGNMENT_BEGINNING,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_POSITION,
            XmNrightPosition, 80,
            XmNtopAttachment, XmATTACH_FORM,
            XmNbottomAttachment, XmATTACH_FORM,
            XmNuserData, (XtPointer)(intptr_t)i,
            NULL);

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
            "headerSortIndicator",
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
            XmNuserData, (XtPointer)(intptr_t)i,
            NULL);
        XtAddCallback(indicator, XmNactivateCallback, header_activate_cb, table);
        XtUnmanageChild(indicator);

        XmStringFree(title);
        table->header_buttons[i] = header;
        XtAddCallback(header, XmNactivateCallback, header_activate_cb, table);

        table->header_indicators[i] = indicator;
        XtManageChild(header_frame);
    }

    Arg scroll_args[10];
    int sn = 0;
    XtSetArg(scroll_args[sn], XmNtopAttachment, XmATTACH_WIDGET); sn++;
    XtSetArg(scroll_args[sn], XmNtopWidget, table->header_row); sn++;
    XtSetArg(scroll_args[sn], XmNleftAttachment, XmATTACH_FORM); sn++;
    XtSetArg(scroll_args[sn], XmNrightAttachment, XmATTACH_FORM); sn++;
    XtSetArg(scroll_args[sn], XmNbottomAttachment, XmATTACH_FORM); sn++;
    XtSetArg(scroll_args[sn], XmNscrollingPolicy, XmAUTOMATIC); sn++;
    XtSetArg(scroll_args[sn], XmNshadowThickness, 0); sn++;
    Widget scroll = XmCreateScrolledWindow(form, "tableScroll", scroll_args, sn);
    XtManageChild(scroll);
    table->scroll_window = scroll;
    table_widget_enable_backing_store(scroll);
    Widget vscroll = NULL;
    XtVaGetValues(scroll, XmNverticalScrollBar, &vscroll, NULL);
    if (vscroll) {
        XtAddEventHandler(vscroll, StructureNotifyMask, False, table_widget_scrollbar_structure_event, (XtPointer)table);
    }
    table_widget_update_header_offset(table);
    XtAddEventHandler(scroll, StructureNotifyMask, False, table_widget_scroll_resize_cb, table);

    table->rows_column = XmCreateRowColumn(scroll, "tableRows", NULL, 0);
    XtVaSetValues(table->rows_column,
                  XmNorientation, XmVERTICAL,
                  XmNpacking, XmPACK_TIGHT,
                  XmNspacing, 0,
                  XmNmarginWidth, 0,
                  XmNmarginHeight, 0,
                  NULL);
    table_widget_enable_backing_store(table->rows_column);
    XtManageChild(table->rows_column);
    table_widget_update_row_width(table);

    XtManageChild(table->container);
    return table;
}

void table_widget_destroy(TableWidget *table)
{
    if (!table) return;
    if (table->rows) {
        for (int i = 0; i < table->row_count; ++i) {
            destroy_row(table->rows[i]);
        }
        free(table->rows);
    }
    if (table->header_buttons) {
        free(table->header_buttons);
    }
    if (table->header_indicators) {
        free(table->header_indicators);
    }
    free(table->columns);
    free(table->column_colors);
    if (table->container) {
        XtDestroyWidget(table->container);
    }
    free(table);
}

Widget table_widget_get_widget(TableWidget *table)
{
    return table ? table->container : NULL;
}

TableRow *table_widget_add_row(TableWidget *table, const char *const values[])
{
    return table_widget_add_row_with_sort_values(table, values, NULL);
}

TableRow *table_widget_add_row_with_sort_values(TableWidget *table,
                                                const char *const values[],
                                                const char *const sort_values[])
{
    if (!table) return NULL;
    ensure_row_capacity(table);
    TableRow *row = table_row_create(table, values, sort_values);
    if (!row) return NULL;
    table->rows[table->row_count++] = row;
    if (table->sort_direction != TABLE_SORT_NONE) {
        if (table->updates_suspended) {
            table->sort_pending = True;
        } else {
            sort_rows(table);
        }
    }
    if (!table->updates_suspended) {
        table_widget_update_row_width(table);
    }
    return row;
}

void table_widget_update_row(TableRow *row, const char *const values[])
{
    table_widget_update_row_with_sort_values(row, values, NULL);
}

void table_widget_update_row_with_sort_values(TableRow *row,
                                              const char *const values[],
                                              const char *const sort_values[])
{
    if (!row || !values) return;
    Boolean updated = False;
    for (int i = 0; i < row->table->column_count; ++i) {
        const char *value = safe_value(values[i]);
        const char *current = row->values[i] ? row->values[i] : "";
        if (strcmp(current, value) == 0) {
            continue;
        }
        free(row->values[i]);
        row->values[i] = strdup(value);
        update_cell_label(row, i, value);
        updated = True;
    }
    Boolean sort_updated = table_row_update_sort_values(row, values, sort_values);
    TableWidget *table = row->table;
    if (table && table->sort_direction != TABLE_SORT_NONE) {
        if (!updated && !sort_updated) return;
        if (table->updates_suspended) {
            table->sort_pending = True;
        } else {
            sort_rows(table);
        }
    }
}

void table_widget_remove_row(TableWidget *table, TableRow *row)
{
    if (!table || !row) return;
    int index = -1;
    for (int i = 0; i < table->row_count; ++i) {
        if (table->rows[i] == row) {
            index = i;
            break;
        }
    }
    if (index < 0) return;
    destroy_row(row);
    for (int i = index; i < table->row_count - 1; ++i) {
        table->rows[i] = table->rows[i + 1];
    }
    table->row_count--;
    for (int i = 0; i < table->row_count; ++i) {
        apply_row_colors(table, table->rows[i], i);
    }
    if (!table->updates_suspended) {
        table_widget_update_row_width(table);
    }
}

void table_widget_clear(TableWidget *table)
{
    if (!table) return;
    for (int i = 0; i < table->row_count; ++i) {
        destroy_row(table->rows[i]);
    }
    table->row_count = 0;
    table->next_index = 0;
    sort_rows(table);
}

Widget table_row_get_widget(TableRow *row)
{
    return row ? row->row_form : NULL;
}

void table_widget_set_grid(TableWidget *table, Boolean enabled)
{
    if (!table) return;
    table->grid = enabled;
    for (int i = 0; i < table->row_count; ++i) {
        XtVaSetValues(table->rows[i]->row_form,
                      XmNshadowThickness, enabled ? 1 : 0,
                      XmNshadowType, enabled ? XmSHADOW_ETCHED_IN : XmSHADOW_OUT,
                      NULL);
    }
}

void table_widget_set_header_font(TableWidget *table, XmFontList font)
{
    if (!table) return;
    table->header_font = font;
    for (int i = 0; i < table->column_count; ++i) {
        XtVaSetValues(table->header_buttons[i], XmNfontList, font, NULL);
    }
}

void table_widget_set_row_font(TableWidget *table, XmFontList font)
{
    if (!table) return;
    table->row_font = font;
    for (int i = 0; i < table->row_count; ++i) {
        for (int col = 0; col < table->column_count; ++col) {
            XtVaSetValues(table->rows[i]->cells[col], XmNfontList, font, NULL);
        }
    }
}

void table_widget_set_row_colors(TableWidget *table, Pixel even_row, Pixel odd_row)
{
    if (!table) return;
    table->even_row_color = even_row;
    table->odd_row_color = odd_row;
    for (int i = 0; i < table->row_count; ++i) {
        apply_row_colors(table, table->rows[i], i);
    }
}

void table_widget_set_column_color(TableWidget *table, int column, Pixel color)
{
    if (!table || column < 0 || column >= table->column_count) return;
    table->column_colors[column] = color;
    for (int i = 0; i < table->row_count; ++i) {
        XtVaSetValues(table->rows[i]->cells[column], XmNbackground, color, NULL);
    }
}

void table_widget_set_alternate_row_colors(TableWidget *table, Boolean enabled)
{
    if (!table) return;
    table->alternate_rows = enabled;
    for (int i = 0; i < table->row_count; ++i) {
        apply_row_colors(table, table->rows[i], i);
    }
}

void table_widget_sort_by_column(TableWidget *table, int column,
                                 TableSortDirection direction)
{
    if (!table || column < 0 || column >= table->column_count) return;
    table->sort_column = column;
    table->sort_direction = direction;
    sort_rows(table);
    update_header_labels(table);
    log_table_event(table, "sort_by_column", table->header_buttons[column], column);
}

void table_widget_toggle_sorting(TableWidget *table, int column)
{
    if (!table || column < 0 || column >= table->column_count) return;
    if (table->sort_column != column || table->sort_direction == TABLE_SORT_NONE) {
        table->sort_column = column;
        table->sort_direction = TABLE_SORT_ASCENDING;
    } else if (table->sort_direction == TABLE_SORT_ASCENDING) {
        table->sort_direction = TABLE_SORT_DESCENDING;
    } else {
        table->sort_direction = TABLE_SORT_NONE;
    }
    sort_rows(table);
    update_header_labels(table);
    log_table_event(table, "toggle_sorting", table->header_buttons[column], column);
}

Boolean table_widget_suspend_updates(TableWidget *table)
{
    if (!table || !table->rows_column) return False;
    if (!XtIsManaged(table->rows_column)) return False;
    table->updates_suspended = True;
    XtUnmanageChild(table->rows_column);
    return True;
}

void table_widget_resume_updates(TableWidget *table, Boolean suspended)
{
    if (!table || !suspended || !table->rows_column) return;
    if (table->updates_suspended && table->sort_pending && table->sort_direction != TABLE_SORT_NONE) {
        sort_rows(table);
    }
    table->sort_pending = False;
    table_widget_refresh_layout(table);
    XtManageChild(table->rows_column);
    XmUpdateDisplay(table->rows_column);
    table->updates_suspended = False;
}
