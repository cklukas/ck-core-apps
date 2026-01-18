/*
 * load_meters.c
 *
 * Simple Motif app using VerticalMeter to display:
 *   - CPU usage (%)
 *   - RAM usage (%)
 *   - Swap usage (%)
 *   - Load averages (1, 5, 15 min) normalized to CPU count (in %)
 *
 * Layout:
 *   - Top-level XmForm, with 6 child XmForms as "columns"
 *   - In each column: XmLabelGadget at the top, VerticalMeter filling below
 *
 * Columns automatically stretch horizontally with window width,
 * meters fill vertically with height.
 *
 * Compile example (adjust paths as needed):
 *   cc -o load_meters load_meters.c vertical_meter.c \
 *      -lXm -lXt -lX11 -lm
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
#include <Xm/Protocols.h>
#include <Dt/Session.h>
#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Dt/WmSettings.h>

#include "vertical_meter.h"
#include "../shared/session_utils.h"

#define NUM_METERS 6

enum {
    METER_CPU = 0,
    METER_RAM,
    METER_SWAP,
    METER_LOAD1,
    METER_LOAD5,
    METER_LOAD15
};

/* Update interval in milliseconds */
#define UPDATE_INTERVAL_MS 1000

/* Maxima in "percent" units for all meters */
#define PERCENT_MAX 100
#define LOAD_PERCENT_DEFAULT_MAX 100
#define ICON_WIDTH 96
#define ICON_HEIGHT 96
#define ICON_MARGIN 4
#define ICON_BAR_GAP 4
#define ICON_SEGMENT_HEIGHT 2
#define ICON_SEGMENT_GAP 2
#define ICON_BORDER_INSET 4
#define ICON_BAR_BG_DARKEN_FACTOR 0.4
#define ICON_MIN_BRIGHTNESS_DIFF 0.45

#ifndef DRAW_ICON_DEBUG_CIRCLES
#define DRAW_ICON_DEBUG_CIRCLES 0
#endif

static Widget meters[NUM_METERS];
static Widget value_labels[NUM_METERS];
static XtAppContext app_context;
static SessionData *session_data = NULL;
static char g_exec_path[PATH_MAX] = "ck-load";
static Pixmap g_icon_pixmap = None;
static GC g_icon_gc = NULL;
static Display *g_icon_display = NULL;
static int g_icon_screen = -1;
static Widget g_toplevel = NULL;
static int g_icon_cpu_percent = 0;
static int g_icon_ram_percent = 0;
static Pixel g_icon_bg_color = 0;
static Pixel g_icon_top_shadow = 0;
static Pixel g_icon_bottom_shadow = 0;
static Pixel g_icon_bar_bg_color = 0;
static Pixel g_icon_bar_icon_color = 0;
static Pixel g_icon_segment_color = 0;
static int g_icon_colors_inited = 0;
static int g_icon_bar_icon_color_ready = 0;
static int g_show_open_icons = 0;
static int g_last_iconified = -1;
static int g_last_show_open_icons = -1;
static Window g_dtwm_window = None;
static Atom g_dtwm_settings_atom = None;
static int g_logged_no_wm_window = 0;
static int g_logged_settings_fail = 0;
static int g_logged_window_ids = 0;

/* ---------- Helper: CPU usage from /proc/stat ---------- */

static int
read_cpu_usage_percent(int *out_percent)
{
    static unsigned long long prev_user = 0, prev_nice = 0, prev_system = 0;
    static unsigned long long prev_idle = 0, prev_iowait = 0;
    static unsigned long long prev_irq = 0, prev_softirq = 0, prev_steal = 0;
    static int initialized = 0;

    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        return -1;
    }

    char buf[256];
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    /* Example line:
     * cpu  4705 150 1222 1048579 77 0 68 0 0 0
     */
    char cpu_label[5];
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    /* ignore guest, guest_nice */
    int scanned = sscanf(buf, "%4s %llu %llu %llu %llu %llu %llu %llu %llu",
                         cpu_label,
                         &user, &nice, &system, &idle,
                         &iowait, &irq, &softirq, &steal);
    if (scanned < 9) {
        return -1;
    }

    unsigned long long prev_idle_all = prev_idle + prev_iowait;
    unsigned long long idle_all      = idle + iowait;

    unsigned long long prev_non_idle =
        prev_user + prev_nice + prev_system + prev_irq + prev_softirq + prev_steal;
    unsigned long long non_idle =
        user + nice + system + irq + softirq + steal;

    unsigned long long prev_total = prev_idle_all + prev_non_idle;
    unsigned long long total      = idle_all + non_idle;

    if (!initialized) {
        /* First call: initialize and return 0% */
        prev_user = user;
        prev_nice = nice;
        prev_system = system;
        prev_idle = idle;
        prev_iowait = iowait;
        prev_irq = irq;
        prev_softirq = softirq;
        prev_steal = steal;
        initialized = 1;
        *out_percent = 0;
        return 0;
    }

    unsigned long long total_diff = total - prev_total;
    unsigned long long idle_diff  = idle_all - prev_idle_all;

    prev_user = user;
    prev_nice = nice;
    prev_system = system;
    prev_idle = idle;
    prev_iowait = iowait;
    prev_irq = irq;
    prev_softirq = softirq;
    prev_steal = steal;

    if (total_diff == 0) {
        *out_percent = 0;
        return 0;
    }

    double cpu_percent = 100.0 * (double)(total_diff - idle_diff) / (double)total_diff;
    if (cpu_percent < 0.0) cpu_percent = 0.0;
    if (cpu_percent > 100.0) cpu_percent = 100.0;

    *out_percent = (int)(cpu_percent + 0.5);
    return 0;
}

/* ---------- Helper: RAM + swap usage from /proc/meminfo ---------- */

static int
read_mem_and_swap_percent(int *out_ram_percent, int *out_swap_percent,
                          double *out_ram_used_gb, double *out_swap_used_gb)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1;

    char key[64];
    unsigned long value;
    char unit[32];

    unsigned long mem_total = 0;
    unsigned long mem_available = 0;
    unsigned long swap_total = 0;
    unsigned long swap_free = 0;

    while (fscanf(fp, "%63s %lu %31s\n", key, &value, unit) == 3) {
        if (strcmp(key, "MemTotal:") == 0) {
            mem_total = value;
        } else if (strcmp(key, "MemAvailable:") == 0) {
            mem_available = value;
        } else if (strcmp(key, "SwapTotal:") == 0) {
            swap_total = value;
        } else if (strcmp(key, "SwapFree:") == 0) {
            swap_free = value;
        }
    }

    fclose(fp);

    if (out_ram_used_gb)  *out_ram_used_gb  = 0.0;
    if (out_swap_used_gb) *out_swap_used_gb = 0.0;

    if (mem_total > 0 && mem_available > 0) {
        unsigned long mem_used = mem_total - mem_available;
        double ram_percent = 100.0 * (double)mem_used / (double)mem_total;
        if (ram_percent < 0.0) ram_percent = 0.0;
        if (ram_percent > 100.0) ram_percent = 100.0;
        *out_ram_percent = (int)(ram_percent + 0.5);
        if (out_ram_used_gb) {
            *out_ram_used_gb = (double)mem_used / (1024.0 * 1024.0);
        }
    } else {
        *out_ram_percent = 0;
    }

    if (swap_total > 0) {
        unsigned long swap_used = swap_total - swap_free;
        double swap_percent = 100.0 * (double)swap_used / (double)swap_total;
        if (swap_percent < 0.0) swap_percent = 0.0;
        if (swap_percent > 100.0) swap_percent = 100.0;
        *out_swap_percent = (int)(swap_percent + 0.5);
        if (out_swap_used_gb) {
            *out_swap_used_gb = (double)swap_used / (1024.0 * 1024.0);
        }
    } else {
        *out_swap_percent = 0;
    }

    return 0;
}

/* ---------- Helper: load averages from /proc/loadavg ---------- */

static int
read_load_percent(int *out_l1, int *out_l5, int *out_l15,
                  double *out_raw_l1, double *out_raw_l5, double *out_raw_l15)
{
    FILE *fp = fopen("/proc/loadavg", "r");
    if (!fp) return -1;

    double l1, l5, l15;
    if (fscanf(fp, "%lf %lf %lf", &l1, &l5, &l15) != 3) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    if (out_raw_l1) *out_raw_l1 = l1;
    if (out_raw_l5) *out_raw_l5 = l5;
    if (out_raw_l15) *out_raw_l15 = l15;

    long n_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (n_cpus <= 0) n_cpus = 1;

    double scale = 100.0 / (double)n_cpus; /* 1.0 load per core = 100% */

    int p1  = (int)(l1  * scale + 0.5);
    int p5  = (int)(l5  * scale + 0.5);
    int p15 = (int)(l15 * scale + 0.5);

    if (p1  < 0) p1  = 0;
    if (p5  < 0) p5  = 0;
    if (p15 < 0) p15 = 0;

    *out_l1 = p1;
    *out_l5 = p5;
    *out_l15 = p15;
    return 0;
}

static void
ensure_icon_resources(Display *display)
{
    if (!display) return;

    int screen = DefaultScreen(display);
    if (g_icon_display == display && g_icon_screen == screen &&
        g_icon_gc != NULL) {
        return;
    }

    if (g_icon_gc) {
        XFreeGC(g_icon_display, g_icon_gc);
        g_icon_gc = NULL;
    }

    g_icon_display = display;
    g_icon_screen = screen;
    Pixmap root = RootWindow(display, screen);

    XGCValues values;
    values.foreground = BlackPixel(display, screen);
    values.background = WhitePixel(display, screen);
    g_icon_gc = XCreateGC(display, root, GCForeground | GCBackground, &values);
    XSetLineAttributes(display, g_icon_gc, 1, LineSolid, CapProjecting, JoinMiter);
}

static Pixel
derive_darker_icon_pixel(Display *display, int screen, Pixel base_pixel)
{
    if (!display) return base_pixel;
    Colormap colormap = DefaultColormap(display, screen);
    if (colormap == None) return base_pixel;

    XColor color;
    color.pixel = base_pixel;
    if (!XQueryColor(display, colormap, &color)) {
        return base_pixel;
    }

    double factor = ICON_BAR_BG_DARKEN_FACTOR;
    color.red = (unsigned short)(color.red * factor);
    color.green = (unsigned short)(color.green * factor);
    color.blue = (unsigned short)(color.blue * factor);
    color.flags = DoRed | DoGreen | DoBlue;

    if (!XAllocColor(display, colormap, &color)) {
        return base_pixel;
    }

    return color.pixel;
}

static void
update_icon_colors(void)
{
    if (!g_toplevel) return;

    Widget sample = meters[METER_CPU];
    Pixel bg = WhitePixel(g_icon_display, g_icon_screen);
    Pixel top_shadow = bg;
    Pixel bottom_shadow = bg;
    Pixel segment_color = BlackPixel(g_icon_display, g_icon_screen);
    Pixel highlight = segment_color;

    if (sample) {
        XtVaGetValues(sample,
                      XmNbackground, &bg,
                      XmNtopShadowColor, &top_shadow,
                      XmNbottomShadowColor, &bottom_shadow,
                      XmNforeground, &segment_color,
                      XmNhighlightColor, &highlight,
                      NULL);
    } else {
        XtVaGetValues(g_toplevel,
                      XmNbackground, &bg,
                      XmNtopShadowColor, &top_shadow,
                      XmNbottomShadowColor, &bottom_shadow,
                      XmNforeground, &segment_color,
                      XmNhighlightColor, &highlight,
                      NULL);
    }

    g_icon_bg_color = bg;
    g_icon_top_shadow = top_shadow;
    g_icon_bottom_shadow = bottom_shadow;
    g_icon_segment_color = (highlight != 0 && highlight != bg) ? highlight : segment_color;
    if (g_icon_segment_color == g_icon_bg_color && segment_color != g_icon_bg_color) {
        g_icon_segment_color = segment_color;
    }
    g_icon_bar_bg_color = g_icon_bottom_shadow;
    g_icon_bar_bg_color = derive_darker_icon_pixel(g_icon_display, g_icon_screen,
                                                   g_icon_bar_bg_color);
    g_icon_bar_icon_color = g_icon_bar_bg_color;
    g_icon_bar_icon_color_ready = 0;
    g_icon_colors_inited = 1;
}

static int
window_is_iconified(void)
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

static void
update_window_title_with_cpu(int cpu_percent, int iconified, int show_open_icons)
{
    if (!g_toplevel) return;

    Display *display = XtDisplay(g_toplevel);
    Window window = (display && XtIsRealized(g_toplevel)) ? XtWindow(g_toplevel) : None;
    char title[64];
    char icon_name[64];
    if (iconified) {
        snprintf(title, sizeof(title), "CPU %d%%", cpu_percent);
        snprintf(icon_name, sizeof(icon_name), "CPU %d%%", cpu_percent);
    } else {
        snprintf(title, sizeof(title), "System Load");
        snprintf(icon_name, sizeof(icon_name), "CPU %d%%", cpu_percent);
    }

    if (iconified) {
        XtVaSetValues(g_toplevel, XmNtitle, title,
                      XmNiconName, icon_name, NULL);
    } else {
        XtVaSetValues(g_toplevel, XmNiconName, icon_name, NULL);
        if (g_last_iconified == 1 || g_last_show_open_icons != show_open_icons) {
            XtVaSetValues(g_toplevel, XmNtitle, title, NULL);
        }
    }

    if (display && window != None) {
        XTextProperty text_prop;
        char *list = icon_name;
        if (XStringListToTextProperty(&list, 1, &text_prop)) {
            XSetWMIconName(display, window, &text_prop);
            if (text_prop.value) {
                XFree(text_prop.value);
            }
        }

        Atom utf8 = XInternAtom(display, "UTF8_STRING", True);
        Atom net_icon = XInternAtom(display, "_NET_WM_ICON_NAME", True);
        if (utf8 != None && net_icon != None) {
            XChangeProperty(display, window, net_icon, utf8, 8, PropModeReplace,
                            (unsigned char *)icon_name,
                            (int)strlen(icon_name));
        }
    }
}

static Window
get_dtwm_workspace_window(Display *display)
{
    if (!display) return None;
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    Atom info_atom = XInternAtom(display, "_MOTIF_WM_INFO", False);
    if (info_atom != None) {
        Atom actual = None;
        int format = 0;
        unsigned long nitems = 0;
        unsigned long bytes_after = 0;
        unsigned char *data = NULL;

        int status = XGetWindowProperty(display, root, info_atom, 0, 2, False,
                                        AnyPropertyType, &actual, &format,
                                        &nitems, &bytes_after, &data);
        if (status == Success && data && format == 32 && nitems >= 2) {
            unsigned long *info = (unsigned long *)data;
            Window wm_window = (Window)info[1];
            XFree(data);
            return wm_window;
        }
        if (data) XFree(data);
    }

    Atom ws_atom = XInternAtom(display, "_DT_WORKSPACE_LIST", True);
    if (ws_atom == None) return None;
    Atom actual = None;
    int format = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;
    int status = XGetWindowProperty(display, root, ws_atom, 0, 16, False,
                                    AnyPropertyType, &actual, &format,
                                    &nitems, &bytes_after, &data);
    if (status != Success || !data || format != 32 || nitems < 1) {
        if (data) XFree(data);
        return None;
    }

    Window wm_window = ((Window *)data)[0];
    XFree(data);
    return wm_window;
}

static void
ensure_dtwm_monitor(Display *display)
{
    if (!display || g_dtwm_window != None) return;
    g_dtwm_window = get_dtwm_workspace_window(display);
    if (!g_dtwm_window) return;
    XSelectInput(display, g_dtwm_window, PropertyChangeMask);
    g_dtwm_settings_atom = XInternAtom(display, _XA_DT_WM_SETTINGS_V1, True);
}

static int
read_dtwm_show_open_icons(Display *display)
{
    if (!display) return 0;
    Window wm_window = get_dtwm_workspace_window(display);
    if (!wm_window) {
        if (!g_logged_no_wm_window) {
            fprintf(stderr, "[ck-load] dtwm settings: WM window not found\n");
            g_logged_no_wm_window = 1;
        }
        return 0;
    }

    Atom prop = XInternAtom(display, _XA_DT_WM_SETTINGS_V1, True);
    if (prop == None) {
        if (!g_logged_settings_fail) {
            fprintf(stderr, "[ck-load] dtwm settings: atom not available\n");
            g_logged_settings_fail = 1;
        }
        return 0;
    }

    Atom actual = None;
    int format = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;
    Window root = RootWindow(display, DefaultScreen(display));
    if (!g_logged_window_ids) {
        fprintf(stderr, "[ck-load] dtwm settings: wm_window=0x%lx root=0x%lx\n",
                (unsigned long)wm_window, (unsigned long)root);
        g_logged_window_ids = 1;
    }

    int status = XGetWindowProperty(display, wm_window, prop, 0, 2, False,
                                    XA_CARDINAL, &actual, &format,
                                    &nitems, &bytes_after, &data);
    if (status != Success || !data || format != 32 || nitems < 2 ||
        actual != XA_CARDINAL) {
        if (data) XFree(data);
        data = NULL;
        actual = None;
        format = 0;
        nitems = 0;
        bytes_after = 0;
        status = XGetWindowProperty(display, root, prop, 0, 2, False,
                                    XA_CARDINAL, &actual, &format,
                                    &nitems, &bytes_after, &data);
    }

    if (status != Success || !data || format != 32 || nitems < 2 ||
        actual != XA_CARDINAL) {
        if (data) XFree(data);
        if (!g_logged_settings_fail) {
            fprintf(stderr,
                    "[ck-load] dtwm settings: read failed (status=%d format=%d nitems=%lu type=%lu)\n",
                    status, format, nitems, (unsigned long)actual);
            g_logged_settings_fail = 1;
        }
        return 0;
    }

    unsigned long *values = (unsigned long *)data;
    unsigned long version = values[0];
    unsigned long flags = values[1];
    int enabled = (version == DT_WM_SETTINGS_V1_VERSION) &&
                  ((flags & DT_WM_SETTINGS_V1_SHOW_OPEN_WINDOW_ICONS) != 0);
    XFree(data);
    if (g_logged_settings_fail || g_last_show_open_icons != enabled) {
        fprintf(stderr,
                "[ck-load] dtwm settings: version=%lu flags=0x%lx show_open_icons=%d\n",
                version, flags, enabled);
    }
    g_logged_settings_fail = 0;
    return enabled;
}

static double
icon_pixel_brightness(Display *display, Colormap cmap, Pixel pixel)
{
    if (!display || cmap == None) return 0.0;
    XColor color;
    color.pixel = pixel;
    if (!XQueryColor(display, cmap, &color)) return 0.0;
    double r = color.red / 65535.0;
    double g = color.green / 65535.0;
    double b = color.blue / 65535.0;
    return 0.299 * r + 0.587 * g + 0.114 * b;
}

static Pixel
icon_mix_with_white(Display *display, Colormap cmap, XColor *base, double factor)
{
    if (!display || !base) return base ? base->pixel : 0;
    if (factor <= 0.0) return base->pixel;
    if (factor > 1.0) factor = 1.0;
    XColor target = *base;
    target.red   = (unsigned short)(base->red   +
                                    (unsigned short)((65535 - base->red)   * factor));
    target.green = (unsigned short)(base->green +
                                    (unsigned short)((65535 - base->green) * factor));
    target.blue  = (unsigned short)(base->blue  +
                                    (unsigned short)((65535 - base->blue)  * factor));
    target.flags = DoRed | DoGreen | DoBlue;
    if (XAllocColor(display, cmap, &target)) {
        return target.pixel;
    }
    return base->pixel;
}

static Pixel
calculate_icon_bar_fill(Display *display, Colormap cmap,
                        Pixel base_pixel, Pixel segment_pixel)
{
    if (!display || cmap == None) return base_pixel;

    double segment_brightness = icon_pixel_brightness(display, cmap, segment_pixel);
    double fill_brightness = icon_pixel_brightness(display, cmap, base_pixel);
    double diff = fabs(segment_brightness - fill_brightness);
    if (diff >= ICON_MIN_BRIGHTNESS_DIFF) {
        return base_pixel;
    }

    XColor fill_color_info = {0};
    fill_color_info.pixel = base_pixel;
    if (!XQueryColor(display, cmap, &fill_color_info)) {
        return base_pixel;
    }

    double factor = (ICON_MIN_BRIGHTNESS_DIFF - diff) / ICON_MIN_BRIGHTNESS_DIFF;
    if (factor > 1.0) factor = 1.0;
    return icon_mix_with_white(display, cmap, &fill_color_info, factor);
}

static Pixel
get_icon_bar_icon_color(Display *display)
{
    if (!display) return g_icon_bar_bg_color;
    if (!g_icon_colors_inited) return g_icon_bar_bg_color;
    if (g_icon_bar_icon_color_ready) {
        return g_icon_bar_icon_color;
    }

    int screen = (g_icon_screen >= 0) ? g_icon_screen : DefaultScreen(display);
    Colormap cmap = DefaultColormap(display, screen);
    g_icon_bar_icon_color =
        calculate_icon_bar_fill(display, cmap, g_icon_bar_bg_color, g_icon_segment_color);
    g_icon_bar_icon_color_ready = 1;
    return g_icon_bar_icon_color;
}

static void
draw_icon_bars(Display *display, Pixmap pixmap, int cpu_percent, int ram_percent,
               unsigned long fill_color)
{
    int screen = (g_icon_screen >= 0) ? g_icon_screen : DefaultScreen(display);
    unsigned long bg = g_icon_colors_inited ? g_icon_bg_color :
                       WhitePixel(display, screen);
    unsigned long fill = g_icon_colors_inited ? fill_color :
                         BlackPixel(display, screen);
    unsigned long segment_color = g_icon_colors_inited ? g_icon_segment_color :
                                  BlackPixel(display, screen);
    unsigned long top_color = g_icon_colors_inited ? g_icon_top_shadow :
                       WhitePixel(display, screen);
    unsigned long bottom_color = g_icon_colors_inited ? g_icon_bottom_shadow :
                                  BlackPixel(display, screen);

    XSetForeground(display, g_icon_gc, bg);
    XFillRectangle(display, pixmap, g_icon_gc, 0, 0, ICON_WIDTH, ICON_HEIGHT);

    int available_width = ICON_WIDTH - ICON_MARGIN * 2;
    int bar_height = ICON_HEIGHT - ICON_MARGIN * 2;
    int bar_width = (available_width - ICON_BAR_GAP) / 2;
    if (bar_height <= 0 || bar_width <= 0) return;

    if (cpu_percent < 0) cpu_percent = 0;
    if (cpu_percent > 100) cpu_percent = 100;
    if (ram_percent < 0) ram_percent = 0;
    if (ram_percent > 100) ram_percent = 100;

    int cpu_x = ICON_MARGIN;
    int ram_x = ICON_MARGIN + bar_width + ICON_BAR_GAP;

    int inner_height = bar_height - ICON_BORDER_INSET * 2;
    if (inner_height < 1) inner_height = 1;
    int inner_top = ICON_MARGIN + ICON_BORDER_INSET;
    int inner_bottom = inner_top + inner_height - 1;
    int inner_width = bar_width - ICON_BORDER_INSET * 2;

    int segment_stride = ICON_SEGMENT_HEIGHT + ICON_SEGMENT_GAP;
    int max_segments = 0;
    if (segment_stride > 0 && inner_height > 0) {
        max_segments = (inner_height + ICON_SEGMENT_GAP) / segment_stride;
        if (max_segments < 1) max_segments = 1;
    }

    int cpu_segments = (cpu_percent * max_segments + 50) / 100;
    if (cpu_segments > max_segments) cpu_segments = max_segments;
    int cpu_inner_x = cpu_x + ICON_BORDER_INSET;
    if (inner_width < 1) inner_width = 1;

    XSetForeground(display, g_icon_gc, fill);
    XFillRectangle(display, pixmap, g_icon_gc,
                   cpu_x, ICON_MARGIN, bar_width, bar_height);
    XSetForeground(display, g_icon_gc, segment_color);
    for (int i = 0; i < cpu_segments; ++i) {
        int seg_bottom = inner_bottom - i * segment_stride;
        int seg_top = seg_bottom - ICON_SEGMENT_HEIGHT + 1;
        if (seg_top < inner_top) break;
        XFillRectangle(display, pixmap, g_icon_gc,
                       cpu_inner_x, seg_top, inner_width, ICON_SEGMENT_HEIGHT);
    }
    int cpu_top = ICON_MARGIN;
    int cpu_bottom = ICON_MARGIN + bar_height - 1;
    int cpu_left = cpu_x;
    int cpu_right = cpu_x + bar_width - 1;

    XSetForeground(display, g_icon_gc, bottom_color);
    XDrawLine(display, pixmap, g_icon_gc, cpu_left, cpu_top, cpu_right, cpu_top);
    XDrawLine(display, pixmap, g_icon_gc, cpu_left, cpu_top, cpu_left, cpu_bottom);
    XSetForeground(display, g_icon_gc, top_color);
    XDrawLine(display, pixmap, g_icon_gc, cpu_left, cpu_bottom, cpu_right, cpu_bottom);
    XDrawLine(display, pixmap, g_icon_gc, cpu_right, cpu_top, cpu_right, cpu_bottom);

    int ram_segments = (ram_percent * max_segments + 50) / 100;
    if (ram_segments > max_segments) ram_segments = max_segments;
    int ram_inner_x = ram_x + ICON_BORDER_INSET;

    XSetForeground(display, g_icon_gc, fill);
    XFillRectangle(display, pixmap, g_icon_gc,
                   ram_x, ICON_MARGIN, bar_width, bar_height);
    XSetForeground(display, g_icon_gc, segment_color);
    for (int i = 0; i < ram_segments; ++i) {
        int seg_bottom = inner_bottom - i * segment_stride;
        int seg_top = seg_bottom - ICON_SEGMENT_HEIGHT + 1;
        if (seg_top < inner_top) break;
        XFillRectangle(display, pixmap, g_icon_gc,
                       ram_inner_x, seg_top, inner_width, ICON_SEGMENT_HEIGHT);
    }
    int ram_top = ICON_MARGIN;
    int ram_bottom = ICON_MARGIN + bar_height - 1;
    int ram_left = ram_x;
    int ram_right = ram_x + bar_width - 1;

    XSetForeground(display, g_icon_gc, bottom_color);
    XDrawLine(display, pixmap, g_icon_gc, ram_left, ram_top, ram_right, ram_top);
    XDrawLine(display, pixmap, g_icon_gc, ram_left, ram_top, ram_left, ram_bottom);
    XSetForeground(display, g_icon_gc, top_color);
    XDrawLine(display, pixmap, g_icon_gc, ram_left, ram_bottom, ram_right, ram_bottom);
    XDrawLine(display, pixmap, g_icon_gc, ram_right, ram_top, ram_right, ram_bottom);

    int circle_radius = (ICON_WIDTH < ICON_HEIGHT ? ICON_WIDTH : ICON_HEIGHT) / 2;
    circle_radius -= ICON_MARGIN + 1;
    if (circle_radius < 1) circle_radius = 1;
    if (DRAW_ICON_DEBUG_CIRCLES) {
        XSetForeground(display, g_icon_gc, BlackPixel(display, screen));
        int cx = ICON_WIDTH / 2;
        int cy = ICON_HEIGHT / 2;
        int diameter = circle_radius * 2;
        XDrawArc(display, pixmap, g_icon_gc,
                 cx - circle_radius, cy - circle_radius,
                 diameter, diameter, 0, 360 * 64);
        int inner_radius = circle_radius - 5;
        if (inner_radius > 0) {
            int inner_diameter = inner_radius * 2;
            XDrawArc(display, pixmap, g_icon_gc,
                     cx - inner_radius, cy - inner_radius,
                     inner_diameter, inner_diameter, 0, 360 * 64);
        }
    }
}

static void
update_wm_icon_pixmap(Display *display)
{
    if (!g_icon_pixmap || !g_toplevel || !XtIsRealized(g_toplevel)) return;
    Window window = XtWindow(g_toplevel);
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
    local.icon_pixmap = g_icon_pixmap;
    XSetWMHints(display, window, &local);
}

static void
refresh_dynamic_icon(int cpu_percent, int ram_percent)
{
    if (!g_toplevel) return;

    Display *display = XtDisplay(g_toplevel);
    if (!display) return;

    ensure_dtwm_monitor(display);
    if (g_dtwm_window != None && g_dtwm_settings_atom != None) {
        XEvent event;
        while (XCheckWindowEvent(display, g_dtwm_window, PropertyChangeMask, &event)) {
            if (event.type == PropertyNotify &&
                event.xproperty.atom == g_dtwm_settings_atom) {
                g_show_open_icons = read_dtwm_show_open_icons(display);
            }
        }
    }

    int iconified = window_is_iconified();
    if (g_last_iconified == -1 || (g_last_iconified == 1 && !iconified)) {
        g_show_open_icons = read_dtwm_show_open_icons(display);
    }
    int update_icon = iconified || g_show_open_icons;

    ensure_icon_resources(display);
    update_icon_colors();
    int screen = (g_icon_screen >= 0) ? g_icon_screen : DefaultScreen(display);
    Pixel icon_fill = g_icon_colors_inited ? get_icon_bar_icon_color(display) :
                      BlackPixel(display, screen);
    Pixmap root = RootWindow(display, screen);
    Pixmap new_pixmap = None;
    if (update_icon) {
        new_pixmap = XCreatePixmap(display, root, ICON_WIDTH, ICON_HEIGHT,
                                   DefaultDepth(display, screen));
        if (new_pixmap == None) return;

        draw_icon_bars(display, new_pixmap, cpu_percent, ram_percent, icon_fill);
        XFlush(display);
        Pixmap prev_pixmap = g_icon_pixmap;
        g_icon_pixmap = new_pixmap;
        XtVaSetValues(g_toplevel, XmNiconPixmap, g_icon_pixmap, NULL);
        if (XtIsRealized(g_toplevel)) {
            update_wm_icon_pixmap(display);
        }
        if (prev_pixmap != None) {
            XFreePixmap(display, prev_pixmap);
        }
    }
    update_window_title_with_cpu(cpu_percent, iconified, g_show_open_icons);
    g_last_iconified = iconified;
    g_last_show_open_icons = g_show_open_icons;
}

/* ---------- Timer callback: update all meters ---------- */

static void
update_meters_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)client_data;
    (void)id;

    static int last_values[NUM_METERS] = { -1, -1, -1, -1, -1, -1 };
    static int last_load_max = -1;
    static char last_labels[NUM_METERS][32] = {{0}};

    int cpu_percent;
    int ram_percent, swap_percent;
    int load1_percent, load5_percent, load15_percent;
    int load_max = LOAD_PERCENT_DEFAULT_MAX;
    double ram_used_gb = 0.0, swap_used_gb = 0.0;
    double load1_raw = 0.0, load5_raw = 0.0, load15_raw = 0.0;

    if (read_cpu_usage_percent(&cpu_percent) == 0) {
        if (cpu_percent != last_values[METER_CPU]) {
            VerticalMeterSetValue(meters[METER_CPU], cpu_percent);
            last_values[METER_CPU] = cpu_percent;
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "%d%%", cpu_percent);
        if (strcmp(buf, last_labels[METER_CPU]) != 0) {
            XmString s = XmStringCreateLocalized(buf);
            XtVaSetValues(value_labels[METER_CPU], XmNlabelString, s, NULL);
            XmStringFree(s);
            snprintf(last_labels[METER_CPU], sizeof(last_labels[METER_CPU]), "%s", buf);
        }
        g_icon_cpu_percent = cpu_percent;
    }

    if (read_mem_and_swap_percent(&ram_percent, &swap_percent,
                                  &ram_used_gb, &swap_used_gb) == 0) {
        if (ram_percent != last_values[METER_RAM]) {
            VerticalMeterSetValue(meters[METER_RAM],  ram_percent);
            last_values[METER_RAM] = ram_percent;
        }
        if (swap_percent != last_values[METER_SWAP]) {
            VerticalMeterSetValue(meters[METER_SWAP], swap_percent);
            last_values[METER_SWAP] = swap_percent;
        }

        char buf_ram[32];
        snprintf(buf_ram, sizeof(buf_ram), "%.1f GB", ram_used_gb);
        if (strcmp(buf_ram, last_labels[METER_RAM]) != 0) {
            XmString s_ram = XmStringCreateLocalized(buf_ram);
            XtVaSetValues(value_labels[METER_RAM], XmNlabelString, s_ram, NULL);
            XmStringFree(s_ram);
            snprintf(last_labels[METER_RAM], sizeof(last_labels[METER_RAM]), "%s", buf_ram);
        }

        char buf_swap[32];
        snprintf(buf_swap, sizeof(buf_swap), "%.1f GB", swap_used_gb);
        if (strcmp(buf_swap, last_labels[METER_SWAP]) != 0) {
            XmString s_swap = XmStringCreateLocalized(buf_swap);
            XtVaSetValues(value_labels[METER_SWAP], XmNlabelString, s_swap, NULL);
            XmStringFree(s_swap);
            snprintf(last_labels[METER_SWAP], sizeof(last_labels[METER_SWAP]), "%s", buf_swap);
        }
        g_icon_ram_percent = ram_percent;
    }

    if (read_load_percent(&load1_percent, &load5_percent, &load15_percent,
                          &load1_raw, &load5_raw, &load15_raw) == 0) {
        /* Dynamically raise the maximum if any load value exceeds the default.
           Keep all three load meters on the same scale. */
        if (load1_percent > load_max) load_max = load1_percent;
        if (load5_percent > load_max) load_max = load5_percent;
        if (load15_percent > load_max) load_max = load15_percent;

        if (load_max != last_load_max) {
            VerticalMeterSetMaximum(meters[METER_LOAD1],  load_max);
            VerticalMeterSetMaximum(meters[METER_LOAD5],  load_max);
            VerticalMeterSetMaximum(meters[METER_LOAD15], load_max);
            last_load_max = load_max;
        }

        VerticalMeterSetDefaultMaximum(meters[METER_LOAD1],  LOAD_PERCENT_DEFAULT_MAX);
        VerticalMeterSetDefaultMaximum(meters[METER_LOAD5],  LOAD_PERCENT_DEFAULT_MAX);
        VerticalMeterSetDefaultMaximum(meters[METER_LOAD15], LOAD_PERCENT_DEFAULT_MAX);

        if (load1_percent != last_values[METER_LOAD1]) {
            VerticalMeterSetValue(meters[METER_LOAD1],  load1_percent);
            last_values[METER_LOAD1] = load1_percent;
        }
        if (load5_percent != last_values[METER_LOAD5]) {
            VerticalMeterSetValue(meters[METER_LOAD5],  load5_percent);
            last_values[METER_LOAD5] = load5_percent;
        }
        if (load15_percent != last_values[METER_LOAD15]) {
            VerticalMeterSetValue(meters[METER_LOAD15], load15_percent);
            last_values[METER_LOAD15] = load15_percent;
        }

        char buf1[32], buf5[32], buf15[32];
        snprintf(buf1, sizeof(buf1), "%.2f", load1_raw);
        snprintf(buf5, sizeof(buf5), "%.2f", load5_raw);
        snprintf(buf15, sizeof(buf15), "%.2f", load15_raw);

        if (strcmp(buf1, last_labels[METER_LOAD1]) != 0) {
            XmString s1 = XmStringCreateLocalized(buf1);
            XtVaSetValues(value_labels[METER_LOAD1],  XmNlabelString, s1,  NULL);
            XmStringFree(s1);
            snprintf(last_labels[METER_LOAD1], sizeof(last_labels[METER_LOAD1]), "%s", buf1);
        }
        if (strcmp(buf5, last_labels[METER_LOAD5]) != 0) {
            XmString s5 = XmStringCreateLocalized(buf5);
            XtVaSetValues(value_labels[METER_LOAD5],  XmNlabelString, s5,  NULL);
            XmStringFree(s5);
            snprintf(last_labels[METER_LOAD5], sizeof(last_labels[METER_LOAD5]), "%s", buf5);
        }
        if (strcmp(buf15, last_labels[METER_LOAD15]) != 0) {
            XmString s15 = XmStringCreateLocalized(buf15);
            XtVaSetValues(value_labels[METER_LOAD15], XmNlabelString, s15, NULL);
            XmStringFree(s15);
            snprintf(last_labels[METER_LOAD15], sizeof(last_labels[METER_LOAD15]), "%s", buf15);
        }
    }

    refresh_dynamic_icon(g_icon_cpu_percent, g_icon_ram_percent);

    /* Re-arm timer */
    XtAppAddTimeOut(app_context, UPDATE_INTERVAL_MS, update_meters_cb, NULL);
}

static void wm_delete_callback(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    XtAppContext app = (XtAppContext)client_data;
    XtAppSetExitFlag(app);
}

/* ---------- Session handling ---------- */

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

/* ---------- Main + UI setup ---------- */

    int
    main(int argc, char *argv[])
    {
        Widget toplevel, main_form;
        XmString xm_title;
    static char *meter_labels[NUM_METERS] = {
        "CPU",
        "RAM",
        "Swap",
        "Load 1",
        "Load 5",
        "Load 15"
    };

    XtSetLanguageProc(NULL, NULL, NULL);

    toplevel = XtVaAppInitialize(
        &app_context,
        "LoadMeters",
        NULL, 0,
        &argc, argv,
        NULL,
        NULL
    );

    /* Session handling: parse -session, remember exec path */
    char *session_id = session_parse_argument(&argc, argv);
    session_data = session_data_create(session_id);
    free(session_id);
    init_exec_path(argv[0]);

    /* Main form, fractional positions used for equal-width columns */
    main_form = XtVaCreateManagedWidget(
        "mainForm",
        xmFormWidgetClass, toplevel,
        XmNfractionBase, NUM_METERS * 10, /* 10 units per column */
        NULL
    );

    for (int i = 0; i < NUM_METERS; ++i) {
        int left_pos  = i * 10;
        int right_pos = (i + 1) * 10;

        /* Column Form */
        Widget col_form = XtVaCreateManagedWidget(
            "colForm",
            xmFormWidgetClass, main_form,
            XmNleftAttachment,   XmATTACH_POSITION,
            XmNleftPosition,     left_pos,
            XmNrightAttachment,  XmATTACH_POSITION,
            XmNrightPosition,    right_pos,
            XmNtopAttachment,    XmATTACH_FORM,
            XmNbottomAttachment, XmATTACH_FORM,
            NULL
        );

        /* Column label */
        xm_title = XmStringCreateLocalized(meter_labels[i]);
        Widget label = XtVaCreateManagedWidget(
            "meterLabel",
            xmLabelGadgetClass, col_form,
            XmNlabelString,    xm_title,
            XmNalignment,      XmALIGNMENT_CENTER,
            XmNtopAttachment,  XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment,XmATTACH_FORM,
            NULL
        );
        XmStringFree(xm_title);

        /* Vertical meter below the label, filling remaining space */
        Arg args[8];
        Cardinal n = 0;
        /* Initial reasonable size; will be overridden by attachments */
        XtSetArg(args[n], XmNwidth,  40); n++;
        XtSetArg(args[n], XmNheight, 150); n++;

        Widget meter = VerticalMeterCreate(col_form, "verticalMeter", args, n);

        XtVaSetValues(
            meter,
            XmNtopAttachment,    XmATTACH_WIDGET,
            XmNtopWidget,        label,
            XmNleftAttachment,   XmATTACH_FORM,
            XmNrightAttachment,  XmATTACH_FORM,
            NULL
        );

        /* Value label at the bottom */
        Widget value_label = XtVaCreateManagedWidget(
            "valueLabel",
            xmLabelGadgetClass, col_form,
            XmNalignment,      XmALIGNMENT_CENTER,
            XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment,XmATTACH_FORM,
            NULL
        );
        XmString initial = XmStringCreateLocalized("-");
        XtVaSetValues(value_label, XmNlabelString, initial, NULL);
        XmStringFree(initial);

        /* Meter sits above the value label */
        XtVaSetValues(
            meter,
            XmNbottomAttachment, XmATTACH_WIDGET,
            XmNbottomWidget,     value_label,
            XmNbottomOffset,     2,
            NULL
        );

        /* Configure meter maxima and cell height */
        if (i == METER_LOAD1 || i == METER_LOAD5 || i == METER_LOAD15) {
            VerticalMeterSetMaximum(meter, LOAD_PERCENT_DEFAULT_MAX);
            VerticalMeterSetDefaultMaximum(meter, LOAD_PERCENT_DEFAULT_MAX);
        } else {
            VerticalMeterSetMaximum(meter, PERCENT_MAX);
        }
        VerticalMeterSetCellHeight(meter, 4); /* 0 = square cells in your implementation */

        meters[i] = meter;
        value_labels[i] = value_label;
    }

    g_toplevel = toplevel;

    /* Set an application icon or window title if desired */
    const char *window_title = "System Load";
    XtVaSetValues(toplevel,
                  XmNtitle, window_title,
                  XmNiconName, window_title,
                  NULL);
    refresh_dynamic_icon(g_icon_cpu_percent, g_icon_ram_percent);

    /* Session restore (geometry) */
    if (session_data && session_load(toplevel, session_data)) {
        session_apply_geometry(toplevel, session_data, "x", "y", "w", "h");
    }

    /* WM protocol handling */
    Atom wm_delete = XmInternAtom(XtDisplay(toplevel),
                                  "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(toplevel, wm_delete,
                            wm_delete_callback, (XtPointer)app_context);
    XmActivateWMProtocol(toplevel, wm_delete);

    Atom wm_save = XmInternAtom(XtDisplay(toplevel),
                                "WM_SAVE_YOURSELF", False);
    XmAddWMProtocolCallback(toplevel, wm_save,
                            session_save_cb, NULL);
    XmActivateWMProtocol(toplevel, wm_save);

    XtRealizeWidget(toplevel);

    /* Start periodic updates */
    XtAppAddTimeOut(app_context, UPDATE_INTERVAL_MS, update_meters_cb, NULL);

    XtAppMainLoop(app_context);
    return 0;
}
