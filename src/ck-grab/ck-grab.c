/*
 * ck-grab.c - Simple screenshot capture UI shell for CK-Core.
 */

#include <Dt/Dt.h>
#include <Dt/Session.h>
#include <Xm/CascadeB.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/MainW.h>
#include <Xm/MessageB.h>
#include <Xm/FileSB.h>
#include <Xm/Protocols.h>
#include <Xm/PushB.h>
#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/Scale.h>
#include <Xm/SeparatoG.h>
#include <Xm/ToggleB.h>
#include <Xm/Xm.h>
#include <Xm/MwmUtil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/xpm.h>
#include <png.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../shared/about_dialog.h"
#include "../shared/config_utils.h"
#include "../shared/gridlayout/gridlayout.h"
#include "../shared/session_utils.h"
#include "ck-grab-camera.pm"

typedef enum {
    TARGET_FULL_SCREEN = 0,
    TARGET_WINDOW = 1
} GrabTarget;

typedef enum {
    FORMAT_PNG = 0,
    FORMAT_XPM = 1
} GrabFormat;

typedef struct {
    XtAppContext app;
    Widget toplevel;
    Widget mainw;
    Widget menubar;
    Widget work_form;
    Widget target_option;
    Widget target_item_full;
    Widget target_item_window;
    Widget delay_scale;
    Widget include_frame_toggle;
    Widget include_cursor_toggle;
    Widget hide_window_toggle;
    Widget about_shell;
    Widget save_dialog;
    Widget overwrite_dialog;
    Widget format_option;
    Widget format_item_png;
    Widget format_item_xpm;
    Widget create_button;
    Widget close_button;
    Widget grid_widget;
    Widget separator;
    SessionData *session_data;
    GrabTarget target;
    int delay_seconds;
    int include_frame;
    int include_cursor;
    int hide_window;
    GrabFormat format;
    char exec_path[PATH_MAX];
    XImage *capture_image;
    int capture_width;
    int capture_height;
    char *last_dir;
    char *pending_path;
    int shell_locked;
} GrabApp;

static GrabApp G = {0};

#define GRAB_SETTINGS_FILENAME "ck-grab.view"
#define KEY_TARGET "target"
#define KEY_DELAY "delay"
#define KEY_INCLUDE_FRAME "include_frame"
#define KEY_INCLUDE_CURSOR "include_cursor"
#define KEY_HIDE_WINDOW "hide_window"
#define KEY_FORMAT "format"
#define GRAB_PATHS_FILENAME "ck-grab.paths"
#define KEY_LAST_DIR "last_dir"

static XmString make_string(const char *text)
{
    return XmStringCreateLocalized((String)(text ? text : ""));
}

static void show_error_dialog(const char *title, const char *msg)
{
    if (!G.toplevel) return;

    Arg args[8];
    int n = 0;
    XtSetArg(args[n], XmNdialogStyle, XmDIALOG_FULL_APPLICATION_MODAL); n++;
    Widget dlg = XmCreateErrorDialog(G.toplevel, "error_dialog", args, n);

    Widget helpb = XmMessageBoxGetChild(dlg, XmDIALOG_HELP_BUTTON);
    if (helpb) XtUnmanageChild(helpb);

    XmString s = XmStringCreateLocalized((char *)msg);
    XtVaSetValues(dlg, XmNmessageString, s, NULL);
    XmStringFree(s);

    Widget shell = XtParent(dlg);
    XtVaSetValues(shell, XmNtitle, title ? title : "Error", XmNdeleteResponse, XmUNMAP, NULL);

    XtManageChild(dlg);
}

static void init_exec_path(const char *argv0)
{
    ssize_t len = readlink("/proc/self/exe", G.exec_path, sizeof(G.exec_path) - 1);
    if (len > 0) {
        G.exec_path[len] = '\0';
        return;
    }

    if (argv0 && argv0[0]) {
        if (argv0[0] == '/') {
            strncpy(G.exec_path, argv0, sizeof(G.exec_path) - 1);
            G.exec_path[sizeof(G.exec_path) - 1] = '\0';
            return;
        }

        if (strchr(argv0, '/')) {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd))) {
                size_t cwd_len = strlen(cwd);
                size_t argv_len = strlen(argv0);
                size_t needed = cwd_len + 1 + argv_len + 1;
                if (needed <= sizeof(G.exec_path)) {
                    memcpy(G.exec_path, cwd, cwd_len);
                    G.exec_path[cwd_len] = '/';
                    memcpy(G.exec_path + cwd_len + 1, argv0, argv_len);
                    G.exec_path[cwd_len + 1 + argv_len] = '\0';
                    return;
                }
            }
        }

        strncpy(G.exec_path, argv0, sizeof(G.exec_path) - 1);
        G.exec_path[sizeof(G.exec_path) - 1] = '\0';
    }
}

static void grab_init_settings_defaults(void)
{
    G.target = TARGET_FULL_SCREEN;
    G.delay_seconds = 0;
    G.include_frame = 1;
    G.include_cursor = 1;
    G.hide_window = 1;
    G.format = FORMAT_PNG;
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void grab_load_settings_from_config(void)
{
    G.target = (GrabTarget)clamp_int(config_read_int_map(GRAB_SETTINGS_FILENAME, KEY_TARGET, G.target), 0, 1);
    G.delay_seconds = clamp_int(config_read_int_map(GRAB_SETTINGS_FILENAME, KEY_DELAY, G.delay_seconds), 0, 10);
    G.include_frame = config_read_int_map(GRAB_SETTINGS_FILENAME, KEY_INCLUDE_FRAME, G.include_frame) ? 1 : 0;
    G.include_cursor = config_read_int_map(GRAB_SETTINGS_FILENAME, KEY_INCLUDE_CURSOR, G.include_cursor) ? 1 : 0;
    G.hide_window = config_read_int_map(GRAB_SETTINGS_FILENAME, KEY_HIDE_WINDOW, G.hide_window) ? 1 : 0;
    G.format = (GrabFormat)clamp_int(config_read_int_map(GRAB_SETTINGS_FILENAME, KEY_FORMAT, G.format), 0, 1);
}

static void grab_apply_session_settings(void)
{
    if (!G.session_data) return;
    if (session_data_has(G.session_data, KEY_TARGET)) {
        G.target = (GrabTarget)clamp_int(session_data_get_int(G.session_data, KEY_TARGET, G.target), 0, 1);
    }
    if (session_data_has(G.session_data, KEY_DELAY)) {
        G.delay_seconds = clamp_int(session_data_get_int(G.session_data, KEY_DELAY, G.delay_seconds), 0, 10);
    }
    if (session_data_has(G.session_data, KEY_INCLUDE_FRAME)) {
        G.include_frame = session_data_get_int(G.session_data, KEY_INCLUDE_FRAME, G.include_frame) ? 1 : 0;
    }
    if (session_data_has(G.session_data, KEY_INCLUDE_CURSOR)) {
        G.include_cursor = session_data_get_int(G.session_data, KEY_INCLUDE_CURSOR, G.include_cursor) ? 1 : 0;
    }
    if (session_data_has(G.session_data, KEY_HIDE_WINDOW)) {
        G.hide_window = session_data_get_int(G.session_data, KEY_HIDE_WINDOW, G.hide_window) ? 1 : 0;
    }
    if (session_data_has(G.session_data, KEY_FORMAT)) {
        G.format = (GrabFormat)clamp_int(session_data_get_int(G.session_data, KEY_FORMAT, G.format), 0, 1);
    }
}

static void grab_sync_settings_to_session(void)
{
    if (!G.session_data) return;
    session_data_set_int(G.session_data, KEY_TARGET, G.target);
    session_data_set_int(G.session_data, KEY_DELAY, G.delay_seconds);
    session_data_set_int(G.session_data, KEY_INCLUDE_FRAME, G.include_frame);
    session_data_set_int(G.session_data, KEY_INCLUDE_CURSOR, G.include_cursor);
    session_data_set_int(G.session_data, KEY_HIDE_WINDOW, G.hide_window);
    session_data_set_int(G.session_data, KEY_FORMAT, G.format);
}

static void save_setting_int(const char *key, int value)
{
    config_write_int_map(GRAB_SETTINGS_FILENAME, key, value);
    if (G.session_data) {
        session_data_set_int(G.session_data, key, value);
    }
}

static void free_capture_image(void)
{
    if (G.capture_image) {
        XDestroyImage(G.capture_image);
        G.capture_image = NULL;
    }
    G.capture_width = 0;
    G.capture_height = 0;
}

static int dir_exists(const char *path)
{
    if (!path || !path[0]) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

static void set_last_dir(const char *path)
{
    if (!path || !path[0]) return;
    if (!dir_exists(path)) return;
    free(G.last_dir);
    G.last_dir = strdup(path);
    if (G.last_dir) {
        config_write_string(GRAB_PATHS_FILENAME, KEY_LAST_DIR, G.last_dir);
    }
}

static int file_exists(const char *path)
{
    if (!path || !path[0]) return 0;
    return access(path, F_OK) == 0;
}

static void init_last_dir(void)
{
    free(G.last_dir);
    G.last_dir = NULL;

    char *saved = config_read_string(GRAB_PATHS_FILENAME, KEY_LAST_DIR, NULL);
    if (saved && dir_exists(saved)) {
        G.last_dir = saved;
        return;
    }
    if (saved) free(saved);

    const char *home = getenv("HOME");
    if (home && dir_exists(home)) {
        G.last_dir = strdup(home);
    }
}

static char *extract_directory(const char *path)
{
    if (!path || !path[0]) return NULL;
    const char *slash = strrchr(path, '/');
    if (!slash) return NULL;
    if (slash == path) {
        return strdup("/");
    }
    size_t len = (size_t)(slash - path);
    char *dir = (char *)malloc(len + 1);
    if (!dir) return NULL;
    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

typedef struct {
    unsigned long rmask;
    unsigned long gmask;
    unsigned long bmask;
    int rshift;
    int gshift;
    int bshift;
    int rbits;
    int gbits;
    int bbits;
} PixelFormat;

static int count_bits(unsigned long v)
{
    int count = 0;
    while (v) {
        count += (int)(v & 1UL);
        v >>= 1;
    }
    return count;
}

static int lowest_bit(unsigned long v)
{
    int shift = 0;
    if (!v) return 0;
    while ((v & 1UL) == 0) {
        v >>= 1;
        shift++;
    }
    return shift;
}

static PixelFormat get_pixel_format(const XImage *img)
{
    PixelFormat fmt;
    memset(&fmt, 0, sizeof(fmt));
    if (!img) return fmt;
    fmt.rmask = img->red_mask;
    fmt.gmask = img->green_mask;
    fmt.bmask = img->blue_mask;
    fmt.rshift = lowest_bit(fmt.rmask);
    fmt.gshift = lowest_bit(fmt.gmask);
    fmt.bshift = lowest_bit(fmt.bmask);
    fmt.rbits = count_bits(fmt.rmask);
    fmt.gbits = count_bits(fmt.gmask);
    fmt.bbits = count_bits(fmt.bmask);
    return fmt;
}

static unsigned char scale_to_8(unsigned long v, int bits)
{
    if (bits <= 0) return 0;
    unsigned long max = (1UL << bits) - 1UL;
    return (unsigned char)((v * 255UL) / max);
}

static unsigned long scale_from_8(unsigned char v, int bits)
{
    if (bits <= 0) return 0;
    unsigned long max = (1UL << bits) - 1UL;
    return (unsigned long)((v * max + 127U) / 255U);
}

static void pixel_to_rgb(const XImage *img, const PixelFormat *fmt, unsigned long pixel,
                         unsigned char *r, unsigned char *g, unsigned char *b)
{
    if (!img || !fmt || !r || !g || !b) return;
    unsigned long rv = (pixel & fmt->rmask) >> fmt->rshift;
    unsigned long gv = (pixel & fmt->gmask) >> fmt->gshift;
    unsigned long bv = (pixel & fmt->bmask) >> fmt->bshift;
    *r = scale_to_8(rv, fmt->rbits);
    *g = scale_to_8(gv, fmt->gbits);
    *b = scale_to_8(bv, fmt->bbits);
}

static unsigned long rgb_to_pixel(const PixelFormat *fmt,
                                  unsigned char r, unsigned char g, unsigned char b)
{
    unsigned long rv = scale_from_8(r, fmt->rbits) << fmt->rshift;
    unsigned long gv = scale_from_8(g, fmt->gbits) << fmt->gshift;
    unsigned long bv = scale_from_8(b, fmt->bbits) << fmt->bshift;
    return (rv & fmt->rmask) | (gv & fmt->gmask) | (bv & fmt->bmask);
}

static Window get_active_window(Display *dpy)
{
    if (!dpy) return None;
    Window root = DefaultRootWindow(dpy);
    Atom net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True);
    if (net_active != None) {
        Atom actual = None;
        int format = 0;
        unsigned long nitems = 0;
        unsigned long bytes_after = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, root, net_active, 0, 1, False,
                               AnyPropertyType, &actual, &format,
                               &nitems, &bytes_after, &data) == Success) {
            if (data) {
                if (nitems >= 1) {
                    Window w = *(Window *)data;
                    XFree(data);
                    if (w != None) return w;
                } else {
                    XFree(data);
                }
            }
        }
    }
    Window focus = None;
    int revert = 0;
    XGetInputFocus(dpy, &focus, &revert);
    return focus;
}

static Window resolve_capture_window(Display *dpy, Window base, int include_frame)
{
    if (!dpy || base == None) return None;
    if (!include_frame) return base;
    Window root = None;
    Window parent = None;
    Window *children = NULL;
    unsigned int count = 0;
    Window current = base;
    Window last = base;

    while (current != None) {
        if (!XQueryTree(dpy, current, &root, &parent, &children, &count)) {
            break;
        }
        if (children) XFree(children);
        if (parent == None || parent == root) {
            break;
        }
        last = parent;
        current = parent;
    }

    return last;
}

static int capture_window_image(Display *dpy, Window win, XImage **out_image,
                                int *out_w, int *out_h)
{
    if (!dpy || win == None || !out_image) return 0;
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(dpy, win, &attrs)) return 0;
    if (attrs.map_state != IsViewable) return 0;
    int w = attrs.width;
    int h = attrs.height;
    if (w <= 0 || h <= 0) return 0;

    XImage *img = XGetImage(dpy, win, 0, 0, (unsigned)w, (unsigned)h, AllPlanes, ZPixmap);
    if (!img) return 0;
    *out_image = img;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return 1;
}

static void overlay_cursor_on_image(Display *dpy, XImage *img, Window win)
{
    if (!dpy || !img || !G.include_cursor) return;
    XFixesCursorImage *cursor = XFixesGetCursorImage(dpy);
    if (!cursor) return;

    Window root = DefaultRootWindow(dpy);
    int wx = 0;
    int wy = 0;
    Window child = None;
    XTranslateCoordinates(dpy, win, root, 0, 0, &wx, &wy, &child);

    int cursor_x = (int)cursor->x - (int)cursor->xhot - wx;
    int cursor_y = (int)cursor->y - (int)cursor->yhot - wy;

    PixelFormat fmt = get_pixel_format(img);
    for (int cy = 0; cy < (int)cursor->height; ++cy) {
        int iy = cursor_y + cy;
        if (iy < 0 || iy >= img->height) continue;
        for (int cx = 0; cx < (int)cursor->width; ++cx) {
            int ix = cursor_x + cx;
            if (ix < 0 || ix >= img->width) continue;

            unsigned long argb = cursor->pixels[cy * cursor->width + cx];
            unsigned char a = (unsigned char)((argb >> 24) & 0xFF);
            if (a == 0) continue;
            unsigned char cr = (unsigned char)((argb >> 16) & 0xFF);
            unsigned char cg = (unsigned char)((argb >> 8) & 0xFF);
            unsigned char cb = (unsigned char)(argb & 0xFF);

            unsigned long dst_pixel = XGetPixel(img, ix, iy);
            unsigned char dr, dg, db;
            pixel_to_rgb(img, &fmt, dst_pixel, &dr, &dg, &db);

            unsigned char nr = (unsigned char)((a * cr + (255 - a) * dr) / 255);
            unsigned char ng = (unsigned char)((a * cg + (255 - a) * dg) / 255);
            unsigned char nb = (unsigned char)((a * cb + (255 - a) * db) / 255);
            unsigned long out = rgb_to_pixel(&fmt, nr, ng, nb);
            XPutPixel(img, ix, iy, out);
        }
    }

    XFree(cursor);
}

static int capture_current_image(void)
{
    Display *dpy = XtDisplay(G.toplevel);
    if (!dpy) return 0;

    free_capture_image();

    Window target = None;
    if (G.target == TARGET_FULL_SCREEN) {
        target = DefaultRootWindow(dpy);
    } else {
        Window active = get_active_window(dpy);
        if (active == None || active == PointerRoot) {
            return 0;
        }
        target = resolve_capture_window(dpy, active, G.include_frame);
    }

    if (!capture_window_image(dpy, target, &G.capture_image, &G.capture_width, &G.capture_height)) {
        return 0;
    }

    overlay_cursor_on_image(dpy, G.capture_image, target);
    return 1;
}

static int write_png_file(const char *path, XImage *img)
{
    if (!path || !img) return 0;
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        fclose(fp);
        return 0;
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, NULL);
        fclose(fp);
        return 0;
    }
    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(fp);
        return 0;
    }

    png_init_io(png_ptr, fp);
    png_set_IHDR(png_ptr, info_ptr,
                 (png_uint_32)img->width, (png_uint_32)img->height,
                 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_write_info(png_ptr, info_ptr);

    PixelFormat fmt = get_pixel_format(img);
    png_bytep row = (png_bytep)malloc((size_t)img->width * 4);
    if (!row) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(fp);
        return 0;
    }

    for (int y = 0; y < img->height; ++y) {
        png_bytep p = row;
        for (int x = 0; x < img->width; ++x) {
            unsigned long pix = XGetPixel(img, x, y);
            unsigned char r, g, b;
            pixel_to_rgb(img, &fmt, pix, &r, &g, &b);
            *p++ = r;
            *p++ = g;
            *p++ = b;
            *p++ = 255;
        }
        png_write_row(png_ptr, row);
    }
    free(row);

    png_write_end(png_ptr, NULL);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose(fp);
    return 1;
}

static int write_xpm_file(const char *path, XImage *img)
{
    if (!path || !img) return 0;
    Display *dpy = XtDisplay(G.toplevel);
    if (!dpy) return 0;
    return XpmWriteFileFromImage(dpy, (char *)path, img, NULL, NULL) == XpmSuccess;
}

static const char *format_extension(GrabFormat fmt)
{
    return (fmt == FORMAT_XPM) ? ".xpm" : ".png";
}

static char *ensure_extension(const char *path, GrabFormat fmt)
{
    if (!path) return NULL;
    const char *ext = format_extension(fmt);
    size_t len = strlen(path);
    size_t ext_len = strlen(ext);
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(path, '.');
    if (dot && (!slash || dot > slash)) {
        return strdup(path);
    }
    size_t out_len = len + ext_len + 1;
    char *out = (char *)malloc(out_len);
    if (!out) return NULL;
    memcpy(out, path, len);
    memcpy(out + len, ext, ext_len + 1);
    return out;
}

static void update_save_dialog_pattern(void)
{
    if (!G.save_dialog) return;
    const char *pattern = (G.format == FORMAT_XPM) ? "*.xpm" : "*.png";
    XmString xm_pattern = make_string(pattern);
    XtVaSetValues(G.save_dialog, XmNpattern, xm_pattern, NULL);
    XmStringFree(xm_pattern);
}

static void on_format_select(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)call;
    GrabFormat fmt = (GrabFormat)(intptr_t)client;
    G.format = fmt;
    save_setting_int(KEY_FORMAT, G.format);
    if (G.format_option) {
        XtVaSetValues(G.format_option, XmNmenuHistory,
                      (fmt == FORMAT_XPM) ? G.format_item_xpm : G.format_item_png, NULL);
    }
    update_save_dialog_pattern();
}

static void close_save_dialog(void)
{
    if (!G.save_dialog) return;
    XtUnmanageChild(G.save_dialog);
}

static void close_overwrite_dialog(void)
{
    if (!G.overwrite_dialog) return;
    XtUnmanageChild(G.overwrite_dialog);
}

static void save_pending_path_now(void)
{
    if (!G.pending_path || !G.capture_image) return;
    int ok = 0;
    if (G.format == FORMAT_XPM) {
        ok = write_xpm_file(G.pending_path, G.capture_image);
    } else {
        ok = write_png_file(G.pending_path, G.capture_image);
    }
    if (!ok) {
        show_error_dialog("Save Error", "Failed to save the screenshot.");
        return;
    }
    char *dir = extract_directory(G.pending_path);
    if (dir) {
        set_last_dir(dir);
        free(dir);
    }
    close_save_dialog();
    free_capture_image();
    free(G.pending_path);
    G.pending_path = NULL;
}

static void on_overwrite_yes(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    (void)call;
    close_overwrite_dialog();
    save_pending_path_now();
}

static void on_overwrite_no(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    (void)call;
    close_overwrite_dialog();
    free(G.pending_path);
    G.pending_path = NULL;
    /* Keep save dialog open for a new filename. */
}

static void on_overwrite_cancel(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    (void)call;
    close_overwrite_dialog();
    free(G.pending_path);
    G.pending_path = NULL;
    close_save_dialog();
    free_capture_image();
}

static void show_overwrite_dialog(void)
{
    if (!G.overwrite_dialog) {
        Arg args[8];
        int n = 0;
        XtSetArg(args[n], XmNdialogStyle, XmDIALOG_FULL_APPLICATION_MODAL); n++;
        G.overwrite_dialog = XmCreateQuestionDialog(G.toplevel, "overwrite_dialog", args, n);

        Widget okb = XmMessageBoxGetChild(G.overwrite_dialog, XmDIALOG_OK_BUTTON);
        Widget cancelb = XmMessageBoxGetChild(G.overwrite_dialog, XmDIALOG_CANCEL_BUTTON);
        Widget helpb = XmMessageBoxGetChild(G.overwrite_dialog, XmDIALOG_HELP_BUTTON);
        if (okb) {
            XmString s = XmStringCreateLocalized("Yes");
            XtVaSetValues(okb, XmNlabelString, s, NULL);
            XmStringFree(s);
            XtAddCallback(okb, XmNactivateCallback, on_overwrite_yes, NULL);
        }
        if (cancelb) {
            XmString s = XmStringCreateLocalized("No");
            XtVaSetValues(cancelb, XmNlabelString, s, NULL);
            XmStringFree(s);
        }
        if (helpb) {
            XmString s = XmStringCreateLocalized("Cancel");
            XtVaSetValues(helpb, XmNlabelString, s, NULL);
            XmStringFree(s);
            XtManageChild(helpb);
        }

        XmString s_msg = XmStringCreateLocalized("File exists. Overwrite?");
        XtVaSetValues(G.overwrite_dialog, XmNmessageString, s_msg, NULL);
        XmStringFree(s_msg);

        Widget shell = XtParent(G.overwrite_dialog);
        XtVaSetValues(shell, XmNtitle, "Overwrite File?", XmNdeleteResponse, XmUNMAP, NULL);

        XtAddCallback(G.overwrite_dialog, XmNcancelCallback, on_overwrite_no, NULL);
        XtAddCallback(G.overwrite_dialog, XmNhelpCallback, on_overwrite_cancel, NULL);
    }
    XtManageChild(G.overwrite_dialog);
}

static void on_save_dialog_ok(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    XmFileSelectionBoxCallbackStruct *cb = (XmFileSelectionBoxCallbackStruct *)call;
    if (!cb || !G.capture_image) {
        close_save_dialog();
        free_capture_image();
        return;
    }
    char *raw_path = NULL;
    if (cb->value) {
        XmStringGetLtoR(cb->value, XmSTRING_DEFAULT_CHARSET, &raw_path);
    }
    if (!raw_path) {
        close_save_dialog();
        free_capture_image();
        return;
    }

    char *path = ensure_extension(raw_path, G.format);
    XtFree(raw_path);
    if (!path) {
        show_error_dialog("Save Error", "Unable to determine output path.");
        return;
    }

    free(G.pending_path);
    G.pending_path = path;
    if (file_exists(G.pending_path)) {
        show_overwrite_dialog();
        return;
    }
    save_pending_path_now();
}

static void on_save_dialog_cancel(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    (void)call;
    close_save_dialog();
    free_capture_image();
}

static void show_save_dialog(void)
{
    if (!G.save_dialog) {
        G.save_dialog = XmCreateFileSelectionDialog(G.toplevel, "saveDialog", NULL, 0);
        XtVaSetValues(G.save_dialog, XmNautoUnmanage, False, NULL);
        XtAddCallback(G.save_dialog, XmNokCallback, on_save_dialog_ok, NULL);
        XtAddCallback(G.save_dialog, XmNcancelCallback, on_save_dialog_cancel, NULL);
        XtAddCallback(G.save_dialog, XmNhelpCallback, on_save_dialog_cancel, NULL);
        XtVaSetValues(XtParent(G.save_dialog), XmNtitle, "Save Screenshot As...", NULL);

        Widget work = XmFileSelectionBoxGetChild(G.save_dialog, XmDIALOG_WORK_AREA);
        if (work) {
            Widget format_row = XmCreateForm(work, "formatRow", NULL, 0);
            XtVaSetValues(format_row,
                          XmNfractionBase, 100,
                          XmNleftAttachment, XmATTACH_FORM,
                          XmNrightAttachment, XmATTACH_FORM,
                          NULL);
            XtManageChild(format_row);

            XmString s_format = make_string("Format:");
            Widget format_label = XtVaCreateManagedWidget("formatLabel",
                                                          xmLabelWidgetClass, format_row,
                                                          XmNlabelString, s_format,
                                                          XmNleftAttachment, XmATTACH_FORM,
                                                          XmNalignment, XmALIGNMENT_BEGINNING,
                                                          NULL);
            XmStringFree(s_format);

            Widget format_pd = XmCreatePulldownMenu(format_row, "formatPD", NULL, 0);
            XmString s_png = make_string("PNG");
            G.format_item_png = XtVaCreateManagedWidget("formatPng",
                                                        xmPushButtonWidgetClass, format_pd,
                                                        XmNlabelString, s_png,
                                                        NULL);
            XmStringFree(s_png);
            XtAddCallback(G.format_item_png, XmNactivateCallback, on_format_select,
                          (XtPointer)(intptr_t)FORMAT_PNG);

            XmString s_xpm = make_string("XPM");
            G.format_item_xpm = XtVaCreateManagedWidget("formatXpm",
                                                        xmPushButtonWidgetClass, format_pd,
                                                        XmNlabelString, s_xpm,
                                                        NULL);
            XmStringFree(s_xpm);
            XtAddCallback(G.format_item_xpm, XmNactivateCallback, on_format_select,
                          (XtPointer)(intptr_t)FORMAT_XPM);

            Arg args[4];
            int n = 0;
            XtSetArg(args[n], XmNsubMenuId, format_pd); n++;
            XtSetArg(args[n], XmNmenuHistory,
                     (G.format == FORMAT_XPM) ? G.format_item_xpm : G.format_item_png); n++;
            G.format_option = XmCreateOptionMenu(format_row, "formatOption", args, n);
            XtVaSetValues(G.format_option,
                          XmNleftAttachment, XmATTACH_WIDGET,
                          XmNleftWidget, format_label,
                          XmNleftOffset, 8,
                          XmNrightAttachment, XmATTACH_FORM,
                          NULL);
            XtManageChild(G.format_option);
        }
    }

    if (G.last_dir && dir_exists(G.last_dir)) {
        XmString xm_dir = make_string(G.last_dir);
        XtVaSetValues(G.save_dialog, XmNdirectory, xm_dir, NULL);
        XmStringFree(xm_dir);
    }

    update_save_dialog_pattern();
    XtManageChild(G.save_dialog);
}

static void on_app_exit(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    (void)call;
    XtAppSetExitFlag(G.app);
}

static void about_destroy_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    (void)call;
    G.about_shell = NULL;
}

static void on_about(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    (void)call;

    if (G.about_shell && XtIsWidget(G.about_shell)) {
        XtPopup(G.about_shell, XtGrabNone);
        return;
    }

    Widget shell = NULL;
    Widget notebook = about_dialog_build(G.toplevel, "about_dialog", "About Grab", &shell);
    if (!notebook || !shell) return;

    about_add_standard_pages(notebook, 1,
                             "Grab",
                             "Grab - Screenshot Utility",
                             "Capture screenshots of the full screen or\na window with optional delay.",
                             True);

    XtVaSetValues(shell, XmNwidth, 640, XmNheight, 420, NULL);
    XtAddCallback(shell, XmNdestroyCallback, about_destroy_cb, NULL);

    G.about_shell = shell;
    XtPopup(shell, XtGrabNone);
}

static void update_frame_toggle_sensitivity(void)
{
    Boolean enable = (G.target == TARGET_WINDOW) ? True : False;
    if (G.include_frame_toggle) {
        XtSetSensitive(G.include_frame_toggle, enable);
    }
}

static void on_toggle_changed(Widget w, XtPointer client, XtPointer call)
{
    (void)call;
    const char *key = (const char *)client;
    int set = XmToggleButtonGetState(w) ? 1 : 0;
    if (strcmp(key, KEY_INCLUDE_FRAME) == 0) {
        G.include_frame = set;
    } else if (strcmp(key, KEY_INCLUDE_CURSOR) == 0) {
        G.include_cursor = set;
    } else if (strcmp(key, KEY_HIDE_WINDOW) == 0) {
        G.hide_window = set;
    }
    save_setting_int(key, set);
}

static void on_delay_changed(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    XmScaleCallbackStruct *cbs = (XmScaleCallbackStruct *)call;
    if (!cbs) return;
    G.delay_seconds = clamp_int(cbs->value, 0, 10);
    save_setting_int(KEY_DELAY, G.delay_seconds);
}

static void on_create_screenshot(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    (void)call;
    if (G.hide_window) {
        XtUnmapWidget(G.toplevel);
        XSync(XtDisplay(G.toplevel), False);
    }

    if (G.delay_seconds > 0) {
        sleep((unsigned int)G.delay_seconds);
    }

    if (!capture_current_image()) {
        if (G.hide_window) {
            XtMapWidget(G.toplevel);
            XSync(XtDisplay(G.toplevel), False);
        }
        show_error_dialog("Capture Error", "Failed to capture the requested window.");
        return;
    }

    if (G.hide_window) {
        XtMapWidget(G.toplevel);
        XSync(XtDisplay(G.toplevel), False);
    }

    show_save_dialog();
}

static void on_target_select(Widget w, XtPointer client, XtPointer call)
{
    (void)call;
    GrabTarget target = (GrabTarget)(intptr_t)client;
    G.target = target;

    if (G.target_option) {
        XtVaSetValues(G.target_option, XmNmenuHistory, w, NULL);
    }

    update_frame_toggle_sensitivity();
    save_setting_int(KEY_TARGET, G.target);
}

static void session_save_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)client;
    (void)call;
    if (!G.session_data) return;
    session_capture_geometry(w, G.session_data, "x", "y", "w", "h");
    session_save(w, G.session_data, G.exec_path);
}

static void wm_delete_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)call;
    XtAppContext app = (XtAppContext)client;
    XtAppSetExitFlag(app);
}

static void create_menu(Widget parent)
{
    Widget menubar = XmCreateMenuBar(parent, "menubar", NULL, 0);

    Widget file_pd = XmCreatePulldownMenu(menubar, "filePD", NULL, 0);
    XtVaCreateManagedWidget("File", xmCascadeButtonWidgetClass, menubar, XmNsubMenuId, file_pd, NULL);

    XmString s_acc = make_string("Alt+F4");
    Widget mi_exit = XtVaCreateManagedWidget("Exit",
                                             xmPushButtonWidgetClass, file_pd,
                                             XmNaccelerator, "Alt<Key>F4",
                                             XmNacceleratorText, s_acc,
                                             NULL);
    XmStringFree(s_acc);
    XtAddCallback(mi_exit, XmNactivateCallback, on_app_exit, NULL);

    Widget settings_pd = XmCreatePulldownMenu(menubar, "settingsPD", NULL, 0);
    XtVaCreateManagedWidget("Settings", xmCascadeButtonWidgetClass, menubar, XmNsubMenuId, settings_pd, NULL);

    XmString s_frame = make_string("Include Window Frame");
    G.include_frame_toggle = XtVaCreateManagedWidget("includeFrame",
                                                     xmToggleButtonWidgetClass, settings_pd,
                                                     XmNlabelString, s_frame,
                                                     XmNset, G.include_frame ? True : False,
                                                     NULL);
    XmStringFree(s_frame);
    XtAddCallback(G.include_frame_toggle, XmNvalueChangedCallback, on_toggle_changed,
                  (XtPointer)KEY_INCLUDE_FRAME);

    XmString s_cursor = make_string("Include Mouse Cursor");
    G.include_cursor_toggle = XtVaCreateManagedWidget("includeCursor",
                                                      xmToggleButtonWidgetClass, settings_pd,
                                                      XmNlabelString, s_cursor,
                                                      XmNset, G.include_cursor ? True : False,
                                                      NULL);
    XmStringFree(s_cursor);
    XtAddCallback(G.include_cursor_toggle, XmNvalueChangedCallback, on_toggle_changed,
                  (XtPointer)KEY_INCLUDE_CURSOR);

    XmString s_hide = make_string("Hide Grab Window");
    G.hide_window_toggle = XtVaCreateManagedWidget("hideGrabWindow",
                                                   xmToggleButtonWidgetClass, settings_pd,
                                                   XmNlabelString, s_hide,
                                                   XmNset, G.hide_window ? True : False,
                                                   NULL);
    XmStringFree(s_hide);
    XtAddCallback(G.hide_window_toggle, XmNvalueChangedCallback, on_toggle_changed,
                  (XtPointer)KEY_HIDE_WINDOW);

    Widget help_pd = XmCreatePulldownMenu(menubar, "helpPD", NULL, 0);
    Widget help_cas = XtVaCreateManagedWidget("Help", xmCascadeButtonWidgetClass, menubar, XmNsubMenuId, help_pd, NULL);
    XtVaSetValues(menubar, XmNmenuHelpWidget, help_cas, NULL);

    Widget mi_about = XtVaCreateManagedWidget("About", xmPushButtonWidgetClass, help_pd, NULL);
    XtAddCallback(mi_about, XmNactivateCallback, on_about, NULL);

    XtManageChild(menubar);
    G.menubar = menubar;
}

static void build_ui(void)
{
    G.mainw = XtVaCreateManagedWidget("mainw",
                                      xmMainWindowWidgetClass, G.toplevel,
                                      XmNshadowThickness, 0,
                                      XmNnoResize, True,
                                      XmNresizePolicy, XmRESIZE_NONE,
                                      NULL);

    create_menu(G.mainw);

    G.work_form = XtVaCreateManagedWidget("workForm",
                                          xmFormWidgetClass, G.mainw,
                                          XmNfractionBase, 100,
                                          XmNmarginWidth, 12,
                                          XmNmarginHeight, 12,
                                          NULL);

    GridLayout *layout = gridlayout_create(G.work_form, "grabGrid", 2);
    gridlayout_set_row_spacing(layout, 10);
    Widget grid = gridlayout_get_widget(layout);
    G.grid_widget = grid;
    XtVaSetValues(grid,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_NONE,
                  NULL);

    int row_target = gridlayout_add_row(layout);
    Widget row_form = gridlayout_get_row_form(layout, row_target);

    XmString s_target = make_string("Target:");
    Widget target_label = XmCreateLabel(row_form, "targetLabel", NULL, 0);
    XtVaSetValues(target_label,
                  XmNlabelString, s_target,
                  XmNalignment, XmALIGNMENT_BEGINNING,
                  NULL);
    XmStringFree(s_target);

    Widget target_pd = XmCreatePulldownMenu(G.work_form, "targetPD", NULL, 0);
    XmString s_full = make_string("Full Screen");
    G.target_item_full = XtVaCreateManagedWidget("targetFull",
                                                 xmPushButtonWidgetClass, target_pd,
                                                 XmNlabelString, s_full,
                                                 NULL);
    XmStringFree(s_full);
    XtAddCallback(G.target_item_full, XmNactivateCallback, on_target_select,
                  (XtPointer)(intptr_t)TARGET_FULL_SCREEN);

    XmString s_window = make_string("Active Window");
    G.target_item_window = XtVaCreateManagedWidget("targetWindow",
                                                   xmPushButtonWidgetClass, target_pd,
                                                   XmNlabelString, s_window,
                                                   NULL);
    XmStringFree(s_window);
    XtAddCallback(G.target_item_window, XmNactivateCallback, on_target_select,
                  (XtPointer)(intptr_t)TARGET_WINDOW);

    Arg opt_args[4];
    int n = 0;
    XtSetArg(opt_args[n], XmNsubMenuId, target_pd); n++;
    XtSetArg(opt_args[n], XmNmenuHistory,
             (G.target == TARGET_WINDOW) ? G.target_item_window : G.target_item_full); n++;
    G.target_option = XmCreateOptionMenu(row_form, "targetOption", opt_args, n);
    XtVaSetValues(G.target_option,
                  XmNmarginWidth, 0,
                  XmNmarginHeight, 0,
                  XmNmarginLeft, 0,
                  XmNmarginRight, 0,
                  XmNspacing, 0,
                  XmNborderWidth, 0,
                  NULL);
    Widget option_label = XmOptionLabelGadget(G.target_option);
    if (option_label) {
        XtUnmanageChild(option_label);
    }

    gridlayout_add_cell(layout, row_target, 0, target_label, 1);
    gridlayout_add_cell(layout, row_target, 1, G.target_option, 1);

    int row_delay = gridlayout_add_row(layout);
    row_form = gridlayout_get_row_form(layout, row_delay);

    XmString s_delay = make_string("Delay (s):");
    Widget delay_label = XmCreateLabel(row_form, "delayLabel", NULL, 0);
    XtVaSetValues(delay_label,
                  XmNlabelString, s_delay,
                  XmNalignment, XmALIGNMENT_BEGINNING,
                  NULL);
    XmStringFree(s_delay);

    G.delay_scale = XmCreateScale(row_form, "delayScale", NULL, 0);
    XtVaSetValues(G.delay_scale,
                  XmNorientation, XmHORIZONTAL,
                  XmNminimum, 0,
                  XmNmaximum, 10,
                  XmNvalue, G.delay_seconds,
                  XmNshowValue, True,
                  XmNscaleHeight, 24,
                  XmNheight, 54,
                  XmNmarginWidth, 0,
                  XmNmarginHeight, 0,
                  NULL);
    XtAddCallback(G.delay_scale, XmNvalueChangedCallback, on_delay_changed, NULL);
    XtAddCallback(G.delay_scale, XmNdragCallback, on_delay_changed, NULL);

    gridlayout_add_cell(layout, row_delay, 0, delay_label, 1);
    gridlayout_add_cell(layout, row_delay, 1, G.delay_scale, 1);

    Arg sep_args[8];
    int sn = 0;
    XtSetArg(sep_args[sn], XmNleftAttachment, XmATTACH_FORM); sn++;
    XtSetArg(sep_args[sn], XmNrightAttachment, XmATTACH_FORM); sn++;
    XtSetArg(sep_args[sn], XmNleftOffset, -12); sn++;
    XtSetArg(sep_args[sn], XmNrightOffset, -12); sn++;
    XtSetArg(sep_args[sn], XmNtopAttachment, XmATTACH_WIDGET); sn++;
    XtSetArg(sep_args[sn], XmNtopWidget, grid); sn++;
    XtSetArg(sep_args[sn], XmNtopOffset, 10); sn++;
    Widget separator = XmCreateSeparatorGadget(G.work_form, "buttonSeparator", sep_args, sn);
    XtManageChild(separator);
    G.separator = separator;

    XmString s_create = make_string("Create Screenshot");
    Widget create_btn = XmCreatePushButtonGadget(G.work_form, "createButton", NULL, 0);
    XtVaSetValues(create_btn,
                  XmNlabelString, s_create,
                  XmNmarginWidth, 12,
                  XmNmarginHeight, 6,
                  XmNleftAttachment, XmATTACH_POSITION,
                  XmNleftPosition, 2,
                  XmNrightAttachment, XmATTACH_POSITION,
                  XmNrightPosition, 48,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, separator,
                  XmNtopOffset, 10,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNbottomOffset, 6,
                  NULL);
    XmStringFree(s_create);
    XtAddCallback(create_btn, XmNactivateCallback, on_create_screenshot, NULL);
    XtManageChild(create_btn);
    G.create_button = create_btn;

    XmString s_close = make_string("Close");
    Widget close_btn = XmCreatePushButtonGadget(G.work_form, "closeButton", NULL, 0);
    XtVaSetValues(close_btn,
                  XmNlabelString, s_close,
                  XmNmarginWidth, 12,
                  XmNmarginHeight, 6,
                  XmNleftAttachment, XmATTACH_POSITION,
                  XmNleftPosition, 52,
                  XmNrightAttachment, XmATTACH_POSITION,
                  XmNrightPosition, 98,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, separator,
                  XmNtopOffset, 10,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNbottomOffset, 6,
                  NULL);
    XmStringFree(s_close);
    XtAddCallback(close_btn, XmNactivateCallback, on_app_exit, NULL);
    XtManageChild(close_btn);
    G.close_button = close_btn;

    XmMainWindowSetAreas(G.mainw, G.menubar, NULL, NULL, NULL, G.work_form);
    XtVaSetValues(G.work_form,
                  XmNdefaultButton, create_btn,
                  XmNcancelButton, close_btn,
                  NULL);

    update_frame_toggle_sensitivity();
}

static void apply_initial_geometry_from_layout(void)
{
    XtWidgetGeometry preferred;
    memset(&preferred, 0, sizeof(preferred));
    XtGeometryResult result = XtQueryGeometry(G.mainw, NULL, &preferred);
    if (result == XtGeometryYes || result == XtGeometryAlmost) {
        Dimension w = preferred.width;
        Dimension h = preferred.height;
        if (w > 0 && h > 0) {
            XtVaSetValues(G.toplevel, XmNwidth, w, XmNheight, h, NULL);
        }
    }
}

static void apply_wm_hints(void)
{
    if (!G.toplevel) return;
    unsigned int funcs = MWM_FUNC_ALL ^ (MWM_FUNC_RESIZE | MWM_FUNC_MAXIMIZE);
    XtVaSetValues(G.toplevel,
                  XmNmwmFunctions, funcs,
                  XmNallowShellResize, False,
                  NULL);
}

static void lock_shell_on_map(Widget w, XtPointer client, XEvent *event, Boolean *cont)
{
    (void)client;
    (void)cont;
    if (!event || event->type != MapNotify) return;
    if (G.shell_locked) return;
    Display *dpy = XtDisplay(w);
    Window win = XtWindow(w);
    if (!dpy || !win) return;

    Dimension w_width = 0;
    Dimension w_height = 0;
    XtVaGetValues(w, XmNwidth, &w_width, XmNheight, &w_height, NULL);

    Dimension mainw_h = 0;
    Dimension menu_h = 0;
    Dimension shell_h = w_height;
    Dimension chrome = 0;
    if (G.mainw) {
        XtVaGetValues(G.mainw, XmNheight, &mainw_h, NULL);
    }
    if (G.menubar) {
        XtVaGetValues(G.menubar, XmNheight, &menu_h, NULL);
    }
    if (shell_h > 0 && mainw_h > 0 && shell_h > mainw_h) {
        chrome = shell_h - mainw_h;
    }

    Dimension desired_shell_h = shell_h;
    if (G.work_form && G.grid_widget && G.separator && G.close_button) {
        XtWidgetGeometry pref;
        Dimension grid_h = 0;
        Dimension sep_h = 0;
        Dimension btn_h = 0;

        memset(&pref, 0, sizeof(pref));
        if (XtQueryGeometry(G.grid_widget, NULL, &pref) == XtGeometryYes && pref.height > 0) {
            grid_h = pref.height;
        } else {
            XtVaGetValues(G.grid_widget, XmNheight, &grid_h, NULL);
        }

        memset(&pref, 0, sizeof(pref));
        if (XtQueryGeometry(G.separator, NULL, &pref) == XtGeometryYes && pref.height > 0) {
            sep_h = pref.height;
        } else {
            XtVaGetValues(G.separator, XmNheight, &sep_h, NULL);
        }

        memset(&pref, 0, sizeof(pref));
        if (XtQueryGeometry(G.close_button, NULL, &pref) == XtGeometryYes && pref.height > 0) {
            btn_h = pref.height;
        } else {
            XtVaGetValues(G.close_button, XmNheight, &btn_h, NULL);
        }

        /* Match layout constants from build_ui */
        const int form_margin = 12;
        const int sep_top = 10;
        const int btn_top = 10;
        const int btn_bottom = 6;

        Dimension form_h = (Dimension)(form_margin * 2 +
                                       grid_h +
                                       sep_top + sep_h +
                                       btn_top + btn_h + btn_bottom);

        Dimension desired_mainw_h = form_h + menu_h;
        desired_shell_h = desired_mainw_h + chrome;

        fprintf(stderr,
                "[ck-grab] sizing: grid_h=%u sep_h=%u btn_h=%u menu_h=%u chrome=%u form_h=%u mainw_h=%u shell_h=%u desired_shell_h=%u\n",
                (unsigned)grid_h,
                (unsigned)sep_h,
                (unsigned)btn_h,
                (unsigned)menu_h,
                (unsigned)chrome,
                (unsigned)form_h,
                (unsigned)desired_mainw_h,
                (unsigned)shell_h,
                (unsigned)desired_shell_h);
    }

    if (desired_shell_h > 0 && desired_shell_h != shell_h) {
        XtVaSetValues(w, XmNheight, desired_shell_h, NULL);
        w_height = desired_shell_h;
    }

    XSizeHints hints;
    hints.min_width = (int)w_width;
    hints.min_height = (int)w_height;
    hints.max_width = hints.min_width;
    hints.max_height = hints.min_height;
    hints.flags = PMinSize | PMaxSize;
    XSetWMNormalHints(dpy, win, &hints);
    G.shell_locked = 1;
}

int main(int argc, char *argv[])
{
    int session_loaded = 0;
    XtSetLanguageProc(NULL, NULL, NULL);

    char *session_id = session_parse_argument(&argc, argv);
    G.session_data = session_data_create(session_id);
    free(session_id);
    init_exec_path(argv[0]);

    grab_init_settings_defaults();
    grab_load_settings_from_config();
    init_last_dir();

    G.toplevel = XtVaAppInitialize(&G.app, "CkGrab", NULL, 0, &argc, argv, NULL, NULL);
    DtInitialize(XtDisplay(G.toplevel), G.toplevel, "CkGrab", "CkGrab");
    XtVaSetValues(G.toplevel,
                  XmNtitle, "Grab",
                  XmNiconName, "Grab",
                  NULL);

    if (G.session_data && session_load(G.toplevel, G.session_data)) {
        session_loaded = 1;
        grab_apply_session_settings();
    }
    grab_sync_settings_to_session();

    build_ui();

    if (session_loaded && G.session_data) {
        session_apply_geometry(G.toplevel, G.session_data, "x", "y", "w", "h");
    } else {
        apply_initial_geometry_from_layout();
    }
    apply_wm_hints();
    XtAddEventHandler(G.toplevel, StructureNotifyMask, False, lock_shell_on_map, NULL);

    Atom wm_delete = XmInternAtom(XtDisplay(G.toplevel), "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(G.toplevel, wm_delete, wm_delete_cb, (XtPointer)G.app);
    XmActivateWMProtocol(G.toplevel, wm_delete);

    Atom wm_save = XmInternAtom(XtDisplay(G.toplevel), "WM_SAVE_YOURSELF", False);
    XmAddWMProtocolCallback(G.toplevel, wm_save, session_save_cb, NULL);
    XmActivateWMProtocol(G.toplevel, wm_save);

    XtRealizeWidget(G.toplevel);
    about_set_window_icon_from_xpm(G.toplevel, ck_grab_camera_pm);
    XtAppMainLoop(G.app);

    session_data_free(G.session_data);
    free(G.last_dir);
    return 0;
}
