#ifndef CK_SHARED_GRID_LAYOUT_H
#define CK_SHARED_GRID_LAYOUT_H

#include <Xm/Xm.h>

typedef struct GridLayout GridLayout;

GridLayout *gridlayout_create(Widget parent, const char *name, int columns);
void gridlayout_destroy(GridLayout *layout);
Widget gridlayout_get_widget(GridLayout *layout);

int gridlayout_add_row(GridLayout *layout);
Widget gridlayout_add_cell(GridLayout *layout, int row, int column,
                           Widget child, int colspan);

void gridlayout_set_row_spacing(GridLayout *layout, int pixels);
void gridlayout_clear_rows(GridLayout *layout, int keep_rows);
int gridlayout_get_row_spacing(GridLayout *layout);

Widget gridlayout_get_row_form(GridLayout *layout, int row);

#endif /* CK_SHARED_GRID_LAYOUT_H */
