/*
 * ck-clock.c - Simple analog clock for CDE front panel (TYPE client)
 *
 * Uses cairo on Xlib (backed by XRender) for anti-aliased drawing.
 * Colors are taken from a Motif color set derived via XmGetColors():
 *
 *   - background  : clock face fill
 *   - top shadow  : outer circle outline
 *   - bottom shad.: inner circle outline
 *   - select      : hour + minute hands
 *   - foreground  : tick marks, label, second hand
 *
 * Build on Devuan/Debian (you may need libcairo2-dev, libmotif-dev):
 *   gcc -O2 -Wall -o ck-clock ck-clock.c -lX11 -lcairo -lXm -lXt -lm
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Timezones to cycle through on click.
 * "" means: use system default (unset TZ).
 */
static const char *tz_list[] = {
    "",                 // local
    "UTC",
    "Europe/Berlin",
    "Asia/Shanghai",
    "America/New_York"
};
static const int tz_count = sizeof(tz_list) / sizeof(tz_list[0]);
static int tz_index = 0;

static Display *dpy = NULL;
static int screen;
static Window win;

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

/* if set by --no-embedd, we use a different WM_CLASS so dtwm won't embed */
static int no_embed = 0;

/* second hand mode */
enum {
    SEC_NONE   = 0,  /* no second hand */
    SEC_TICK   = 1,  /* jumps once per second */
    SEC_SMOOTH = 2   /* smooth, uses fractional seconds */
};
static int sec_mode = SEC_SMOOTH;

/* ----- Timezone helpers -------------------------------------------------- */

static void apply_timezone(void)
{
    if (tz_index < 0 || tz_index >= tz_count) {
        tz_index = 0;
    }
    const char *tz = tz_list[tz_index];
    if (tz[0] == '\0') {
        unsetenv("TZ");   /* system default */
    } else {
        setenv("TZ", tz, 1);
    }
    tzset();
}

static const char *current_tz_label(void)
{
    const char *tz = tz_list[tz_index];

    if (tz[0] == '\0') {
        return "LOCAL";
    }

    /* take only substring after last '/' */
    const char *part = tz;
    const char *slash = strrchr(tz, '/');
    if (slash && slash[1] != '\0') {
        part = slash + 1;
    }

    /* copy into a static buffer and replace '_' -> ' ' */
    static char buf[64];
    size_t n = strlen(part);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;

    for (size_t i = 0; i < n; i++) {
        buf[i] = (part[i] == '_') ? ' ' : part[i];
    }
    buf[n] = '\0';

    return buf;
}

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

        /* use background_pixel from parent window */
        bg_pixel = attrs.backing_pixel;
        if (bg_pixel == 0) {
            bg_pixel = WhitePixel(dpy, screen);
        }
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

/* Draw a filled “hand” as a small kite/triangle shape */
static void draw_hand_shape(double angle,
                            double length,
                            double width,
                            double back_len)
{
    int cx = win_w / 2;
    int cy = win_h / 2;

    /* direction from center to tip */
    double dx = sin(angle);
    double dy = -cos(angle);  /* minus: X coords grow downward */

    /* perpendicular for width */
    double px = -dy;
    double py = dx;

    double half_w = width / 2.0;

    /* tail behind center */
    double bx = cx - dx * back_len;
    double by = cy - dy * back_len;

    /* base left/right near center */
    double lx = cx + px * half_w;
    double ly = cy + py * half_w;
    double rx = cx - px * half_w;
    double ry = cy - py * half_w;

    /* tip */
    double tx = cx + dx * length;
    double ty = cy + dy * length;

    cairo_move_to(cr, bx, by);
    cairo_line_to(cr, lx, ly);
    cairo_line_to(cr, tx, ty);
    cairo_line_to(cr, rx, ry);
    cairo_close_path(cr);
    cairo_fill(cr);
}

/* ----- Drawing ----------------------------------------------------------- */

static void draw_clock(void)
{
    if (!dpy || !win) return;

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
    double ts_r, ts_g, ts_b;
    double bs_r, bs_g, bs_b;
    double fg_r, fg_g, fg_b;
    double sel_r, sel_g, sel_b;

    pixel_to_rgb(bg_pixel,  &bg_r,  &bg_g,  &bg_b);
    pixel_to_rgb(ts_pixel,  &ts_r,  &ts_g,  &ts_b);
    pixel_to_rgb(bs_pixel,  &bs_r,  &bs_g,  &bs_b);
    pixel_to_rgb(fg_pixel,  &fg_r,  &fg_g,  &fg_b);
    pixel_to_rgb(sel_pixel, &sel_r, &sel_g, &sel_b);

    int size   = (win_w < win_h ? win_w : win_h);
    int radius = size / 2 - 1;
    if (radius <= 4) {
        cairo_surface_flush(cs);
        return;
    }

    int cx = win_w / 2;
    int cy = win_h / 2;

    /* Clear our drawing area by filling with panel background color */
    cairo_set_source_rgb(cr, bg_r, bg_g, bg_b);
    cairo_rectangle(cr, 0, 0, win_w, win_h);
    cairo_fill(cr);

    /* Face */
    cairo_set_source_rgb(cr, bg_r * 0.95, bg_g * 0.95, bg_b * 0.95);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, radius - 0.5, 0, 2 * M_PI);
    cairo_fill(cr);

    /* Outer ring: top shadow */
    cairo_set_source_rgb(cr, ts_r, ts_g, ts_b);
    cairo_set_line_width(cr, 0.8);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, radius - 0.5, 0, 2 * M_PI);
    cairo_stroke(cr);

    /* Inner ring: bottom shadow */
    cairo_set_source_rgb(cr, bs_r, bs_g, bs_b);
    cairo_set_line_width(cr, 0.8);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, radius - 2.0, 0, 2 * M_PI);
    cairo_stroke(cr);

    /* Tick marks in fg */
    cairo_set_source_rgb(cr, fg_r, fg_g, fg_b);
    cairo_set_line_width(cr, 1.0);
    for (int i = 0; i < 12; ++i) {
        double angle = 2.0 * M_PI * (double)i / 12.0;
        double s = sin(angle);
        double c = cos(angle);

        double r1 = radius * 0.80;
        double r2 = radius * 0.95;

        double x1 = cx + s * r1;
        double y1 = cy - c * r1;
        double x2 = cx + s * r2;
        double y2 = cy - c * r2;

        cairo_move_to(cr, x1, y1);
        cairo_line_to(cr, x2, y2);
    }
    cairo_stroke(cr);

    /* Time with fractional seconds (for smooth mode) */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t tsec = tv.tv_sec;
    double frac = (sec_mode == SEC_SMOOTH) ? (tv.tv_usec / 1000000.0) : 0.0;

    struct tm lt;
    localtime_r(&tsec, &lt);

    int hour = lt.tm_hour;
    int min  = lt.tm_min;
    int sec  = lt.tm_sec;

    double sec_base = (double)sec + frac;

    double sec_angle  = 2.0 * M_PI * (sec_base / 60.0);
    double min_angle  = 2.0 * M_PI * ( (min + sec_base / 60.0) / 60.0 );
    double hour_angle = 2.0 * M_PI * ( ((hour % 12) + (min + sec_base / 60.0) / 60.0) / 12.0 );

    /* Timezone label in fg, if extra height */
    const char *label = current_tz_label();
    if (label && *label && win_h > size) {
        cairo_set_source_rgb(cr, fg_r, fg_g, fg_b);
        cairo_select_font_face(cr, "sans",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 6.0);

        cairo_text_extents_t ext;
        cairo_text_extents(cr, label, &ext);

        double text_x = cx - ext.width / 2.0 - ext.x_bearing;
        double text_y = win_h - 15;

        cairo_move_to(cr, text_x, text_y);
        cairo_show_text(cr, label);
    }

    /* Hour + minute hands in select color */
    cairo_set_source_rgb(cr, sel_r, sel_g, sel_b);
    cairo_set_source_rgb(cr, fg_r, fg_g, fg_b);
    cairo_set_line_width(cr, 1.0);

    double hour_len   = radius * 0.55;
    double hour_width = radius * 0.18;
    double hour_back  = radius * 0.12;
    draw_hand_shape(hour_angle, hour_len, hour_width, hour_back);

    double min_len   = radius * 0.85;
    double min_width = radius * 0.13;
    double min_back  = radius * 0.12;
    draw_hand_shape(min_angle, min_len, min_width, min_back);

    /* Second hand in fg (unless disabled) */
    if (sec_mode != SEC_NONE) {
        cairo_set_source_rgb(cr, fg_r, fg_g, fg_b);
        double sec_len = radius * 0.90;
        double sx = cx + sin(sec_angle) * sec_len;
        double sy = cy - cos(sec_angle) * sec_len;
        cairo_set_line_width(cr, 0.8);
        cairo_move_to(cr, cx, cy);
        cairo_line_to(cr, sx, sy);
        cairo_stroke(cr);
    }

    /* Center dot in select color */
    cairo_set_source_rgb(cr, sel_r, sel_g, sel_b);
    double center_r = (radius > 4) ? radius * 0.10 : 2.0;
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, center_r, 0, 2 * M_PI);
    cairo_fill(cr);

    cairo_surface_flush(cs);
}

/* Resize */
static void handle_configure(XConfigureEvent *cev)
{
    if (cev->width > 0)  win_w = cev->width;
    if (cev->height > 0) win_h = cev->height;
}

/* Left-click = cycle timezone */
static void handle_button(XButtonEvent *bev)
{
    if (bev->button == Button1) {
        tz_index = (tz_index + 1) % tz_count;
        apply_timezone();
        draw_clock();
    }
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
        } else if (strncmp(argv[i], "--seconds=", 10) == 0) {
            const char *val = argv[i] + 10;
            if (strcmp(val, "none") == 0 || strcmp(val, "off") == 0) {
                sec_mode = SEC_NONE;
            } else if (strcmp(val, "smooth") == 0) {
                sec_mode = SEC_SMOOTH;
            } else {
                sec_mode = SEC_TICK;
            }
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

    XSelectInput(dpy, win,
                 ExposureMask |
                 StructureNotifyMask |
                 ButtonPressMask);

    apply_timezone();

    for (;;) {
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
            case ButtonPress:
                handle_button(&ev.xbutton);
                break;
            default:
                break;
            }
        }

        draw_clock();
        usleep(200000);  /* 0.2s -> 5 FPS for smooth second hand */
    }

    /* Not normally reached */
    if (cr) cairo_destroy(cr);
    if (cs) cairo_surface_destroy(cs);
    XDestroyWindow(dpy, win);
    XtCloseDisplay(dpy);
    return 0;
}
