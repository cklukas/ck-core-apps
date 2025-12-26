/*
 * ck-clock-calendar.c - Right calendar view widget for ck-clock.
 */

#include "ck-clock.h"

#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/SpinB.h>
#include <Xm/TextF.h>
#include <Xm/DrawingA.h>

#include <langinfo.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

static int guess_initial_year(const CkClockApp *app)
{
    if (app->have_local_time) {
        return app->current_local_tm.tm_year + 1900;
    }

    time_t now = time(NULL);
    if (now == (time_t)-1) return 2000;

    struct tm lt;
    if (!localtime_r(&now, &lt)) return 2000;

    return lt.tm_year + 1900;
}

int ck_calendar_view_first_weekday(void)
{
#ifdef _NL_TIME_FIRST_WEEKDAY
    const char *fw = nl_langinfo(_NL_TIME_FIRST_WEEKDAY);
    if (fw && *fw) {
        int v = (unsigned char)fw[0];
        if (v >= 1 && v <= 7) {
            return (v % 7);
        }
    }
#endif
    return 1;
}

static int days_in_month(int year, int mon)
{
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (mon == 1) {
        int y = year + 1900;
        int leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
        return leap ? 29 : 28;
    }
    return days[mon % 12];
}

static int iso_week_number(int year, int mon, int mday)
{
    struct tm t = {0};
    t.tm_year = year;
    t.tm_mon = mon;
    t.tm_mday = mday;
    mktime(&t);
    int wday = (t.tm_wday + 6) % 7;
    int yday = t.tm_yday;

    int thursday = yday + (3 - wday);
    struct tm th = {0};
    th.tm_year = year;
    th.tm_mday = 1 + thursday;
    mktime(&th);
    return 1 + th.tm_yday / 7;
}

static void ensure_calendar_cairo(CkClockApp *app)
{
    if (!app->calendar_widget || !XtIsRealized(app->calendar_widget)) return;
    Dimension w = 0;
    Dimension h = 0;
    XtVaGetValues(app->calendar_widget, XmNwidth, &w, XmNheight, &h, NULL);
    if (w == 0 || h == 0) {
        return;
    }
    if (app->cal_cs && app->cal_w == (int)w && app->cal_h == (int)h) {
        return;
    }

    if (app->cal_cr) {
        cairo_destroy(app->cal_cr);
        app->cal_cr = NULL;
    }
    if (app->cal_cs) {
        cairo_surface_destroy(app->cal_cs);
        app->cal_cs = NULL;
    }

    Window win = XtWindow(app->calendar_widget);
    Visual *visual = DefaultVisual(app->dpy, app->screen);
    app->cal_w = (int)w;
    app->cal_h = (int)h;
    app->cal_cs = cairo_xlib_surface_create(app->dpy, win, visual, app->cal_w, app->cal_h);
    app->cal_cr = cairo_create(app->cal_cs);

    cairo_set_antialias(app->cal_cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_line_cap(app->cal_cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(app->cal_cr, CAIRO_LINE_JOIN_ROUND);
}

static bool cairo_text_fits_box(cairo_t *cr,
                                const char *text,
                                double font_size,
                                double max_w,
                                double max_h)
{
    if (!cr || !text || max_w <= 0.0 || max_h <= 0.0) return false;
    cairo_text_extents_t ext = {0};
    cairo_set_font_size(cr, font_size);
    cairo_text_extents(cr, text, &ext);
    if (ext.width <= 0.0 || ext.height <= 0.0) return false;
    return ext.width <= max_w && ext.height <= max_h;
}

static double cairo_max_font_size_for_box(cairo_t *cr,
                                         const char *sample,
                                         double max_w,
                                         double max_h)
{
    if (!cr || !sample || max_w <= 0.0 || max_h <= 0.0) return 1.0;

    double lo = 0.5;
    double hi = 1.0;
    if (!cairo_text_fits_box(cr, sample, hi, max_w, max_h)) {
        while (hi > 0.01 && !cairo_text_fits_box(cr, sample, hi, max_w, max_h)) {
            hi *= 0.5;
        }
        return hi;
    }

    while (hi < 512.0 && cairo_text_fits_box(cr, sample, hi, max_w, max_h)) {
        lo = hi;
        hi *= 2.0;
    }

    for (int i = 0; i < 24; ++i) {
        double mid = (lo + hi) * 0.5;
        if (cairo_text_fits_box(cr, sample, mid, max_w, max_h)) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static double cairo_scale_down_to_fit_labels(cairo_t *cr,
                                             double font_size,
                                             const char *const *labels,
                                             int label_count,
                                             double max_w,
                                             double max_h)
{
    if (!cr || !labels || label_count <= 0) return font_size;
    if (font_size <= 0.0) return font_size;
    if (max_w <= 0.0 || max_h <= 0.0) return font_size;

    double scale = 1.0;
    for (int i = 0; i < label_count; ++i) {
        const char *s = labels[i];
        if (!s || !s[0]) continue;
        cairo_text_extents_t ext = {0};
        cairo_set_font_size(cr, font_size);
        cairo_text_extents(cr, s, &ext);
        if (ext.width <= 0.0 || ext.height <= 0.0) continue;
        double w_scale = max_w / ext.width;
        double h_scale = max_h / ext.height;
        double local = w_scale < h_scale ? w_scale : h_scale;
        if (local < scale) scale = local;
    }
    if (scale > 1.0) scale = 1.0;
    if (scale < 0.01) scale = 0.01;
    return font_size * scale;
}

static void draw_month_view(cairo_t *cr,
                            CkClockApp *app,
                            double x,
                            double y,
                            double w,
                            double h,
                            double fg_r,
                            double fg_g,
                            double fg_b,
                            double bg_r,
                            double bg_g,
                            double bg_b,
                            int year,
                            int mon,
                            int first_weekday,
                            double controls_bottom)
{
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);

    double text_r, text_g, text_b;
    ck_clock_choose_contrast_color(bg_r, bg_g, bg_b, fg_r, fg_g, fg_b,
                                   &text_r, &text_g, &text_b);

    cairo_set_source_rgb(cr, bg_r, bg_g, bg_b);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);

    double padding = w * 0.05;
    if (padding < 4.0) padding = 4.0;
    double header_h = h * 0.12;
    if (controls_bottom > y) {
        double controls_h = controls_bottom - y;
        if (controls_h > header_h) header_h = controls_h;
    }
    double grid_y = y + header_h + padding;
    double grid_h = h - header_h - padding * 1.5;
    if (grid_h < 10.0) grid_h = 10.0;

    int today = app->current_local_tm.tm_mday;
    int days = days_in_month(year, mon);

    int cols = 8;
    int rows = 7;
    double cell_w = (w - 2.0 * padding) / cols;
    double cell_h = grid_h / rows;
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    double day_font = cairo_max_font_size_for_box(cr, ".88.", cell_w * 0.88, cell_h * 0.72);
    if (day_font < 6.0) day_font = 6.0;
    double weekday_font = day_font * 0.68;
    double week_font = day_font * 0.62;
    weekday_font = cairo_scale_down_to_fit_labels(cr,
                                                  weekday_font,
                                                  (const char *const *)ck_clock_weekday_labels,
                                                  7,
                                                  cell_w * 0.85,
                                                  cell_h * 0.55);
    static const char *const WEEK_SAMPLES[] = {"88", "00", "53"};
    week_font = cairo_scale_down_to_fit_labels(cr,
                                               week_font,
                                               WEEK_SAMPLES,
                                               3,
                                               cell_w * 0.75,
                                               cell_h * 0.55);
    cairo_set_font_size(cr, weekday_font);
    cairo_set_source_rgb(cr, text_r, text_g, text_b);
    for (int c = 0; c < 7; ++c) {
        int widx = (c + first_weekday) % 7;
        const char *lbl = ck_clock_weekday_labels[widx];
        double cx = x + padding + cell_w * (c + 1) + cell_w / 2.0;
        double cy = grid_y + cell_h * 0.6;
        ck_clock_draw_centered_text(cr, lbl, cx, cy, NULL, NULL);
    }

    struct tm first = {0};
    first.tm_year = year;
    first.tm_mon = mon;
    first.tm_mday = 1;
    mktime(&first);
    int first_wday = (first.tm_wday - first_weekday + 7) % 7;

    int row = 1;
    int col = 1;
    cairo_set_font_size(cr, weekday_font);

    int leading_slots = first_wday;
    int prev_mon = (mon + 11) % 12;
    int prev_year = year;
    if (mon == 0) prev_year -= 1;
    int prev_days = days_in_month(prev_year, prev_mon);
    int start_prev = prev_days - leading_slots + 1;
    if (start_prev < 1) start_prev = 1;
    for (int i = 0; i < leading_slots; ++i) {
        int d = start_prev + i;
        double cx = x + padding + cell_w * col + cell_w / 2.0;
        double cy = grid_y + cell_h * (row + 0.55);
        char buf[12];
        snprintf(buf, sizeof(buf), "%d", d);
        cairo_set_source_rgb(cr,
                             ck_clock_clamp01(fg_r * 0.45 + bg_r * 0.55),
                             ck_clock_clamp01(fg_g * 0.45 + bg_g * 0.55),
                             ck_clock_clamp01(fg_b * 0.45 + bg_b * 0.55));
        ck_clock_draw_centered_text(cr, buf, cx, cy, NULL, NULL);
        col++;
    }
    col = leading_slots + 1;

    cairo_set_font_size(cr, day_font);
    for (int d = 1; d <= days; ++d) {
        if (col >= cols) {
            col = 1;
            row++;
        }
        double cx = x + padding + cell_w * col + cell_w / 2.0;
        double cy = grid_y + cell_h * (row + 0.55);

        char buf[12];
        snprintf(buf, sizeof(buf), "%d", d);

        if ((mon == app->current_local_tm.tm_mon) &&
            (year == app->current_local_tm.tm_year) &&
            d == today) {
            cairo_set_source_rgb(cr, ck_clock_clamp01(fg_r * 0.2 + bg_r * 0.8),
                                 ck_clock_clamp01(fg_g * 0.2 + bg_g * 0.8),
                                 ck_clock_clamp01(fg_b * 0.2 + bg_b * 0.8));
            cairo_rectangle(cr,
                            x + padding + cell_w * col + 2.0,
                            grid_y + cell_h * row + 2.0,
                            cell_w - 4.0, cell_h - 4.0);
            cairo_fill(cr);
        }

        cairo_set_source_rgb(cr, text_r, text_g, text_b);
        ck_clock_draw_centered_text(cr, buf, cx, cy, NULL, NULL);
        col++;
    }

    int next_day = 1;
    cairo_set_font_size(cr, weekday_font);
    while (row <= 6) {
        if (col >= cols) {
            col = 1;
            row++;
            if (row > 6) break;
        }
        double cx = x + padding + cell_w * col + cell_w / 2.0;
        double cy = grid_y + cell_h * (row + 0.55);
        char buf[12];
        snprintf(buf, sizeof(buf), "%d", next_day++);
        cairo_set_source_rgb(cr,
                             ck_clock_clamp01(fg_r * 0.45 + bg_r * 0.55),
                             ck_clock_clamp01(fg_g * 0.45 + bg_g * 0.55),
                             ck_clock_clamp01(fg_b * 0.45 + bg_b * 0.55));
        ck_clock_draw_centered_text(cr, buf, cx, cy, NULL, NULL);
        col++;
    }

    cairo_set_font_size(cr, week_font);
    cairo_set_source_rgb(cr, text_r, text_g, text_b);
    int week_row = 1;
    int day_for_week = 1;
    int col_for_week = first_wday;
    while (week_row <= 6 && day_for_week <= days) {
        int day_this_row = day_for_week - col_for_week;
        if (day_this_row < 1) day_this_row = 1;
        int week = iso_week_number(year, mon, day_this_row);
        char wbuf[12];
        snprintf(wbuf, sizeof(wbuf), "%d", week);
        double cx = x + padding + cell_w * 0.5;
        double cy = grid_y + cell_h * (week_row + 0.55);
        ck_clock_draw_centered_text(cr, wbuf, cx, cy, NULL, NULL);

        week_row++;
        day_for_week += 7;
    }

    cairo_restore(cr);
}

void ck_calendar_view_render(cairo_t *cr,
                             CkClockApp *app,
                             double width,
                             double height,
                             double controls_bottom,
                             int first_weekday)
{
    if (!cr || !app || !app->have_local_time) return;

    double bg_r, bg_g, bg_b;
    double fg_r, fg_g, fg_b;
    ck_clock_pixel_to_rgb(app, app->bg_pixel,  &bg_r,  &bg_g,  &bg_b);
    ck_clock_pixel_to_rgb(app, app->fg_pixel,  &fg_r,  &fg_g,  &fg_b);

    cairo_set_source_rgb(cr, bg_r, bg_g, bg_b);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    draw_month_view(cr,
                    app,
                    0,
                    0,
                    width,
                    height,
                    fg_r, fg_g, fg_b,
                    bg_r, bg_g, bg_b,
                    app->view_year,
                    app->view_mon,
                    first_weekday,
                    controls_bottom);
}

static void calendar_view_event_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont)
{
    (void)w;
    (void)cont;
    CkClockApp *app = (CkClockApp *)client_data;
    if (!app || !event) return;
    switch (event->type) {
    case Expose:
        if (event->xexpose.count == 0) {
            ck_calendar_view_draw(app);
        }
        break;
    case ConfigureNotify:
        ck_calendar_view_draw(app);
        break;
    default:
        break;
    }
}

static void month_menu_select_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)call_data;
    CkClockApp *app = (CkClockApp *)client_data;
    if (!app) return;
    XtPointer value = NULL;
    XtVaGetValues(w, XmNuserData, &value, NULL);
    int mon = (int)(uintptr_t)value;
    app->view_mon = mon;
    if (app->month_option && app->month_items[mon]) {
        XtVaSetValues(app->month_option, XmNmenuHistory, app->month_items[mon], NULL);
    }
    app->force_full_redraw = true;
    app->calendar_force_redraw = true;
    app->time_calendar_force_redraw = true;
    ck_clock_request_redraw(app);
}

static void year_text_changed_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    CkClockApp *app = (CkClockApp *)client_data;
    if (!app || app->updating_year_spin || !app->year_text) return;

    char *s = XmTextFieldGetString(app->year_text);
    if (!s) return;
    int y = atoi(s);
    XtFree(s);

    if (y < 1900 || y > 2200) {
        return;
    }

    app->view_year = y - 1900;
    app->force_full_redraw = true;
    app->calendar_force_redraw = true;
    app->time_calendar_force_redraw = true;
    ck_clock_request_redraw(app);
}

static void ensure_month_menu(CkClockApp *app)
{
    if ((app->month_menu && app->month_option) || !app->top_widget) return;
    if (!app->form_widget) return;

    Arg args[4];
    int n = 0;
    app->month_pulldown = XmCreatePulldownMenu(app->form_widget, (char *)"monthPulldown", args, n);
    app->month_menu = app->month_pulldown;
    for (int i = 0; i < 12; ++i) {
        XmString label = XmStringCreateLocalized((char *)ck_clock_month_full_labels[i]);
        Widget item = XmCreatePushButtonGadget(app->month_pulldown, (char *)ck_clock_month_full_labels[i], NULL, 0);
        XtVaSetValues(item, XmNlabelString, label, NULL);
        XmStringFree(label);
        XtVaSetValues(item, XmNuserData, (XtPointer)(uintptr_t)i, NULL);
        XtAddCallback(item, XmNactivateCallback, month_menu_select_cb, app);
        XtManageChild(item);
        app->month_items[i] = item;
    }

    Arg args_menu[4];
    int n2 = 0;
    XtSetArg(args_menu[n2], XmNsubMenuId, app->month_pulldown); n2++;
    app->month_option = XmCreateOptionMenu(app->form_widget, (char *)"monthOption", args_menu, n2);
    XtVaSetValues(app->month_option,
                  XmNtopAttachment, XmATTACH_NONE,
                  XmNbottomAttachment, XmATTACH_NONE,
                  XmNleftAttachment, XmATTACH_NONE,
                  XmNrightAttachment, XmATTACH_NONE,
                  NULL);

    app->year_spin = XmCreateSpinBox(app->form_widget, (char *)"yearSpin", NULL, 0);
    XtVaSetValues(app->year_spin,
                  XmNtopAttachment, XmATTACH_NONE,
                  XmNbottomAttachment, XmATTACH_NONE,
                  XmNleftAttachment, XmATTACH_NONE,
                  XmNrightAttachment, XmATTACH_NONE,
                  NULL);

    int initial_year = guess_initial_year(app);
    if (initial_year < 1900) initial_year = 1900;
    else if (initial_year > 2200) initial_year = 2200;

    app->year_text = XmCreateTextField(app->year_spin, (char *)"yearText", NULL, 0);
    XtVaSetValues(app->year_text,
                  XmNcolumns, 5,
                  XmNspinBoxChildType, XmNUMERIC,
                  XmNminimumValue, 1900,
                  XmNmaximumValue, 2200,
                  XmNincrementValue, 1,
                  XmNposition, initial_year,
                  NULL);
    XtAddCallback(app->year_text, XmNvalueChangedCallback, year_text_changed_cb, app);
    XtAddCallback(app->year_text, XmNactivateCallback, year_text_changed_cb, app);
    XtAddCallback(app->year_text, XmNlosingFocusCallback, year_text_changed_cb, app);
    XtManageChild(app->year_text);
    XtManageChild(app->year_spin);
}

static void update_controls_layout(CkClockApp *app, const CkLayout *layout)
{
    if (!app || !layout || !app->form_widget) return;

    int want_controls = (layout->split_mode && layout->right_w > 40.0);

    if (!want_controls) {
        if (app->controls_visible) {
            if (app->month_option && XtIsManaged(app->month_option)) XtUnmanageChild(app->month_option);
            if (app->year_spin && XtIsManaged(app->year_spin)) XtUnmanageChild(app->year_spin);
            app->controls_visible = 0;
        }
        app->right_controls_bottom = 0.0;
        return;
    }

    ensure_month_menu(app);
    if (!app->month_option || !app->year_spin || !app->year_text) return;

    if (app->have_local_time) {
        if (app->view_year < 0) app->view_year = app->current_local_tm.tm_year;
        if (app->view_mon < 0) app->view_mon = app->current_local_tm.tm_mon;
    }

    int pad = (int)fmax(4.0, layout->right_w * 0.04);
    int header_y = (int)fmax(2.0, pad * 0.5);
    int opt_x = (int)(layout->right_x + pad);
    int right_edge = (int)(layout->right_x + layout->right_w - pad);

    int year_w = (int)fmax(70.0, layout->right_w * 0.25);
    if (year_w > layout->right_w * 0.35) year_w = (int)(layout->right_w * 0.35);
    if (year_w < 65) year_w = 65;

    int max_month_w = right_edge - opt_x - year_w - pad;
    if (max_month_w < 60) max_month_w = 60;
    int month_w = (int)fmax(100.0, layout->right_w * 0.25);
    if (month_w > 160) month_w = 160;
    if (month_w > max_month_w) month_w = max_month_w;

    int year_x = opt_x + month_w + pad;
    int year_y = header_y;
    int month_h = 0;
    XtVaGetValues(app->month_option, XmNheight, &month_h, NULL);
    int row_h = month_h > 0 ? month_h : 28;
    int year_h = row_h;

    XtVaSetValues(app->month_option,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftOffset, opt_x,
                  XmNtopOffset, header_y,
                  XmNwidth, (Dimension)month_w,
                  NULL);

    XtVaSetValues(app->year_spin,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftOffset, year_x,
                  XmNtopOffset, year_y,
                  XmNwidth, (Dimension)year_w,
                  XmNheight, (Dimension)year_h,
                  NULL);

    if (!XtIsManaged(app->month_option)) XtManageChild(app->month_option);
    if (!XtIsManaged(app->year_spin)) XtManageChild(app->year_spin);
    app->controls_visible = 1;

    if (app->view_mon >= 0 && app->view_mon < 12 && app->month_items[app->view_mon]) {
        XtVaSetValues(app->month_option, XmNmenuHistory, app->month_items[app->view_mon], NULL);
    }

    int month_bottom = header_y + row_h;
    int year_bottom = year_y + year_h;
    int header_bottom = month_bottom > year_bottom ? month_bottom : year_bottom;
    app->right_controls_bottom = (double)header_bottom + pad * 0.5;

    if (XtIsRealized(app->month_option)) XRaiseWindow(app->dpy, XtWindow(app->month_option));
    if (XtIsRealized(app->year_spin)) XRaiseWindow(app->dpy, XtWindow(app->year_spin));

    if (app->view_year >= 0) {
        app->updating_year_spin = 1;
        XtVaSetValues(app->year_text, XmNposition, app->view_year + 1900, NULL);
        app->updating_year_spin = 0;
    }
}

Widget ck_calendar_view_create(CkClockApp *app, Widget parent)
{
    if (!app || !parent) return NULL;
    app->calendar_widget = XmCreateDrawingArea(parent, (char *)"calendarView", NULL, 0);
    XtManageChild(app->calendar_widget);
    XtAddEventHandler(app->calendar_widget,
                      ExposureMask | StructureNotifyMask,
                      False,
                      calendar_view_event_handler,
                      app);
    ensure_month_menu(app);
    return app->calendar_widget;
}

void ck_calendar_view_update_layout(CkClockApp *app, const CkLayout *layout)
{
    if (!app || !layout || !app->calendar_widget) return;

    if (!layout->split_mode) {
        if (XtIsManaged(app->calendar_widget)) {
            XtUnmanageChild(app->calendar_widget);
        }
        update_controls_layout(app, layout);
        return;
    }

    XtVaSetValues(app->calendar_widget,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNleftOffset, (int)layout->right_x,
                  XmNwidth, (Dimension)layout->right_w,
                  NULL);
    if (!XtIsManaged(app->calendar_widget)) XtManageChild(app->calendar_widget);

    update_controls_layout(app, layout);
}

void ck_calendar_view_apply_colors(CkClockApp *app)
{
    if (!app) return;
    if (app->month_option) {
        XtVaSetValues(app->month_option,
                      XmNbackground, app->bg_pixel,
                      XmNforeground, app->fg_pixel,
                      NULL);
    }
    if (app->year_spin) {
        XtVaSetValues(app->year_spin,
                      XmNbackground, app->bg_pixel,
                      XmNforeground, app->fg_pixel,
                      NULL);
    }
    if (app->year_text) {
        XtVaSetValues(app->year_text,
                      XmNbackground, app->bg_pixel,
                      XmNforeground, app->fg_pixel,
                      NULL);
    }
}

void ck_calendar_view_draw(CkClockApp *app)
{
    if (!app || !app->calendar_widget || !app->have_local_time) return;
    if (!XtIsRealized(app->calendar_widget)) return;

    if (app->view_year < 0) app->view_year = app->current_local_tm.tm_year;
    if (app->view_mon < 0) app->view_mon = app->current_local_tm.tm_mon;

    ensure_calendar_cairo(app);
    if (!app->cal_cr) return;

    int first_wday = ck_calendar_view_first_weekday();
    ck_calendar_view_render(app->cal_cr,
                            app,
                            app->cal_w,
                            app->cal_h,
                            app->right_controls_bottom,
                            first_wday);
    cairo_surface_flush(app->cal_cs);
    if (app->have_local_time) {
        app->calendar_last_drawn_year = app->current_local_tm.tm_year;
        app->calendar_last_drawn_mon = app->current_local_tm.tm_mon;
        app->calendar_last_drawn_mday = app->current_local_tm.tm_mday;
    }
    app->calendar_last_drawn_view_year = app->view_year;
    app->calendar_last_drawn_view_mon = app->view_mon;
    app->calendar_force_redraw = false;
}
