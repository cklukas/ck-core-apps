/*
 * ck-clock.c - Digital LED clock with a NeXTStep-style calendar view
 * for the CDE front panel (TYPE client).
 *
 * Uses cairo on Xlib (backed by XRender) for drawing scalable LEDs and
 * the paper-like calendar block. Colors are derived from the parent
 * window background via XmGetColors so the clock follows the panel theme.
 *
 * Build on Devuan/Debian (you may need libcairo2-dev, libmotif-dev):
 *   gcc -O2 -Wall -o ck-clock ck-clock.c -lX11 -lcairo -lXm -lXt
 *
 * Place the binary somewhere in PATH and use CLIENT_NAME ck-clock
 * in your ~/.dt/types/ckclock.fp.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include <X11/Intrinsic.h>  /* XtAppInitialize, XtDisplay, XtWindow, ... */
#include <Xm/Xm.h>          /* XmGetColors */
#include <Xm/AtomMgr.h>     /* XmInternAtom */
#include <Xm/RowColumn.h>
#include <Xm/PushBG.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/ArrowB.h>
#include <Xm/DrawingA.h>
#include <Xm/SpinB.h>
#include <Xm/TextF.h>
#include <Xm/Protocols.h>

#include <errno.h>
#include <ctype.h>
#include <locale.h>
#include <langinfo.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

static Display *dpy = NULL;
static int screen;
static Window win;
static Widget top_widget = NULL;
static Widget draw_widget = NULL;
static Widget form_widget = NULL;
static XtAppContext app_ctx = NULL;

static int win_w = 250, win_h = 300;
static Pixmap icon_pixmap = None;
static int last_split_mode = 0;
static int view_year = -1;
static int view_mon = -1; /* 0-based */
static Widget month_menu = NULL;
static Widget month_pulldown = NULL;
static Widget month_option = NULL;
static Widget month_items[12] = {0};
static Widget year_spin = NULL;
static Widget year_text = NULL;
static int controls_visible = 0;
static int updating_year_spin = 0;
static double right_controls_bottom = 0.0;


/* cairo state */
static cairo_surface_t *cs = NULL;
static cairo_t         *cr = NULL;
static int surf_w = 0, surf_h = 0;

/* Motif color set derived from a background */
static Pixel bg_pixel = 0;
static Pixel fg_pixel = 0;
static Pixel ts_pixel = 0;
static Pixel bs_pixel = 0;
static Pixel sel_pixel = 0;
static Colormap panel_cmap = None;
static int colors_inited = 0;

/* Digital display state */
static struct tm current_local_tm;
static bool have_local_time = false;
static time_t last_time_check = 0;
static int last_display_hour = -1;
static int last_display_min = -1;
static int last_display_mday = -1;
static int last_display_mon = -1;
static int last_display_year = -1;
static bool force_full_redraw = true;
static int last_icon_minute = -1;

#define FORCE_USE_AM_PM 1
static bool show_ampm_indicator = FORCE_USE_AM_PM;
static char current_ampm[8] = "";

static const char *weekday_labels[] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
};
static const char *month_labels[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};
/* Fine-tune x offsets for the AM/PM triplet glyphs.
   [0] lead glyph (A/P), [1] first arch, [2] extra tweak for the 2nd arch.
   The 2nd arch is automatically positioned to overlap the 1st arch by one
   segment thickness; [2] is applied on top of that. */
static const double PM_INDICATOR_P_OFFSETS[3] = { 0.0, -0.35, 0.0 };

/* if set by --no-embedd, we use a different WM_CLASS so dtwm won't embed */
static int no_embed = 0;

/* ----- Timezone helpers -------------------------------------------------- */

/* ----- Color helpers (Pixel -> RGB double) ------------------------------- */

static void pixel_to_rgb(Pixel p, double *r, double *g, double *b)
{
    if (panel_cmap == None) {
        panel_cmap = DefaultColormap(dpy, screen);
    }

    XColor xc;
    xc.pixel = p;
    XQueryColor(dpy, panel_cmap, &xc);
    *r = xc.red   / 65535.0;
    *g = xc.green / 65535.0;
    *b = xc.blue  / 65535.0;
}

/* Initialize bg/fg/ts/bs/sel pixels using XmGetColors on the parent window background */
static void init_motif_colors(void)
{
    if (colors_inited || !dpy) return;

    /* Get parent window (the panel socket) instead of root */
    Window root_return, parent = None;
    Window *children;
    unsigned int nchildren;

    Window query_win = win;
    if (top_widget && XtIsRealized(top_widget)) {
        Window shell_win = XtWindow(top_widget);
        if (shell_win != None) query_win = shell_win;
    }

    if (XQueryTree(dpy, query_win, &root_return, &parent, &children, &nchildren)) {
        if (children) XFree(children);
    }
    
    /* Fallback to root if no parent found */
    if (parent == None) {
        parent = RootWindow(dpy, screen);
    }
    
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(dpy, parent, &attrs)) {
        panel_cmap = DefaultColormap(dpy, screen);
        bg_pixel   = WhitePixel(dpy, screen);
    } else {
        panel_cmap = (attrs.colormap != None)
                       ? attrs.colormap
                       : DefaultColormap(dpy, screen);

        Pixel chosen_bg = attrs.backing_pixel;
        if (top_widget) {
            Pixel widget_bg = 0;
            XtVaGetValues(top_widget, XmNbackground, &widget_bg, NULL);
            if (widget_bg != 0) {
                chosen_bg = widget_bg;
            }
        }
        if (chosen_bg == 0 || chosen_bg == None) {
            chosen_bg = WhitePixel(dpy, screen);
        }
        bg_pixel = chosen_bg;
    }

    /* XmGetColors: given a background pixel, derive
       foreground, top_shadow, bottom_shadow, select. */
    XmGetColors(ScreenOfDisplay(dpy, screen), panel_cmap, bg_pixel,
                &fg_pixel, &ts_pixel, &bs_pixel, &sel_pixel);

    if (form_widget) {
        XtVaSetValues(form_widget,
                      XmNbackground, bg_pixel,
                      XmNforeground, fg_pixel,
                      NULL);
    }
    if (draw_widget) {
        XtVaSetValues(draw_widget,
                      XmNbackground, bg_pixel,
                      XmNforeground, fg_pixel,
                      NULL);
    }
    if (month_option) {
        XtVaSetValues(month_option,
                      XmNbackground, bg_pixel,
                      XmNforeground, fg_pixel,
                      NULL);
    }
    if (year_spin) {
        XtVaSetValues(year_spin,
                      XmNbackground, bg_pixel,
                      XmNforeground, fg_pixel,
                      NULL);
    }
    if (year_text) {
        XtVaSetValues(year_text,
                      XmNbackground, bg_pixel,
                      XmNforeground, fg_pixel,
                      NULL);
    }

    colors_inited = 1;
}

/* ----- cairo helpers ----------------------------------------------------- */

static void ensure_cairo(void)
{
    if (cs && (surf_w == win_w) && (surf_h == win_h)) {
        return;
    }

    if (cr) {
        cairo_destroy(cr);
        cr = NULL;
    }
    if (cs) {
        cairo_surface_destroy(cs);
        cs = NULL;
    }

    Visual *visual = DefaultVisual(dpy, screen);
    cs = cairo_xlib_surface_create(dpy, win, visual, win_w, win_h);
    cr = cairo_create(cs);
    surf_w = win_w;
    surf_h = win_h;

    /* Basic drawing config */
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
}

static double clamp01(double v)
{
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

static double rgb_luma(double r, double g, double b)
{
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

static void choose_contrast_color(double bg_r, double bg_g, double bg_b,
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

static double fit_font_size(cairo_t *cr, const char *text, double max_w, double max_h)
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

static void draw_centered_text(cairo_t *cr,
                               const char *text,
                               double cx,
                               double cy,
                               cairo_text_extents_t *out_ext,
                               double *out_y);
static void draw_clock(void);
static void update_time_if_needed(void);
static void update_controls_layout(void);
static void draw_month_view(cairo_t *cr,
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
                            int first_weekday);
static void ensure_month_menu(void);
static void canvas_event_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont);
static void tick_cb(XtPointer client_data, XtIntervalId *id);
static void wm_delete_cb(Widget w, XtPointer client_data, XtPointer call_data);
static void handle_configure(XConfigureEvent *cev);

static int guess_initial_year(void)
{
    if (have_local_time) {
        return current_local_tm.tm_year + 1900;
    }

    time_t now = time(NULL);
    if (now == (time_t)-1) return 2000;

    struct tm lt;
    if (!localtime_r(&now, &lt)) return 2000;

    return lt.tm_year + 1900;
}

typedef struct {
    int split_mode;
    double left_w;
    double right_x;
    double right_w;
} CkLayout;

static CkLayout compute_layout(int w, int h)
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

static int window_is_iconified(void)
{
    if (!top_widget || !XtIsRealized(top_widget)) return 0;
    Display *display = XtDisplay(top_widget);
    if (!display) return 0;
    Window window = XtWindow(top_widget);
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
    XtAppContext app = (XtAppContext)client_data;
    XtAppSetExitFlag(app);
}

static void tick_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)client_data;
    (void)id;
    update_time_if_needed();
    if (app_ctx) {
        XtAppAddTimeOut(app_ctx, 250, tick_cb, NULL);
    }
}

static void canvas_event_handler(Widget w, XtPointer client_data, XEvent *event, Boolean *cont)
{
    (void)w;
    (void)client_data;
    (void)cont;
    if (!event) return;
    switch (event->type) {
    case Expose:
        if (event->xexpose.count == 0) {
            draw_clock();
        }
        break;
    case ConfigureNotify:
        handle_configure(&event->xconfigure);
        break;
    default:
        break;
    }
}

enum {
    LED_SEG_BAR_T = 0x01,
    LED_SEG_R_T   = 0x02,
    LED_SEG_R_B   = 0x04,
    LED_SEG_BAR_B = 0x08,
    LED_SEG_L_B   = 0x10,
    LED_SEG_L_T   = 0x20,
    LED_SEG_BAR_M = 0x40
};

static const unsigned char PM_INDICATOR_LEAD = LED_SEG_BAR_T | LED_SEG_R_T |
                                               LED_SEG_L_T | LED_SEG_L_B |
                                               LED_SEG_BAR_M;
static const unsigned char AM_INDICATOR_LEAD = LED_SEG_BAR_T | LED_SEG_R_T |
                                               LED_SEG_R_B | LED_SEG_L_T |
                                               LED_SEG_L_B | LED_SEG_BAR_M;
static const unsigned char INDICATOR_M_ARCH = LED_SEG_BAR_T | LED_SEG_L_T |
                                               LED_SEG_L_B | LED_SEG_R_T |
                                               LED_SEG_R_B;

static unsigned char led_segmap_for_digit(int d)
{
    static const unsigned char map[10] = {
        LED_SEG_BAR_T | LED_SEG_R_T | LED_SEG_R_B | LED_SEG_BAR_B |
            LED_SEG_L_B | LED_SEG_L_T,
        LED_SEG_R_T | LED_SEG_R_B,
        LED_SEG_BAR_T | LED_SEG_R_T | LED_SEG_BAR_M | LED_SEG_L_B |
            LED_SEG_BAR_B,
        LED_SEG_BAR_T | LED_SEG_R_T | LED_SEG_BAR_M | LED_SEG_R_B |
            LED_SEG_BAR_B,
        LED_SEG_L_T | LED_SEG_BAR_M | LED_SEG_R_T | LED_SEG_R_B,
        LED_SEG_BAR_T | LED_SEG_L_T | LED_SEG_BAR_M | LED_SEG_R_B |
            LED_SEG_BAR_B,
        LED_SEG_BAR_T | LED_SEG_L_T | LED_SEG_BAR_M | LED_SEG_L_B |
            LED_SEG_R_B | LED_SEG_BAR_B,
        LED_SEG_BAR_T | LED_SEG_R_T | LED_SEG_R_B,
        LED_SEG_BAR_T | LED_SEG_R_T | LED_SEG_R_B | LED_SEG_BAR_B |
            LED_SEG_L_B | LED_SEG_L_T | LED_SEG_BAR_M,
        LED_SEG_BAR_T | LED_SEG_L_T | LED_SEG_BAR_M | LED_SEG_R_T |
            LED_SEG_R_B | LED_SEG_BAR_B
    };
    if (d < 0 || d > 9) return 0;
    return map[d];
}

static unsigned char led_segmap_for_char(char c)
{
    switch (toupper((unsigned char)c)) {
    case 'A': return LED_SEG_BAR_T | LED_SEG_R_T | LED_SEG_R_B | LED_SEG_L_T |
                 LED_SEG_L_B | LED_SEG_BAR_M;
    case 'P': return LED_SEG_BAR_T | LED_SEG_R_T | LED_SEG_L_T | LED_SEG_L_B |
                 LED_SEG_BAR_M;
    case 'M': return LED_SEG_BAR_T | LED_SEG_L_T | LED_SEG_L_B | LED_SEG_R_T |
                 LED_SEG_R_B;
    default: return 0;
    }
}

static void draw_led_segment(cairo_t *cr,
                             double x,
                             double y,
                             double w,
                             double h,
                             int enabled,
                             double on_r,
                             double on_g,
                             double on_b,
                             double off_r,
                             double off_g,
                             double off_b)
{
    cairo_set_source_rgb(cr,
                         enabled ? on_r : off_r,
                         enabled ? on_g : off_g,
                         enabled ? on_b : off_b);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
}

static void draw_led_digit(cairo_t *cr,
                           double x,
                           double y,
                           double w,
                           double h,
                           unsigned char segs,
                           double on_r,
                           double on_g,
                           double on_b,
                           double off_r,
                           double off_g,
                           double off_b)
{
    double thickness = h * 0.12;
    if (thickness < 2.0) thickness = 2.0;
    double half_h = h / 2.0;

    draw_led_segment(cr, x + thickness, y, w - 2.0 * thickness, thickness,
                     (segs & 0x01) != 0, on_r, on_g, on_b, off_r, off_g, off_b);
    draw_led_segment(cr, x + w - thickness, y + thickness, thickness,
                     half_h - thickness, (segs & 0x02) != 0,
                     on_r, on_g, on_b, off_r, off_g, off_b);
    draw_led_segment(cr, x + w - thickness, y + half_h + 1.0, thickness,
                     half_h - thickness - 1.0, (segs & 0x04) != 0,
                     on_r, on_g, on_b, off_r, off_g, off_b);
    draw_led_segment(cr, x + thickness, y + h - thickness, w - 2.0 * thickness, thickness,
                     (segs & 0x08) != 0, on_r, on_g, on_b, off_r, off_g, off_b);
    draw_led_segment(cr, x, y + half_h + 1.0, thickness,
                     half_h - thickness - 1.0, (segs & 0x10) != 0,
                     on_r, on_g, on_b, off_r, off_g, off_b);
    draw_led_segment(cr, x, y + thickness, thickness,
                     half_h - thickness, (segs & 0x20) != 0,
                     on_r, on_g, on_b, off_r, off_g, off_b);
    draw_led_segment(cr, x + thickness, y + half_h - thickness / 2.0,
                     w - 2.0 * thickness, thickness,
                     (segs & 0x40) != 0, on_r, on_g, on_b, off_r, off_g, off_b);
}

static void draw_led_time(cairo_t *cr,
                          int hour,
                          int minute,
                          double zone_x,
                          double zone_y,
                          double zone_w,
                          double zone_h,
                          double on_r,
                          double on_g,
                          double on_b,
                          double off_r,
                          double off_g,
                          double off_b,
                          const char *indicator)
{
    if (zone_w <= 0.0 || zone_h <= 0.0) return;
    double margin = fmax(3.0, fmin(zone_w, zone_h) * 0.02);
    double max_height = zone_h - 2.0 * margin;
    if (max_height <= 0.0) return;

    double digit_height = max_height * 0.96;
    double digit_width = digit_height * 0.5;
    double gap = digit_width * 0.25;
    double colon_width = digit_width * 0.22;
    if (colon_width < 2.0) colon_width = 2.0;

    double available_width = zone_w - 2.0 * margin;
    bool use_triplet = indicator &&
                       (strcmp(indicator, "PM") == 0 ||
                        strcmp(indicator, "AM") == 0);
    unsigned char indicator_masks[4] = {0};
    int indicator_len = 0;
    if (use_triplet) {
        indicator_len = 3;
        unsigned char lead =
            (strcmp(indicator, "PM") == 0)
                ? PM_INDICATOR_LEAD
                : AM_INDICATOR_LEAD;
        indicator_masks[0] = lead;
        indicator_masks[1] = INDICATOR_M_ARCH;
        indicator_masks[2] = INDICATOR_M_ARCH;
    } else if (indicator && indicator[0]) {
        indicator_len = (int)strlen(indicator);
        if (indicator_len > 3) indicator_len = 3;
        for (int i = 0; i < indicator_len; ++i) {
            indicator_masks[i] = led_segmap_for_char(indicator[i]);
        }
    }
    double indicator_gap = gap;             /* match digit spacing */
    double indicator_digit_width = digit_width * 0.54;  /* +20% width */
    double indicator_digit_height = digit_height * 0.65; /* 30% taller */
    double indicator_thickness = indicator_digit_height * 0.12;
    if (indicator_thickness < 2.0) indicator_thickness = 2.0;
    double indicator_lead_gap = 0; /* minimal gap: one bar */
    double indicator_offsets[4] = {0};
    double indicator_min_off = 0.0;
    double indicator_max_off = 0.0;
    if (use_triplet) {
        for (int i = 0; i < indicator_len; ++i) {
            if (i == 2) {
                /* Place #3 immediately after #2, overlapping by one segment thickness. */
                indicator_offsets[i] = indicator_offsets[1] - (indicator_gap + indicator_thickness) +
                                       (PM_INDICATOR_P_OFFSETS[2] * indicator_gap);
            } else {
                indicator_offsets[i] = PM_INDICATOR_P_OFFSETS[i] * indicator_gap;
            }
            indicator_min_off = fmin(indicator_min_off, indicator_offsets[i]);
            indicator_max_off = fmax(indicator_max_off, indicator_offsets[i]);
        }
    }
    bool leading_zero_hour = (hour < 10);
    double digits_total = leading_zero_hour
                          ? (3.0 * digit_width + 2.0 * gap + colon_width)
                          : (4.0 * digit_width + 3.0 * gap + colon_width);
    double indicator_section = 0.0;
    if (indicator_len > 0) {
        indicator_section = indicator_len * indicator_digit_width +
                            (indicator_len - 1) * indicator_gap;
    }
    double indicator_span_left = 0.0;
    double indicator_span_right = 0.0;
    if (indicator_len > 0) {
        indicator_span_left = -fmin(indicator_min_off, 0.0);
        indicator_span_right = fmax(indicator_max_off, 0.0);
    }
    double overlap_comp = (use_triplet && indicator_len == 3)
                          ? (indicator_gap + indicator_thickness)
                          : 0.0;
    double total_width = digits_total +
                         ((indicator_len > 0)
                              ? (indicator_lead_gap + indicator_section -
                                 overlap_comp +
                                 indicator_span_left + indicator_span_right)
                              : 0.0);

    if (total_width > available_width && total_width > 0.0) {
        double scale = available_width / total_width;
        digit_width *= scale;
        digit_height *= scale;
        gap *= scale;
        colon_width *= scale;
        indicator_gap = gap;             /* keep indicator spacing aligned with digits */
        indicator_digit_width = digit_width * 0.54;  /* +20% width */
        indicator_digit_height = digit_height * 0.65; /* 30% taller */
        indicator_thickness = indicator_digit_height * 0.12;
        if (indicator_thickness < 2.0) indicator_thickness = 2.0;
        indicator_lead_gap = indicator_thickness; /* minimal gap: one bar */
        indicator_min_off = 0.0;
        indicator_max_off = 0.0;
        if (use_triplet) {
            for (int i = 0; i < indicator_len; ++i) {
                if (i == 2) {
                    /* Place #3 immediately after #2, overlapping by one segment thickness. */
                    indicator_offsets[i] = indicator_offsets[1] - (indicator_gap + indicator_thickness) +
                                           (PM_INDICATOR_P_OFFSETS[2] * indicator_gap);
                } else {
                    indicator_offsets[i] = PM_INDICATOR_P_OFFSETS[i] * indicator_gap;
                }
                indicator_min_off = fmin(indicator_min_off, indicator_offsets[i]);
                indicator_max_off = fmax(indicator_max_off, indicator_offsets[i]);
            }
        }
        overlap_comp = (use_triplet && indicator_len == 3)
                       ? (indicator_gap + indicator_thickness)
                       : 0.0;
        indicator_span_left = indicator_len > 0 ? -fmin(indicator_min_off, 0.0) : 0.0;
        indicator_span_right = indicator_len > 0 ? fmax(indicator_max_off, 0.0) : 0.0;
        digits_total = leading_zero_hour
                       ? (3.0 * digit_width + 2.0 * gap + colon_width)
                       : (4.0 * digit_width + 3.0 * gap + colon_width);
        indicator_section = indicator_len > 0
                            ? indicator_len * indicator_digit_width +
                              (indicator_len - 1) * indicator_gap
                            : 0.0;
        total_width = digits_total +
                      ((indicator_len > 0)
                           ? (indicator_lead_gap + indicator_section -
                              overlap_comp +
                              indicator_span_left + indicator_span_right)
                           : 0.0);
    }

    double start_x = zone_x + margin + (available_width - total_width) / 2.0;
    if (start_x < zone_x + margin) start_x = zone_x + margin;
    double start_y = zone_y + (zone_h - digit_height) / 2.0;
    if (start_y < zone_y) start_y = zone_y;

    double gap_mid = gap + colon_width;
    double positions[4];
    positions[0] = start_x;
    if (leading_zero_hour) {
        positions[1] = positions[0] + digit_width + gap_mid;   /* gap with colon */
        positions[2] = positions[1] + digit_width + gap;
        positions[3] = positions[2] + digit_width + gap;
    } else {
        positions[1] = positions[0] + digit_width + gap;
        positions[2] = positions[1] + digit_width + gap_mid;
        positions[3] = positions[2] + digit_width + gap;
    }

    unsigned char digits[4];
    digits[0] = (hour / 10) % 10;
    digits[1] = hour % 10;
    digits[2] = (minute / 10) % 10;
    digits[3] = minute % 10;

    if (leading_zero_hour) {
        /* draw H:MM */
        draw_led_digit(cr, positions[0], start_y, digit_width, digit_height,
                       led_segmap_for_digit(digits[1]), on_r, on_g, on_b,
                       off_r, off_g, off_b);
        draw_led_digit(cr, positions[1], start_y, digit_width, digit_height,
                       led_segmap_for_digit(digits[2]), on_r, on_g, on_b,
                       off_r, off_g, off_b);
        draw_led_digit(cr, positions[2], start_y, digit_width, digit_height,
                       led_segmap_for_digit(digits[3]), on_r, on_g, on_b,
                       off_r, off_g, off_b);
    } else {
        for (int i = 0; i < 4; ++i) {
            draw_led_digit(cr, positions[i], start_y, digit_width, digit_height,
                           led_segmap_for_digit(digits[i]), on_r, on_g, on_b,
                           off_r, off_g, off_b);
        }
    }

    double gap_start = leading_zero_hour ? (positions[0] + digit_width)
                                         : (positions[1] + digit_width);
    double gap_end = leading_zero_hour ? positions[1] : positions[2];
    double colon_space = gap_end - gap_start;
    if (colon_space < colon_width) colon_space = colon_width;
    double colon_x = gap_start + (colon_space - colon_width) / 2.0;
    double colon_height = digit_height * 0.1;
    if (colon_height < 2.0) colon_height = 2.0;
    double colon_spacing = colon_height * 1.5;
    double colon_top_y = start_y + digit_height * 0.25;
    double colon_bottom_y = colon_top_y + colon_height + colon_spacing;

    cairo_set_source_rgb(cr, on_r, on_g, on_b);
    cairo_rectangle(cr, colon_x, colon_top_y, colon_width, colon_height);
    cairo_fill(cr);
    cairo_rectangle(cr, colon_x, colon_bottom_y, colon_width, colon_height);
    cairo_fill(cr);

    if (indicator_len > 0) {
        double digits_width = digits_total;
        double indicator_x = start_x + digits_width + indicator_lead_gap +
                             indicator_span_left;
        double current_x = indicator_x;
        double indicator_y = zone_y + (zone_h - indicator_digit_height) / 2.0;
        if (indicator_y < zone_y) indicator_y = zone_y;
        for (int i = 0; i < indicator_len; ++i) {
            unsigned char segs = indicator_masks[i];
            double char_x = current_x + indicator_offsets[i];
            double char_on_r = on_r;
            double char_on_g = on_g;
            double char_on_b = on_b;
            draw_led_digit(cr, char_x, indicator_y,
                           indicator_digit_width, indicator_digit_height,
                           segs, char_on_r, char_on_g, char_on_b,
                           off_r, off_g, off_b);
            current_x += indicator_digit_width + indicator_gap;
        }
    }
}

static void update_wm_icon_pixmap(Display *display)
{
    if (!display || !top_widget || !XtIsRealized(top_widget)) return;
    Window window = XtWindow(top_widget);
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
    local.icon_pixmap = icon_pixmap;
    XSetWMHints(display, window, &local);
}

static void refresh_icon_pixmap(void)
{
    if (!top_widget || !XtIsRealized(top_widget) || !have_local_time) return;
    Display *display = XtDisplay(top_widget);
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

    /* Temporarily render using the same pipeline as the main window */
    cairo_surface_t *old_cs = cs;
    cairo_t *old_cr = cr;
    int old_surf_w = surf_w;
    int old_surf_h = surf_h;
    int old_win_w = win_w;
    int old_win_h = win_h;
    bool old_force = force_full_redraw;

    cs = isurf;
    cr = icr;
    surf_w = icon_w;
    surf_h = icon_h;
    win_w = icon_w;
    win_h = icon_h;
    force_full_redraw = true;

    draw_clock();

    /* restore */
    cs = old_cs;
    cr = old_cr;
    surf_w = old_surf_w;
    surf_h = old_surf_h;
    win_w = old_win_w;
    win_h = old_win_h;
    force_full_redraw = old_force;

    cairo_surface_flush(isurf);
    cairo_destroy(icr);
    cairo_surface_destroy(isurf);
    XFlush(display);

    if (icon_pixmap != None) {
        XFreePixmap(display, icon_pixmap);
    }
    icon_pixmap = new_pixmap;
    XtVaSetValues(top_widget, XmNiconPixmap, icon_pixmap, NULL);
    update_wm_icon_pixmap(display);

    last_icon_minute = current_local_tm.tm_min;
}

static void draw_centered_text(cairo_t *cr,
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

static int get_first_weekday(void)
{
#ifdef _NL_TIME_FIRST_WEEKDAY
    /* glibc extension: returned string's first byte:
       1 = Monday, 2 = Tuesday, ... 7 = Sunday (see locale(7)). */
    const char *fw = nl_langinfo(_NL_TIME_FIRST_WEEKDAY);
    if (fw && *fw) {
        int v = (unsigned char)fw[0];
        if (v >= 1 && v <= 7) {
            /* convert to tm_wday: 0=Sunday..6=Saturday */
            return (v % 7);
        }
    }
#endif
    /* Default to Monday-first */
    return 1;
}

static int days_in_month(int year, int mon)
{
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (mon == 1) { /* Feb */
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
    int wday = (t.tm_wday + 6) % 7; /* Monday=0 */
    int yday = t.tm_yday;

    int thursday = yday + (3 - wday);
    struct tm th = {0};
    th.tm_year = year;
    th.tm_mday = 1 + thursday;
    mktime(&th);
    return 1 + th.tm_yday / 7;
}

static void draw_month_view(cairo_t *cr,
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
                            int first_weekday)
{
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);

    double text_r, text_g, text_b;
    choose_contrast_color(bg_r, bg_g, bg_b, fg_r, fg_g, fg_b, &text_r, &text_g, &text_b);

    cairo_set_source_rgb(cr, clamp01(bg_r * 0.95 + 0.05),
                         clamp01(bg_g * 0.95 + 0.05),
                         clamp01(bg_b * 0.95 + 0.05));
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);

    double padding = w * 0.05;
    if (padding < 4.0) padding = 4.0;
    double header_h = h * 0.12;
    if (right_controls_bottom > y) {
        double controls_h = right_controls_bottom - y;
        if (controls_h > header_h) header_h = controls_h;
    }
    double grid_y = y + header_h + padding;
    double grid_h = h - header_h - padding * 1.5;
    if (grid_h < 10.0) grid_h = 10.0;

    int today = current_local_tm.tm_mday;
    int days = days_in_month(year, mon);

    /* Header area is reserved for Motif controls (month OptionMenu + year spinner). */

    /* weekday headers */
    int cols = 8; /* week number + 7 days */
    int rows = 7; /* header + up to 6 weeks */
    double cell_w = (w - 2.0 * padding) / cols;
    double cell_h = grid_h / rows;
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    double weekday_font = fit_font_size(cr, "THU", cell_w * 0.85, cell_h * 0.6);
    double day_font = fit_font_size(cr, "88", cell_w * 0.85, cell_h * 0.7);
    double week_font = fit_font_size(cr, "52", cell_w * 0.75, cell_h * 0.6);
    cairo_set_font_size(cr, weekday_font);
    cairo_set_source_rgb(cr, text_r, text_g, text_b);
    for (int c = 0; c < 7; ++c) {
        int widx = (c + first_weekday) % 7;
        const char *lbl = weekday_labels[widx];
        double cx = x + padding + cell_w * (c + 1) + cell_w / 2.0;
        double cy = grid_y + cell_h * 0.6;
        draw_centered_text(cr, lbl, cx, cy, NULL, NULL);
    }

    /* month grid */
    struct tm first = {0};
    first.tm_year = year;
    first.tm_mon = mon;
    first.tm_mday = 1;
    mktime(&first);
    int first_wday = (first.tm_wday - first_weekday + 7) % 7; /* offset by locale */

    int row = 1;
    int col = 1;
    cairo_set_font_size(cr, day_font);

    /* previous month filler */
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
                             clamp01(fg_r * 0.45 + bg_r * 0.55),
                             clamp01(fg_g * 0.45 + bg_g * 0.55),
                             clamp01(fg_b * 0.45 + bg_b * 0.55));
        draw_centered_text(cr, buf, cx, cy, NULL, NULL);
        col++;
    }
    /* start current month after fillers */
    col = leading_slots + 1;

    for (int d = 1; d <= days; ++d) {
        if (col >= cols) {
            col = 1;
            row++;
        }
        double cx = x + padding + cell_w * col + cell_w / 2.0;
        double cy = grid_y + cell_h * (row + 0.55);

        char buf[12];
        snprintf(buf, sizeof(buf), "%d", d);

        /* highlight today */
        if ((mon == current_local_tm.tm_mon) &&
            (year == current_local_tm.tm_year) &&
            d == today) {
            cairo_set_source_rgb(cr, clamp01(fg_r * 0.2 + bg_r * 0.8),
                                 clamp01(fg_g * 0.2 + bg_g * 0.8),
                                 clamp01(fg_b * 0.2 + bg_b * 0.8));
            cairo_rectangle(cr,
                            x + padding + cell_w * col + 2.0,
                            grid_y + cell_h * row + 2.0,
                            cell_w - 4.0, cell_h - 4.0);
            cairo_fill(cr);
        }

        cairo_set_source_rgb(cr, text_r, text_g, text_b);
        draw_centered_text(cr, buf, cx, cy, NULL, NULL);
        col++;
    }

    /* next month filler */
    int next_day = 1;
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
                             clamp01(fg_r * 0.45 + bg_r * 0.55),
                             clamp01(fg_g * 0.45 + bg_g * 0.55),
                             clamp01(fg_b * 0.45 + bg_b * 0.55));
        draw_centered_text(cr, buf, cx, cy, NULL, NULL);
        col++;
    }

    /* week numbers */
    cairo_set_font_size(cr, week_font);
    cairo_set_source_rgb(cr, text_r, text_g, text_b);
    int week_row = 1;
    int day_for_week = 1;
    int col_for_week = first_wday;
    while (week_row <= 6 && day_for_week <= days) {
        /* find Monday of this row */
        int day_this_row = day_for_week - col_for_week;
        if (day_this_row < 1) day_this_row = 1;
        int week = iso_week_number(year, mon, day_this_row);
        char wbuf[12];
        snprintf(wbuf, sizeof(wbuf), "%d", week);
        double cx = x + padding + cell_w * 0.5;
        double cy = grid_y + cell_h * (week_row + 0.55);
        draw_centered_text(cr, wbuf, cx, cy, NULL, NULL);

        week_row++;
        day_for_week += 7;
    }

    cairo_restore(cr);
}

static void month_menu_select_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    int mon = (int)(uintptr_t)client_data;
    view_mon = mon;
    if (month_option && month_items[mon]) {
        XtVaSetValues(month_option, XmNmenuHistory, month_items[mon], NULL);
    }
    force_full_redraw = true;
    draw_clock();
}

static void year_text_changed_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    (void)call_data;
    if (updating_year_spin) return;
    if (!year_text) return;

    char *s = XmTextFieldGetString(year_text);
    if (!s) return;
    int y = atoi(s);
    XtFree(s);

    if (y < 1900 || y > 2200) {
        return;
    }

    view_year = y - 1900;
    force_full_redraw = true;
    draw_clock();
}

static void ensure_month_menu(void)
{
    if ((month_menu && month_option) || !top_widget) return;
    if (!form_widget) return;

    Arg args[4];
    int n = 0;
    month_pulldown = XmCreatePulldownMenu(form_widget, (char *)"monthPulldown", args, n);
    month_menu = month_pulldown;
    for (int i = 0; i < 12; ++i) {
        XmString label = XmStringCreateLocalized((char *)month_labels[i]);
        Widget item = XmCreatePushButtonGadget(month_pulldown, (char *)month_labels[i], NULL, 0);
        XtVaSetValues(item, XmNlabelString, label, NULL);
        XmStringFree(label);
        XtAddCallback(item, XmNactivateCallback, month_menu_select_cb, (XtPointer)(uintptr_t)i);
        XtManageChild(item);
        month_items[i] = item;
    }

    Arg args_menu[4];
    int n2 = 0;
    XtSetArg(args_menu[n2], XmNsubMenuId, month_pulldown); n2++;
    month_option = XmCreateOptionMenu(form_widget, (char *)"monthOption", args_menu, n2);
    XtVaSetValues(month_option,
                  XmNtopAttachment, XmATTACH_NONE,
                  XmNbottomAttachment, XmATTACH_NONE,
                  XmNleftAttachment, XmATTACH_NONE,
                  XmNrightAttachment, XmATTACH_NONE,
                  NULL);

    year_spin = XmCreateSpinBox(form_widget, (char *)"yearSpin", NULL, 0);
    XtVaSetValues(year_spin,
                  XmNtopAttachment, XmATTACH_NONE,
                  XmNbottomAttachment, XmATTACH_NONE,
                  XmNleftAttachment, XmATTACH_NONE,
                  XmNrightAttachment, XmATTACH_NONE,
                  NULL);

    int initial_year = guess_initial_year();
    if (initial_year < 1900) initial_year = 1900;
    else if (initial_year > 2200) initial_year = 2200;

    year_text = XmCreateTextField(year_spin, (char *)"yearText", NULL, 0);
    XtVaSetValues(year_text,
                  XmNcolumns, 5,
                  XmNspinBoxChildType, XmNUMERIC,
                  XmNminimumValue, 1900,
                  XmNmaximumValue, 2200,
                  XmNincrementValue, 1,
                  XmNposition, initial_year,
                  NULL);
    XtAddCallback(year_text, XmNvalueChangedCallback, year_text_changed_cb, NULL);
    XtAddCallback(year_text, XmNactivateCallback, year_text_changed_cb, NULL);
    XtAddCallback(year_text, XmNlosingFocusCallback, year_text_changed_cb, NULL);
    XtManageChild(year_text);
    XtManageChild(year_spin);
}

static void update_controls_layout(void)
{
    if (!form_widget) return;

    CkLayout lay = compute_layout(win_w, win_h);
    int want_controls = (lay.split_mode && lay.right_w > 40.0);

    if (!want_controls) {
        if (controls_visible) {
            if (month_option && XtIsManaged(month_option)) XtUnmanageChild(month_option);
            if (year_spin && XtIsManaged(year_spin)) XtUnmanageChild(year_spin);
            controls_visible = 0;
        }
        right_controls_bottom = 0.0;
        if (top_widget && XtIsRealized(top_widget) && last_split_mode != lay.split_mode) {
            const char *title = lay.split_mode ? "Time and Calendar" : "Time";
            XtVaSetValues(top_widget, XmNtitle, title, XmNiconName, title, NULL);
            last_split_mode = lay.split_mode;
        }
        return;
    }

    ensure_month_menu();
    if (!month_option || !year_spin || !year_text) return;

    if (have_local_time) {
        if (view_year < 0) view_year = current_local_tm.tm_year;
        if (view_mon < 0) view_mon = current_local_tm.tm_mon;
    }

    int pad = (int)fmax(4.0, lay.right_w * 0.05);
    int header_y = (int)fmax(2.0, pad * 0.25);
    int opt_x = (int)(lay.right_x + pad);
    int right_edge = (int)(lay.right_x + lay.right_w - pad);
    int available_width = right_edge - opt_x;
    if (available_width < 0) available_width = 0;
    int base_month_w = (int)fmax(80.0, lay.right_w * 0.35);
    int spin_gap = (int)fmax(4.0, pad * 0.5);
    int year_min_w = 70;
    int max_month_w = available_width - year_min_w - spin_gap;
    if (max_month_w < 60) max_month_w = available_width;
    if (base_month_w > max_month_w) base_month_w = max_month_w;
    if (base_month_w < 60) base_month_w = 60;
    int inline_space = available_width - base_month_w - spin_gap;
    if (inline_space < 0) inline_space = 0;
    int spin_w = (int)fmax(80.0, lay.right_w * 0.2);
    if (spin_w > inline_space) spin_w = inline_space;
    if (spin_w < 50) spin_w = 50;

    XtVaSetValues(month_option,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftOffset, opt_x,
                  XmNtopOffset, header_y,
                  XmNwidth, (Dimension)base_month_w,
                  NULL);

    if (!XtIsManaged(month_option)) XtManageChild(month_option);
    if (!XtIsManaged(year_spin)) XtManageChild(year_spin);
    controls_visible = 1;

    if (view_mon >= 0 && view_mon < 12 && month_items[view_mon]) {
        XtVaSetValues(month_option, XmNmenuHistory, month_items[view_mon], NULL);
    }

    int month_h = 0;
    XtVaGetValues(month_option, XmNheight, &month_h, NULL);
    int row_h = month_h > 0 ? (int)month_h : 28;
    int year_x = opt_x + base_month_w + spin_gap;
    int year_y = header_y;
    int year_w = spin_w;
    int year_h = row_h;
    bool inline_year = inline_space >= year_min_w;
    if (!inline_year || year_w < 60) {
        year_x = opt_x;
        year_y = header_y + row_h + spin_gap;
        year_w = right_edge - opt_x;
        if (year_w < 60) year_w = 60;
    }

    XtVaSetValues(year_spin,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftOffset, year_x,
                  XmNtopOffset, year_y,
                  XmNwidth, (Dimension)year_w,
                  XmNheight, (Dimension)year_h,
                  NULL);

    int month_bottom = header_y + row_h;
    int year_bottom = year_y + year_h;
    int header_bottom = month_bottom > year_bottom ? month_bottom : year_bottom;
    right_controls_bottom = (double)header_bottom + pad * 0.5;

    if (XtIsRealized(month_option)) XRaiseWindow(dpy, XtWindow(month_option));
    if (XtIsRealized(year_spin)) XRaiseWindow(dpy, XtWindow(year_spin));

    /* Keep spinbox value in sync with current view_year without causing callback loops. */
    if (view_year >= 0) {
        updating_year_spin = 1;
        XtVaSetValues(year_text, XmNposition, view_year + 1900, NULL);
        updating_year_spin = 0;
    }

    if (top_widget && XtIsRealized(top_widget) && last_split_mode != lay.split_mode) {
        const char *title = lay.split_mode ? "Time and Calendar" : "Time";
        XtVaSetValues(top_widget, XmNtitle, title, XmNiconName, title, NULL);
        last_split_mode = lay.split_mode;
    }
}

static void update_time_if_needed(void)
{
    time_t now = time(NULL);
    if (now == (time_t)-1) return;
    if (!force_full_redraw && have_local_time && now == last_time_check) {
        return;
    }

    struct tm lt;
    if (!localtime_r(&now, &lt)) return;

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
        strncpy(current_ampm, trimmed_ampm, sizeof(current_ampm));
        current_ampm[sizeof(current_ampm) - 1] = '\0';
    } else if (FORCE_USE_AM_PM) {
        const char *fallback = (lt.tm_hour >= 12) ? "PM" : "AM";
        strncpy(current_ampm, fallback, sizeof(current_ampm));
        current_ampm[sizeof(current_ampm) - 1] = '\0';
        has_ampm = true;
    } else {
        current_ampm[0] = '\0';
    }
    show_ampm_indicator = has_ampm;

    int minute_changed = (lt.tm_min != last_display_min);
    int changed = force_full_redraw ||
                  !have_local_time ||
                  minute_changed ||
                  lt.tm_hour != last_display_hour ||
                  lt.tm_mday != last_display_mday ||
                  lt.tm_mon != last_display_mon ||
                  lt.tm_year != last_display_year;

    last_time_check = now;

    if (changed) {
        last_display_hour = lt.tm_hour;
        last_display_min  = lt.tm_min;
        last_display_mday = lt.tm_mday;
        last_display_mon  = lt.tm_mon;
        last_display_year = lt.tm_year;
        current_local_tm = lt;
        have_local_time = true;
        if (view_year < 0) view_year = lt.tm_year;
        if (view_mon < 0) view_mon = lt.tm_mon;
        update_controls_layout();
        force_full_redraw = false;
        draw_clock();
        if (minute_changed || last_icon_minute == -1 || window_is_iconified()) {
            refresh_icon_pixmap();
        }
    }
}

/* ----- Drawing ----------------------------------------------------------- */

static void draw_clock(void)
{
    if (!dpy || !win || !have_local_time) return;

    ensure_cairo();
    init_motif_colors();

    if (!colors_inited) {
        bg_pixel  = WhitePixel(dpy, screen);
        fg_pixel  = BlackPixel(dpy, screen);
        ts_pixel  = fg_pixel;
        bs_pixel  = fg_pixel;
        sel_pixel = fg_pixel;
        panel_cmap = DefaultColormap(dpy, screen);
    }

    double bg_r, bg_g, bg_b;
    double fg_r, fg_g, fg_b;
    double ts_r, ts_g, ts_b;
    double bs_r, bs_g, bs_b;
    double sel_r, sel_g, sel_b;

    pixel_to_rgb(bg_pixel,  &bg_r,  &bg_g,  &bg_b);
    pixel_to_rgb(fg_pixel,  &fg_r,  &fg_g,  &fg_b);
    pixel_to_rgb(ts_pixel,  &ts_r,  &ts_g,  &ts_b);
    pixel_to_rgb(bs_pixel,  &bs_r,  &bs_g,  &bs_b);
    pixel_to_rgb(sel_pixel, &sel_r, &sel_g, &sel_b);

    cairo_set_source_rgb(cr, bg_r, bg_g, bg_b);
    cairo_rectangle(cr, 0, 0, win_w, win_h);
    cairo_fill(cr);

    CkLayout lay = compute_layout(win_w, win_h);
    int split_mode = lay.split_mode;
    double left_w = lay.left_w;
    double right_x = lay.right_x;
    double right_w = lay.right_w;

    double time_area_h = win_h / 3.0;

    double led_on_r = 0.4;
    double led_on_g = 0.95;
    double led_on_b = 0.55;
    double led_off_r = clamp01(fg_r * 0.45 + 0.05);
    double led_off_g = clamp01(fg_g * 0.45 + 0.05);
    double led_off_b = clamp01(fg_b * 0.45 + 0.05);

    int display_hour = current_local_tm.tm_hour;
    if (show_ampm_indicator) {
        if (display_hour == 0) {
            display_hour = 12;
        } else if (display_hour > 12) {
            display_hour -= 12;
        }
    }
    const char *ampm_indicator = show_ampm_indicator ? current_ampm : NULL;

    double time_pad = fmax(3.0, time_area_h * 0.08);
    double indent_offset = time_pad * 0.25;
    double inner_x = time_pad;
    double inner_y = time_pad + indent_offset;
    double inner_w = left_w - 2.0 * time_pad;
    double inner_h = time_area_h - time_pad - indent_offset;
    if (inner_h < 12.0) inner_h = 12.0;
    if (inner_w < 20.0) inner_w = 20.0;

    const double BAR_BG_FACTOR = 0.4;
    double bar_r = clamp01(bs_r * BAR_BG_FACTOR);
    double bar_g = clamp01(bs_g * BAR_BG_FACTOR);
    double bar_b = clamp01(bs_b * BAR_BG_FACTOR);
    cairo_set_source_rgb(cr, bar_r, bar_g, bar_b);
    cairo_rectangle(cr, inner_x, inner_y, inner_w, inner_h);
    cairo_fill(cr);

    double frame_width = 2.0;
    cairo_set_line_width(cr, frame_width);
    cairo_set_source_rgb(cr, bs_r, bs_g, bs_b);
    cairo_move_to(cr, inner_x, inner_y);
    cairo_line_to(cr, inner_x + inner_w, inner_y);
    cairo_move_to(cr, inner_x, inner_y);
    cairo_line_to(cr, inner_x, inner_y + inner_h);
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, ts_r, ts_g, ts_b);
    cairo_move_to(cr, inner_x, inner_y + inner_h);
    cairo_line_to(cr, inner_x + inner_w, inner_y + inner_h);
    cairo_move_to(cr, inner_x + inner_w, inner_y);
    cairo_line_to(cr, inner_x + inner_w, inner_y + inner_h);
    cairo_stroke(cr);

    draw_led_time(cr,
                  display_hour,
                  current_local_tm.tm_min,
                  inner_x,
                  inner_y,
                  inner_w,
                  inner_h,
                  led_on_r, led_on_g, led_on_b,
                  led_off_r, led_off_g, led_off_b,
                  ampm_indicator);

    double cal_margin = left_w * 0.05;
    if (cal_margin < 6.0) cal_margin = 6.0;
    double cal_top = time_area_h + cal_margin * 0.4;
    double cal_height = win_h - cal_top - cal_margin;
    if (cal_height <= 12.0) {
        cairo_surface_flush(cs);
        return;
    }
    double cal_max_width = left_w - 2.0 * cal_margin;
    if (cal_max_width <= 12.0) {
        cairo_surface_flush(cs);
        return;
    }

    double weekday_font = cal_height * 0.1725; /* +15% */
    if (weekday_font < 10.0) weekday_font = 10.0;
    double day_font = cal_height * 0.55;
    if (day_font < weekday_font * 1.5) day_font = weekday_font * 1.5;
    if (day_font > cal_height * 0.7) day_font = cal_height * 0.7;
    double desired_width = day_font * 2.0;
    if (desired_width < 48.0) desired_width = 48.0;
    double cal_width = fmin(cal_max_width, desired_width);
    if (cal_width <= 12.0) {
        cairo_surface_flush(cs);
        return;
    }
    double cal_left = (left_w - cal_width) / 2.0;

    double shadow_offset = left_w * 0.015;
    if (shadow_offset < 2.0) shadow_offset = 2.0;
    double shadow_offset_x = shadow_offset * 2.0;
    double shadow_offset_y = shadow_offset * 2.0;
    cairo_set_source_rgb(cr,
                         clamp01(bs_r * 0.85 + bg_r * 0.15),
                         clamp01(bs_g * 0.85 + bg_g * 0.15),
                         clamp01(bs_b * 0.85 + bg_b * 0.15));
    cairo_rectangle(cr, cal_left + shadow_offset_x,
                    cal_top + shadow_offset_y,
                    cal_width, cal_height);
    cairo_fill(cr);

    double paper_r = clamp01(ts_r * 0.6 + bg_r * 0.4 + 0.05);
    double paper_g = clamp01(ts_g * 0.6 + bg_g * 0.4 + 0.05);
    double paper_b = clamp01(ts_b * 0.6 + bg_b * 0.4 + 0.05);
    cairo_set_source_rgb(cr, paper_r, paper_g, paper_b);
    cairo_rectangle(cr, cal_left, cal_top, cal_width, cal_height);
    cairo_fill(cr);

    cairo_set_line_width(cr, 1.5);
    double paper_fg_r, paper_fg_g, paper_fg_b;
    choose_contrast_color(paper_r, paper_g, paper_b, fg_r, fg_g, fg_b,
                          &paper_fg_r, &paper_fg_g, &paper_fg_b);
    cairo_set_source_rgb(cr, paper_fg_r, paper_fg_g, paper_fg_b);
    cairo_rectangle(cr, cal_left, cal_top, cal_width, cal_height);
    cairo_stroke(cr);

    double center_x = cal_left + cal_width / 2.0;

    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);

    char day_buf[8];
    snprintf(day_buf, sizeof(day_buf), "%d", current_local_tm.tm_mday);
    cairo_set_font_size(cr, day_font);
    cairo_text_extents_t day_ext = {0};
    cairo_text_extents(cr, day_buf, &day_ext);

    double spacing = cal_height * 0.05;
    if (spacing < 4.0) spacing = 4.0;
    double label_padding = cal_height * 0.02;
    if (label_padding < 2.0) label_padding = 2.0;
    double remaining_height = cal_height - day_ext.height - 2.0 * spacing - label_padding;
    if (remaining_height < 0.0) remaining_height = 0.0;
    double label_font = (remaining_height / 2.0) * 1.15; /* boost 15% */
    double label_max = day_font / 1.5;
    if (label_font > label_max) label_font = label_max;
    if (label_font < 10.0) label_font = 10.0;
    weekday_font = label_font;

    /* Compute vertical positions based on actual day height:
       split remaining space evenly above/below the day, then
       center weekday/month in those halves. */
    double space_remain = cal_height - day_ext.height;
    if (space_remain < 0.0) space_remain = 0.0;
    double half_space = space_remain / 2.0;
    double weekday_y = cal_top + half_space / 2.0;
    double day_y     = cal_top + half_space + day_ext.height / 2.0;
    double month_y   = cal_top + half_space + day_ext.height + half_space / 2.0;

    cairo_set_source_rgb(cr, paper_fg_r, paper_fg_g, paper_fg_b);
    cairo_set_font_size(cr, day_font);
    draw_centered_text(cr, day_buf, center_x, day_y, &day_ext, NULL);

    cairo_set_font_size(cr, weekday_font);
    draw_centered_text(cr,
                       weekday_labels[(current_local_tm.tm_wday + 7) % 7],
                       center_x, weekday_y,
                       NULL, NULL);

    double month_font = weekday_font;
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_ITALIC, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, month_font);
    draw_centered_text(cr,
                       month_labels[current_local_tm.tm_mon % 12],
                       center_x, month_y,
                       NULL, NULL);


    cairo_surface_flush(cs);
    if (split_mode && right_w > 20.0) {
        if (view_year < 0) {
            view_year = current_local_tm.tm_year;
        }
        if (view_mon < 0) {
            view_mon = current_local_tm.tm_mon;
        }
        int first_wday = get_first_weekday();
        draw_month_view(cr,
                        right_x,
                        0,
                        right_w,
                        win_h,
                        fg_r, fg_g, fg_b,
                        bg_r, bg_g, bg_b,
                        view_year, view_mon, first_wday);
        cairo_surface_flush(cs);
    }

    /* X flush is handled by the Xt event loop */
}

/* Resize */
static void handle_configure(XConfigureEvent *cev)
{
    if (cev->width > 0)  win_w = cev->width;
    if (cev->height > 0) win_h = cev->height;
    force_full_redraw = true;
    update_controls_layout();
    update_time_if_needed();
}

/* ----- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    /* parse flags: --no-embedd and --seconds=none|off|tick|smooth */
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--no-embedd") == 0) {
            no_embed = 1;
            for (int j = i; j < argc - 1; ++j) argv[j] = argv[j + 1];
            argc--;
            continue;
        }
        i++;
    }

    Widget toplevel;

    /* Initialize Xt/Motif – this sets up per-display info for XmGetColors */
    toplevel = XtAppInitialize(
        &app_ctx,
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
                  XmNwidth,  win_w,
                  XmNheight, win_h,
                  NULL);

    top_widget = toplevel;
    dpy = XtDisplay(toplevel);
    screen = DefaultScreen(dpy);

    form_widget = XmCreateForm(toplevel, (char *)"form", NULL, 0);
    XtManageChild(form_widget);

    draw_widget = XmCreateDrawingArea(form_widget, (char *)"canvas", NULL, 0);
    XtVaSetValues(draw_widget,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  NULL);
    XtManageChild(draw_widget);

    XtRealizeWidget(toplevel);
    win = XtWindow(draw_widget);

    /* Set WM_CLASS: normal embedding vs debug */
    XClassHint class_hint;
    if (no_embed) {
        class_hint.res_name  = (char *)"ck-clock-debug";
        class_hint.res_class = (char *)"CKClockDebug";
    } else {
        class_hint.res_name  = (char *)"ck-clock";
        class_hint.res_class = (char *)"CKClock";
    }
    Window shell_win = XtWindow(toplevel);
    XSetClassHint(dpy, shell_win, &class_hint);

    XSizeHints sh;
    sh.flags      = PSize;
    sh.width      = win_w;
    sh.height     = win_h;
    XSetWMNormalHints(dpy, shell_win, &sh);
    XtVaSetValues(toplevel, XmNtitle, "Time", XmNiconName, "Time", NULL);

    Atom wm_delete_atom = XmInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(toplevel, wm_delete_atom, wm_delete_cb, (XtPointer)app_ctx);

    ensure_month_menu();
    update_time_if_needed();
    update_controls_layout();
    XtAddEventHandler(draw_widget,
                      ExposureMask | StructureNotifyMask,
                      False,
                      canvas_event_handler,
                      NULL);
    XtAppAddTimeOut(app_ctx, 250, tick_cb, NULL);
    XtAppMainLoop(app_ctx);

    /* Not normally reached */
    if (cr) cairo_destroy(cr);
    if (cs) cairo_surface_destroy(cs);
    if (icon_pixmap != None && dpy) {
        XFreePixmap(dpy, icon_pixmap);
    }
    if (toplevel) {
        XtDestroyWidget(toplevel);
    }
    if (dpy) {
        XtCloseDisplay(dpy);
    }
    return 0;
}
