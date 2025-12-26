#ifndef CK_CLOCK_H
#define CK_CLOCK_H

#include <X11/Intrinsic.h>
#include <Xm/Xm.h>
#include <cairo/cairo.h>

#include <stdbool.h>
#include <time.h>

typedef struct {
    int split_mode;
    double left_w;
    double right_x;
    double right_w;
} CkLayout;

typedef struct {
    Display *dpy;
    int screen;
    Window win;
    Widget top_widget;
    Widget form_widget;
    XtAppContext app_ctx;

    int win_w;
    int win_h;

    Widget time_widget;
    Widget calendar_widget;

    /* calendar controls */
    Widget month_menu;
    Widget month_pulldown;
    Widget month_option;
    Widget month_items[12];
    Widget year_spin;
    Widget year_text;
    int controls_visible;
    int updating_year_spin;
    double right_controls_bottom;

    /* cairo surfaces */
    cairo_surface_t *time_cs;
    cairo_t *time_cr;
    int time_w;
    int time_h;
    cairo_surface_t *cal_cs;
    cairo_t *cal_cr;
    int cal_w;
    int cal_h;

    /* Motif colors */
    Pixel bg_pixel;
    Pixel fg_pixel;
    Pixel ts_pixel;
    Pixel bs_pixel;
    Pixel sel_pixel;
    Colormap panel_cmap;
    int colors_inited;

    /* Digital display state */
    struct tm current_local_tm;
    bool have_local_time;
    time_t last_time_check;
    int last_display_hour;
    int last_display_min;
    int last_display_sec;
    int last_display_mday;
    int last_display_mon;
    int last_display_year;
    bool force_full_redraw;
    int last_icon_minute;

    int view_year;
    int view_mon;
    bool show_ampm_indicator;
    bool show_colon;
    char current_ampm[8];

    Pixmap icon_pixmap;
    int last_split_mode;
    int no_embed;
} CkClockApp;

extern const char *ck_clock_weekday_labels[];
extern const char *ck_clock_month_labels[];

double ck_clock_clamp01(double v);
void ck_clock_choose_contrast_color(double bg_r, double bg_g, double bg_b,
                                    double in_r, double in_g, double in_b,
                                    double *out_r, double *out_g, double *out_b);
double ck_clock_fit_font_size(cairo_t *cr, const char *text, double max_w, double max_h);
void ck_clock_draw_centered_text(cairo_t *cr,
                                 const char *text,
                                 double cx,
                                 double cy,
                                 cairo_text_extents_t *out_ext,
                                 double *out_y);
void ck_clock_pixel_to_rgb(CkClockApp *app, Pixel p, double *r, double *g, double *b);

CkLayout ck_clock_compute_layout(int w, int h);
int ck_clock_window_is_iconified(const CkClockApp *app);
void ck_clock_request_redraw(CkClockApp *app);

Widget ck_time_view_create(CkClockApp *app, Widget parent);
void ck_time_view_update_layout(CkClockApp *app, const CkLayout *layout);
void ck_time_view_draw(CkClockApp *app);
void ck_time_view_render(cairo_t *cr, CkClockApp *app, double width, double height);

Widget ck_calendar_view_create(CkClockApp *app, Widget parent);
void ck_calendar_view_update_layout(CkClockApp *app, const CkLayout *layout);
void ck_calendar_view_draw(CkClockApp *app);
void ck_calendar_view_apply_colors(CkClockApp *app);
int ck_calendar_view_first_weekday(void);
void ck_calendar_view_render(cairo_t *cr,
                             CkClockApp *app,
                             double width,
                             double height,
                             double controls_bottom,
                             int first_weekday);

#endif /* CK_CLOCK_H */
