#include "app_state_utils.h"

#include <locale.h>

#include "../shared/config_utils.h"

#ifndef VIEW_STATE_FILENAME
#define VIEW_STATE_FILENAME "ck-calc.view"
#endif

void ck_calc_init_locale_settings(AppState *app)
{
    if (!app) return;
    struct lconv *lc = localeconv();
    app->decimal_char = (lc && lc->decimal_point && lc->decimal_point[0])
                            ? lc->decimal_point[0]
                            : '.';
    app->thousands_char = (lc && lc->thousands_sep && lc->thousands_sep[0])
                              ? lc->thousands_sep[0]
                              : ',';
}

void ck_calc_load_view_state(AppState *app)
{
    if (!app) return;
    int val = config_read_int_map(VIEW_STATE_FILENAME, "show_thousands", 1);
    app->show_thousands = (val != 0);
    app->mode = config_read_int_map(VIEW_STATE_FILENAME, "mode", app->mode);
}

void ck_calc_save_view_state(const AppState *app)
{
    if (!app) return;
    config_write_int_map(VIEW_STATE_FILENAME, "show_thousands", app->show_thousands ? 1 : 0);
    config_write_int_map(VIEW_STATE_FILENAME, "mode", app->mode);
}
