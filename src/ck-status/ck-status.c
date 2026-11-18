/*
 * ck-status.c - Authentic CDE Motif Workstation & Laptop Status Console,
 *               FreeDesktop Notification Server, StatusNotifier Host, and
 *               Front Panel Live Instrument Gauge (Wayland & X11/XLibre).
 */

#define _GNU_SOURCE
#include "ck-status.h"
#include "ck-ac-plug-icons.h"

#include "../shared/about_dialog.h"
#include "../shared/cde_palette.h"
#include "../shared/session_utils.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include <Xm/CascadeB.h>
#include <Xm/DrawingA.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/Label.h>
#include <Xm/List.h>
#include <Xm/MainW.h>
#include <Xm/Protocols.h>
#include <Xm/PushB.h>
#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/SeparatoG.h>
#include <Xm/Separator.h>
#include <Xm/ToggleB.h>
#include <Xm/ToggleBG.h>

#include <dbus/dbus.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern void ck_status_set_raise_callback(void (*cb)(const char *tab, void *userdata));

typedef struct {
    XtAppContext app_ctx;
    Display *dpy;
    int screen_num;
    Visual *visual;
    Colormap cmap;
    int depth;

    Widget toplevel;
    bool gauge_mode;
    bool daemon_hidden;
    bool standalone;

    CkStatusModel model;
    CkStatusTab current_tab;

    /* Colors derived from Motif/CDE palette */
    Pixel bg_px, fg_px, ts_px, bs_px, sel_px;
    double bg_r, bg_g, bg_b;
    double fg_r, fg_g, fg_b;
    double ts_r, ts_g, ts_b;
    double bs_r, bs_g, bs_b;

    /* Console widgets */
    Widget main_win;
    Widget header_canvas;
    Widget tab_buttons[CK_TAB_COUNT];
    Widget tab_pages[CK_TAB_COUNT];

    /* Overview widgets */
    Widget ov_summary_lbl;
    Widget ov_dnd_toggle;
    Widget ov_wifi_toggle;
    Widget ov_bt_toggle;
    Widget ov_pin_toggle;

    /* Notifications & SNI Tray widgets */
    Widget notif_count_lbl;
    Widget notif_dnd_toggle;
    Widget notif_list;
    Widget sni_strip_rc;
    Widget sni_empty_lbl;
    int sni_btn_count;
    Widget sni_btns[CK_STATUS_MAX_SNI_ITEMS];
    Pixmap sni_pixmaps[CK_STATUS_MAX_SNI_ITEMS];

    /* Power tab widgets */
    Widget pwr_canvas;
    Widget pwr_detail_lbl;

    /* Network tab widgets */
    Widget net_detail_lbl;
    Widget net_wifi_toggle;
    Widget net_ap_list;

    /* Bluetooth tab widgets */
    Widget bt_detail_lbl;
    Widget bt_power_toggle;
    Widget bt_dev_list;
    int selected_bt_idx;

    /* Toast notification banner */
    Widget toast_shell;
    Widget toast_app_lbl;
    Widget toast_sum_lbl;
    Widget toast_body_lbl;
    XtIntervalId toast_timer;

    /* Gauge mode widgets */
    Widget gauge_canvas;
    Widget gauge_popup;

    /* Dynamic WM_HINTS icon pixmap */
    Pixmap icon_pixmap;
    int icon_w, icon_h;
} CkStatusApp;

static CkStatusApp g_app = {0};

static void refresh_all_ui(void);
static void switch_to_tab(CkStatusTab tab);

static void pixel_to_rgb(Display *dpy, Colormap cmap, Pixel px, double *r, double *g, double *b) {
    XColor xc = {.pixel = px};
    if (dpy && XQueryColor(dpy, cmap, &xc)) {
        *r = (double)xc.red / 65535.0;
        *g = (double)xc.green / 65535.0;
        *b = (double)xc.blue / 65535.0;
    } else {
        *r = 0.68;
        *g = 0.68;
        *b = 0.70;
    }
}

static void refresh_theme_colors(Widget ref_w) {
    if (!ref_w || !g_app.dpy) return;
    XtVaGetValues(ref_w, XmNbackground, &g_app.bg_px, XmNforeground, &g_app.fg_px, NULL);
    XmGetColors(XtScreen(ref_w), g_app.cmap, g_app.bg_px,
                &g_app.fg_px, &g_app.ts_px, &g_app.bs_px, &g_app.sel_px);
    pixel_to_rgb(g_app.dpy, g_app.cmap, g_app.bg_px, &g_app.bg_r, &g_app.bg_g, &g_app.bg_b);
    pixel_to_rgb(g_app.dpy, g_app.cmap, g_app.fg_px, &g_app.fg_r, &g_app.fg_g, &g_app.fg_b);
    pixel_to_rgb(g_app.dpy, g_app.cmap, g_app.ts_px, &g_app.ts_r, &g_app.ts_g, &g_app.ts_b);
    pixel_to_rgb(g_app.dpy, g_app.cmap, g_app.bs_px, &g_app.bs_r, &g_app.bs_g, &g_app.bs_b);
}

static void draw_bevel_box(cairo_t *cr, double x, double y, double w, double h, bool recessed) {
    double r1 = recessed ? g_app.bs_r : g_app.ts_r;
    double g1 = recessed ? g_app.bs_g : g_app.ts_g;
    double b1 = recessed ? g_app.bs_b : g_app.ts_b;
    double r2 = recessed ? g_app.ts_r : g_app.bs_r;
    double g2 = recessed ? g_app.ts_g : g_app.bs_g;
    double b2 = recessed ? g_app.ts_b : g_app.bs_b;

    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgb(cr, r1, g1, b1);
    cairo_move_to(cr, x + 0.5, y + h - 0.5);
    cairo_line_to(cr, x + 0.5, y + 0.5);
    cairo_line_to(cr, x + w - 0.5, y + 0.5);
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, r2, g2, b2);
    cairo_move_to(cr, x + w - 0.5, y + 0.5);
    cairo_line_to(cr, x + w - 0.5, y + h - 0.5);
    cairo_line_to(cr, x + 0.5, y + h - 0.5);
    cairo_stroke(cr);
}

static void draw_ac_plug_sprite(cairo_t *cr, double x, double y, double target_sz, bool large) {
    int sw = large ? CK_AC_PLUG_LARGE_W : CK_AC_PLUG_SMALL_W;
    int sh = large ? CK_AC_PLUG_LARGE_H : CK_AC_PLUG_SMALL_H;
    const uint32_t *src = large ? ck_ac_plug_40x40 : ck_ac_plug_18x18;
    cairo_surface_t *img = cairo_image_surface_create_for_data(
        (unsigned char *)src, CAIRO_FORMAT_ARGB32, sw, sh, sw * 4
    );
    cairo_save(cr);
    cairo_translate(cr, floor(x), floor(y));
    double sc = target_sz / (double)sw;
    if (fabs(sc - 1.0) > 0.02) {
        cairo_scale(cr, sc, sc);
    }
    cairo_set_source_surface(cr, img, 0.0, 0.0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(img);
}

static void draw_aa_battery_cell(
    cairo_t *cr, double x, double y, double body_w, double body_h,
    int pct, bool ac_online
) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    double cap_w = fmax(2.0, floor(body_h * 0.24));
    double cap_h = fmax(4.0, floor(body_h * 0.46));

    /* Metallic 3D AA shell + positive nub */
    cairo_set_source_rgb(cr, 0.72, 0.76, 0.80);
    cairo_set_line_width(cr, (body_h >= 24.0) ? 2.0 : 1.0);
    cairo_rectangle(cr, x + 0.5, y + 0.5, body_w - 1.0, body_h - 1.0);
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.78, 0.82, 0.86);
    cairo_rectangle(cr, x + body_w + 0.5, y + floor((body_h - cap_h) * 0.5), cap_w, cap_h);
    cairo_fill(cr);

    /* 4 compact segments inside the AA-proportioned body (~2.3:1 ratio) */
    int cells = 4;
    double pad = (body_h >= 24.0) ? 4.0 : 2.0;
    double gap = (body_h >= 24.0) ? 3.0 : 1.5;
    double inner_w = body_w - pad * 2.0;
    double inner_h = body_h - pad * 2.0;
    double cw = (inner_w - (cells - 1) * gap) / (double)cells;
    for (int i = 0; i < cells; ++i) {
        int thresh = i * 25 + 6;
        if (pct >= thresh) {
            if (pct <= 15 && !ac_online) cairo_set_source_rgb(cr, 0.92, 0.26, 0.26);
            else if (pct <= 28 && !ac_online) cairo_set_source_rgb(cr, 0.95, 0.72, 0.20);
            else cairo_set_source_rgb(cr, 0.24, 0.84, 0.48);
        } else {
            cairo_set_source_rgb(cr, 0.22, 0.26, 0.30);
        }
        cairo_rectangle(cr, x + pad + i * (cw + gap), y + pad, fmax(1.5, cw), fmax(2.0, inner_h));
        cairo_fill(cr);
    }
}

/* -------------------------------------------------------------------------
 * Shared 4-Quadrant Sun/IBM Instrument Cluster Renderer
 * Used by both Front Panel --gauge (66x54, borderless) and WM_HINTS icon (48/96)
 * ------------------------------------------------------------------------- */
static void render_instrument_cluster(cairo_t *cr, int w, int h, bool with_outer_bevel) {
    const CkStatusModel *m = &g_app.model;
    cairo_set_source_rgb(cr, g_app.bg_r, g_app.bg_g, g_app.bg_b);
    cairo_paint(cr);
    if (with_outer_bevel) {
        draw_bevel_box(cr, 0, 0, w, h, false);
    }

    double pad = with_outer_bevel ? ((w >= 72) ? 5.0 : 3.0) : 2.0;
    double gap_mid = 3.0;
    double top_h = floor((h - pad * 2.0 - gap_mid) * 0.49);
    double bot_y = pad + top_h + gap_mid;
    double bot_h = h - bot_y - pad;
    double half_w = floor((w - pad * 2.0 - gap_mid) * 0.5);
    double q2_x = pad + half_w + gap_mid;
    double q2_w = w - pad - q2_x;

    /* Dark recessed instrument wells (classic 1995 Sun/IBM dark slate meter face) */
    cairo_set_source_rgb(cr, 0.13, 0.16, 0.19);
    cairo_rectangle(cr, pad, pad, half_w, top_h);
    cairo_rectangle(cr, q2_x, pad, q2_w, top_h);
    cairo_rectangle(cr, pad, bot_y, w - pad * 2.0, bot_h);
    cairo_fill(cr);

    draw_bevel_box(cr, pad, pad, half_w, top_h, true);
    draw_bevel_box(cr, q2_x, pad, q2_w, top_h, true);
    draw_bevel_box(cr, pad, bot_y, w - pad * 2.0, bot_h, true);

    /* Quadrant 1 (Top-Left): 4-bar Wi-Fi / Network signal meter */
    int active_bars = 0;
    if (m->net.connected) {
        if (m->net.wifi_present && m->net.signal_pct > 0) {
            active_bars = (m->net.signal_pct + 24) / 25;
            if (active_bars > 4) active_bars = 4;
        } else {
            active_bars = 4;
        }
    }
    double bw = fmax(3.0, floor((half_w - 8.0) / 5.2));
    double gap = fmax(1.5, floor(bw * 0.42));
    double bx0 = pad + floor((half_w - (4.0 * bw + 3.0 * gap)) * 0.5);
    for (int b = 0; b < 4; ++b) {
        double bh = floor((top_h - 5.0) * (0.30 + 0.23 * b));
        double by = pad + top_h - 2.5 - bh;
        if (b < active_bars) {
            cairo_set_source_rgb(cr, 0.22, 0.84, 0.58);
        } else {
            cairo_set_source_rgb(cr, 0.28, 0.33, 0.37);
        }
        cairo_rectangle(cr, bx0 + b * (bw + gap), by, bw, bh);
        cairo_fill(cr);
    }

    /* Quadrant 2 (Top-Right): Bluetooth + Unread Notification / SNI amber lamp */
    double bt_cx = q2_x + q2_w * 0.31;
    double bt_cy = pad + top_h * 0.50;
    double bt_r = fmin(q2_w * 0.24, top_h * 0.35);
    if (m->bt.adapter_present && m->bt.powered) {
        cairo_set_source_rgb(cr, 0.35, 0.68, 0.98);
    } else {
        cairo_set_source_rgb(cr, 0.35, 0.38, 0.42);
    }
    cairo_set_line_width(cr, (w >= 64) ? 1.5 : 1.2);
    cairo_move_to(cr, bt_cx, bt_cy - bt_r);
    cairo_line_to(cr, bt_cx, bt_cy + bt_r);
    cairo_line_to(cr, bt_cx + bt_r * 0.75, bt_cy + bt_r * 0.45);
    cairo_line_to(cr, bt_cx - bt_r * 0.65, bt_cy - bt_r * 0.45);
    cairo_move_to(cr, bt_cx - bt_r * 0.65, bt_cy + bt_r * 0.45);
    cairo_line_to(cr, bt_cx + bt_r * 0.75, bt_cy - bt_r * 0.45);
    cairo_line_to(cr, bt_cx, bt_cy - bt_r);
    cairo_stroke(cr);

    /* Notification / SNI indicator lamp on right half of Q2 */
    double lamp_cx = q2_x + q2_w * 0.72;
    double lamp_cy = bt_cy;
    double lamp_r = fmin(q2_w * 0.21, top_h * 0.30);
    if (m->unread_count > 0) {
        cairo_set_source_rgb(cr, 0.98, 0.66, 0.16); /* Lit Sun amber alert */
    } else if (m->sni_count > 0) {
        cairo_set_source_rgb(cr, 0.32, 0.78, 0.82); /* Active SNI host cyan */
    } else {
        cairo_set_source_rgb(cr, 0.28, 0.32, 0.35);
    }
    cairo_arc(cr, lamp_cx, lamp_cy, lamp_r, 0, 2.0 * M_PI);
    cairo_fill(cr);

    /* Bottom Well: AA-Proportioned Battery (+ AC plug & %) when battery present;
     * Classic CDE AC Power Plug + live CPU % when no battery is present! */
    double gx = pad + 3.0;
    double gy = bot_y + 2.0;
    double gw = (w - pad * 2.0) - 6.0;
    double gh = bot_h - 4.0;

    if (m->battery.present) {
        int pct = (m->battery.capacity_pct >= 0) ? m->battery.capacity_pct : 0;
        double bat_h = fmax(9.0, fmin(13.0, floor(gh * 0.64)));
        double bat_w = floor(bat_h * 2.25); /* Standard AA ~2.25:1 width:height ratio */
        double bat_y = gy + floor((gh - bat_h) * 0.5);
        draw_aa_battery_cell(cr, gx, bat_y, bat_w, bat_h, pct, m->battery.ac_online);

        char txt[16];
        snprintf(txt, sizeof(txt), "%d%%", pct);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, (w < 60) ? 8.5 : 9.0);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, txt, &ext);
        double text_x = gx + gw - ext.width - 1.0;

        double cur_x = gx + bat_w + fmax(3.0, floor(bat_h * 0.24)) + 2.0;
        double p_sz = fmin(13.0, gh - 2.0);
        if (m->battery.ac_online && (cur_x + p_sz + 2.0 <= text_x)) {
            draw_ac_plug_sprite(cr, cur_x, gy + floor((gh - p_sz) * 0.5), p_sz, false);
        }

        cairo_set_source_rgb(cr, 0.92, 0.95, 0.94);
        cairo_move_to(cr, text_x, gy + (gh + ext.height) * 0.5 - 0.5);
        cairo_show_text(cr, txt);
    } else {
        /* Desktop workstation without battery: NEVER draw battery bars!
         * Show the classic CDE 3D AC Power Plug icon + live CPU usage % */
        double p_sz = fmin(18.0, gh);
        draw_ac_plug_sprite(cr, gx + 1.0, gy + floor((gh - p_sz) * 0.5), p_sz, false);

        char txt[24];
        snprintf(txt, sizeof(txt), "%d%%", m->load.cpu_usage_pct);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, (w < 60) ? 8.5 : 9.5);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, "CPU", &ext);
        cairo_set_source_rgb(cr, 0.68, 0.76, 0.82);
        cairo_set_font_size(cr, 7.5);
        cairo_move_to(cr, gx + p_sz + 4.0, gy + gh * 0.44);
        cairo_show_text(cr, "CPU");

        cairo_set_font_size(cr, (w < 60) ? 8.5 : 9.5);
        cairo_text_extents(cr, txt, &ext);
        cairo_set_source_rgb(cr, 0.34, 0.90, 0.62);
        cairo_move_to(cr, gx + gw - ext.width - 1.0, gy + gh * 0.84);
        cairo_show_text(cr, txt);
    }
}

/* -------------------------------------------------------------------------
 * Query WM_ICON_SIZE and publish live WM_HINTS icon_pixmap
 * ------------------------------------------------------------------------- */
static void query_icon_target_size(int *out_w, int *out_h) {
    *out_w = 48;
    *out_h = 48;
    if (!g_app.dpy) return;
    XIconSize *list = NULL;
    int count = 0;
    if (XGetIconSizes(g_app.dpy, DefaultRootWindow(g_app.dpy), &list, &count) && list && count > 0) {
        if (list[0].max_width >= 64 && list[0].max_height >= 64) {
            *out_w = 96;
            *out_h = 96;
        }
        XFree(list);
    }
}

static void update_wm_icon_pixmap(void) {
    if (!g_app.dpy || !g_app.toplevel || !XtIsRealized(g_app.toplevel) || g_app.gauge_mode) return;
    int iw = 48, ih = 48;
    query_icon_target_size(&iw, &ih);
    if (!g_app.icon_pixmap || g_app.icon_w != iw || g_app.icon_h != ih) {
        if (g_app.icon_pixmap) XFreePixmap(g_app.dpy, g_app.icon_pixmap);
        g_app.icon_pixmap = XCreatePixmap(g_app.dpy, XtWindow(g_app.toplevel),
                                          (unsigned int)iw, (unsigned int)ih, (unsigned int)g_app.depth);
        g_app.icon_w = iw;
        g_app.icon_h = ih;
    }
    cairo_surface_t *surf = cairo_xlib_surface_create(
        g_app.dpy, g_app.icon_pixmap, g_app.visual, iw, ih
    );
    cairo_t *cr = cairo_create(surf);
    render_instrument_cluster(cr, iw, ih, true);
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    cairo_surface_destroy(surf);

    Window win = XtWindow(g_app.toplevel);
    XWMHints *hints = XGetWMHints(g_app.dpy, win);
    if (!hints) hints = XAllocWMHints();
    if (hints) {
        hints->flags |= IconPixmapHint;
        hints->icon_pixmap = g_app.icon_pixmap;
        XSetWMHints(g_app.dpy, win, hints);
        XFree(hints);
    }
    XFlush(g_app.dpy);
}

/* -------------------------------------------------------------------------
 * Pin / Unpin Live Status Gauge in CDE Front Panel (~/.dt/types/fp_dynamic)
 * ------------------------------------------------------------------------- */
static void get_gauge_fp_path(char *out, size_t sz) {
    const char *home = getenv("HOME");
    snprintf(out, sz, "%s/.dt/types/ckstatus_gauge.fp", (home && home[0]) ? home : "/tmp");
}

static bool is_gauge_pinned_in_fp(void) {
    char p[1024];
    get_gauge_fp_path(p, sizeof(p));
    struct stat st;
    return stat(p, &st) == 0 && st.st_size > 0;
}

static void request_front_panel_reload(void) {
    if (!g_app.dpy) return;
    Window root = DefaultRootWindow(g_app.dpy);
    Atom info_a = XInternAtom(g_app.dpy, "_MOTIF_WM_INFO", False);
    Atom act = None;
    int fmt = 0;
    unsigned long n = 0, rem = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(g_app.dpy, root, info_a, 0, 2, False, AnyPropertyType,
                           &act, &fmt, &n, &rem, &data) == Success && data && n >= 2) {
        Window wm_w = (Window)((unsigned long *)data)[1];
        XFree(data);
        Atom req_a = XInternAtom(g_app.dpy, "_DT_WM_REQUEST", False);
        const char cmd[] = "f.restart -noconfirm";
        XChangeProperty(g_app.dpy, wm_w, req_a, XA_STRING, 8, PropModeAppend,
                        (const unsigned char *)cmd, (int)sizeof(cmd));
        XFlush(g_app.dpy);
    }
}

static void set_gauge_pinned_in_fp(bool pin) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) return;
    char d1[1024], d2[1024], fp_path[1088];
    snprintf(d1, sizeof(d1), "%s/.dt/types", home);
    snprintf(d2, sizeof(d2), "%s/.dt/types/fp_dynamic", home);
    mkdir(d1, 0755);
    mkdir(d2, 0755);
    get_gauge_fp_path(fp_path, sizeof(fp_path));

    if (pin) {
        FILE *f = fopen(fp_path, "w");
        if (f) {
            fprintf(
                f,
                "CONTROL Blank2\n"
                "{\n"
                "  TYPE            client\n"
                "  CONTAINER_NAME  Top\n"
                "  CONTAINER_TYPE  BOX\n"
                "  POSITION_HINTS  8\n"
                "  CLIENT_NAME     ck-status-gauge\n"
                "  CLIENT_GEOMETRY 74x54\n"
                "  LABEL           System Status\n"
                "}\n"
            );
            fclose(f);
        }
        request_front_panel_reload();
        if (fork() == 0) {
            usleep(900000);
            execlp("ck-status", "ck-status", "--gauge", (char *)NULL);
            _exit(127);
        }
    } else {
        unlink(fp_path);
        int rc = system("pkill -TERM -f 'ck-status --gauge' >/dev/null 2>&1");
        (void)rc;
        request_front_panel_reload();
    }
}

/* -------------------------------------------------------------------------
 * Toast Notification Banner (Authentic CDE 3D-Beveled Top-Right/Bottom-Right)
 * ------------------------------------------------------------------------- */
static void dismiss_toast_timeout_cb(XtPointer client_data, XtIntervalId *id) {
    (void)client_data;
    (void)id;
    g_app.toast_timer = 0;
    if (g_app.toast_shell && XtIsRealized(g_app.toast_shell)) {
        XtPopdown(g_app.toast_shell);
    }
}

static void toast_click_eh(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont) {
    (void)w;
    (void)client_data;
    (void)cont;
    if (!ev || ev->type != ButtonPress) return;
    if (g_app.toast_timer) {
        XtRemoveTimeOut(g_app.toast_timer);
        g_app.toast_timer = 0;
    }
    if (g_app.toast_shell) XtPopdown(g_app.toast_shell);
    if (ev->xbutton.button == Button1 && g_app.toplevel && !g_app.gauge_mode) {
        XtMapWidget(g_app.toplevel);
        XRaiseWindow(g_app.dpy, XtWindow(g_app.toplevel));
        switch_to_tab(CK_TAB_NOTIFICATIONS);
    }
}

static void ensure_toast_banner(void) {
    if (g_app.toast_shell || !g_app.toplevel) return;
    g_app.toast_shell = XtVaAppCreateShell(
        "ckNotifyBanner", "CkStatus",
        topLevelShellWidgetClass, g_app.dpy,
        XmNoverrideRedirect, True,
        XmNsaveUnder, True,
        XmNallowShellResize, True,
        NULL
    );
    Widget outer = XtVaCreateManagedWidget(
        "toastFrame", xmFrameWidgetClass, g_app.toast_shell,
        XmNshadowType, XmSHADOW_OUT,
        XmNshadowThickness, 3,
        XmNwidth, 344,
        NULL
    );
    Widget rc = XtVaCreateManagedWidget(
        "toastRC", xmRowColumnWidgetClass, outer,
        XmNorientation, XmVERTICAL,
        XmNmarginWidth, 10,
        XmNmarginHeight, 8,
        XmNspacing, 4,
        NULL
    );
    g_app.toast_app_lbl = XtVaCreateManagedWidget(
        "toastApp", xmLabelWidgetClass, rc,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL
    );
    XtVaCreateManagedWidget("toastSep", xmSeparatorGadgetClass, rc, NULL);
    g_app.toast_sum_lbl = XtVaCreateManagedWidget(
        "toastSummary", xmLabelWidgetClass, rc,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL
    );
    g_app.toast_body_lbl = XtVaCreateManagedWidget(
        "toastBody", xmLabelWidgetClass, rc,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL
    );
    XtAddEventHandler(outer, ButtonPressMask, False, toast_click_eh, NULL);
    XtAddEventHandler(rc, ButtonPressMask, False, toast_click_eh, NULL);
    XtAddEventHandler(g_app.toast_sum_lbl, ButtonPressMask, False, toast_click_eh, NULL);
    XtAddEventHandler(g_app.toast_body_lbl, ButtonPressMask, False, toast_click_eh, NULL);
}

static void set_label_text(Widget lbl, const char *txt) {
    if (!lbl) return;
    XmString xs = XmStringCreateLocalized((char *)(txt ? txt : ""));
    XtVaSetValues(lbl, XmNlabelString, xs, NULL);
    XmStringFree(xs);
}

static void wrap_toast_body(const char *src, char *dst, size_t dst_sz, int max_col) {
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src || !src[0]) return;

    size_t di = 0;
    int col = 0;
    const char *p = src;
    while (*p && di + 1 < dst_sz) {
        if (*p == '\n') {
            dst[di++] = *p++;
            col = 0;
            continue;
        }
        /* Find next word length */
        const char *w_end = p;
        while (*w_end && *w_end != ' ' && *w_end != '\t' && *w_end != '\n') w_end++;
        int w_len = (int)(w_end - p);
        if (col > 0 && col + w_len > max_col) {
            dst[di++] = '\n';
            col = 0;
            while (*p == ' ' || *p == '\t') p++;
            continue;
        }
        while (p < w_end && di + 1 < dst_sz) {
            dst[di++] = *p++;
            col++;
        }
        while ((*p == ' ' || *p == '\t') && di + 1 < dst_sz) {
            if (col >= max_col) {
                dst[di++] = '\n';
                col = 0;
                while (*p == ' ' || *p == '\t') p++;
                break;
            }
            dst[di++] = *p++;
            col++;
        }
    }
    dst[di] = '\0';
}

static void on_dbus_toast(const CkNotification *notif, void *userdata) {
    (void)userdata;
    if (!notif || g_app.gauge_mode) return;
    ensure_toast_banner();
    if (!g_app.toast_shell) return;

    char hdr[128];
    char wrapped_body[512];
    const char *urg = (notif->urgency == 2) ? " [CRITICAL]" : "";
    snprintf(hdr, sizeof(hdr), "%s  |  %s%s", notif->app_name, notif->time_str, urg);
    wrap_toast_body(notif->body, wrapped_body, sizeof(wrapped_body), 48);
    set_label_text(g_app.toast_app_lbl, hdr);
    set_label_text(g_app.toast_sum_lbl, notif->summary);
    set_label_text(g_app.toast_body_lbl, wrapped_body);

    int sw = DisplayWidth(g_app.dpy, g_app.screen_num);
    int sh = DisplayHeight(g_app.dpy, g_app.screen_num);
    XtPopup(g_app.toast_shell, XtGrabNone);
    Dimension tw = 0, th = 0;
    XtVaGetValues(g_app.toast_shell, XmNwidth, &tw, XmNheight, &th, NULL);
    int bw = (tw > 40) ? (int)tw : 352;
    int bh = (th > 24) ? (int)th : 96;
    int tx = sw - bw - 16;
    int ty = sh - bh - 96;
    if (tx < 8) tx = 8;
    if (ty < 8) ty = 8;
    XtMoveWidget(g_app.toast_shell, (Position)tx, (Position)ty);
    if (XtIsRealized(g_app.toast_shell)) {
        XRaiseWindow(g_app.dpy, XtWindow(g_app.toast_shell));
    }

    if (g_app.toast_timer) XtRemoveTimeOut(g_app.toast_timer);
    unsigned long ms = (notif->expire_ms > 0) ? (unsigned long)notif->expire_ms : 4500UL;
    if (ms > 15000UL) ms = 15000UL;
    g_app.toast_timer = XtAppAddTimeOut(g_app.app_ctx, ms, dismiss_toast_timeout_cb, NULL);
}

static void on_dbus_state_changed(void *userdata) {
    (void)userdata;
    refresh_all_ui();
}

static void on_dbus_raise_tab(const char *tab_name, void *userdata) {
    (void)userdata;
    if (g_app.gauge_mode || !g_app.toplevel) return;
    g_app.daemon_hidden = false;
    XtMapWidget(g_app.toplevel);
    if (XtIsRealized(g_app.toplevel)) {
        XMapRaised(g_app.dpy, XtWindow(g_app.toplevel));
    }
    if (!tab_name) return;
    if (strcasecmp(tab_name, "notifications") == 0 || strcasecmp(tab_name, "tray") == 0 ||
        strcasecmp(tab_name, "notify") == 0) {
        ck_status_model_mark_all_read(&g_app.model);
        switch_to_tab(CK_TAB_NOTIFICATIONS);
    } else if (strcasecmp(tab_name, "power") == 0 || strcasecmp(tab_name, "battery") == 0) {
        switch_to_tab(CK_TAB_POWER);
    } else if (strcasecmp(tab_name, "network") == 0 || strcasecmp(tab_name, "wifi") == 0) {
        switch_to_tab(CK_TAB_NETWORK);
    } else if (strcasecmp(tab_name, "bluetooth") == 0 || strcasecmp(tab_name, "bt") == 0) {
        switch_to_tab(CK_TAB_BLUETOOTH);
    } else {
        switch_to_tab(CK_TAB_OVERVIEW);
    }
    refresh_all_ui();
}

/* -------------------------------------------------------------------------
 * Console Header Instrument Strip & Power Fuel-Gauge Canvas Drawing
 * ------------------------------------------------------------------------- */
static void draw_header_strip_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)client_data;
    (void)call_data;
    if (!XtIsRealized(w)) return;
    Dimension ww = 0, wh = 0;
    XtVaGetValues(w, XmNwidth, &ww, XmNheight, &wh, NULL);
    if (ww < 20 || wh < 20) return;

    refresh_theme_colors(w);
    cairo_surface_t *surf = cairo_xlib_surface_create(g_app.dpy, XtWindow(w), g_app.visual, ww, wh);
    cairo_t *cr = cairo_create(surf);

    cairo_set_source_rgb(cr, g_app.bg_r, g_app.bg_g, g_app.bg_b);
    cairo_paint(cr);

    const CkStatusModel *m = &g_app.model;
    const char *titles[4] = {
        m->battery.present ? "POWER / BATTERY" : "AC POWER / LOAD",
        "NETWORK / WI-FI",
        "BLUETOOTH",
        "NOTIFICATIONS & TRAY"
    };
    char vals[4][64];

    if (m->battery.present) {
        snprintf(vals[0], sizeof(vals[0]), "%d%%  (%s)", m->battery.capacity_pct, m->battery.status_text);
    } else {
        snprintf(vals[0], sizeof(vals[0]), "AC  |  CPU %d%%", m->load.cpu_usage_pct);
    }
    if (m->net.connected) {
        if (m->net.ssid[0]) {
            snprintf(vals[1], sizeof(vals[1]), "%s (%d%%)", m->net.ssid, m->net.signal_pct);
        } else {
            snprintf(vals[1], sizeof(vals[1]), "%s  %s",
                     m->net.primary_iface[0] ? m->net.primary_iface : "Wired",
                     m->net.ipv4[0] ? m->net.ipv4 : "Connected");
        }
    } else {
        snprintf(vals[1], sizeof(vals[1]), "Offline");
    }
    if (m->bt.adapter_present) {
        snprintf(vals[2], sizeof(vals[2]), "%s (%d dev)", m->bt.powered ? "Powered On" : "Off", m->bt.device_count);
    } else {
        snprintf(vals[2], sizeof(vals[2]), "No Adapter");
    }
    snprintf(vals[3], sizeof(vals[3]), "%d unread  |  %d tray", m->unread_count, m->sni_count);

    double gap = 8.0;
    double card_w = ((double)ww - gap * 5.0) / 4.0;
    double card_h = (double)wh - 12.0;

    for (int i = 0; i < 4; ++i) {
        double cx = gap + i * (card_w + gap);
        double cy = 6.0;
        cairo_set_source_rgb(cr, 0.14, 0.17, 0.20);
        cairo_rectangle(cr, cx, cy, card_w, card_h);
        cairo_fill(cr);
        draw_bevel_box(cr, cx, cy, card_w, card_h, true);

        cairo_save(cr);
        cairo_rectangle(cr, cx + 2.0, cy + 2.0, fmax(1.0, card_w - 4.0), fmax(1.0, card_h - 4.0));
        cairo_clip(cr);

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 10.0);
        cairo_set_source_rgb(cr, 0.72, 0.79, 0.84);
        cairo_move_to(cr, cx + 8.0, cy + 16.0);
        cairo_show_text(cr, titles[i]);

        cairo_set_font_size(cr, 11.5);
        if ((i == 3 && m->unread_count > 0)) {
            cairo_set_source_rgb(cr, 0.98, 0.72, 0.22);
        } else {
            cairo_set_source_rgb(cr, 0.34, 0.90, 0.62);
        }
        cairo_move_to(cr, cx + 8.0, cy + 36.0);
        cairo_show_text(cr, vals[i]);
        cairo_restore(cr);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surf);
}

static void header_strip_click_eh(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont) {
    (void)client_data;
    (void)cont;
    if (!ev || ev->type != ButtonPress || ev->xbutton.button != Button1) return;
    Dimension ww = 1;
    XtVaGetValues(w, XmNwidth, &ww, NULL);
    int col = (ev->xbutton.x * 4) / (int)(ww > 0 ? ww : 1);
    if (col == 0) switch_to_tab(CK_TAB_POWER);
    else if (col == 1) switch_to_tab(CK_TAB_NETWORK);
    else if (col == 2) switch_to_tab(CK_TAB_BLUETOOTH);
    else switch_to_tab(CK_TAB_NOTIFICATIONS);
}

static void draw_power_gauge_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)client_data;
    (void)call_data;
    if (!XtIsRealized(w)) return;
    Dimension ww = 0, wh = 0;
    XtVaGetValues(w, XmNwidth, &ww, XmNheight, &wh, NULL);
    if (ww < 20 || wh < 20) return;

    refresh_theme_colors(w);
    cairo_surface_t *surf = cairo_xlib_surface_create(g_app.dpy, XtWindow(w), g_app.visual, ww, wh);
    cairo_t *cr = cairo_create(surf);

    cairo_set_source_rgb(cr, 0.13, 0.16, 0.19);
    cairo_paint(cr);
    draw_bevel_box(cr, 0, 0, ww, wh, true);

    const CkBatteryState *b = &g_app.model.battery;
    const CkSystemLoadState *ld = &g_app.model.load;

    if (b->present) {
        /* Realistic AA-proportioned battery (~2.35:1 ratio) on the left + AC plug & load info on the right */
        int pct = (b->capacity_pct >= 0) ? b->capacity_pct : 0;
        double bat_h = fmax(28.0, fmin(38.0, (double)wh - 24.0));
        double bat_w = floor(bat_h * 2.35);
        double bx = 16.0;
        double by = floor(((double)wh - bat_h) * 0.5);
        draw_aa_battery_cell(cr, bx, by, bat_w, bat_h, pct, b->ac_online);

        double info_x = bx + bat_w + floor(bat_h * 0.24) + 16.0;
        if (b->ac_online) {
            draw_ac_plug_sprite(cr, info_x, floor(((double)wh - 40.0) * 0.5), 40.0, true);
            info_x += 52.0;
        }

        char line1[128], line2[160];
        snprintf(line1, sizeof(line1), "%d%%  —  %s%s",
                 pct, b->status_text, b->ac_online ? " (AC Mains)" : "");
        snprintf(line2, sizeof(line2), "Draw: %.2f W   |   CPU: %d%%   |   Load: %.2f, %.2f, %.2f",
                 (double)b->power_mw / 1000.0, ld->cpu_usage_pct,
                 ld->load_1m, ld->load_5m, ld->load_15m);

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 14.0);
        cairo_set_source_rgb(cr, 0.34, 0.90, 0.62);
        cairo_move_to(cr, info_x, (double)wh * 0.43);
        cairo_show_text(cr, line1);

        cairo_set_font_size(cr, 11.5);
        cairo_set_source_rgb(cr, 0.78, 0.84, 0.88);
        cairo_move_to(cr, info_x, (double)wh * 0.74);
        cairo_show_text(cr, line2);
    } else {
        /* Desktop workstation with NO battery: do NOT draw battery bars!
         * Show the classic CDE 3D AC Power Plug icon + AC Mains & CPU/Load readout */
        draw_ac_plug_sprite(cr, 16.0, floor(((double)wh - 40.0) * 0.5), 40.0, true);

        char line1[128], line2[160];
        snprintf(line1, sizeof(line1), "AC MAINS ONLINE  —  Desktop Workstation");
        snprintf(line2, sizeof(line2), "CPU Activity: %d%% (%d cores)   |   Load Avg: %.2f, %.2f, %.2f",
                 ld->cpu_usage_pct, ld->cpu_cores, ld->load_1m, ld->load_5m, ld->load_15m);

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 13.5);
        cairo_set_source_rgb(cr, 0.34, 0.90, 0.62);
        cairo_move_to(cr, 68.0, (double)wh * 0.43);
        cairo_show_text(cr, line1);

        cairo_set_font_size(cr, 11.5);
        cairo_set_source_rgb(cr, 0.78, 0.84, 0.88);
        cairo_move_to(cr, 68.0, (double)wh * 0.75);
        cairo_show_text(cr, line2);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surf);
}

/* -------------------------------------------------------------------------
 * Build XImage/Pixmap from SNI ARGB32 IconPixmap for Motif Tray Buttons
 * ------------------------------------------------------------------------- */
static Pixmap create_pixmap_from_argb(const uint32_t *argb, int w, int h) {
    if (!g_app.dpy || !argb || w <= 0 || h <= 0) return None;
    int dst_sz = 24;
    Pixmap pm = XCreatePixmap(g_app.dpy, XtWindow(g_app.toplevel),
                              (unsigned int)dst_sz, (unsigned int)dst_sz, (unsigned int)g_app.depth);
    cairo_surface_t *dst = cairo_xlib_surface_create(g_app.dpy, pm, g_app.visual, dst_sz, dst_sz);
    cairo_t *cr = cairo_create(dst);
    cairo_set_source_rgb(cr, g_app.bg_r, g_app.bg_g, g_app.bg_b);
    cairo_paint(cr);

    /* Convert unpremultiplied ARGB32 to premultiplied CAIRO_FORMAT_ARGB32 */
    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    unsigned char *dst_px = cairo_image_surface_get_data(img);
    int stride = cairo_image_surface_get_stride(img);
    cairo_surface_flush(img);
    for (int y = 0; y < h; ++y) {
        uint32_t *row = (uint32_t *)(dst_px + y * stride);
        for (int x = 0; x < w; ++x) {
            uint32_t p = argb[y * w + x];
            uint32_t a = (p >> 24) & 0xffU;
            uint32_t r = (((p >> 16) & 0xffU) * a) / 255U;
            uint32_t g = (((p >> 8) & 0xffU) * a) / 255U;
            uint32_t b = ((p & 0xffU) * a) / 255U;
            row[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    cairo_surface_mark_dirty(img);
    cairo_scale(cr, (double)dst_sz / (double)w, (double)dst_sz / (double)h);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_paint(cr);
    cairo_surface_destroy(img);
    cairo_destroy(cr);
    cairo_surface_flush(dst);
    cairo_surface_destroy(dst);
    return pm;
}

static void sni_btn_event_eh(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont) {
    (void)w;
    (void)cont;
    if (!ev || ev->type != ButtonPress) return;
    int idx = (int)(intptr_t)client_data;
    if (idx < 0 || idx >= g_app.model.sni_count) return;
    ck_status_dbus_sni_activate(&g_app.model.sni_items[idx], ev->xbutton.x_root, ev->xbutton.y_root, (int)ev->xbutton.button);
}

static void rebuild_sni_strip(void) {
    if (!g_app.sni_strip_rc || !XtIsRealized(g_app.toplevel)) return;
    for (int i = 0; i < g_app.sni_btn_count; ++i) {
        if (g_app.sni_btns[i]) XtDestroyWidget(g_app.sni_btns[i]);
        if (g_app.sni_pixmaps[i]) XFreePixmap(g_app.dpy, g_app.sni_pixmaps[i]);
        g_app.sni_btns[i] = NULL;
        g_app.sni_pixmaps[i] = None;
    }
    g_app.sni_btn_count = 0;

    if (g_app.model.sni_count == 0) {
        if (g_app.sni_empty_lbl) XtManageChild(g_app.sni_empty_lbl);
        return;
    }
    if (g_app.sni_empty_lbl) XtUnmanageChild(g_app.sni_empty_lbl);

    for (int i = 0; i < g_app.model.sni_count; ++i) {
        const CkSniItem *it = &g_app.model.sni_items[i];
        char caption[160];
        snprintf(caption, sizeof(caption), " %s (%s) ",
                 it->title[0] ? it->title : it->id, it->status);
        XmString xs = XmStringCreateLocalized(caption);
        Widget btn = XtVaCreateManagedWidget(
            "sniItemBtn", xmPushButtonWidgetClass, g_app.sni_strip_rc,
            XmNlabelString, xs,
            NULL
        );
        XmStringFree(xs);
        if (it->argb_pixels && it->icon_w > 0 && it->icon_h > 0) {
            Pixmap pm = create_pixmap_from_argb(it->argb_pixels, it->icon_w, it->icon_h);
            if (pm != None) {
                XtVaSetValues(btn, XmNlabelType, XmPIXMAP_AND_STRING, XmNlabelPixmap, pm, NULL);
                g_app.sni_pixmaps[i] = pm;
            }
        }
        XtAddEventHandler(btn, ButtonPressMask, False, sni_btn_event_eh, (XtPointer)(intptr_t)i);
        g_app.sni_btns[i] = btn;
        g_app.sni_btn_count++;
    }
}

/* -------------------------------------------------------------------------
 * Refresh All UI Views & Gauge
 * ------------------------------------------------------------------------- */
static void populate_xm_list(Widget list_w, char items[][256], int count) {
    if (!list_w) return;
    XmListDeleteAllItems(list_w);
    if (count <= 0) return;
    XmString *xitems = (XmString *)calloc((size_t)count, sizeof(XmString));
    if (!xitems) return;
    for (int i = 0; i < count; ++i) {
        xitems[i] = XmStringCreateLocalized(items[i]);
    }
    XmListAddItemsUnselected(list_w, xitems, count, 1);
    for (int i = 0; i < count; ++i) XmStringFree(xitems[i]);
    free(xitems);
}

static void draw_gauge_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)client_data;
    (void)call_data;
    if (!XtIsRealized(w)) return;
    Dimension ww = 0, wh = 0;
    XtVaGetValues(w, XmNwidth, &ww, XmNheight, &wh, NULL);
    if (ww < 8 || wh < 8) return;
    refresh_theme_colors(w);
    cairo_surface_t *surf = cairo_xlib_surface_create(g_app.dpy, XtWindow(w), g_app.visual, ww, wh);
    cairo_t *cr = cairo_create(surf);
    render_instrument_cluster(cr, (int)ww, (int)wh, false);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
}

static void refresh_all_ui(void) {
    if (g_app.gauge_mode) {
        if (g_app.gauge_canvas && XtIsRealized(g_app.gauge_canvas)) {
            draw_gauge_cb(g_app.gauge_canvas, NULL, NULL);
        }
        return;
    }

    update_wm_icon_pixmap();
    if (g_app.header_canvas && XtIsRealized(g_app.header_canvas)) {
        draw_header_strip_cb(g_app.header_canvas, NULL, NULL);
    }
    if (g_app.pwr_canvas && XtIsRealized(g_app.pwr_canvas)) {
        draw_power_gauge_cb(g_app.pwr_canvas, NULL, NULL);
    }

    const CkStatusModel *m = &g_app.model;

    /* Overview summary */
    char ov[768];
    snprintf(
        ov, sizeof(ov),
        "Power Source:      %s%s\n"
        "Primary Network:   %s  (%s)\n"
        "Bluetooth Radio:   %s\n"
        "Notifications:     %d total (%d unread)  |  Do Not Disturb: %s\n"
        "StatusNotifier:    %d active background tray item(s)",
        m->battery.present ? m->battery.status_text : "Workstation AC",
        m->battery.present ? "" : " (No internal battery)",
        m->net.connected ? (m->net.primary_iface[0] ? m->net.primary_iface : "Online") : "Disconnected",
        m->net.ipv4[0] ? m->net.ipv4 : "no IPv4",
        m->bt.adapter_present ? (m->bt.powered ? "Powered On" : "Powered Off") : "Not Present",
        m->notif_count, m->unread_count, m->dnd_enabled ? "ON" : "OFF",
        m->sni_count
    );
    set_label_text(g_app.ov_summary_lbl, ov);
    if (g_app.ov_dnd_toggle) XmToggleButtonSetState(g_app.ov_dnd_toggle, m->dnd_enabled, False);
    if (g_app.ov_wifi_toggle) XmToggleButtonSetState(g_app.ov_wifi_toggle, m->net.wifi_enabled, False);
    if (g_app.ov_bt_toggle) XmToggleButtonSetState(g_app.ov_bt_toggle, m->bt.powered, False);
    if (g_app.ov_pin_toggle) XmToggleButtonSetState(g_app.ov_pin_toggle, is_gauge_pinned_in_fp(), False);

    /* Notifications & SNI */
    char ncnt[160];
    snprintf(ncnt, sizeof(ncnt), "Desktop Notifications (%d stored, %d unread):",
             m->notif_count, m->unread_count);
    set_label_text(g_app.notif_count_lbl, ncnt);
    if (g_app.notif_dnd_toggle) XmToggleButtonSetState(g_app.notif_dnd_toggle, m->dnd_enabled, False);

    char nlines[CK_STATUS_MAX_NOTIFICATIONS][256];
    int n_items = m->notif_count;
    if (n_items == 0) {
        snprintf(nlines[0], sizeof(nlines[0]), "(No desktop notifications received in this session)");
        populate_xm_list(g_app.notif_list, nlines, 1);
    } else {
        for (int i = 0; i < n_items; ++i) {
            const CkNotification *n = &m->notifs[i];
            snprintf(nlines[i], sizeof(nlines[i]), "%s[%s] %s: %s -- %s",
                     n->unread ? "* " : "  ", n->time_str, n->app_name, n->summary, n->body);
        }
        populate_xm_list(g_app.notif_list, nlines, n_items);
    }
    rebuild_sni_strip();

    /* Power tab */
    char pwr[512];
    if (m->battery.present) {
        char rem[64] = "Estimating...";
        if (m->battery.minutes_remaining > 0) {
            snprintf(rem, sizeof(rem), "%dh %02dm",
                     m->battery.minutes_remaining / 60, m->battery.minutes_remaining % 60);
        } else if (m->battery.full) {
            snprintf(rem, sizeof(rem), "Fully Charged");
        }
        snprintf(
            pwr, sizeof(pwr),
            "Battery Device:    %s (%s)\n"
            "State / Charge:    %s -- %d%%\n"
            "AC Adapter:        %s\n"
            "Power Draw / Rate: %.2f W\n"
            "Remaining Time:    %s\n"
            "Platform Profile:  %s",
            m->battery.bat_name, m->battery.model_name[0] ? m->battery.model_name : "Standard Pack",
            m->battery.status_text, m->battery.capacity_pct,
            m->battery.ac_online ? "Online" : "On Battery",
            (double)m->battery.power_mw / 1000.0,
            rem,
            m->battery.power_profile
        );
    } else {
        snprintf(
            pwr, sizeof(pwr),
            "Power Configuration: Desktop Workstation (AC Mains Online)\n"
            "Battery Pack:        None detected under /sys/class/power_supply\n"
            "CPU & System Load:   %d%% active across %d core(s)  |  Load avg %.2f, %.2f, %.2f\n"
            "Platform Profile:    %s",
            m->load.cpu_usage_pct, m->load.cpu_cores,
            m->load.load_1m, m->load.load_5m, m->load.load_15m,
            m->battery.power_profile
        );
    }
    set_label_text(g_app.pwr_detail_lbl, pwr);

    /* Network tab */
    char net_txt[512];
    snprintf(
        net_txt, sizeof(net_txt),
        "Primary Interface: %s  (%s)\n"
        "IPv4 Address:      %s\n"
        "IPv6 Address:      %s\n"
        "Traffic Counters:  RX %.2f MiB  /  TX %.2f MiB\n"
        "Wi-Fi Subsystem:   %s%s",
        m->net.primary_iface[0] ? m->net.primary_iface : "(none)",
        m->net.connected ? "Link Up" : "Offline",
        m->net.ipv4[0] ? m->net.ipv4 : "--",
        m->net.ipv6[0] ? m->net.ipv6 : "--",
        (double)m->net.rx_bytes / (1024.0 * 1024.0),
        (double)m->net.tx_bytes / (1024.0 * 1024.0),
        m->net.wifi_present ? (m->net.wifi_enabled ? "Enabled " : "Disabled ") : "No wireless adapter",
        m->net.ssid[0] ? m->net.ssid : ""
    );
    set_label_text(g_app.net_detail_lbl, net_txt);
    if (g_app.net_wifi_toggle) XmToggleButtonSetState(g_app.net_wifi_toggle, m->net.wifi_enabled, False);

    char aplines[CK_STATUS_MAX_WIFI_APS][256];
    if (m->net.ap_count == 0) {
        snprintf(aplines[0], sizeof(aplines[0]),
                 m->net.wifi_present ? "(No Wi-Fi access points reported)" : "(Wired Ethernet workstation - no Wi-Fi hardware)");
        populate_xm_list(g_app.net_ap_list, aplines, 1);
    } else {
        for (int i = 0; i < m->net.ap_count; ++i) {
            const CkWifiAp *ap = &m->net.aps[i];
            snprintf(aplines[i], sizeof(aplines[i]), "%s%-32s  Signal: %3d%%  %s",
                     ap->in_use ? "* " : "  ", ap->ssid, ap->signal_pct, ap->secured ? "[WPA/RSN]" : "[Open]");
        }
        populate_xm_list(g_app.net_ap_list, aplines, m->net.ap_count);
    }

    /* Bluetooth tab */
    char bt_txt[256];
    snprintf(
        bt_txt, sizeof(bt_txt),
        "Bluetooth Adapter: %s\n"
        "Radio State:       %s  (BlueZ D-Bus: %s)",
        m->bt.adapter_present ? m->bt.adapter_name : "No Bluetooth controller detected",
        m->bt.powered ? "Powered ON" : "Powered OFF",
        m->bt.bluez_available ? "Active" : "Unavailable"
    );
    set_label_text(g_app.bt_detail_lbl, bt_txt);
    if (g_app.bt_power_toggle) XmToggleButtonSetState(g_app.bt_power_toggle, m->bt.powered, False);

    char btlines[CK_STATUS_MAX_BT_DEVICES][256];
    if (m->bt.device_count == 0) {
        snprintf(btlines[0], sizeof(btlines[0]), "(No paired or discovered Bluetooth devices)");
        populate_xm_list(g_app.bt_dev_list, btlines, 1);
    } else {
        for (int i = 0; i < m->bt.device_count; ++i) {
            const CkBtDevice *dv = &m->bt.devices[i];
            snprintf(btlines[i], sizeof(btlines[i]), "%s%-28s  [%s]  %s",
                     dv->connected ? "* " : "  ", dv->name, dv->address,
                     dv->connected ? "Connected" : (dv->paired ? "Paired" : "Available"));
        }
        populate_xm_list(g_app.bt_dev_list, btlines, m->bt.device_count);
    }
}

/* -------------------------------------------------------------------------
 * UI Callbacks & Periodic Poll Timer
 * ------------------------------------------------------------------------- */
static void switch_to_tab(CkStatusTab tab) {
    if (tab < 0 || tab >= CK_TAB_COUNT) return;
    g_app.current_tab = tab;
    for (int i = 0; i < CK_TAB_COUNT; ++i) {
        if (g_app.tab_pages[i]) {
            if (i == (int)tab) XtManageChild(g_app.tab_pages[i]);
            else XtUnmanageChild(g_app.tab_pages[i]);
        }
        if (g_app.tab_buttons[i]) {
            XmToggleButtonSetState(g_app.tab_buttons[i], (i == (int)tab), False);
        }
    }
    if (tab == CK_TAB_NOTIFICATIONS && g_app.model.unread_count > 0) {
        ck_status_model_mark_all_read(&g_app.model);
        refresh_all_ui();
    }
}

static void tab_btn_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call_data;
    if (cbs && !cbs->set) {
        XmToggleButtonSetState(w, True, False);
        return;
    }
    switch_to_tab((CkStatusTab)(intptr_t)client_data);
}

static void toggle_dnd_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)client_data;
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call_data;
    g_app.model.dnd_enabled = (cbs && cbs->set);
    refresh_all_ui();
}

static void toggle_wifi_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)client_data;
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call_data;
    ck_status_net_set_wifi_enabled(&g_app.model, cbs && cbs->set);
    ck_status_hw_poll(&g_app.model);
    refresh_all_ui();
}

static void toggle_bt_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)client_data;
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call_data;
    ck_status_bt_set_powered(&g_app.model, cbs && cbs->set);
    ck_status_hw_poll(&g_app.model);
    refresh_all_ui();
}

static void toggle_pin_gauge_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    (void)client_data;
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call_data;
    set_gauge_pinned_in_fp(cbs && cbs->set);
    refresh_all_ui();
}

static void mark_read_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data; (void)call_data;
    ck_status_model_mark_all_read(&g_app.model);
    refresh_all_ui();
}

static void clear_notifs_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data; (void)call_data;
    ck_status_model_clear_notifications(&g_app.model);
    refresh_all_ui();
}

static void send_test_notif_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data; (void)call_data;
    ck_status_model_add_notification(
        &g_app.model, 0, "System Status", "Workstation Status Check",
        "Desktop notifications and central Switch lamp are active.", 1, 4000
    );
    on_dbus_toast(&g_app.model.notifs[0], NULL);
    refresh_all_ui();
}

static void spawn_cmd_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    const char *cmd = (const char *)client_data;
    if (!cmd) return;
    if (fork() == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

static void bt_select_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data;
    XmListCallbackStruct *cbs = (XmListCallbackStruct *)call_data;
    if (cbs && cbs->item_position >= 1) {
        g_app.selected_bt_idx = cbs->item_position - 1;
    }
}

static void bt_connect_action_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    bool connect = ((intptr_t)client_data != 0);
    int idx = g_app.selected_bt_idx;
    if (idx >= 0 && idx < g_app.model.bt.device_count) {
        ck_status_bt_set_device_connected(g_app.model.bt.devices[idx].dbus_path, connect);
        ck_status_hw_poll(&g_app.model);
        refresh_all_ui();
    }
}

static void refresh_now_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data; (void)call_data;
    ck_status_hw_poll(&g_app.model);
    refresh_all_ui();
}

static void hide_window_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data; (void)call_data;
    if (g_app.toplevel && XtIsRealized(g_app.toplevel)) {
        g_app.daemon_hidden = true;
        XWithdrawWindow(g_app.dpy, XtWindow(g_app.toplevel), g_app.screen_num);
    }
}

static void quit_app_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data; (void)call_data;
    XtAppSetExitFlag(g_app.app_ctx);
}

static void about_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data; (void)call_data;
    static Widget about_shell = NULL;
    if (about_shell && XtIsWidget(about_shell)) {
        XtPopup(about_shell, XtGrabNone);
        return;
    }
    Widget nb = about_dialog_build(g_app.toplevel, "aboutStatus", "About System Status", &about_shell);
    if (nb) {
        about_add_standard_pages(
            nb, 1, "Status", "CK Core System Status & Notifications",
            "Workstation & Laptop Power, Wi-Fi, Bluetooth,\n"
            "Desktop Notifications & StatusNotifier Host for CK Core CDE.",
            True
        );
    }
    if (about_shell) XtPopup(about_shell, XtGrabNone);
}

static void dbus_fd_input_cb(XtPointer client_data, int *source, XtInputId *id) {
    (void)client_data; (void)source; (void)id;
    ck_status_dbus_step();
}

static void periodic_timer_cb(XtPointer client_data, XtIntervalId *id) {
    (void)client_data; (void)id;
    ck_status_dbus_step();
    ck_status_hw_poll(&g_app.model);
    refresh_all_ui();
    XtAppAddTimeOut(g_app.app_ctx, 3000UL, periodic_timer_cb, NULL);
}

/* -------------------------------------------------------------------------
 * Gauge Mode Input Handling (4-Quadrant Clicks + Right-Click Popup Menu)
 * ------------------------------------------------------------------------- */
static void launch_status_tab(const char *tab) {
    if (fork() == 0) {
        execlp("ck-status", "ck-status", "--tab", tab, (char *)NULL);
        _exit(127);
    }
}

static void unpin_self_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data; (void)call_data;
    set_gauge_pinned_in_fp(false);
    XtAppSetExitFlag(g_app.app_ctx);
}

static void gauge_button_eh(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont) {
    (void)client_data; (void)cont;
    if (!ev || ev->type != ButtonPress) return;
    if (ev->xbutton.button == Button3) {
        if (g_app.gauge_popup) {
            XmMenuPosition(g_app.gauge_popup, &ev->xbutton);
            XtManageChild(g_app.gauge_popup);
        }
        return;
    }
    if (ev->xbutton.button == Button1) {
        Dimension ww = 54, wh = 48;
        XtVaGetValues(w, XmNwidth, &ww, XmNheight, &wh, NULL);
        if (ev->xbutton.y >= (int)wh / 2) {
            launch_status_tab("power");
        } else if (ev->xbutton.x < (int)ww / 2) {
            launch_status_tab("network");
        } else if (ev->xbutton.x < ((int)ww * 3) / 4) {
            launch_status_tab("bluetooth");
        } else {
            launch_status_tab("notifications");
        }
    }
}

static void build_gauge_ui(void) {
    Widget form = XtVaCreateManagedWidget(
        "gaugeForm", xmFormWidgetClass, g_app.toplevel,
        XmNwidth, 74,
        XmNheight, 54,
        NULL
    );
    g_app.gauge_canvas = XtVaCreateManagedWidget(
        "gaugeCanvas", xmDrawingAreaWidgetClass, form,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNwidth, 74,
        XmNheight, 54,
        NULL
    );
    XtAddCallback(g_app.gauge_canvas, XmNexposeCallback, draw_gauge_cb, NULL);
    XtAddCallback(g_app.gauge_canvas, XmNresizeCallback, draw_gauge_cb, NULL);
    XtAddEventHandler(g_app.gauge_canvas, ButtonPressMask, False, gauge_button_eh, NULL);

    g_app.gauge_popup = XmCreatePopupMenu(g_app.gauge_canvas, "gaugePopup", NULL, 0);
    struct { const char *lbl; const char *cmd; } items[] = {
        {"Open System Status", "ck-status --tab overview"},
        {"Notifications & Tray", "ck-status --tab notifications"},
        {"Power & Battery", "ck-status --tab power"},
        {"Network & Wi-Fi", "ck-status --tab network"},
        {"Bluetooth", "ck-status --tab bluetooth"},
        {"Audio Mixer", "ck-mixer"},
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        XmString xs = XmStringCreateLocalized((char *)items[i].lbl);
        Widget b = XtVaCreateManagedWidget("gpItem", xmPushButtonGadgetClass, g_app.gauge_popup, XmNlabelString, xs, NULL);
        XmStringFree(xs);
        XtAddCallback(b, XmNactivateCallback, spawn_cmd_cb, (XtPointer)items[i].cmd);
    }
    XtVaCreateManagedWidget("gpSep", xmSeparatorGadgetClass, g_app.gauge_popup, NULL);
    XmString xun = XmStringCreateLocalized("Remove Gauge from Main Panel");
    Widget bun = XtVaCreateManagedWidget("gpUnpin", xmPushButtonGadgetClass, g_app.gauge_popup, XmNlabelString, xun, NULL);
    XmStringFree(xun);
    XtAddCallback(bun, XmNactivateCallback, unpin_self_cb, NULL);
}

/* -------------------------------------------------------------------------
 * Build Full Motif Workstation & Laptop Status Console
 * ------------------------------------------------------------------------- */
static Widget add_menu_item(Widget menu, const char *label, XtCallbackProc cb, XtPointer data) {
    XmString xs = XmStringCreateLocalized((char *)label);
    Widget btn = XtVaCreateManagedWidget("menuBtn", xmPushButtonGadgetClass, menu, XmNlabelString, xs, NULL);
    XmStringFree(xs);
    if (cb) XtAddCallback(btn, XmNactivateCallback, cb, data);
    return btn;
}

static void build_console_ui(void) {
    g_app.main_win = XtVaCreateManagedWidget("mainWin", xmMainWindowWidgetClass, g_app.toplevel, NULL);

    /* Menu Bar */
    Widget mbar = XmCreateMenuBar(g_app.main_win, "menuBar", NULL, 0);
    XtManageChild(mbar);

    Widget file_pd = XmCreatePulldownMenu(mbar, "filePD", NULL, 0);
    XtVaCreateManagedWidget("File", xmCascadeButtonWidgetClass, mbar, XmNsubMenuId, file_pd, NULL);
    add_menu_item(file_pd, "Refresh Now", refresh_now_cb, NULL);
    add_menu_item(file_pd, "Send Test Notification", send_test_notif_cb, NULL);
    XtVaCreateManagedWidget("sep1", xmSeparatorGadgetClass, file_pd, NULL);
    add_menu_item(file_pd, "Hide to Background Service", hide_window_cb, NULL);
    add_menu_item(file_pd, "Exit", quit_app_cb, NULL);

    Widget panel_pd = XmCreatePulldownMenu(mbar, "panelPD", NULL, 0);
    XtVaCreateManagedWidget("Panel", xmCascadeButtonWidgetClass, mbar, XmNsubMenuId, panel_pd, NULL);
    add_menu_item(panel_pd, "Clear Unread Switch Lamp", mark_read_cb, NULL);
    add_menu_item(panel_pd, "Audio Mixer (ck-mixer)", spawn_cmd_cb, (XtPointer)"ck-mixer");
    add_menu_item(panel_pd, "Display & Monitors (dtstyle)", spawn_cmd_cb, (XtPointer)"dtstyle");

    Widget help_pd = XmCreatePulldownMenu(mbar, "helpPD", NULL, 0);
    Widget help_cas = XtVaCreateManagedWidget("Help", xmCascadeButtonWidgetClass, mbar, XmNsubMenuId, help_pd, NULL);
    XtVaSetValues(mbar, XmNmenuHelpWidget, help_cas, NULL);
    add_menu_item(help_pd, "About System Status...", about_cb, NULL);

    /* Main Work Form */
    Widget work = XtVaCreateManagedWidget(
        "workForm", xmFormWidgetClass, g_app.main_win,
        XmNwidth, 660,
        XmNheight, 440,
        NULL
    );
    XtVaSetValues(g_app.main_win, XmNmenuBar, mbar, XmNworkWindow, work, NULL);

    /* Top Instrument Strip */
    Widget hdr_frame = XtVaCreateManagedWidget(
        "hdrFrame", xmFrameWidgetClass, work,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNtopOffset, 8,
        XmNleftOffset, 10,
        XmNrightOffset, 10,
        XmNshadowType, XmSHADOW_OUT,
        NULL
    );
    g_app.header_canvas = XtVaCreateManagedWidget(
        "headerCanvas", xmDrawingAreaWidgetClass, hdr_frame,
        XmNheight, 58,
        NULL
    );
    XtAddCallback(g_app.header_canvas, XmNexposeCallback, draw_header_strip_cb, NULL);
    XtAddCallback(g_app.header_canvas, XmNresizeCallback, draw_header_strip_cb, NULL);
    XtAddEventHandler(g_app.header_canvas, ButtonPressMask, False, header_strip_click_eh, NULL);

    /* Segmented Tab Bar */
    Widget tab_rc = XtVaCreateManagedWidget(
        "tabBar", xmRowColumnWidgetClass, work,
        XmNorientation, XmHORIZONTAL,
        XmNpacking, XmPACK_TIGHT,
        XmNspacing, 4,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, hdr_frame,
        XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        XmNleftOffset, 10,
        NULL
    );
    const char *tab_names[CK_TAB_COUNT] = {
        "Overview", "Notifications & Tray", "Power & Battery", "Network & Wi-Fi", "Bluetooth"
    };
    for (int i = 0; i < CK_TAB_COUNT; ++i) {
        XmString xs = XmStringCreateLocalized((char *)tab_names[i]);
        g_app.tab_buttons[i] = XtVaCreateManagedWidget(
            "tabToggle", xmToggleButtonWidgetClass, tab_rc,
            XmNlabelString, xs,
            XmNindicatorOn, False,
            XmNshadowThickness, 2,
            XmNmarginWidth, 10,
            XmNmarginHeight, 4,
            NULL
        );
        XmStringFree(xs);
        XtAddCallback(g_app.tab_buttons[i], XmNvalueChangedCallback, tab_btn_cb, (XtPointer)(intptr_t)i);
    }

    Widget page_frame = XtVaCreateManagedWidget(
        "pageFrame", xmFrameWidgetClass, work,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, tab_rc,
        XmNtopOffset, 6,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNbottomOffset, 10,
        XmNleftAttachment, XmATTACH_FORM,
        XmNleftOffset, 10,
        XmNrightAttachment, XmATTACH_FORM,
        XmNrightOffset, 10,
        XmNshadowType, XmSHADOW_ETCHED_IN,
        NULL
    );
    Widget deck = XtVaCreateManagedWidget("pageDeck", xmFormWidgetClass, page_frame, NULL);

    for (int i = 0; i < CK_TAB_COUNT; ++i) {
        g_app.tab_pages[i] = XtVaCreateWidget(
            "tabPage", xmFormWidgetClass, deck,
            XmNtopAttachment, XmATTACH_FORM,
            XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM,
            XmNmarginWidth, 12,
            XmNmarginHeight, 10,
            NULL
        );
    }

    /* --- Page 0: Overview --- */
    Widget p0_rc = XtVaCreateManagedWidget(
        "ovRC", xmRowColumnWidgetClass, g_app.tab_pages[CK_TAB_OVERVIEW],
        XmNorientation, XmVERTICAL,
        XmNspacing, 8,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    g_app.ov_summary_lbl = XtVaCreateManagedWidget(
        "ovSummary", xmLabelWidgetClass, p0_rc,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL
    );
    XtVaCreateManagedWidget("ovSep", xmSeparatorGadgetClass, p0_rc, NULL);
    XmString x_pin = XmStringCreateLocalized("Pin Live 4-Quadrant Status Gauge in Main Front Panel (~/.dt/types)");
    g_app.ov_pin_toggle = XtVaCreateManagedWidget("ovPin", xmToggleButtonWidgetClass, p0_rc, XmNlabelString, x_pin, NULL);
    XmStringFree(x_pin);
    XtAddCallback(g_app.ov_pin_toggle, XmNvalueChangedCallback, toggle_pin_gauge_cb, NULL);

    XmString x_dnd = XmStringCreateLocalized("Do Not Disturb (suppress non-critical popup banners)");
    g_app.ov_dnd_toggle = XtVaCreateManagedWidget("ovDnd", xmToggleButtonWidgetClass, p0_rc, XmNlabelString, x_dnd, NULL);
    XmStringFree(x_dnd);
    XtAddCallback(g_app.ov_dnd_toggle, XmNvalueChangedCallback, toggle_dnd_cb, NULL);

    XmString x_wf = XmStringCreateLocalized("Enable Wi-Fi Radio (NetworkManager)");
    g_app.ov_wifi_toggle = XtVaCreateManagedWidget("ovWifi", xmToggleButtonWidgetClass, p0_rc, XmNlabelString, x_wf, NULL);
    XmStringFree(x_wf);
    XtAddCallback(g_app.ov_wifi_toggle, XmNvalueChangedCallback, toggle_wifi_cb, NULL);

    XmString x_bt = XmStringCreateLocalized("Enable Bluetooth Adapter (BlueZ)");
    g_app.ov_bt_toggle = XtVaCreateManagedWidget("ovBt", xmToggleButtonWidgetClass, p0_rc, XmNlabelString, x_bt, NULL);
    XmStringFree(x_bt);
    XtAddCallback(g_app.ov_bt_toggle, XmNvalueChangedCallback, toggle_bt_cb, NULL);

    /* --- Page 1: Notifications & StatusNotifier Tray --- */
    Widget p1 = g_app.tab_pages[CK_TAB_NOTIFICATIONS];
    Widget sni_hdr = XtVaCreateManagedWidget(
        "sniHdr", xmLabelWidgetClass, p1,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL
    );
    set_label_text(sni_hdr, "Active Background System Tray (StatusNotifierWatcher):");
    Widget sni_frame = XtVaCreateManagedWidget(
        "sniFrame", xmFrameWidgetClass, p1,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, sni_hdr,
        XmNtopOffset, 4,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNshadowType, XmSHADOW_IN,
        NULL
    );
    g_app.sni_strip_rc = XtVaCreateManagedWidget(
        "sniStrip", xmRowColumnWidgetClass, sni_frame,
        XmNorientation, XmHORIZONTAL,
        XmNpacking, XmPACK_TIGHT,
        XmNspacing, 6,
        XmNmarginHeight, 6,
        XmNmarginWidth, 8,
        NULL
    );
    g_app.sni_empty_lbl = XtVaCreateManagedWidget("sniEmpty", xmLabelWidgetClass, g_app.sni_strip_rc, NULL);
    set_label_text(g_app.sni_empty_lbl, "(No background tray apps currently registered)");

    g_app.notif_count_lbl = XtVaCreateManagedWidget(
        "notifCnt", xmLabelWidgetClass, p1,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, sni_frame,
        XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        NULL
    );
    Widget nbtn_rc = XtVaCreateManagedWidget(
        "nbtnRC", xmRowColumnWidgetClass, p1,
        XmNorientation, XmHORIZONTAL,
        XmNspacing, 6,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        NULL
    );
    XmString x_nd = XmStringCreateLocalized("Do Not Disturb");
    g_app.notif_dnd_toggle = XtVaCreateManagedWidget("nDnd", xmToggleButtonWidgetClass, nbtn_rc, XmNlabelString, x_nd, NULL);
    XmStringFree(x_nd);
    XtAddCallback(g_app.notif_dnd_toggle, XmNvalueChangedCallback, toggle_dnd_cb, NULL);

    XmString x_mr = XmStringCreateLocalized("Mark All Read");
    Widget b_mr = XtVaCreateManagedWidget("bMr", xmPushButtonWidgetClass, nbtn_rc, XmNlabelString, x_mr, NULL);
    XmStringFree(x_mr);
    XtAddCallback(b_mr, XmNactivateCallback, mark_read_cb, NULL);

    XmString x_cl = XmStringCreateLocalized("Clear History");
    Widget b_cl = XtVaCreateManagedWidget("bCl", xmPushButtonWidgetClass, nbtn_rc, XmNlabelString, x_cl, NULL);
    XmStringFree(x_cl);
    XtAddCallback(b_cl, XmNactivateCallback, clear_notifs_cb, NULL);

    g_app.notif_list = XmCreateScrolledList(p1, "notifList", NULL, 0);
    XtVaSetValues(
        XtParent(g_app.notif_list),
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, g_app.notif_count_lbl,
        XmNtopOffset, 4,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, nbtn_rc,
        XmNbottomOffset, 6,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    XtManageChild(g_app.notif_list);

    /* --- Page 2: Power & Battery --- */
    Widget p2 = g_app.tab_pages[CK_TAB_POWER];
    g_app.pwr_canvas = XtVaCreateManagedWidget(
        "pwrCanvas", xmDrawingAreaWidgetClass, p2,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNheight, 64,
        NULL
    );
    XtAddCallback(g_app.pwr_canvas, XmNexposeCallback, draw_power_gauge_cb, NULL);
    XtAddCallback(g_app.pwr_canvas, XmNresizeCallback, draw_power_gauge_cb, NULL);
    g_app.pwr_detail_lbl = XtVaCreateManagedWidget(
        "pwrDetail", xmLabelWidgetClass, p2,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, g_app.pwr_canvas,
        XmNtopOffset, 12,
        XmNleftAttachment, XmATTACH_FORM,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL
    );

    /* --- Page 3: Network & Wi-Fi --- */
    Widget p3 = g_app.tab_pages[CK_TAB_NETWORK];
    g_app.net_detail_lbl = XtVaCreateManagedWidget(
        "netDetail", xmLabelWidgetClass, p3,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL
    );
    Widget net_bar = XtVaCreateManagedWidget(
        "netBar", xmRowColumnWidgetClass, p3,
        XmNorientation, XmHORIZONTAL,
        XmNspacing, 8,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        NULL
    );
    XmString x_nwf = XmStringCreateLocalized("Wi-Fi Radio Enabled");
    g_app.net_wifi_toggle = XtVaCreateManagedWidget("nWf", xmToggleButtonWidgetClass, net_bar, XmNlabelString, x_nwf, NULL);
    XmStringFree(x_nwf);
    XtAddCallback(g_app.net_wifi_toggle, XmNvalueChangedCallback, toggle_wifi_cb, NULL);

    XmString x_nmt = XmStringCreateLocalized("Configure Connections...");
    Widget b_nmt = XtVaCreateManagedWidget("bNmt", xmPushButtonWidgetClass, net_bar, XmNlabelString, x_nmt, NULL);
    XmStringFree(x_nmt);
    XtAddCallback(b_nmt, XmNactivateCallback, spawn_cmd_cb,
                  (XtPointer)"command -v nm-connection-editor >/dev/null 2>&1 && exec nm-connection-editor || exec dtterm -e nmtui");

    g_app.net_ap_list = XmCreateScrolledList(p3, "netApList", NULL, 0);
    XtVaSetValues(
        XtParent(g_app.net_ap_list),
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, g_app.net_detail_lbl,
        XmNtopOffset, 8,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, net_bar,
        XmNbottomOffset, 6,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    XtManageChild(g_app.net_ap_list);

    /* --- Page 4: Bluetooth --- */
    Widget p4 = g_app.tab_pages[CK_TAB_BLUETOOTH];
    g_app.bt_detail_lbl = XtVaCreateManagedWidget(
        "btDetail", xmLabelWidgetClass, p4,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL
    );
    Widget bt_bar = XtVaCreateManagedWidget(
        "btBar", xmRowColumnWidgetClass, p4,
        XmNorientation, XmHORIZONTAL,
        XmNspacing, 8,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        NULL
    );
    XmString x_btp = XmStringCreateLocalized("Adapter Powered");
    g_app.bt_power_toggle = XtVaCreateManagedWidget("btPwr", xmToggleButtonWidgetClass, bt_bar, XmNlabelString, x_btp, NULL);
    XmStringFree(x_btp);
    XtAddCallback(g_app.bt_power_toggle, XmNvalueChangedCallback, toggle_bt_cb, NULL);

    XmString x_btc = XmStringCreateLocalized("Connect Selected");
    Widget b_btc = XtVaCreateManagedWidget("btConn", xmPushButtonWidgetClass, bt_bar, XmNlabelString, x_btc, NULL);
    XmStringFree(x_btc);
    XtAddCallback(b_btc, XmNactivateCallback, bt_connect_action_cb, (XtPointer)1);

    XmString x_btd = XmStringCreateLocalized("Disconnect Selected");
    Widget b_btd = XtVaCreateManagedWidget("btDisc", xmPushButtonWidgetClass, bt_bar, XmNlabelString, x_btd, NULL);
    XmStringFree(x_btd);
    XtAddCallback(b_btd, XmNactivateCallback, bt_connect_action_cb, (XtPointer)0);

    g_app.bt_dev_list = XmCreateScrolledList(p4, "btDevList", NULL, 0);
    XtVaSetValues(
        XtParent(g_app.bt_dev_list),
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, g_app.bt_detail_lbl,
        XmNtopOffset, 8,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, bt_bar,
        XmNbottomOffset, 6,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL
    );
    XtAddCallback(g_app.bt_dev_list, XmNbrowseSelectionCallback, bt_select_cb, NULL);
    XtManageChild(g_app.bt_dev_list);

    switch_to_tab(g_app.current_tab);
}

static CkStatusTab parse_tab_arg(const char *s) {
    if (!s) return CK_TAB_OVERVIEW;
    if (strcasecmp(s, "notifications") == 0 || strcasecmp(s, "tray") == 0 || strcasecmp(s, "notify") == 0)
        return CK_TAB_NOTIFICATIONS;
    if (strcasecmp(s, "power") == 0 || strcasecmp(s, "battery") == 0)
        return CK_TAB_POWER;
    if (strcasecmp(s, "network") == 0 || strcasecmp(s, "wifi") == 0)
        return CK_TAB_NETWORK;
    if (strcasecmp(s, "bluetooth") == 0 || strcasecmp(s, "bt") == 0)
        return CK_TAB_BLUETOOTH;
    return CK_TAB_OVERVIEW;
}

static bool bus_name_has_owner(const char *name) {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *c = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!c) {
        dbus_error_free(&err);
        return false;
    }
    dbus_bool_t owned = dbus_bus_name_has_owner(c, name, &err);
    dbus_error_free(&err);
    dbus_connection_unref(c);
    return owned != FALSE;
}

int main(int argc, char **argv) {
    signal(SIGCHLD, SIG_IGN);

    const char *req_tab_str = "overview";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--gauge") == 0 || strcmp(argv[i], "--panel") == 0) {
            g_app.gauge_mode = true;
        } else if (strcmp(argv[i], "--daemon") == 0) {
            g_app.daemon_hidden = true;
        } else if (strcmp(argv[i], "--standalone") == 0) {
            g_app.standalone = true;
        } else if (strcmp(argv[i], "--pin-gauge") == 0) {
            set_gauge_pinned_in_fp(true);
            return 0;
        } else if (strcmp(argv[i], "--unpin-gauge") == 0) {
            set_gauge_pinned_in_fp(false);
            return 0;
        } else if (strcmp(argv[i], "--tab") == 0 && i + 1 < argc) {
            req_tab_str = argv[++i];
            g_app.current_tab = parse_tab_arg(req_tab_str);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: ck-status [--tab overview|notifications|power|network|bluetooth] [--gauge] [--daemon] [--pin-gauge|--unpin-gauge]\n");
            return 0;
        }
    }

    /* Single-instance handoff for interactive invocations when daemon is already active */
    if (!g_app.gauge_mode && !g_app.daemon_hidden && !g_app.standalone &&
        bus_name_has_owner("org.ckcore.Status")) {
        ck_status_dbus_raise_existing(req_tab_str);
        return 0;
    }

    const char *app_name = g_app.gauge_mode ? "ck-status-gauge" : "ck-status";
    const char *win_title = g_app.gauge_mode ? "System Status" : "System Status & Notifications";
    if (g_app.gauge_mode && argc > 0) {
        argv[0] = (char *)"ck-status-gauge";
    }

    XtSetLanguageProc(NULL, NULL, NULL);
    g_app.toplevel = XtVaOpenApplication(
        &g_app.app_ctx,
        "CkStatus",
        NULL, 0,
        &argc, argv,
        NULL,
        sessionShellWidgetClass,
        XmNtitle, win_title,
        XmNiconName, "Status",
        XmNmappedWhenManaged, (g_app.daemon_hidden || g_app.gauge_mode) ? False : True,
        NULL
    );

    g_app.dpy = XtDisplay(g_app.toplevel);
    g_app.screen_num = DefaultScreen(g_app.dpy);
    g_app.visual = DefaultVisual(g_app.dpy, g_app.screen_num);
    g_app.cmap = DefaultColormap(g_app.dpy, g_app.screen_num);
    g_app.depth = DefaultDepth(g_app.dpy, g_app.screen_num);

    ck_status_hw_poll(&g_app.model);
    if (!g_app.gauge_mode) {
        ck_status_set_raise_callback(on_dbus_raise_tab);
        ck_status_dbus_init(&g_app.model, on_dbus_toast, on_dbus_state_changed, NULL);
        build_console_ui();
    } else {
        build_gauge_ui();
    }

    XtRealizeWidget(g_app.toplevel);
    refresh_theme_colors(g_app.toplevel);

    /* Ensure WM_CLASS res_name matches ck-status-gauge BEFORE MapNotify for .fp TYPE client */
    XClassHint ch = {
        .res_name = (char *)app_name,
        .res_class = (char *)"CkStatus",
    };
    XSetClassHint(g_app.dpy, XtWindow(g_app.toplevel), &ch);
    XSync(g_app.dpy, False);
    if (g_app.gauge_mode) {
        XtMapWidget(g_app.toplevel);
        XFlush(g_app.dpy);
    }

    Atom wm_del = XInternAtom(g_app.dpy, "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(g_app.toplevel, wm_del,
                            g_app.gauge_mode ? quit_app_cb : hide_window_cb, NULL);

    int dbus_fd = ck_status_dbus_get_fd();
    if (dbus_fd >= 0) {
        XtAppAddInput(g_app.app_ctx, dbus_fd, (XtPointer)XtInputReadMask, dbus_fd_input_cb, NULL);
    }

    refresh_all_ui();
    XtAppAddTimeOut(g_app.app_ctx, 3000UL, periodic_timer_cb, NULL);

    XtAppMainLoop(g_app.app_ctx);

    if (!g_app.gauge_mode) {
        ck_status_dbus_shutdown(&g_app.model);
    }
    return 0;
}
