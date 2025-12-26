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

#include <errno.h>
#include <ctype.h>
#include <locale.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static Display *dpy = NULL;
static int screen;
static Window win;
static Widget top_widget = NULL;

static int win_w = 48, win_h = 48;

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
    
    if (XQueryTree(dpy, win, &root_return, &parent, &children, &nchildren)) {
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

static unsigned char led_segmap_for_digit(int d)
{
    static const unsigned char map[10] = {
        0x3F, 0x06, 0x5B, 0x4F, 0x66,
        0x6D, 0x7D, 0x07, 0x7F, 0x6F
    };
    if (d < 0 || d > 9) return 0;
    return map[d];
}

static unsigned char led_segmap_for_char(char c)
{
    switch (toupper((unsigned char)c)) {
    case 'A': return 0x77;
    case 'P': return 0x73;
    case 'M': return 0x37;
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
    double margin = zone_w * 0.05;
    double max_height = zone_h - 2.0 * margin;
    if (max_height <= 0.0) return;

    double digit_height = max_height * 0.9;
    double digit_width = digit_height * 0.5;
    double gap = digit_width * 0.25;
    double colon_width = digit_width * 0.22;
    if (colon_width < 2.0) colon_width = 2.0;

    double available_width = zone_w - 2.0 * margin;
    int indicator_len = (indicator && indicator[0]) ? (int)strlen(indicator) : 0;
    double indicator_gap = digit_width * 0.35;
    double indicator_digit_width = digit_width * 0.45;
    double indicator_digit_height = digit_height * 0.45;
    double digits_total = 4.0 * digit_width + 3.0 * gap + colon_width;
    double indicator_section = 0.0;
    if (indicator_len > 0) {
        indicator_section = indicator_len * indicator_digit_width +
                            (indicator_len - 1) * indicator_gap;
    }
    double total_width = digits_total +
                         ((indicator_len > 0) ? (indicator_gap + indicator_section) : 0.0);

    if (total_width > available_width && total_width > 0.0) {
        double scale = available_width / total_width;
        digit_width *= scale;
        digit_height *= scale;
        gap *= scale;
        colon_width *= scale;
        indicator_gap = digit_width * 0.35;
        indicator_digit_width = digit_width * 0.45;
        indicator_digit_height = digit_height * 0.45;
        digits_total = 4.0 * digit_width + 3.0 * gap + colon_width;
        indicator_section = indicator_len > 0
                            ? indicator_len * indicator_digit_width +
                              (indicator_len - 1) * indicator_gap
                            : 0.0;
        total_width = digits_total +
                      ((indicator_len > 0) ? (indicator_gap + indicator_section) : 0.0);
    }

    double start_x = zone_x + margin + (available_width - total_width) / 2.0;
    if (start_x < zone_x + margin) start_x = zone_x + margin;
    double start_y = zone_y + (zone_h - digit_height) / 2.0;
    if (start_y < zone_y) start_y = zone_y;

    double gap_mid = gap + colon_width;
    double positions[4];
    positions[0] = start_x;
    positions[1] = positions[0] + digit_width + gap;
    positions[2] = positions[1] + digit_width + gap_mid;
    positions[3] = positions[2] + digit_width + gap;

    unsigned char digits[4];
    digits[0] = (hour / 10) % 10;
    digits[1] = hour % 10;
    digits[2] = (minute / 10) % 10;
    digits[3] = minute % 10;

    for (int i = 0; i < 4; ++i) {
        draw_led_digit(cr, positions[i], start_y, digit_width, digit_height,
                       led_segmap_for_digit(digits[i]), on_r, on_g, on_b,
                       off_r, off_g, off_b);
    }

    double gap_start = positions[1] + digit_width;
    double gap_end = positions[2];
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
        double indicator_x = start_x + digits_width + indicator_gap;
        double indicator_y = zone_y + (zone_h - indicator_digit_height) / 2.0;
        if (indicator_y < zone_y) indicator_y = zone_y;
        for (int i = 0; i < indicator_len; ++i) {
            unsigned char segs = led_segmap_for_char(indicator[i]);
            draw_led_digit(cr, indicator_x, indicator_y,
                           indicator_digit_width, indicator_digit_height,
                           segs, on_r, on_g, on_b, off_r, off_g, off_b);
            indicator_x += indicator_digit_width + indicator_gap;
        }
    }
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

static void draw_clock(void);

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

    int changed = force_full_redraw ||
                  !have_local_time ||
                  lt.tm_min != last_display_min ||
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
        force_full_redraw = false;
        draw_clock();
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
    double inner_w = win_w - 2.0 * time_pad;
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

    double cal_margin = win_w * 0.05;
    if (cal_margin < 6.0) cal_margin = 6.0;
    double cal_top = time_area_h + cal_margin * 0.4;
    double cal_height = win_h - cal_top - cal_margin;
    if (cal_height <= 12.0) {
        cairo_surface_flush(cs);
        return;
    }
    double cal_max_width = win_w - 2.0 * cal_margin;
    if (cal_max_width <= 12.0) {
        cairo_surface_flush(cs);
        return;
    }

    double weekday_font_coef = cal_height * 0.15;
    double weekday_font = weekday_font_coef < 10.0 ? 10.0 : weekday_font_coef;
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
    double cal_left = (win_w - cal_width) / 2.0;

    double shadow_offset = win_w * 0.015;
    if (shadow_offset < 2.0) shadow_offset = 2.0;
    cairo_set_source_rgb(cr,
                         clamp01(bs_r * 0.85 + bg_r * 0.15),
                         clamp01(bs_g * 0.85 + bg_g * 0.15),
                         clamp01(bs_b * 0.85 + bg_b * 0.15));
    cairo_rectangle(cr, cal_left + shadow_offset,
                    cal_top + shadow_offset / 2.0,
                    cal_width, cal_height);
    cairo_fill(cr);

    double paper_r = clamp01(ts_r * 0.6 + bg_r * 0.4 + 0.05);
    double paper_g = clamp01(ts_g * 0.6 + bg_g * 0.4 + 0.05);
    double paper_b = clamp01(ts_b * 0.6 + bg_b * 0.4 + 0.05);
    cairo_set_source_rgb(cr, paper_r, paper_g, paper_b);
    cairo_rectangle(cr, cal_left, cal_top, cal_width, cal_height);
    cairo_fill(cr);

    cairo_set_line_width(cr, 1.5);
    cairo_set_source_rgb(cr, fg_r, fg_g, fg_b);
    cairo_rectangle(cr, cal_left, cal_top, cal_width, cal_height);
    cairo_stroke(cr);

    double center_x = cal_left + cal_width / 2.0;

    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);

    char day_buf[8];
    snprintf(day_buf, sizeof(day_buf), "%d", current_local_tm.tm_mday);
    cairo_set_font_size(cr, day_font);
    double day_y = cal_top + cal_height * 0.5;
    cairo_text_extents_t day_ext = {0};
    draw_centered_text(cr, day_buf, center_x, day_y,
                       &day_ext, NULL);

    double spacing = cal_height * 0.05;
    double weekday_y = day_y - (day_ext.height / 2.0) - (weekday_font / 2.0) - spacing;
    if (weekday_y < cal_top + weekday_font) {
        weekday_y = cal_top + weekday_font;
    }
    cairo_set_font_size(cr, weekday_font);
    draw_centered_text(cr,
                       weekday_labels[(current_local_tm.tm_wday + 7) % 7],
                       center_x, weekday_y,
                       NULL, NULL);

    double month_font = weekday_font;
    double month_y = day_y + (day_ext.height / 2.0) + (month_font / 2.0) + spacing;
    double month_limit = cal_top + cal_height - month_font;
    if (month_y > month_limit) {
        month_y = month_limit;
    }
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_ITALIC, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, month_font);
    draw_centered_text(cr,
                       month_labels[current_local_tm.tm_mon % 12],
                       center_x, month_y,
                       NULL, NULL);


    cairo_surface_flush(cs);
}

/* Resize */
static void handle_configure(XConfigureEvent *cev)
{
    if (cev->width > 0)  win_w = cev->width;
    if (cev->height > 0) win_h = cev->height;
    force_full_redraw = true;
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

    XtAppContext app;
    Widget toplevel;

    /* Initialize Xt/Motif – this sets up per-display info for XmGetColors */
    toplevel = XtAppInitialize(
        &app,
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

    top_widget = toplevel;
    dpy = XtDisplay(toplevel);
    screen = DefaultScreen(dpy);

    XtRealizeWidget(toplevel);
    win = XtWindow(toplevel);

    /* Set WM_CLASS: normal embedding vs debug */
    XClassHint class_hint;
    if (no_embed) {
        class_hint.res_name  = (char *)"ck-clock-debug";
        class_hint.res_class = (char *)"CKClockDebug";
    } else {
        class_hint.res_name  = (char *)"ck-clock";
        class_hint.res_class = (char *)"CKClock";
    }
    XSetClassHint(dpy, win, &class_hint);

    XSizeHints sh;
    sh.flags      = PSize;
    sh.width      = win_w;
    sh.height     = win_h;
    XSetWMNormalHints(dpy, win, &sh);

    XStoreName(dpy, win, no_embed ? "ck-clock (debug)" : "ck-clock");

    Atom wm_delete_atom = XmInternAtom(dpy, "WM_DELETE_WINDOW", False);
    /* let dtwm send WM_DELETE_WINDOW so we can exit when the panel restarts */
    XSetWMProtocols(dpy, win, &wm_delete_atom, 1);

    XSelectInput(dpy, win,
                 ExposureMask |
                 StructureNotifyMask);

    update_time_if_needed();

    int running = 1;
    int xfd = ConnectionNumber(dpy);
    fd_set rfds;
    struct timeval tv;

    while (running) {
        FD_ZERO(&rfds);
        FD_SET(xfd, &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sel = select(xfd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) {
                    draw_clock();
                }
                break;
            case ConfigureNotify:
                handle_configure(&ev.xconfigure);
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.message_type == wm_delete_atom) {
                    running = 0;
                }
                break;
            default:
                break;
            }
        }

        if (!running) {
            break;
        }

        update_time_if_needed();
    }

    /* Not normally reached */
    if (cr) cairo_destroy(cr);
    if (cs) cairo_surface_destroy(cs);
    XDestroyWindow(dpy, win);
    XtCloseDisplay(dpy);
    return 0;
}
