#include "gridlayout.h"

#include <Xm/Form.h>
#include <Xm/LabelG.h>

#include <stdlib.h>
#include <string.h>

typedef struct {
    Widget form;
} GridRow;

struct GridLayout {
    Widget container;
    GridRow *rows;
    int row_count;
    int row_capacity;
    int columns;
    int row_spacing;
};

static void ensure_row_capacity(GridLayout *layout)
{
    if (layout->row_count >= layout->row_capacity) {
        int new_cap = layout->row_capacity > 0 ? layout->row_capacity * 2 : 16;
        GridRow *new_rows = (GridRow *)realloc(layout->rows, new_cap * sizeof(GridRow));
        if (new_rows) {
            layout->rows = new_rows;
            layout->row_capacity = new_cap;
        }
    }
}

GridLayout *gridlayout_create(Widget parent, const char *name, int columns)
{
    if (!parent || columns <= 0) return NULL;
    GridLayout *layout = (GridLayout *)calloc(1, sizeof(GridLayout));
    if (!layout) return NULL;
    layout->columns = columns;
    layout->container = XmCreateForm(parent, name ? (String)name : "gridLayout", NULL, 0);
    XtVaSetValues(layout->container,
                  XmNfractionBase, columns * 10,
                  XmNautoUnmanage, False,
                  XmNresizePolicy, XmRESIZE_ANY,
                  XmNshadowThickness, 0,
                  NULL);
    XtManageChild(layout->container);
    return layout;
}

void gridlayout_destroy(GridLayout *layout)
{
    if (!layout) return;
    if (layout->rows) {
        for (int i = 0; i < layout->row_count; ++i) {
            if (layout->rows[i].form) {
                XtDestroyWidget(layout->rows[i].form);
            }
        }
        free(layout->rows);
    }
    if (layout->container) {
        XtDestroyWidget(layout->container);
    }
    free(layout);
}

Widget gridlayout_get_widget(GridLayout *layout)
{
    return layout ? layout->container : NULL;
}

int gridlayout_add_row(GridLayout *layout)
{
    if (!layout) return -1;
    ensure_row_capacity(layout);
    Widget prev = layout->row_count > 0 ? layout->rows[layout->row_count - 1].form : NULL;
    Widget row_form = XmCreateForm(layout->container, "gridRow", NULL, 0);
    XtVaSetValues(row_form,
                  XmNfractionBase, layout->columns * 10,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNtopAttachment, prev ? XmATTACH_WIDGET : XmATTACH_FORM,
                  XmNtopWidget, prev ? prev : NULL,
                  XmNtopOffset, layout->row_spacing,
                  XmNbottomAttachment, XmATTACH_NONE,
                  XmNmarginHeight, 2,
                  XmNmarginWidth, 2,
                  NULL);
    if (!prev) {
        XtVaSetValues(row_form, XmNtopOffset, layout->row_spacing, NULL);
    }

    XtManageChild(row_form);
    layout->rows[layout->row_count].form = row_form;
    return layout->row_count++;
}

Widget gridlayout_add_cell(GridLayout *layout, int row, int column,
                           Widget child, int colspan)
{
    if (!layout || row < 0 || column < 0 || column >= layout->columns ||
        !child || row >= layout->row_count) {
        return NULL;
    }
    if (colspan <= 0) colspan = 1;
    if (column + colspan > layout->columns) {
        colspan = layout->columns - column;
    }
    Widget row_form = layout->rows[row].form;
    if (!row_form) return NULL;
    int base = layout->columns * 10;
    int left = column * 10;
    int right = (column + colspan) * 10;
    if (right > base) right = base;
    XtVaSetValues(child,
                  XmNleftAttachment, XmATTACH_POSITION,
                  XmNleftPosition, left,
                  XmNrightAttachment, XmATTACH_POSITION,
                  XmNrightPosition, right,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNmarginHeight, 2,
                  XmNmarginWidth, 2,
                  NULL);
    XtManageChild(child);
    return child;
}

void gridlayout_set_row_spacing(GridLayout *layout, int pixels)
{
    if (!layout) return;
    layout->row_spacing = pixels;
}

int gridlayout_get_row_spacing(GridLayout *layout)
{
    return layout ? layout->row_spacing : 0;
}

Widget gridlayout_get_row_form(GridLayout *layout, int row)
{
    if (!layout || row < 0 || row >= layout->row_count) return NULL;
    return layout->rows[row].form;
}

void gridlayout_clear_rows(GridLayout *layout, int keep_rows)
{
    if (!layout) return;
    if (keep_rows < 0) keep_rows = 0;
    if (keep_rows >= layout->row_count) return;
    for (int i = layout->row_count - 1; i >= keep_rows; --i) {
        if (layout->rows[i].form) {
            XtDestroyWidget(layout->rows[i].form);
            layout->rows[i].form = NULL;
        }
    }
    layout->row_count = keep_rows;
}
