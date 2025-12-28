#ifndef CK_SHARED_CK_TABLE_H
#define CK_SHARED_CK_TABLE_H

#include <Xm/Xm.h>

#include "../table/table_widget.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CK_TABLE_MODE_STANDARD,
    CK_TABLE_MODE_VIRTUAL,
} CkTableMode;

typedef struct CkTable CkTable;

typedef const char *(*CkTableCellTextFn)(void *context,
                                         const void *entries,
                                         int row,
                                         int column,
                                         char *buffer,
                                         size_t buffer_len);

typedef double (*CkTableCellNumberFn)(void *context,
                                      const void *entries,
                                      int row,
                                      int column,
                                      Boolean *has_value);

typedef int (*CkTableSortCompareFn)(void *context,
                                    const void *entries,
                                    int left,
                                    int right,
                                    int column,
                                    TableSortDirection direction);

typedef void (*CkTableViewportChangedFn)(void *context);

CkTable *ck_table_create_standard(Widget parent, const char *name,
                                 const TableColumnDef *columns,
                                 int column_count);
CkTable *ck_table_create_virtual(Widget parent, const char *name,
                                 const TableColumnDef *columns,
                                 int column_count);
void ck_table_destroy(CkTable *table);
Widget ck_table_get_widget(CkTable *table);
CkTableMode ck_table_get_mode(const CkTable *table);

TableRow *ck_table_add_row(CkTable *table, const char *const values[]);
TableRow *ck_table_add_row_with_sort_values(CkTable *table,
                                            const char *const values[],
                                            const char *const sort_values[]);
void ck_table_update_row(TableRow *row, const char *const values[]);
void ck_table_update_row_with_sort_values(TableRow *row,
                                          const char *const values[],
                                          const char *const sort_values[]);
void ck_table_remove_row(CkTable *table, TableRow *row);
void ck_table_clear(CkTable *table);
Widget ck_table_row_get_widget(TableRow *row);

void ck_table_set_grid(CkTable *table, Boolean enabled);
void ck_table_set_header_font(CkTable *table, XmFontList font);
void ck_table_set_row_font(CkTable *table, XmFontList font);
void ck_table_set_row_colors(CkTable *table, Pixel even_row, Pixel odd_row);
void ck_table_set_column_color(CkTable *table, int column, Pixel color);
void ck_table_set_alternate_row_colors(CkTable *table, Boolean enabled);

void ck_table_sort_by_column(CkTable *table, int column,
                             TableSortDirection direction);
void ck_table_toggle_sorting(CkTable *table, int column);

Boolean ck_table_suspend_updates(CkTable *table);
void ck_table_resume_updates(CkTable *table, Boolean suspended);

void ck_table_set_virtual_callbacks(CkTable *table,
                                    CkTableCellTextFn text_fn,
                                    CkTableCellNumberFn number_fn,
                                    CkTableSortCompareFn compare_fn,
                                    void *context);
void ck_table_set_virtual_data(CkTable *table, const void *entries, int count);
void ck_table_set_virtual_row_window(CkTable *table, int start);
int ck_table_get_virtual_row_page_size(const CkTable *table);
void ck_table_set_virtual_row_spacing(CkTable *table, int pixels);
void ck_table_set_virtual_viewport_changed_callback(CkTable *table,
                                                    CkTableViewportChangedFn callback,
                                                    void *context);

#ifdef __cplusplus
}
#endif

#endif /* CK_SHARED_CK_TABLE_H */
