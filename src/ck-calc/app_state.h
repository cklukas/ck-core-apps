#ifndef CK_CALC_APP_STATE_H
#define CK_CALC_APP_STATE_H

#include <limits.h>
#include <stdbool.h>

#include <X11/Intrinsic.h>
#include <Xm/Xm.h>

#include "../shared/session_utils.h"
#include "../shared/cde_palette.h"
#include "formula_eval.h"
#include "calc_state.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_DISPLAY_LEN 64

typedef struct AppState {
    XtAppContext app_context;
    Widget       shell;
    Widget       main_form;
    Widget       content_form;
    Widget       key_focus_proxy;
    Widget       display_label;
    Widget       display_menu;
    Widget       keypad;
    SessionData *session_data;

    char         exec_path[PATH_MAX];
    char         display[MAX_DISPLAY_LEN];

    CalcState    calc_state;
    FormulaCtx   formula_ctx;
    bool         formula_showing_result;
    double       formula_last_result;

    bool         show_thousands;
    char         decimal_char;
    char         thousands_char;
    int          mode; /* 0=basic, 1=scientific */

    bool         shift_left_down;
    bool         shift_right_down;
    bool         second_mouse_pressed;
    Widget       btn_second;
    Pixel        second_shadow_top;
    Pixel        second_shadow_bottom;
    Boolean      second_shadow_cached;
    short        second_shadow_thickness;
    Boolean      second_thickness_cached;
    Pixel        second_bg_normal;
    Pixel        second_bg_active;
    Boolean      second_color_cached;
    Boolean      second_border_prev_active;
    XmFontList   sci_font_list;
    XFontStruct *sci_font_struct;

    /* Widgets for keyboard activation */
    Widget       btn_digits[10];
    Widget       btn_decimal;
    Widget       btn_eq;
    Widget       btn_plus;
    Widget       btn_minus;
    Widget       btn_mul;
    Widget       btn_div;
    Widget       btn_percent;
    Widget       btn_sign;
    Widget       btn_back;
    Widget       btn_ac;
    Widget       view_mode_basic_btn;
    Widget       view_mode_sci_btn;
    Widget       btn_sci_exp;
    Widget       btn_sci_10x;
    Widget       btn_sci_ln;
    Widget       btn_sci_log10;
    Widget       btn_sci_sin;
    Widget       btn_sci_cos;
    Widget       btn_sci_tan;
    Widget       btn_sci_sinh;
    Widget       btn_sci_cosh;
    Widget       btn_sci_tanh;

    XtIntervalId copy_flash_id;
    char         copy_flash_backup[MAX_DISPLAY_LEN];
    XtIntervalId paste_flash_id;
    char         paste_flash_backup[MAX_DISPLAY_LEN];

    Dimension    chrome_dy;
    Boolean      chrome_inited;

    CdePalette   palette;
    Boolean      palette_ok;
} AppState;

#endif /* CK_CALC_APP_STATE_H */
