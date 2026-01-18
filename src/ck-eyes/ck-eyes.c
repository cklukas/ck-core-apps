/*
 * ck-eyes.c
 *
 * Simple Motif "xeyes" style app. Eyes track the mouse cursor.
 * When iconified, updates the icon pixmap with the same tracking.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <math.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#include <Xm/Xm.h>
#include <Xm/DrawingA.h>
#include <Xm/Protocols.h>
#include <Dt/Session.h>

#include "../shared/session_utils.h"

#define UPDATE_INTERVAL_MS 50
#define ICON_SIZE_FALLBACK 64

static XtAppContext app_context;
static Widget g_toplevel = NULL;
static Widget g_draw = NULL;
static Display *g_display = NULL;
static GC g_gc = None;
static Pixmap g_icon_pixmap = None;
static Pixmap g_icon_mask = None;
static GC g_icon_gc = None;
static GC g_icon_mask_gc = None;
static int g_iconified = 0;
static int g_logged_missing_icon_geom = 0;
static int g_logged_pointer_fail = 0;
static int g_logged_using_screen_mapping = 0;
static int g_icon_w = ICON_SIZE_FALLBACK;
static int g_icon_h = ICON_SIZE_FALLBACK;
static int g_eye_style = 1;

static char g_exec_path[PATH_MAX] = "ck-eyes";
static SessionData *session_data = NULL;

static void init_exec_path(const char *argv0)
{
    ssize_t len = readlink("/proc/self/exe", g_exec_path,
                           sizeof(g_exec_path) - 1);
    if (len > 0) {
        g_exec_path[len] = '\0';
        return;
    }

    if (argv0 && argv0[0]) {
        if (argv0[0] == '/') {
            strncpy(g_exec_path, argv0, sizeof(g_exec_path) - 1);
            g_exec_path[sizeof(g_exec_path) - 1] = '\0';
            return;
        }

        if (strchr(argv0, '/')) {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd))) {
                size_t cwd_len = strlen(cwd);
                size_t argv_len = strlen(argv0);
                size_t needed = cwd_len + 1 + argv_len + 1;
                if (needed <= sizeof(g_exec_path)) {
                    memcpy(g_exec_path, cwd, cwd_len);
                    g_exec_path[cwd_len] = '/';
                    memcpy(g_exec_path + cwd_len + 1, argv0, argv_len);
                    g_exec_path[cwd_len + 1 + argv_len] = '\0';
                    return;
                }
            }
        }

        strncpy(g_exec_path, argv0, sizeof(g_exec_path) - 1);
        g_exec_path[sizeof(g_exec_path) - 1] = '\0';
    }
}

static void session_save_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    (void)call_data;
    if (!session_data) return;

    session_capture_geometry(w, session_data, "x", "y", "w", "h");
    session_save(w, session_data, g_exec_path);
}

static int window_is_iconified(void)
{
    if (!g_toplevel || !XtIsRealized(g_toplevel)) return 0;
    Display *display = XtDisplay(g_toplevel);
    if (!display) return 0;
    Window window = XtWindow(g_toplevel);
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

static int window_is_viewable(void)
{
    if (!g_toplevel || !XtIsRealized(g_toplevel)) return 0;
    Display *display = XtDisplay(g_toplevel);
    if (!display) return 0;
    Window window = XtWindow(g_toplevel);
    if (!window) return 0;
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(display, window, &attrs)) return 0;
    return (attrs.map_state == IsViewable);
}

static void ensure_gc(Display *display, Drawable drawable, GC *gc_out)
{
    if (*gc_out != None) return;
    XGCValues values;
    values.foreground = BlackPixel(display, DefaultScreen(display));
    values.background = WhitePixel(display, DefaultScreen(display));
    *gc_out = XCreateGC(display, drawable, GCForeground | GCBackground, &values);
    XSetLineAttributes(display, *gc_out, 1, LineSolid, CapRound, JoinRound);
}

static void draw_eye(Display *display, Drawable drawable, GC gc,
                     int cx, int cy, int radius,
                     int target_x, int target_y,
                     unsigned long bg, unsigned long fg,
                     unsigned long white)
{
    int pupil_radius = radius / 3;
    if (pupil_radius < 2) pupil_radius = 2;

    int dx = target_x - cx;
    int dy = target_y - cy;
    double dist = sqrt((double)dx * dx + (double)dy * dy);
    double max_offset = radius - pupil_radius - 2;
    double scale = 0.0;
    if (dist > 0.0) {
        scale = max_offset / dist;
        if (scale > 1.0) scale = 1.0;
    }

    int pupil_cx = cx + (int)(dx * scale);
    int pupil_cy = cy + (int)(dy * scale);

    XSetForeground(display, gc, white);
    XFillArc(display, drawable, gc,
             cx - radius, cy - radius,
             radius * 2, radius * 2,
             0, 360 * 64);

    XSetForeground(display, gc, fg);
    XDrawArc(display, drawable, gc,
             cx - radius, cy - radius,
             radius * 2, radius * 2,
             0, 360 * 64);

    XSetForeground(display, gc, fg);
    XFillArc(display, drawable, gc,
             pupil_cx - pupil_radius, pupil_cy - pupil_radius,
             pupil_radius * 2, pupil_radius * 2,
             0, 360 * 64);
}

static void draw_eyes(Display *display, Drawable drawable, GC gc,
                      int width, int height,
                      int target_x, int target_y,
                      unsigned long bg, unsigned long fg,
                      int style)
{
    unsigned long white = WhitePixel(display, DefaultScreen(display));
    XSetForeground(display, gc, bg);
    XFillRectangle(display, drawable, gc, 0, 0, width, height);

    int min_dim = (width < height ? width : height);
    int eye_radius = min_dim / 4;
    if (eye_radius < 6) eye_radius = 6;

    int center_y = height / 2;
    int left_cx = width / 2 - eye_radius;
    int right_cx = width / 2 + eye_radius;

    if (style == 0) {
        draw_eye(display, drawable, gc, left_cx, center_y, eye_radius,
                 target_x, target_y, bg, fg, white);
        draw_eye(display, drawable, gc, right_cx, center_y, eye_radius,
                 target_x, target_y, bg, fg, white);
        return;
    }

    int pad = (min_dim * 5) / 100;
    if (pad < 1) pad = 1;
    int gap = pad * 2;
    int avail_w = width - 2 * pad;
    int eye_h = height - 2 * pad;
    if (avail_w <= 0 || eye_h <= 0) return;
    int eye_w = (avail_w - gap) / 2;
    if (eye_w < 2) eye_w = 2;
    if (eye_w * 2 + gap > avail_w) {
        eye_w = (avail_w - gap) / 2;
        if (eye_w < 2) eye_w = 2;
    }
    if (eye_h < 2) eye_h = 2;

    int left_x = width / 2 - gap / 2 - eye_w;
    int right_x = width / 2 + gap / 2;
    int top_y = pad;

    double rx = eye_w / 2.0;
    double ry = eye_h / 2.0;
    int left_cx2 = left_x + (int)rx;
    int right_cx2 = right_x + (int)rx;
    int cy2 = top_y + (int)ry;

    int pupil_size = eye_w < eye_h ? eye_w : eye_h;
    pupil_size /= 3;
    if (pupil_size < 6) pupil_size = 6;

    int outline = (min_dim * 2) / 100;
    if (outline < 1) outline = 1;
    outline *= 2;

    double eff_rx = rx - outline - pupil_size / 2.0 - 1.0;
    double eff_ry = ry - outline - pupil_size / 2.0 - 1.0;
    if (eff_rx < 1.0) eff_rx = 1.0;
    if (eff_ry < 1.0) eff_ry = 1.0;

    double dx = target_x - left_cx2;
    double dy = target_y - cy2;
    double norm = (eff_rx > 0.0 && eff_ry > 0.0) ?
                  sqrt((dx * dx) / (eff_rx * eff_rx) + (dy * dy) / (eff_ry * eff_ry)) : 0.0;
    if (norm < 1.0) norm = 1.0;
    double scale = 1.0 / norm;

    int left_px = left_cx2 + (int)(dx * scale) - pupil_size / 2;
    int left_py = cy2 + (int)(dy * scale) - pupil_size / 2;

    dx = target_x - right_cx2;
    dy = target_y - cy2;
    norm = (eff_rx > 0.0 && eff_ry > 0.0) ?
           sqrt((dx * dx) / (eff_rx * eff_rx) + (dy * dy) / (eff_ry * eff_ry)) : 0.0;
    if (norm < 1.0) norm = 1.0;
    scale = 1.0 / norm;

    int right_px = right_cx2 + (int)(dx * scale) - pupil_size / 2;
    int right_py = cy2 + (int)(dy * scale) - pupil_size / 2;

    XSetLineAttributes(display, gc, outline, LineSolid, CapRound, JoinRound);
    XSetForeground(display, gc, white);
    XFillArc(display, drawable, gc, left_x, top_y, eye_w, eye_h, 0, 360 * 64);
    XFillArc(display, drawable, gc, right_x, top_y, eye_w, eye_h, 0, 360 * 64);
    XSetForeground(display, gc, fg);
    XDrawArc(display, drawable, gc, left_x, top_y, eye_w, eye_h, 0, 360 * 64);
    XDrawArc(display, drawable, gc, right_x, top_y, eye_w, eye_h, 0, 360 * 64);

    XFillArc(display, drawable, gc, left_px, left_py, pupil_size, pupil_size, 0, 360 * 64);
    XFillArc(display, drawable, gc, right_px, right_py, pupil_size, pupil_size, 0, 360 * 64);

    XSetLineAttributes(display, gc, 1, LineSolid, CapRound, JoinRound);
}

static void update_icon_size_from_wm(Display *display)
{
    if (!display) return;
    int count = 0;
    XIconSize *sizes = NULL;
    if (!XGetIconSizes(display, RootWindow(display, DefaultScreen(display)),
                       &sizes, &count) || !sizes || count <= 0) {
        if (sizes) XFree(sizes);
        return;
    }

    int best_w = ICON_SIZE_FALLBACK;
    int best_h = ICON_SIZE_FALLBACK;
    for (int i = 0; i < count; ++i) {
        int w = sizes[i].max_width > 0 ? sizes[i].max_width : sizes[i].min_width;
        int h = sizes[i].max_height > 0 ? sizes[i].max_height : sizes[i].min_height;
        if (w > best_w) best_w = w;
        if (h > best_h) best_h = h;
    }
    if (best_w > 0) g_icon_w = best_w;
    if (best_h > 0) g_icon_h = best_h;
    XFree(sizes);
}

static int get_icon_geometry(Display *display, Window window,
                             int *out_x, int *out_y, int *out_w, int *out_h)
{
    if (!display || !window) return 0;

    Atom geom_atom = XInternAtom(display, "_NET_WM_ICON_GEOMETRY", True);
    if (geom_atom != None) {
        Atom actual = None;
        int format = 0;
        unsigned long nitems = 0;
        unsigned long bytes_after = 0;
        unsigned char *data = NULL;
        int status = XGetWindowProperty(display, window, geom_atom, 0, 4, False,
                                        XA_CARDINAL, &actual, &format,
                                        &nitems, &bytes_after, &data);
        if (status == Success && data && format == 32 && nitems >= 4 &&
            actual == XA_CARDINAL) {
            unsigned long *vals = (unsigned long *)data;
            *out_x = (int)vals[0];
            *out_y = (int)vals[1];
            *out_w = (int)vals[2];
            *out_h = (int)vals[3];
            XFree(data);
            return 1;
        }
        if (data) XFree(data);
    }

    XWMHints *hints = XGetWMHints(display, window);
    if (hints) {
        if (hints->flags & IconWindowHint) {
            Window icon_win = hints->icon_window;
            if (icon_win != None) {
                XWindowAttributes attrs;
                if (XGetWindowAttributes(display, icon_win, &attrs)) {
                    Window root = RootWindow(display, DefaultScreen(display));
                    int rx = 0, ry = 0;
                    Window child = None;
                    if (XTranslateCoordinates(display, icon_win, root, 0, 0,
                                              &rx, &ry, &child)) {
                        *out_x = rx;
                        *out_y = ry;
                        *out_w = attrs.width;
                        *out_h = attrs.height;
                        XFree(hints);
                        return 1;
                    }
                }
            }
        }
        XFree(hints);
    }

    return 0;
}

static void update_wm_icon_pixmap(Display *display, Window window, Pixmap pixmap)
{
    if (!display || !window || !pixmap) return;
    XWMHints *hints = XGetWMHints(display, window);
    XWMHints local;
    if (hints) {
        local = *hints;
        XFree(hints);
    } else {
        memset(&local, 0, sizeof(local));
    }
    local.flags |= IconPixmapHint;
    local.icon_pixmap = pixmap;
    if (g_icon_mask != None) {
        local.flags |= IconMaskHint;
        local.icon_mask = g_icon_mask;
    }
    XSetWMHints(display, window, &local);
}

static void draw_window_eyes(int target_x, int target_y)
{
    if (!g_draw || !XtIsRealized(g_draw)) return;
    Display *display = XtDisplay(g_draw);
    Window window = XtWindow(g_draw);
    if (!display || !window) return;

    ensure_gc(display, window, &g_gc);

    Dimension w = 0, h = 0;
    Pixel bg = WhitePixel(display, DefaultScreen(display));
    Pixel fg = BlackPixel(display, DefaultScreen(display));
    XtVaGetValues(g_draw,
                  XmNwidth, &w,
                  XmNheight, &h,
                  XmNbackground, &bg,
                  XmNforeground, &fg,
                  NULL);

    draw_eyes(display, window, g_gc, (int)w, (int)h, target_x, target_y, bg, fg,
              g_eye_style);
}

static void update_icon_eyes(int target_x, int target_y, int icon_w, int icon_h)
{
    if (!g_toplevel || !XtIsRealized(g_toplevel)) return;
    Display *display = XtDisplay(g_toplevel);
    Window window = XtWindow(g_toplevel);
    if (!display || !window) return;

    ensure_gc(display, window, &g_icon_gc);

    int screen = DefaultScreen(display);
    Pixmap root = RootWindow(display, screen);
    if (icon_w < 1) icon_w = ICON_SIZE_FALLBACK;
    if (icon_h < 1) icon_h = ICON_SIZE_FALLBACK;

    if (g_icon_pixmap != None) {
        XFreePixmap(display, g_icon_pixmap);
        g_icon_pixmap = None;
    }
    if (g_icon_mask != None) {
        XFreePixmap(display, g_icon_mask);
        g_icon_mask = None;
    }
    g_icon_pixmap = XCreatePixmap(display, root, (unsigned int)icon_w,
                                  (unsigned int)icon_h,
                                  DefaultDepth(display, screen));
    g_icon_mask = XCreatePixmap(display, root, (unsigned int)icon_w,
                                (unsigned int)icon_h, 1);
    if (g_icon_pixmap == None) return;
    if (g_icon_mask == None) return;

    Pixel bg = WhitePixel(display, screen);
    Pixel fg = BlackPixel(display, screen);
    draw_eyes(display, g_icon_pixmap, g_icon_gc,
              icon_w, icon_h, target_x, target_y, bg, fg, g_eye_style);

    ensure_gc(display, g_icon_mask, &g_icon_mask_gc);
    XSetForeground(display, g_icon_mask_gc, 0);
    XFillRectangle(display, g_icon_mask, g_icon_mask_gc, 0, 0,
                   (unsigned int)icon_w, (unsigned int)icon_h);
    XSetForeground(display, g_icon_mask_gc, 1);
    if (g_eye_style == 0) {
        int eye_radius = (icon_w < icon_h ? icon_w : icon_h) / 4;
        if (eye_radius < 6) eye_radius = 6;
        int center_y = icon_h / 2;
        int left_cx = icon_w / 2 - eye_radius;
        int right_cx = icon_w / 2 + eye_radius;
        XFillArc(display, g_icon_mask, g_icon_mask_gc,
                 left_cx - eye_radius, center_y - eye_radius,
                 eye_radius * 2, eye_radius * 2, 0, 360 * 64);
        XFillArc(display, g_icon_mask, g_icon_mask_gc,
                 right_cx - eye_radius, center_y - eye_radius,
                 eye_radius * 2, eye_radius * 2, 0, 360 * 64);
    } else {
        int min_dim = icon_w < icon_h ? icon_w : icon_h;
        int pad = (min_dim * 5) / 100;
        if (pad < 1) pad = 1;
        int gap = pad * 2;
        int avail_w = icon_w - 2 * pad;
        int eye_h = icon_h - 2 * pad;
        if (avail_w <= 0 || eye_h <= 0) {
            eye_h = 2;
            avail_w = 2;
        }
        int eye_w = (avail_w - gap) / 2;
        if (eye_w < 2) eye_w = 2;
        if (eye_w * 2 + gap > avail_w) {
            eye_w = (avail_w - gap) / 2;
            if (eye_w < 2) eye_w = 2;
        }
        if (eye_h < 2) eye_h = 2;
        int left_x = icon_w / 2 - gap / 2 - eye_w;
        int right_x = icon_w / 2 + gap / 2;
        int top_y = pad;
        XFillArc(display, g_icon_mask, g_icon_mask_gc,
                 left_x, top_y, eye_w, eye_h, 0, 360 * 64);
        XFillArc(display, g_icon_mask, g_icon_mask_gc,
                 right_x, top_y, eye_w, eye_h, 0, 360 * 64);
    }
    XFlush(display);

    update_wm_icon_pixmap(display, window, g_icon_pixmap);
    XtVaSetValues(g_toplevel, XmNiconPixmap, g_icon_pixmap, NULL);
    XtVaSetValues(g_toplevel, XmNiconMask, g_icon_mask, NULL);
}

static void expose_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    XmDrawingAreaCallbackStruct *cbs = (XmDrawingAreaCallbackStruct *)call_data;
    if (cbs->reason != XmCR_EXPOSE) return;

    int win_x = 0, win_y = 0;
    Window root = RootWindow(g_display, DefaultScreen(g_display));
    Window child = None;
    XTranslateCoordinates(g_display, XtWindow(w), root, 0, 0, &win_x, &win_y, &child);

    int root_x = 0, root_y = 0;
    int win_rel_x = 0, win_rel_y = 0;
    unsigned int mask = 0;
    Window r_ret = None, w_ret = None;
    if (XQueryPointer(g_display, XtWindow(w), &r_ret, &w_ret,
                      &root_x, &root_y, &win_rel_x, &win_rel_y, &mask)) {
        draw_window_eyes(win_rel_x, win_rel_y);
    } else {
        draw_window_eyes(0, 0);
    }
}

static void input_event_handler(Widget w, XtPointer client_data,
                                XEvent *event, Boolean *cont)
{
    (void)w;
    (void)client_data;
    (void)cont;
    if (event && event->type == ButtonPress) {
        g_eye_style = (g_eye_style + 1) % 2;
    }
}

static void update_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)client_data;
    (void)id;

    if (!g_toplevel || !XtIsRealized(g_toplevel)) {
        XtAppAddTimeOut(app_context, UPDATE_INTERVAL_MS, update_cb, NULL);
        return;
    }

    g_iconified = window_is_iconified();
    if (!g_iconified && !window_is_viewable()) {
        g_iconified = 1;
    }

    int root_x = 0, root_y = 0;
    int win_rel_x = 0, win_rel_y = 0;
    unsigned int mask = 0;
    Window r_ret = None, w_ret = None;
    Window draw_window = XtWindow(g_draw);

    Window root = RootWindow(g_display, DefaultScreen(g_display));
    int pointer_ok = XQueryPointer(g_display, root, &r_ret, &w_ret,
                                   &root_x, &root_y, &win_rel_x, &win_rel_y, &mask);
    if (!pointer_ok && !g_logged_pointer_fail) {
        fprintf(stderr, "[ck-eyes] pointer query failed on root\n");
        g_logged_pointer_fail = 1;
    }
    if (pointer_ok) {
        if (!g_iconified) {
            int wx = 0, wy = 0;
            Window child = None;
            if (XTranslateCoordinates(g_display, draw_window, root, 0, 0, &wx, &wy, &child)) {
                draw_window_eyes(root_x - wx, root_y - wy);
            }
        }
    }

    if (g_iconified) {
        int icon_x = 0, icon_y = 0, geom_w = 0, geom_h = 0;
        if (get_icon_geometry(g_display, XtWindow(g_toplevel),
                              &icon_x, &icon_y, &geom_w, &geom_h)) {
            int target_x = g_icon_w / 2;
            int target_y = g_icon_h / 2;
            if (pointer_ok) {
                if (geom_w < 1) geom_w = 1;
                if (geom_h < 1) geom_h = 1;
                target_x = (root_x - icon_x) * g_icon_w / geom_w;
                target_y = (root_y - icon_y) * g_icon_h / geom_h;
            }
            update_icon_eyes(target_x, target_y, g_icon_w, g_icon_h);
        } else {
            if (!g_logged_missing_icon_geom) {
                fprintf(stderr, "[ck-eyes] icon geometry unavailable; using center\n");
                g_logged_missing_icon_geom = 1;
            }
            update_icon_size_from_wm(g_display);
            if (pointer_ok) {
                int sw = DisplayWidth(g_display, DefaultScreen(g_display));
                int sh = DisplayHeight(g_display, DefaultScreen(g_display));
                int target_x = sw > 0 ? (root_x * g_icon_w) / sw : (g_icon_w / 2);
                int target_y = sh > 0 ? (root_y * g_icon_h) / sh : (g_icon_h / 2);
                if (!g_logged_using_screen_mapping) {
                    fprintf(stderr, "[ck-eyes] icon geometry missing; mapping to screen\n");
                    g_logged_using_screen_mapping = 1;
                }
                update_icon_eyes(target_x, target_y, g_icon_w, g_icon_h);
            } else {
                update_icon_eyes(g_icon_w / 2, g_icon_h / 2, g_icon_w, g_icon_h);
            }
        }
    }

    XtAppAddTimeOut(app_context, UPDATE_INTERVAL_MS, update_cb, NULL);
}

static void wm_delete_callback(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    XtAppContext app = (XtAppContext)client_data;
    XtAppSetExitFlag(app);
}

int main(int argc, char *argv[])
{
    XtSetLanguageProc(NULL, NULL, NULL);

    g_toplevel = XtVaAppInitialize(
        &app_context,
        "CkEyes",
        NULL, 0,
        &argc, argv,
        NULL,
        NULL
    );

    g_display = XtDisplay(g_toplevel);
    update_icon_size_from_wm(g_display);

    char *session_id = session_parse_argument(&argc, argv);
    session_data = session_data_create(session_id);
    free(session_id);
    init_exec_path(argv[0]);

    g_draw = XtVaCreateManagedWidget(
        "drawingArea",
        xmDrawingAreaWidgetClass, g_toplevel,
        XmNwidth, 200,
        XmNheight, 120,
        NULL
    );

    XtAddCallback(g_draw, XmNexposeCallback, expose_cb, NULL);
    XtAddEventHandler(g_draw, ButtonPressMask, False, input_event_handler, NULL);

    XtVaSetValues(g_toplevel,
                  XmNtitle, "Eyes",
                  XmNiconName, "Eyes",
                  NULL);

    if (session_data && session_load(g_toplevel, session_data)) {
        session_apply_geometry(g_toplevel, session_data, "x", "y", "w", "h");
    }

    Atom wm_delete = XmInternAtom(g_display, "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(g_toplevel, wm_delete, wm_delete_callback, (XtPointer)app_context);
    XmActivateWMProtocol(g_toplevel, wm_delete);

    Atom wm_save = XmInternAtom(g_display, "WM_SAVE_YOURSELF", False);
    XmAddWMProtocolCallback(g_toplevel, wm_save, session_save_cb, NULL);
    XmActivateWMProtocol(g_toplevel, wm_save);

    XtRealizeWidget(g_toplevel);

    XtAppAddTimeOut(app_context, UPDATE_INTERVAL_MS, update_cb, NULL);

    XtAppMainLoop(app_context);
    return 0;
}
