/*
 * ck-clock.c - Digital LED clock with a NeXTStep-style calendar view
 * for the CDE front panel (TYPE client).
 *
 * Uses cairo on Xlib (backed by XRender) for drawing scalable LEDs and
 * the paper-like calendar block. Colors are derived from the parent
 * window background via XmGetColors so the clock follows the panel theme.
 *
 * Build on Devuan/Debian (you may need libcairo2-dev, libmotif-dev):
 *   gcc -O2 -Wall -o ck-clock ck-clock.c ck-clock-time.c ck-clock-calendar.c \
 *       -lX11 -lcairo -lXm -lXt
 *
 * Place the binary somewhere in PATH and use CLIENT_NAME ck-clock
 * in your ~/.dt/types/ckclock.fp.
 */

#include "ck-clock.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include <Xm/ColorObjP.h>   /* XmeGetColorObjData, XmCO_* */
#include <Xm/AtomMgr.h>     /* XmInternAtom */
#include <Xm/Form.h>
#include <Xm/Protocols.h>

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static CkClockApp app = {0};

#define FORCE_USE_AM_PM 1

const char *ck_clock_weekday_labels[] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
};
const char *ck_clock_month_labels[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

/* ----- Timezone helpers -------------------------------------------------- */

/* ----- Color helpers (Pixel -> RGB double) ------------------------------- */

void ck_clock_pixel_to_rgb(CkClockApp *app, Pixel p, double *r, double *g, double *b)
{
    if (app->panel_cmap == None) {
        app->panel_cmap = DefaultColormap(app->dpy, app->screen);
    }

    XColor xc;
    xc.pixel = p;
    XQueryColor(app->dpy, app->panel_cmap, &xc);
    *r = xc.red   / 65535.0;
    *g = xc.green / 65535.0;
    *b = xc.blue  / 65535.0;
}

static bool fetch_panel_bg_from_palette(CkClockApp *app, Pixel *out_pixel)
{
    if (!out_pixel || !app || !app->dpy) return false;
    Screen *scr = ScreenOfDisplay(app->dpy, app->screen);
    if (!scr) return false;

    XmPixelSet pixels[XmCO_MAX_NUM_COLORS];
    short active = 0, inactive = 0, primary = 0, secondary = 0, text = 0;
    int color_use = 0;
    if (!XmeGetColorObjData(scr, &color_use, pixels, XmCO_MAX_NUM_COLORS,
                            &active, &inactive, &primary, &secondary, &text)) {
        return false;
    }

    int count = 0;
    if (color_use == XmCO_HIGH_COLOR) {
        count = 8;
    } else if (color_use == XmCO_MEDIUM_COLOR) {
        count = 4;
    } else if (color_use == XmCO_LOW_COLOR || color_use == XmCO_BLACK_WHITE) {
        count = 2;
    } else {
        count = 2;
    }
    if (count <= 0) return false;

    int target_index = XmCO_MAX_NUM_COLORS - 1;
    if (target_index >= count) target_index = count - 1;
    if (target_index < 0) target_index = 0;
    *out_pixel = pixels[target_index].bg;
    return true;
}

/* Initialize bg/fg/ts/bs/sel pixels using XmGetColors on the parent window background */
static void init_motif_colors(CkClockApp *app)
{
    if (!app || app->colors_inited || !app->dpy) return;

    /* Get parent window (the panel socket) instead of root */
    Window root_return, parent = None;
    Window *children;
    unsigned int nchildren;

    Window query_win = app->win;
    if (app->top_widget && XtIsRealized(app->top_widget)) {
        Window shell_win = XtWindow(app->top_widget);
        if (shell_win != None) query_win = shell_win;
    }

    if (XQueryTree(app->dpy, query_win, &root_return, &parent, &children, &nchildren)) {
        if (children) XFree(children);
    }
    
    /* Fallback to root if no parent found */
    if (parent == None) {
        parent = RootWindow(app->dpy, app->screen);
    }
    
    XWindowAttributes attrs;
    Pixel base_bg = WhitePixel(app->dpy, app->screen);
    if (!XGetWindowAttributes(app->dpy, parent, &attrs)) {
        app->panel_cmap = DefaultColormap(app->dpy, app->screen);
    } else {
        app->panel_cmap = (attrs.colormap != None)
                       ? attrs.colormap
                       : DefaultColormap(app->dpy, app->screen);

        Pixel chosen_bg = attrs.backing_pixel;
        if (app->top_widget) {
            Pixel widget_bg = 0;
            XtVaGetValues(app->top_widget, XmNbackground, &widget_bg, NULL);
            if (widget_bg != 0) {
                chosen_bg = widget_bg;
            }
        }
        if (chosen_bg == 0 || chosen_bg == None) {
            chosen_bg = WhitePixel(app->dpy, app->screen);
        }
        base_bg = chosen_bg;
    }

    Pixel palette_bg = 0;
    if (fetch_panel_bg_from_palette(app, &palette_bg)) {
        base_bg = palette_bg;
    }
    app->bg_pixel = base_bg;

    /* XmGetColors: given a background pixel, derive
       foreground, top_shadow, bottom_shadow, select. */
    XmGetColors(ScreenOfDisplay(app->dpy, app->screen), app->panel_cmap, app->bg_pixel,
                &app->fg_pixel, &app->ts_pixel, &app->bs_pixel, &app->sel_pixel);

    if (app->form_widget) {
        XtVaSetValues(app->form_widget,
                      XmNbackground, app->bg_pixel,
                      XmNforeground, app->fg_pixel,
                      NULL);
    }
    if (app->time_widget) {
        XtVaSetValues(app->time_widget,
                      XmNbackground, app->bg_pixel,
                      XmNforeground, app->fg_pixel,
                      NULL);
    }
    if (app->calendar_widget) {
        XtVaSetValues(app->calendar_widget,
                      XmNbackground, app->bg_pixel,
                      XmNforeground, app->fg_pixel,
                      NULL);
    }

    ck_calendar_view_apply_colors(app);
    app->colors_inited = 1;
}

/* ----- cairo helpers ----------------------------------------------------- */

double ck_clock_clamp01(double v)
{
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

static double rgb_luma(double r, double g, double b)
{
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

void ck_clock_choose_contrast_color(double bg_r, double bg_g, double bg_b,
                                    double in_r, double in_g, double in_b,
                                    double *out_r, double *out_g, double *out_b)
{
    double bg_l = rgb_luma(bg_r, bg_g, bg_b);
    double in_l = rgb_luma(in_r, in_g, in_b);
    /* If theme-provided foreground is too close to background, force contrast. */
    if (fabs(bg_l - in_l) < 0.35) {
        if (bg_l > 0.5) {
            *out_r = 0.0; *out_g = 0.0; *out_b = 0.0;
        } else {
            *out_r = 1.0; *out_g = 1.0; *out_b = 1.0;
        }
        return;
    }
    *out_r = in_r; *out_g = in_g; *out_b = in_b;
}

double ck_clock_fit_font_size(cairo_t *cr, const char *text, double max_w, double max_h)
{
    cairo_text_extents_t ext = {0};
    cairo_set_font_size(cr, 1.0);
    cairo_text_extents(cr, text, &ext);
    if (ext.width <= 0.0 || ext.height <= 0.0) {
        return fmax(6.0, fmin(max_w, max_h));
    }
    double size = fmin(max_w / ext.width, max_h / ext.height);
    if (size < 6.0) size = 6.0;
    return size;
}

void ck_clock_draw_centered_text(cairo_t *cr,
                                 const char *text,
                                 double cx,
                                 double cy,
                                 cairo_text_extents_t *out_ext,
                                 double *out_y);
static void update_time_if_needed(CkClockApp *app);
static void handle_configure(XConfigureEvent *cev);
static void form_event_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont);
static void tick_cb(XtPointer client_data, XtIntervalId *id);
static void wm_delete_cb(Widget w, XtPointer client_data, XtPointer call_data);
static void refresh_icon_pixmap(CkClockApp *app);

CkLayout ck_clock_compute_layout(int w, int h)
{
    CkLayout lay;
    lay.split_mode = (w > (int)(1.4 * h)) ? 1 : 0;

    if (!lay.split_mode) {
        if (w > (int)(1.25 * h)) {
            lay.left_w = fmin((double)h * 0.75, (double)w);
        } else {
            lay.left_w = (double)w;
        }
    } else {
        if (w > (int)(2.0 * h)) {
            lay.left_w = (double)h;
        } else {
            lay.left_w = (double)h * 0.75;
        }
    }
    if (lay.left_w < 0.0) lay.left_w = 0.0;
    if (lay.left_w > (double)w) lay.left_w = (double)w;
    lay.right_x = lay.left_w;
    lay.right_w = (double)w - lay.left_w;
    return lay;
}

int ck_clock_window_is_iconified(const CkClockApp *app)
{
    if (!app || !app->top_widget || !XtIsRealized(app->top_widget)) return 0;
    Display *display = XtDisplay(app->top_widget);
    if (!display) return 0;
    Window window = XtWindow(app->top_widget);
    if (!window) return 0;

    Atom prop = XInternAtom(display, "WM_STATE", True);
    if (prop == None) return 0;

    Atom actual;
    int format;
    unsigned long nitems;
    unsigned long bytes_after;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(display, window, prop, 0, 2, False,
                                    AnyPropertyType, &actual,
                                    &format, &nitems, &bytes_after, &data);
    if (status != Success || !data) return 0;

    long state = -1;
    if (nitems >= 1) {
        long *longs = (long *)data;
        state = longs[0];
    }
    XFree(data);
    return (state == IconicState);
}

static void wm_delete_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    CkClockApp *app = (CkClockApp *)client_data;
    if (app && app->app_ctx) {
        XtAppSetExitFlag(app->app_ctx);
    }
}

static void tick_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)id;
    CkClockApp *app = (CkClockApp *)client_data;
    if (!app) return;
    update_time_if_needed(app);
    if (app->app_ctx) {
        XtAppAddTimeOut(app->app_ctx, 250, tick_cb, app);
    }
}

static void update_wm_icon_pixmap(CkClockApp *app, Display *display)
{
    if (!display || !app || !app->top_widget || !XtIsRealized(app->top_widget)) return;
    Window window = XtWindow(app->top_widget);
    if (!window) return;

    XWMHints *hints = XGetWMHints(display, window);
    XWMHints local;
    if (hints) {
        local = *hints;
        XFree(hints);
    } else {
        memset(&local, 0, sizeof(local));
    }
    local.flags |= IconPixmapHint;
    local.icon_pixmap = app->icon_pixmap;
    XSetWMHints(display, window, &local);
}

static void refresh_icon_pixmap(CkClockApp *app)
{
    if (!app || !app->top_widget || !XtIsRealized(app->top_widget) || !app->have_local_time) return;
    Display *display = XtDisplay(app->top_widget);
    if (!display) return;

    int screen = DefaultScreen(display);
    int icon_w = 96;
    int icon_h = 96;

    Pixmap root = RootWindow(display, screen);
    Pixmap new_pixmap = XCreatePixmap(display, root, icon_w, icon_h,
                                      DefaultDepth(display, screen));
    if (new_pixmap == None) return;

    cairo_surface_t *isurf = cairo_xlib_surface_create(display, new_pixmap,
                                                       DefaultVisual(display, screen),
                                                       icon_w, icon_h);
    cairo_t *icr = cairo_create(isurf);

    CkLayout layout = ck_clock_compute_layout(icon_w, icon_h);
    ck_time_view_render(icr, app, layout.left_w, icon_h);
    if (layout.split_mode && layout.right_w > 20.0) {
        cairo_save(icr);
        cairo_translate(icr, layout.right_x, 0.0);
        ck_calendar_view_render(icr, app, layout.right_w, icon_h, 0.0,
                                ck_calendar_view_first_weekday());
        cairo_restore(icr);
    }

    cairo_surface_flush(isurf);
    cairo_destroy(icr);
    cairo_surface_destroy(isurf);
    XFlush(display);

    if (app->icon_pixmap != None) {
        XFreePixmap(display, app->icon_pixmap);
    }
    app->icon_pixmap = new_pixmap;
    XtVaSetValues(app->top_widget, XmNiconPixmap, app->icon_pixmap, NULL);
    update_wm_icon_pixmap(app, display);

    app->last_icon_minute = app->current_local_tm.tm_min;
}

void ck_clock_draw_centered_text(cairo_t *cr,
                                 const char *text,
                                 double cx,
                                 double cy,
                                 cairo_text_extents_t *out_ext,
                                 double *out_y)
{
    if (!text || !*text) return;
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text, &ext);
    double x = cx - (ext.width / 2.0 + ext.x_bearing);
    double y = cy - (ext.height / 2.0) - ext.y_bearing;
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, text);
    if (out_ext) *out_ext = ext;
    if (out_y) *out_y = y;
}

void ck_clock_request_redraw(CkClockApp *app)
{
    if (!app) return;
    ck_time_view_draw(app);
    ck_calendar_view_draw(app);
}

static void update_time_if_needed(CkClockApp *app)
{
    time_t now = time(NULL);
    if (now == (time_t)-1) return;
    if (!app->force_full_redraw && app->have_local_time && now == app->last_time_check) {
        return;
    }

    struct tm lt;
    if (!localtime_r(&now, &lt)) return;

    int old_year = app->current_local_tm.tm_year;
    int old_mon = app->current_local_tm.tm_mon;
    bool had_time = app->have_local_time;

    char ampm_buf[16] = "";
    char trimmed_ampm[16];
    size_t trimmed_len = 0;
    if (strftime(ampm_buf, sizeof(ampm_buf), "%p", &lt) > 0) {
        for (size_t i = 0; i < sizeof(ampm_buf) && ampm_buf[i]; ++i) {
            unsigned char ch = (unsigned char)ampm_buf[i];
            if (!isspace(ch)) {
                if (trimmed_len < sizeof(trimmed_ampm) - 1) {
                    trimmed_ampm[trimmed_len++] = (char)toupper(ch);
                }
            }
        }
    }
    trimmed_ampm[trimmed_len] = '\0';
    bool has_ampm = trimmed_ampm[0] != '\0';
    if (has_ampm) {
        strncpy(app->current_ampm, trimmed_ampm, sizeof(app->current_ampm));
        app->current_ampm[sizeof(app->current_ampm) - 1] = '\0';
    } else if (FORCE_USE_AM_PM) {
        const char *fallback = (lt.tm_hour >= 12) ? "PM" : "AM";
        strncpy(app->current_ampm, fallback, sizeof(app->current_ampm));
        app->current_ampm[sizeof(app->current_ampm) - 1] = '\0';
        has_ampm = true;
    } else {
        app->current_ampm[0] = '\0';
    }
    app->show_ampm_indicator = has_ampm;
    app->show_colon = (lt.tm_sec % 2) == 0;

    int minute_changed = (lt.tm_min != app->last_display_min);
    int second_changed = (lt.tm_sec != app->last_display_sec);
    int changed = app->force_full_redraw ||
                  !app->have_local_time ||
                  minute_changed ||
                  second_changed ||
                  lt.tm_hour != app->last_display_hour ||
                  lt.tm_mday != app->last_display_mday ||
                  lt.tm_mon != app->last_display_mon ||
                  lt.tm_year != app->last_display_year;

    app->last_time_check = now;

    if (changed) {
        app->last_display_hour = lt.tm_hour;
        app->last_display_min  = lt.tm_min;
        app->last_display_sec  = lt.tm_sec;
        app->last_display_mday = lt.tm_mday;
        app->last_display_mon  = lt.tm_mon;
        app->last_display_year = lt.tm_year;
        app->current_local_tm = lt;
        app->have_local_time = true;
        bool view_follows_current = !had_time ||
                                    (app->view_year == old_year &&
                                     app->view_mon == old_mon);
        if (app->view_year < 0 || view_follows_current) app->view_year = lt.tm_year;
        if (app->view_mon < 0 || view_follows_current) app->view_mon = lt.tm_mon;
        init_motif_colors(app);
        if (!app->colors_inited) {
            app->bg_pixel  = WhitePixel(app->dpy, app->screen);
            app->fg_pixel  = BlackPixel(app->dpy, app->screen);
            app->ts_pixel  = app->fg_pixel;
            app->bs_pixel  = app->fg_pixel;
            app->sel_pixel = app->fg_pixel;
            app->panel_cmap = DefaultColormap(app->dpy, app->screen);
        }
        if (view_follows_current) {
            CkLayout layout = ck_clock_compute_layout(app->win_w, app->win_h);
            ck_calendar_view_update_layout(app, &layout);
        }
        app->force_full_redraw = false;
        ck_clock_request_redraw(app);
        if (minute_changed || app->last_icon_minute == -1 ||
            ck_clock_window_is_iconified(app)) {
            refresh_icon_pixmap(app);
        }
    }
}

static void handle_configure(XConfigureEvent *cev)
{
    if (cev->width > 0)  app.win_w = cev->width;
    if (cev->height > 0) app.win_h = cev->height;
    app.force_full_redraw = true;
    CkLayout layout = ck_clock_compute_layout(app.win_w, app.win_h);
    ck_time_view_update_layout(&app, &layout);
    ck_calendar_view_update_layout(&app, &layout);
    if (app.top_widget && XtIsRealized(app.top_widget) &&
        app.last_split_mode != layout.split_mode) {
        const char *title = layout.split_mode ? "Time and Calendar" : "Time";
        XtVaSetValues(app.top_widget, XmNtitle, title, XmNiconName, title, NULL);
        app.last_split_mode = layout.split_mode;
    }
    update_time_if_needed(&app);
}

static void form_event_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont)
{
    (void)w;
    (void)client_data;
    (void)cont;
    if (!event) return;
    if (event->type == ConfigureNotify) {
        handle_configure(&event->xconfigure);
    }
}

/* ----- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    /* parse flags: --no-embedd and --seconds=none|off|tick|smooth */
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--no-embedd") == 0) {
            app.no_embed = 1;
            for (int j = i; j < argc - 1; ++j) argv[j] = argv[j + 1];
            argc--;
            continue;
        }
        i++;
    }

    Widget toplevel;
    app.win_w = 250;
    app.win_h = 300;
    app.view_year = -1;
    app.view_mon = -1;
    app.last_display_hour = -1;
    app.last_display_min = -1;
    app.last_display_sec = -1;
    app.last_display_mday = -1;
    app.last_display_mon = -1;
    app.last_display_year = -1;
    app.last_icon_minute = -1;
    app.force_full_redraw = true;

    /* Initialize Xt/Motif – this sets up per-display info for XmGetColors */
    toplevel = XtAppInitialize(
        &app.app_ctx,
        "CkClock",          /* application class */
        NULL, 0,
        &argc, argv,
        NULL,               /* no fallback resources */
        NULL, 0
    );
    if (!toplevel) {
        fprintf(stderr, "ck-clock: XtAppInitialize failed\n");
        return 1;
    }

    /* force a sane preferred shell size so we do not start tiny */
    XtVaSetValues(toplevel,
                  XmNwidth,  app.win_w,
                  XmNheight, app.win_h,
                  NULL);

    app.top_widget = toplevel;
    app.dpy = XtDisplay(toplevel);
    app.screen = DefaultScreen(app.dpy);

    app.form_widget = XmCreateForm(toplevel, (char *)"form", NULL, 0);
    XtManageChild(app.form_widget);
    XtAddEventHandler(app.form_widget,
                      StructureNotifyMask,
                      False,
                      form_event_handler,
                      &app);

    ck_time_view_create(&app, app.form_widget);
    ck_calendar_view_create(&app, app.form_widget);

    XtRealizeWidget(toplevel);
    app.win = XtWindow(app.form_widget);

    /* Set WM_CLASS: normal embedding vs debug */
    XClassHint class_hint;
    if (app.no_embed) {
        class_hint.res_name  = (char *)"ck-clock-debug";
        class_hint.res_class = (char *)"CKClockDebug";
    } else {
        class_hint.res_name  = (char *)"ck-clock";
        class_hint.res_class = (char *)"CKClock";
    }
    Window shell_win = XtWindow(toplevel);
    XSetClassHint(app.dpy, shell_win, &class_hint);

    XSizeHints sh;
    sh.flags      = PSize;
    sh.width      = app.win_w;
    sh.height     = app.win_h;
    XSetWMNormalHints(app.dpy, shell_win, &sh);
    XtVaSetValues(toplevel, XmNtitle, "Time", XmNiconName, "Time", NULL);

    Atom wm_delete_atom = XmInternAtom(app.dpy, "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(toplevel, wm_delete_atom, wm_delete_cb, (XtPointer)&app);

    init_motif_colors(&app);
    update_time_if_needed(&app);
    CkLayout layout = ck_clock_compute_layout(app.win_w, app.win_h);
    ck_time_view_update_layout(&app, &layout);
    ck_calendar_view_update_layout(&app, &layout);
    XtAppAddTimeOut(app.app_ctx, 250, tick_cb, &app);
    XtAppMainLoop(app.app_ctx);

    /* Not normally reached */
    if (app.time_cr) cairo_destroy(app.time_cr);
    if (app.time_cs) cairo_surface_destroy(app.time_cs);
    if (app.cal_cr) cairo_destroy(app.cal_cr);
    if (app.cal_cs) cairo_surface_destroy(app.cal_cs);
    if (app.icon_pixmap != None && app.dpy) {
        XFreePixmap(app.dpy, app.icon_pixmap);
    }
    if (toplevel) {
        XtDestroyWidget(toplevel);
    }
    if (app.dpy) {
        XtCloseDisplay(app.dpy);
    }
    return 0;
}
