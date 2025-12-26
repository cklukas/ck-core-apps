#ifndef CK_SHARED_TABLE_WIDGET_H
#define CK_SHARED_TABLE_WIDGET_H

#include <Xm/Xm.h>

typedef enum {
    TABLE_ALIGN_LEFT,
    TABLE_ALIGN_CENTER,
    TABLE_ALIGN_RIGHT,
} TableAlignment;

typedef enum {
    TABLE_SORT_NONE = 0,
    TABLE_SORT_ASCENDING = 1,
    TABLE_SORT_DESCENDING = -1,
} TableSortDirection;

typedef struct {
    const char *id;
    const char *label;
    TableAlignment alignment;
    Boolean numeric;
    Boolean sortable;
    int width; /* optional hint; currently unused */
} TableColumnDef;

typedef struct TableWidget TableWidget;
typedef struct TableRow TableRow;

TableWidget *table_widget_create(Widget parent, const char *name,
                                 const TableColumnDef *columns, int column_count);
void table_widget_destroy(TableWidget *table);
Widget table_widget_get_widget(TableWidget *table);

TableRow *table_widget_add_row(TableWidget *table, const char *const values[]);
void table_widget_update_row(TableRow *row, const char *const values[]);
void table_widget_remove_row(TableWidget *table, TableRow *row);
void table_widget_clear(TableWidget *table);
Widget table_row_get_widget(TableRow *row);

void table_widget_set_grid(TableWidget *table, Boolean enabled);
void table_widget_set_header_font(TableWidget *table, XmFontList font);
void table_widget_set_row_font(TableWidget *table, XmFontList font);
void table_widget_set_row_colors(TableWidget *table,
                                 Pixel even_row, Pixel odd_row);
void table_widget_set_column_color(TableWidget *table, int column, Pixel color);
void table_widget_set_alternate_row_colors(TableWidget *table, Boolean enabled);

void table_widget_sort_by_column(TableWidget *table, int column,
                                 TableSortDirection direction);
void table_widget_toggle_sorting(TableWidget *table, int column);

#endif /* CK_SHARED_TABLE_WIDGET_H */
