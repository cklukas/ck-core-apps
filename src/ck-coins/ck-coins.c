/*
 * ck-coins.c
 *
 * Small CDE/Motif app that periodically fetches coin prices (CoinGecko)
 * and renders the selected coin + last check time + price into the WM icon.
 *
 * Icon updates continue regardless of whether the main window is open/iconified.
 *
 * Build (example):
 *   cc -O2 -Wall -Wextra -o ck-coins ck-coins.c \
 *      -lXm -lXt -lX11 -lDtSvc -lcurl
 *
 * Notes:
 * - Uses XDrawString (core X11 fonts). Tries "6x10", then "fixed".
 * - Keeps networking simple (blocking curl) but uses short timeouts.
 * - Stores selected index and window geometry via your session_utils.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>

#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/List.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/CascadeB.h>
#include <Xm/DialogS.h>
#include <Xm/Notebook.h>
#include <Xm/PushBG.h>
#include <Xm/MwmUtil.h>
#include <Xm/MainW.h>
#include <Xm/SeparatoG.h>

#include <Dt/Session.h>

#include <curl/curl.h>
#include <Xm/Protocols.h>
#include "../shared/session_utils.h"
#include "../shared/about_dialog.h"

/* ---------- config ---------- */

#define FETCH_INTERVAL_MS     (15 * 60 * 1000)  /* 15 minutes */
#define ICON_SIZE_FALLBACK    48
#define CURL_CONNECT_TO_S     5L
#define CURL_TOTAL_TIMEOUT_S  10L
#define CACHE_TTL_SEC         (15 * 60)

/* ---------- data model ---------- */

typedef struct {
    char   id[64];      /* coingecko id, e.g. "bitcoin" */
    char   name[64];    /* display name, e.g. "Bitcoin" */
    char   symbol[16];  /* short symbol, e.g. "BTC" */
    double price_usd;
    time_t updated_at_utc;   /* from API: unix seconds */
    int    has_data;
} Coin;

/* ---------- globals ---------- */

static XtAppContext app_context;

static Widget  g_toplevel   = NULL;
static Widget  g_form       = NULL;
static Widget  g_mainw      = NULL;
static Widget  g_list       = NULL;
static Widget  g_info_label = NULL;
static Widget  g_refresh_btn= NULL;
static Widget  g_close_btn  = NULL;
static Widget  g_menubar    = NULL;

static Display *g_display   = NULL;

static Pixmap g_icon_pixmap = None;
static Pixmap g_icon_mask   = None;

static GC g_icon_gc         = None;
static GC g_icon_mask_gc    = None;

typedef struct {
    int size;
    XftFont *font;
} XftFontCacheEntry;

static XftFontCacheEntry g_xft_fonts[64];
static int g_xft_font_count = 0;
static int g_debug_icon_cached = -1;

static int g_icon_w = ICON_SIZE_FALLBACK;
static int g_icon_h = ICON_SIZE_FALLBACK;

static Coin *g_coins = NULL;
static int   g_coin_count = 0;
static int   g_selected = 0;

static time_t g_last_fetch_received_local = 0; /* local wallclock time when response processed */
static int    g_last_fetch_ok = 0;

static char g_exec_path[PATH_MAX] = "ck-coins";
static SessionData *session_data = NULL;
static int g_session_loaded = 0;

static Widget g_about_shell = NULL;

/* ---------- utilities ---------- */

static void init_exec_path(const char *argv0)
{
    ssize_t len = readlink("/proc/self/exe", g_exec_path, sizeof(g_exec_path) - 1);
    if (len > 0) { g_exec_path[len] = '\0'; return; }

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

static void ensure_gc(Display *display, Drawable drawable, GC *gc_out)
{
    if (*gc_out != None) return;
    XGCValues values;
    values.foreground = BlackPixel(display, DefaultScreen(display));
    values.background = WhitePixel(display, DefaultScreen(display));
    *gc_out = XCreateGC(display, drawable, GCForeground | GCBackground, &values);
    XSetLineAttributes(display, *gc_out, 1, LineSolid, CapRound, JoinRound);
}

static int xft_font_height(const XftFont *font);

static int icon_debug_enabled(void)
{
    if (g_debug_icon_cached >= 0) return g_debug_icon_cached;
    const char *env = getenv("CK_COINS_DEBUG_ICON");
    g_debug_icon_cached = (env && env[0]) ? 1 : 0;
    return g_debug_icon_cached;
}

static void update_icon_size_from_wm(Display *display)
{
    if (!display) return;
    int count = 0;
    XIconSize *sizes = NULL;
    if (!XGetIconSizes(display, RootWindow(display, DefaultScreen(display)), &sizes, &count) ||
        !sizes || count <= 0) {
        if (sizes) XFree(sizes);
        return;
    }

    int best_w = ICON_SIZE_FALLBACK;
    int best_h = ICON_SIZE_FALLBACK;

    for (int i = 0; i < count; ++i) {
        int w = sizes[i].max_width  > 0 ? sizes[i].max_width  : sizes[i].min_width;
        int h = sizes[i].max_height > 0 ? sizes[i].max_height : sizes[i].min_height;
        if (w > best_w) best_w = w;
        if (h > best_h) best_h = h;
    }

    if (best_w > 0) g_icon_w = best_w;
    if (best_h > 0) g_icon_h = best_h;

    XFree(sizes);
}

static void update_wm_icon_pixmap(Display *display, Window window, Pixmap pixmap)
{
    if (!display || !window || !pixmap) return;
    XWMHints *hints = XGetWMHints(display, window);
    XWMHints local;
    if (hints) { local = *hints; XFree(hints); }
    else { memset(&local, 0, sizeof(local)); }

    local.flags |= IconPixmapHint;
    local.icon_pixmap = pixmap;

    if (g_icon_mask != None) {
        local.flags |= IconMaskHint;
        local.icon_mask = g_icon_mask;
    }

    XSetWMHints(display, window, &local);
}

/* format HH:MM in local time */
static void fmt_hhmm_local(time_t t, char out[8])
{
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, 8, "%H:%M", &tmv);
}

/* format integer with '.' thousands separators (German-ish) */
static void format_int_thousands(long long val, char *out, size_t out_sz)
{
    char buf[64];
    int neg = (val < 0);
    unsigned long long u = (unsigned long long)(neg ? -val : val);

    snprintf(buf, sizeof(buf), "%llu", u);

    size_t len = strlen(buf);
    char tmp[96];
    size_t ti = 0;

    if (neg && ti + 1 < sizeof(tmp)) tmp[ti++] = '-';

    /* insert dots from left, based on remaining digits */
    for (size_t i = 0; i < len; ++i) {
        size_t remaining = len - i;
        if (i > 0 && (remaining % 3) == 0) {
            if (ti + 1 < sizeof(tmp)) tmp[ti++] = '.';
        }
        if (ti + 1 < sizeof(tmp)) tmp[ti++] = buf[i];
    }
    tmp[ti] = '\0';

    snprintf(out, out_sz, "%s", tmp);
}

/* price formatting:
 * - >= 1000 : integer, thousands '.' (e.g. 89.323$)
 * - >= 1    : 2 decimals
 * - < 1     : 6 decimals
 */
static void format_price_usd(double price, char *out, size_t out_sz)
{
    if (!isfinite(price)) {
        snprintf(out, out_sz, "n/a");
        return;
    }

    if (price >= 1000.0) {
        long long iv = (long long)llround(price);
        char tmp[96];
        format_int_thousands(iv, tmp, sizeof(tmp));
        snprintf(out, out_sz, "%.*s$", (int)(out_sz > 2 ? out_sz - 2 : 0), tmp);
    } else if (price >= 1.0) {
        snprintf(out, out_sz, "%.2f$", price);
    } else {
        snprintf(out, out_sz, "%.6f$", price);
    }
}

/* same as format_price_usd, but without currency suffix */
static void format_price_value(double price, char *out, size_t out_sz)
{
    if (!isfinite(price)) {
        snprintf(out, out_sz, "n/a");
        return;
    }

    if (price >= 1000.0) {
        long long iv = (long long)llround(price);
        char tmp[96];
        format_int_thousands(iv, tmp, sizeof(tmp));
        snprintf(out, out_sz, "%.*s", (int)(out_sz > 0 ? out_sz - 1 : 0), tmp);
    } else if (price >= 1.0) {
        snprintf(out, out_sz, "%.2f", price);
    } else {
        snprintf(out, out_sz, "%.6f", price);
    }
}
/* truncate left text to fit width, add "..." if needed */
static int xft_text_width(Display *dpy, XftFont *font, const char *text)
{
    if (!font || !text || !text[0]) return 0;
    XGlyphInfo ext;
    XftTextExtentsUtf8(dpy, font, (const FcChar8 *)text,
                       (int)strlen(text), &ext);
    return (int)ext.xOff;
}

static XftFont *xft_open_font_family(Display *dpy, int screen,
                                     const char *family, int size_px)
{
    char name[192];
    snprintf(name, sizeof(name),
             "%s:pixelsize=%d:antialias=true:hinting=true:hintstyle=hintslight",
             family, size_px);
    return XftFontOpenName(dpy, screen, name);
}

static XftFont *xft_open_font(Display *dpy, int screen, int size_px)
{
    const char *families[] = {"Sans", "Sans Serif", "DejaVu Sans", "monospace"};
    for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); ++i) {
        XftFont *font = xft_open_font_family(dpy, screen, families[i], size_px);
        if (font) return font;
    }
    return NULL;
}

static XftFont *xft_pick_font_for_slot(Display *dpy, int screen,
                                       const char *text, int max_w, int max_h,
                                       int max_px, int min_px, int *out_size)
{
    if (max_px < min_px) max_px = min_px;
    for (int size = max_px; size >= min_px; --size) {
        XftFont *font = xft_open_font(dpy, screen, size);
        if (!font) continue;
        int h = xft_font_height(font);
        int w = xft_text_width(dpy, font, text);
        if (h <= max_h && w <= max_w) {
            if (out_size) *out_size = size;
            return font;
        }
        XftFontClose(dpy, font);
    }
    if (out_size) *out_size = min_px;
    return xft_open_font(dpy, screen, min_px);
}

/* ---------- curl + parsing ---------- */

typedef struct {
    char  *data;
    size_t len;
} MemBuf;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t n = size * nmemb;
    MemBuf *mb = (MemBuf *)userdata;
    char *p = (char *)realloc(mb->data, mb->len + n + 1);
    if (!p) return 0;
    mb->data = p;
    memcpy(mb->data + mb->len, ptr, n);
    mb->len += n;
    mb->data[mb->len] = '\0';
    return n;
}

static int http_get_json(const char *url, char **out_json, size_t *out_len)
{
    if (!url || !out_json || !out_len) return 0;

    *out_json = NULL;
    *out_len = 0;

    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    MemBuf mb;
    mb.data = NULL;
    mb.len = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mb);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ck-coins/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); /* allow gzip/deflate */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CURL_CONNECT_TO_S);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, CURL_TOTAL_TIMEOUT_S);

    CURLcode res = curl_easy_perform(curl);

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    curl_easy_cleanup(curl);

    if (res != CURLE_OK || code < 200 || code >= 300 || !mb.data || mb.len == 0) {
        free(mb.data);
        return 0;
    }

    *out_json = mb.data;
    *out_len = mb.len;
    return 1;
}

static const char *skip_ws(const char *p)
{
    while (p && *p && isspace((unsigned char)*p)) p++;
    return p;
}

static int parse_coin_from_json(const char *json, const char *id, double *out_price, time_t *out_ts)
{
    if (!json || !id || !out_price || !out_ts) return 0;

    char keypat[128];
    snprintf(keypat, sizeof(keypat), "\"%s\":{", id);

    const char *p = strstr(json, keypat);
    if (!p) return 0;

    const char *usd = strstr(p, "\"usd\":");
    if (!usd) return 0;
    usd += (int)strlen("\"usd\":");
    usd = skip_ws(usd);

    char *endp = NULL;
    double price = strtod(usd, &endp);
    if (!endp || endp == usd) return 0;

    const char *lu = strstr(endp, "\"last_updated_at\":");
    if (!lu) return 0;
    lu += (int)strlen("\"last_updated_at\":");
    lu = skip_ws(lu);

    errno = 0;
    long long ts = strtoll(lu, &endp, 10);
    if (errno != 0 || !endp || endp == lu) return 0;

    *out_price = price;
    *out_ts = (time_t)ts;
    return 1;
}

/* ---------- UI helpers ---------- */

static void set_label_text(Widget w, const char *text)
{
    if (!w) return;
    XmString xs = XmStringCreateLtoR((char *)(text ? text : ""), XmFONTLIST_DEFAULT_TAG);
    XtVaSetValues(w, XmNlabelString, xs, NULL);
    XmStringFree(xs);
}

static void rebuild_list_items(void)
{
    if (!g_list || g_coin_count <= 0) return;

    XmListDeleteAllItems(g_list);

    XmFontList fl = NULL;
    XtVaGetValues(g_list, XmNfontList, &fl, NULL);
    Dimension max_w = 0;

    for (int i = 0; i < g_coin_count; ++i) {
        char pricebuf[64];
        if (g_coins[i].has_data) format_price_usd(g_coins[i].price_usd, pricebuf, sizeof(pricebuf));
        else snprintf(pricebuf, sizeof(pricebuf), "n/a");

        char row[256];
        /* Example row: "Bitcoin (BTC)   89.323$" */
        snprintf(row, sizeof(row), "%s (%s)   %s",
                 g_coins[i].name[0] ? g_coins[i].name : g_coins[i].id,
                 g_coins[i].symbol[0] ? g_coins[i].symbol : "",
                 pricebuf);

        XmString xs = XmStringCreateLtoR(row, XmFONTLIST_DEFAULT_TAG);
        if (fl) {
            Dimension w = 0, h = 0;
            XmStringExtent(fl, xs, &w, &h);
            if (w > max_w) max_w = w;
        }
        XmListAddItem(g_list, xs, 0);
        XmStringFree(xs);
    }

    Dimension info_w = 0;
    if (g_info_label) {
        XmString info_xs = NULL;
        XtVaGetValues(g_info_label, XmNlabelString, &info_xs, NULL);
        if (info_xs && fl) {
            Dimension iw = 0, ih = 0;
            XmStringExtent(fl, info_xs, &iw, &ih);
            info_w = iw;
        }
    }

    if (!g_session_loaded && max_w > 0) {
        Dimension list_w = max_w + 24;
        Widget list_parent = XtParent(g_list);
        XtVaSetValues(g_list, XmNwidth, list_w, NULL);
        XtVaSetValues(list_parent, XmNwidth, list_w, NULL);
        if (g_info_label) {
            Dimension right_w = info_w > 0 ? (info_w + 24) : list_w;
            XtVaSetValues(g_info_label, XmNwidth, right_w, NULL);
            XtVaSetValues(XtParent(g_info_label), XmNwidth, right_w, NULL);
        }
        Dimension total_w = list_w + (info_w > 0 ? (info_w + 24) : list_w) + 40;
        Dimension total_h = 440;
        XtVaSetValues(g_toplevel,
                      XmNwidth, total_w,
                      XmNheight, total_h,
                      NULL);
    }

    int sel = g_selected;
    if (sel < 0) sel = 0;
    if (sel >= g_coin_count) sel = g_coin_count - 1;
    g_selected = sel;

    XmListSelectPos(g_list, g_selected + 1, False);
    XmListSetPos(g_list, g_selected + 1);
}

static void update_info_label_for_selected(void)
{
    if (g_coin_count <= 0) return;
    Coin *c = &g_coins[g_selected];

    char hhmm[8] = "--:--";
    if (g_last_fetch_received_local > 0) fmt_hhmm_local(g_last_fetch_received_local, hhmm);

    char pricebuf[64];
    if (c->has_data) format_price_usd(c->price_usd, pricebuf, sizeof(pricebuf));
    else snprintf(pricebuf, sizeof(pricebuf), "n/a");

    char upd_local[32] = "";
    if (c->has_data && c->updated_at_utc > 0) {
        struct tm tmv;
        time_t t = c->updated_at_utc;
        localtime_r(&t, &tmv);
        strftime(upd_local, sizeof(upd_local), "%Y-%m-%d %H:%M:%S %z", &tmv);
    } else {
        snprintf(upd_local, sizeof(upd_local), "n/a");
    }

    char buf[512];
    snprintf(buf, sizeof(buf),
             "%s (%s)\n"
             "Last check (local): %s   (%s)\n"
             "Price (USD): %s\n"
             "API updated_at (local): %s",
             c->name[0] ? c->name : c->id,
             c->symbol[0] ? c->symbol : "",
             hhmm,
             g_last_fetch_ok ? "ok" : "fail",
             pricebuf,
             upd_local);

    set_label_text(g_info_label, buf);
}

/* ---------- icon drawing ---------- */

static int xft_font_height(const XftFont *font)
{
    if (!font) return 0;
    return font->ascent + font->descent;
}

static void update_icon_title_for_selected(void)
{
    if (!g_toplevel) return;
    const Coin *c = (g_coin_count > 0 && g_selected >= 0 &&
                     g_selected < g_coin_count) ? &g_coins[g_selected] : NULL;
    const char *name = "Coins";
    if (c) {
        if (c->symbol[0]) name = c->symbol;
        else if (c->name[0]) name = c->name;
        else if (c->id[0]) name = c->id;
    }
    XtVaSetValues(g_toplevel, XmNiconName, name, NULL);
}

static void update_icon_pixmap_for_selected(void)
{
    if (!g_toplevel || !XtIsRealized(g_toplevel)) return;
    Display *dpy = XtDisplay(g_toplevel);
    Window win = XtWindow(g_toplevel);
    if (!dpy || !win) return;

    update_icon_title_for_selected();

    int screen = DefaultScreen(dpy);
    Pixmap root = RootWindow(dpy, screen);

    int iw = (g_icon_w > 0 ? g_icon_w : ICON_SIZE_FALLBACK);
    int ih = (g_icon_h > 0 ? g_icon_h : ICON_SIZE_FALLBACK);

    if (g_icon_pixmap != None) { XFreePixmap(dpy, g_icon_pixmap); g_icon_pixmap = None; }
    if (g_icon_mask   != None) { XFreePixmap(dpy, g_icon_mask);   g_icon_mask   = None; }

    g_icon_pixmap = XCreatePixmap(dpy, root, (unsigned int)iw, (unsigned int)ih,
                                  DefaultDepth(dpy, screen));
    g_icon_mask   = XCreatePixmap(dpy, root, (unsigned int)iw, (unsigned int)ih, 1);

    if (g_icon_pixmap == None || g_icon_mask == None) return;

    ensure_gc(dpy, g_icon_pixmap, &g_icon_gc);
    ensure_gc(dpy, g_icon_mask,   &g_icon_mask_gc);

    Pixel bg = WhitePixel(dpy, screen);
    XtVaGetValues(g_toplevel, XmNbackground, &bg, NULL);

    /* background */
    XSetForeground(dpy, g_icon_gc, bg);
    XFillRectangle(dpy, g_icon_pixmap, g_icon_gc, 0, 0, (unsigned int)iw, (unsigned int)ih);

    /* opaque mask */
    XSetForeground(dpy, g_icon_mask_gc, 1);
    XFillRectangle(dpy, g_icon_mask, g_icon_mask_gc, 0, 0, (unsigned int)iw, (unsigned int)ih);

    /* content */
    const Coin *c = (g_coin_count > 0) ? &g_coins[g_selected] : NULL;

    char pricebuf[64] = "n/a";
    char namebuf[96] = "Coins";

    if (c) {
        snprintf(namebuf, sizeof(namebuf), "%s", (c->name[0] ? c->name : c->id));
        if (c->has_data) format_price_value(c->price_usd, pricebuf, sizeof(pricebuf));
    } else {
        snprintf(pricebuf, sizeof(pricebuf), "n/a");
    }

    int pad = 4;
    int pad_top = 2;

    int avail_h = ih - 2 * pad;
    int avail_w = iw - 2 * pad;
    if (avail_h < 8) avail_h = 8;
    if (avail_w < 4) avail_w = 4;

    const char *unit = "USD";

    int spacing = 1;
    int usd_price_gap = -5;
    XftDraw *xft_draw = XftDrawCreate(dpy, g_icon_pixmap,
                                     DefaultVisual(dpy, screen),
                                     DefaultColormap(dpy, screen));
    if (!xft_draw) {
        if (icon_debug_enabled()) {
            fprintf(stdout, "[ck-coins] icon: XftDrawCreate failed\n");
            fflush(stdout);
        }
        return;
    }

    XftColor xft_fg;
    const char *fg_name = g_last_fetch_ok ? "black" : "gray60";
    if (!XftColorAllocName(dpy, DefaultVisual(dpy, screen),
                           DefaultColormap(dpy, screen), fg_name, &xft_fg)) {
        XftColorAllocName(dpy, DefaultVisual(dpy, screen),
                          DefaultColormap(dpy, screen), "black", &xft_fg);
    }

    int min_name = 6;
    int min_usd = 6;
    int min_price = 8;

    XftFont *font_name = NULL;
    XftFont *font_usd = NULL;
    XftFont *font_price = NULL;

    int inner_h = avail_h - spacing * 2;
    if (inner_h < 6) inner_h = 6;

    /* Slots: name (35%), usd (20%), price (45%) */
    int name_slot = (inner_h * 35) / 100;
    int usd_slot = (inner_h * 20) / 100;
    int price_slot = inner_h - name_slot - usd_slot;
    if (name_slot < min_name) name_slot = min_name;
    if (usd_slot < 14) usd_slot = 14;
    if (price_slot < min_price) price_slot = min_price;

    int name_size = 0, usd_size = 0, price_size = 0;
    font_name = xft_pick_font_for_slot(dpy, screen, namebuf, avail_w, name_slot,
                                       name_slot, min_name, &name_size);
    font_usd = xft_pick_font_for_slot(dpy, screen, unit, avail_w, usd_slot,
                                      usd_slot, min_usd, &usd_size);
    font_price = xft_pick_font_for_slot(dpy, screen, pricebuf, avail_w, price_slot,
                                        price_slot, min_price, &price_size);

    /* If lines overlap, reduce USD then price then name. */
    for (int iter = 0; iter < 64; ++iter) {
        if (!font_name || !font_usd || !font_price) break;
        int y_name = pad_top + font_name->ascent;
        int y_price = ih - pad - font_price->descent;
        int y_usd = y_price - usd_price_gap - (font_usd->ascent + font_usd->descent);
        int min_clear = y_name + font_name->descent + spacing;
        if (y_usd >= min_clear) break;

        if (usd_size > min_usd) usd_size--;
        else if (price_size > min_price) price_size--;
        else if (name_size > min_name) name_size--;
        else break;

        if (font_usd) XftFontClose(dpy, font_usd);
        if (font_price) XftFontClose(dpy, font_price);
        if (font_name) XftFontClose(dpy, font_name);

        font_name = xft_open_font(dpy, screen, name_size);
        font_usd = xft_open_font(dpy, screen, usd_size);
        font_price = xft_open_font(dpy, screen, price_size);
    }
    if (!font_name || !font_usd || !font_price) {
        if (icon_debug_enabled()) {
            fprintf(stdout,
                    "[ck-coins] icon: missing fonts name=%p usd=%p price=%p\n",
                    (void *)font_name, (void *)font_usd, (void *)font_price);
            fflush(stdout);
        }
        XftColorFree(dpy, DefaultVisual(dpy, screen),
                     DefaultColormap(dpy, screen), &xft_fg);
        XftDrawDestroy(xft_draw);
        return;
    }

    int y_name = pad_top + font_name->ascent;
    int y_price = ih - pad - font_price->descent;
    int max_usd_baseline = y_price - font_price->ascent - usd_price_gap - font_usd->descent;
    int min_usd_baseline = pad_top + font_usd->ascent;
    int y_usd = max_usd_baseline;
    if (y_usd < min_usd_baseline) y_usd = min_usd_baseline;

    int tw = xft_text_width(dpy, font_name, namebuf);
    int tx = pad;
    XftDrawStringUtf8(xft_draw, &xft_fg, font_name, tx, y_name,
                      (const FcChar8 *)namebuf, (int)strlen(namebuf));

    tw = xft_text_width(dpy, font_usd, unit);
    tx = iw - pad - tw;
    if (tx < pad) tx = pad;
    XftDrawStringUtf8(xft_draw, &xft_fg, font_usd, tx, y_usd,
                      (const FcChar8 *)unit, (int)strlen(unit));

    tw = xft_text_width(dpy, font_price, pricebuf);
    tx = iw - pad - tw;
    XftDrawStringUtf8(xft_draw, &xft_fg, font_price, tx, y_price,
                      (const FcChar8 *)pricebuf, (int)strlen(pricebuf));

    if (icon_debug_enabled()) {
        fprintf(stdout,
                "[ck-coins] icon: size=%dx%d pad=%d avail=%dx%d\n",
                iw, ih, pad, avail_w, avail_h);
        fprintf(stdout,
                "[ck-coins] icon: text name='%s' usd='%s' price='%s'\n",
                namebuf, unit, pricebuf);
        fprintf(stdout,
                "[ck-coins] icon: font size px name=%d usd=%d price=%d (req=%d/%d/%d)\n",
                name_size, usd_size, price_size,
                name_size, usd_size, price_size);
        fprintf(stdout,
                "[ck-coins] icon: font metrics name(a=%d d=%d) usd(a=%d d=%d) price(a=%d d=%d)\n",
                font_name->ascent, font_name->descent,
                font_usd->ascent, font_usd->descent,
                font_price->ascent, font_price->descent);
        fprintf(stdout,
                "[ck-coins] icon: heights name=%d usd=%d price=%d slots=%d/%d/%d\n",
                xft_font_height(font_name),
                xft_font_height(font_usd),
                xft_font_height(font_price),
                name_slot, usd_slot, price_slot);
        fprintf(stdout,
                "[ck-coins] icon: widths name=%d usd=%d price=%d\n",
                xft_text_width(dpy, font_name, namebuf),
                xft_text_width(dpy, font_usd, unit),
                xft_text_width(dpy, font_price, pricebuf));
        fprintf(stdout,
                "[ck-coins] icon: pos name=(%d,%d) usd=(%d,%d) price=(%d,%d)\n",
                pad, y_name,
                iw - pad - xft_text_width(dpy, font_usd, unit), y_usd,
                iw - pad - xft_text_width(dpy, font_price, pricebuf), y_price);
        fprintf(stdout,
                "[ck-coins] icon: y_name=%d y_usd=%d y_price=%d\n",
                y_name, y_usd, y_price);
        fflush(stdout);
    }

    XftColorFree(dpy, DefaultVisual(dpy, screen),
                 DefaultColormap(dpy, screen), &xft_fg);
    if (font_name) XftFontClose(dpy, font_name);
    if (font_usd) XftFontClose(dpy, font_usd);
    if (font_price) XftFontClose(dpy, font_price);
    XftDrawDestroy(xft_draw);

    XFlush(dpy);

    update_wm_icon_pixmap(dpy, win, g_icon_pixmap);
    XtVaSetValues(g_toplevel, XmNiconPixmap, g_icon_pixmap, NULL);
    XtVaSetValues(g_toplevel, XmNiconMask,   g_icon_mask,   NULL);
}

/* ---------- fetching + timers ---------- */

static unsigned long hash_ids(const char *s)
{
    unsigned long h = 5381UL;
    int c;
    while (s && (c = *s++)) {
        h = ((h << 5) + h) + (unsigned long)c;
    }
    return h;
}

static void ensure_cache_dir(char *out_dir, size_t out_sz)
{
    const char *base = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    if (base && base[0]) {
        snprintf(out_dir, out_sz, "%s/ck-coins", base);
    } else if (home && home[0]) {
        snprintf(out_dir, out_sz, "%s/.cache/ck-coins", home);
    } else {
        snprintf(out_dir, out_sz, "/tmp/ck-coins");
    }
    mkdir(out_dir, 0700);
}

static void build_cache_paths(const char *ids, char *cache_path, size_t cache_sz,
                              char *lock_path, size_t lock_sz)
{
    unsigned long h = hash_ids(ids ? ids : "");
    char dir[PATH_MAX];
    ensure_cache_dir(dir, sizeof(dir));
    size_t dlen = strlen(dir);
    if (dlen + 32 >= cache_sz || dlen + 32 >= lock_sz) {
        cache_path[0] = '\0';
        lock_path[0] = '\0';
        return;
    }
    snprintf(cache_path, cache_sz, "%s/cache-%lu.json", dir, h);
    snprintf(lock_path, lock_sz, "%s/cache-%lu.lock", dir, h);
}

static int read_cache_if_fresh(const char *cache_path, int max_age_sec,
                               char **out_json, size_t *out_len)
{
    if (!cache_path || !out_json || !out_len) return 0;
    struct stat st;
    if (stat(cache_path, &st) != 0) return 0;
    time_t now = time(NULL);
    if (st.st_mtime == 0 || now < st.st_mtime) return 0;
    if ((now - st.st_mtime) > max_age_sec) return 0;

    int fd = open(cache_path, O_RDONLY);
    if (fd < 0) return 0;
    if (flock(fd, LOCK_SH) != 0) {
        close(fd);
        return 0;
    }

    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0 || sz > 5 * 1024 * 1024) {
        flock(fd, LOCK_UN);
        close(fd);
        return 0;
    }
    lseek(fd, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        flock(fd, LOCK_UN);
        close(fd);
        return 0;
    }
    ssize_t rd = read(fd, buf, (size_t)sz);
    if (rd != sz) {
        free(buf);
        flock(fd, LOCK_UN);
        close(fd);
        return 0;
    }
    buf[sz] = '\0';
    flock(fd, LOCK_UN);
    close(fd);

    *out_json = buf;
    *out_len = (size_t)sz;
    return 1;
}

static int write_cache_locked(const char *cache_path, int lock_fd,
                              const char *json, size_t json_len)
{
    if (!cache_path || !json || json_len == 0) return 0;
    char tmp_path[PATH_MAX];
    size_t base_len = strlen(cache_path);
    if (base_len + 4 >= sizeof(tmp_path)) return 0;
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", cache_path);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    ssize_t wr = write(fd, json, json_len);
    fsync(fd);
    close(fd);
    if (wr != (ssize_t)json_len) {
        unlink(tmp_path);
        return 0;
    }
    if (rename(tmp_path, cache_path) != 0) {
        unlink(tmp_path);
        return 0;
    }
    (void)lock_fd;
    return 1;
}

static int fetch_all_prices(void)
{
    if (g_coin_count <= 0) return 0;

    /* build ids=... */
    char ids[1024];
    ids[0] = '\0';

    for (int i = 0; i < g_coin_count; ++i) {
        if (g_coins[i].id[0] == '\0') continue;
        if (ids[0]) strncat(ids, ",", sizeof(ids) - strlen(ids) - 1);
        strncat(ids, g_coins[i].id, sizeof(ids) - strlen(ids) - 1);
    }

    char cache_path[PATH_MAX];
    char lock_path[PATH_MAX];
    build_cache_paths(ids, cache_path, sizeof(cache_path), lock_path, sizeof(lock_path));

    /* Use fresh cache if available */
    char *cached_json = NULL;
    size_t cached_len = 0;
    if (read_cache_if_fresh(cache_path, CACHE_TTL_SEC, &cached_json, &cached_len)) {
        g_last_fetch_received_local = time(NULL);
        g_last_fetch_ok = 1;

        for (int i = 0; i < g_coin_count; ++i) {
            double price = 0.0;
            time_t ts = 0;
            if (parse_coin_from_json(cached_json, g_coins[i].id, &price, &ts)) {
                g_coins[i].price_usd = price;
                g_coins[i].updated_at_utc = ts;
                g_coins[i].has_data = 1;
            } else {
                g_coins[i].has_data = 0;
            }
        }
        free(cached_json);
        return 1;
    }

    /* Lock for update to avoid stampede */
    int lock_fd = open(lock_path, O_RDWR | O_CREAT, 0644);
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_EX);
        /* Re-check cache once lock is held */
        cached_json = NULL;
        cached_len = 0;
        if (read_cache_if_fresh(cache_path, CACHE_TTL_SEC, &cached_json, &cached_len)) {
            g_last_fetch_received_local = time(NULL);
            g_last_fetch_ok = 1;
            for (int i = 0; i < g_coin_count; ++i) {
                double price = 0.0;
                time_t ts = 0;
                if (parse_coin_from_json(cached_json, g_coins[i].id, &price, &ts)) {
                    g_coins[i].price_usd = price;
                    g_coins[i].updated_at_utc = ts;
                    g_coins[i].has_data = 1;
                } else {
                    g_coins[i].has_data = 0;
                }
            }
            free(cached_json);
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
            return 1;
        }
    }

    char url[1400];
    snprintf(url, sizeof(url),
             "https://api.coingecko.com/api/v3/simple/price?ids=%s&vs_currencies=usd&include_last_updated_at=true",
             ids);

    char *json = NULL;
    size_t json_len = 0;
    int ok = http_get_json(url, &json, &json_len);

    g_last_fetch_received_local = time(NULL);
    g_last_fetch_ok = ok ? 1 : 0;

    if (!ok || !json) {
        for (int i = 0; i < g_coin_count; ++i) g_coins[i].has_data = 0;
        free(json);
        if (lock_fd >= 0) { flock(lock_fd, LOCK_UN); close(lock_fd); }
        return 0;
    }

    /* parse each coin */
    for (int i = 0; i < g_coin_count; ++i) {
        double price = 0.0;
        time_t ts = 0;
        if (parse_coin_from_json(json, g_coins[i].id, &price, &ts)) {
            g_coins[i].price_usd = price;
            g_coins[i].updated_at_utc = ts;
            g_coins[i].has_data = 1;
        } else {
            g_coins[i].has_data = 0;
        }
    }

    if (lock_fd >= 0) {
        (void)write_cache_locked(cache_path, lock_fd, json, json_len);
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }

    free(json);
    return 1;
}

static void apply_updates_to_ui(void)
{
    rebuild_list_items();
    update_info_label_for_selected();
    update_icon_pixmap_for_selected();
}

static void fetch_timer_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)client_data;
    (void)id;

    (void)fetch_all_prices();
    apply_updates_to_ui();

    XtAppAddTimeOut(app_context, FETCH_INTERVAL_MS, fetch_timer_cb, NULL);
}

static void refresh_btn_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w; (void)client_data; (void)call_data;
    (void)fetch_all_prices();
    apply_updates_to_ui();
}

static void list_sel_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w; (void)client_data;
    XmListCallbackStruct *cbs = (XmListCallbackStruct *)call_data;
    if (!cbs) return;

    int pos = cbs->item_position; /* 1-based */
    if (pos <= 0) return;
    int idx = pos - 1;
    if (idx < 0 || idx >= g_coin_count) return;

    g_selected = idx;
    update_info_label_for_selected();
    update_icon_pixmap_for_selected();
}

/* ---------- session ---------- */

static void session_save_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    (void)call_data;
    if (!session_data) return;

    session_capture_geometry(w, session_data, "x", "y", "w", "h");
    session_data_set_int(session_data, "selected_index", g_selected);
    session_save(w, session_data, g_exec_path);
}

static void wm_delete_callback(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    XtAppContext app = (XtAppContext)client_data;
    XtAppSetExitFlag(app);
}

static void menu_new_window_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    (void)client_data;
    pid_t pid = fork();
    if (pid == 0) {
        execl(g_exec_path, g_exec_path, (char *)NULL);
        _exit(1);
    }
}

static void menu_exit_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    (void)client_data;
    if (session_data) {
        session_capture_geometry(g_toplevel, session_data, "x", "y", "w", "h");
        session_data_set_int(session_data, "selected_index", g_selected);
        session_save(g_toplevel, session_data, g_exec_path);
    }
    XtAppSetExitFlag(app_context);
}

static void about_destroy_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    g_about_shell = NULL;
}

static void about_close_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    Widget shell = (Widget)client_data;
    if (shell && XtIsWidget(shell)) {
        XtDestroyWidget(shell);
    }
}

static void about_wm_protocol_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    Widget shell = (Widget)client_data;
    if (shell && XtIsWidget(shell)) {
        XtDestroyWidget(shell);
    }
}

static void menu_about_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    (void)client_data;

    if (g_about_shell && XtIsWidget(g_about_shell)) {
        XtPopup(g_about_shell, XtGrabNone);
        return;
    }

    Arg shell_args[8];
    int sn = 0;
    XtSetArg(shell_args[sn], XmNtitle, "About Coins"); sn++;
    XtSetArg(shell_args[sn], XmNallowShellResize, True); sn++;
    XtSetArg(shell_args[sn], XmNtransientFor, g_toplevel); sn++;
    g_about_shell = XmCreateDialogShell(g_toplevel, "aboutCoinsShell", shell_args, sn);
    if (!g_about_shell) return;
    XtAddCallback(g_about_shell, XmNdestroyCallback, about_destroy_cb, NULL);

    Widget form = XmCreateForm(g_about_shell, "aboutForm", NULL, 0);
    XtManageChild(form);

    Arg args[8];
    int n = 0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
    Widget notebook = XmCreateNotebook(form, "aboutNotebook", args, n);
    XtVaSetValues(notebook, XmNmarginWidth, 12, XmNmarginHeight, 12, NULL);
    XtManageChild(notebook);

    Widget ok = XtVaCreateManagedWidget(
        "aboutOk",
        xmPushButtonWidgetClass, form,
        XmNlabelString, XmStringCreateLocalized("OK"),
        XmNbottomAttachment, XmATTACH_FORM,
        XmNbottomOffset,    8,
        XmNleftAttachment,  XmATTACH_POSITION,
        XmNrightAttachment, XmATTACH_POSITION,
        XmNleftPosition,    40,
        XmNrightPosition,   60,
        NULL
    );

    XtVaSetValues(notebook,
                  XmNbottomAttachment, XmATTACH_WIDGET,
                  XmNbottomWidget,     ok,
                  XmNbottomOffset,     8,
                  NULL);

    XtAddCallback(ok, XmNactivateCallback, about_close_cb, (XtPointer)g_about_shell);

    about_add_standard_pages(notebook, 1,
                             "About",
                             "CK-Core Coins",
                             "(c) 2026 by Dr. C. Klukas",
                             True);

    XtRealizeWidget(g_about_shell);
    Atom wm_delete = XmInternAtom(XtDisplay(g_about_shell), "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(g_about_shell, wm_delete, about_wm_protocol_cb, (XtPointer)g_about_shell);
    XmActivateWMProtocol(g_about_shell, wm_delete);

    XtPopup(g_about_shell, XtGrabNone);
}

static void apply_wm_hints(void)
{
    if (!g_toplevel) return;
    unsigned int funcs = MWM_FUNC_ALL ^ (MWM_FUNC_RESIZE | MWM_FUNC_MAXIMIZE);
    XtVaSetValues(g_toplevel,
                  XmNmwmFunctions, funcs,
                  XmNallowShellResize, False,
                  NULL);
}

/* ---------- coin setup ---------- */

static void add_coin(const char *id, const char *name, const char *symbol)
{
    if (!id || !id[0]) return;

    Coin *p = (Coin *)realloc(g_coins, sizeof(Coin) * (size_t)(g_coin_count + 1));
    if (!p) return;
    g_coins = p;

    Coin *c = &g_coins[g_coin_count];
    memset(c, 0, sizeof(*c));
    strncpy(c->id, id, sizeof(c->id) - 1);
    if (name)   strncpy(c->name, name, sizeof(c->name) - 1);
    if (symbol) strncpy(c->symbol, symbol, sizeof(c->symbol) - 1);
    c->has_data = 0;

    g_coin_count++;
}

static void setup_default_coins(void)
{
    add_coin("bitcoin",     "Bitcoin",     "BTC");
    add_coin("ethereum",    "Ethereum",    "ETH");
    add_coin("tether",      "Tether",      "USDT");
    add_coin("binancecoin", "BNB",         "BNB");
    add_coin("solana",      "Solana",      "SOL");
}

/* Optional: allow overriding ids via env CK_COINS_IDS="bitcoin,ethereum" etc.
 * (keeps main() clean and avoids heavy CLI parsing here)
 * If provided, names/symbols will fall back to id.
 */
static void setup_coins_from_env_or_default(void)
{
    const char *env = getenv("CK_COINS_IDS");
    if (!env || !env[0]) {
        setup_default_coins();
        return;
    }

    char *dup = strdup(env);
    if (!dup) { setup_default_coins(); return; }

    char *save = NULL;
    char *tok = strtok_r(dup, ",", &save);
    while (tok) {
        /* trim */
        while (*tok && isspace((unsigned char)*tok)) tok++;
        char *end = tok + strlen(tok);
        while (end > tok && isspace((unsigned char)end[-1])) { end[-1] = '\0'; end--; }
        if (*tok) add_coin(tok, tok, "");
        tok = strtok_r(NULL, ",", &save);
    }

    free(dup);

    if (g_coin_count <= 0) setup_default_coins();
}

/* ---------- main ---------- */

int main(int argc, char *argv[])
{
    XtSetLanguageProc(NULL, NULL, NULL);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    setup_coins_from_env_or_default();

    g_toplevel = XtVaAppInitialize(
        &app_context,
        "CkCoins",
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

    /* UI layout: MainWindow + work Form + bottom bar */
    g_mainw = XmCreateMainWindow(g_toplevel, "mainw", NULL, 0);
    XtManageChild(g_mainw);

    g_form = XmCreateForm(g_mainw, "workForm", NULL, 0);
    XtManageChild(g_form);

    g_menubar = XmCreateMenuBar(g_mainw, "menuBar", NULL, 0);
    XtVaSetValues(g_menubar,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  NULL);
    XtManageChild(g_menubar);

    Widget file_pane = XmCreatePulldownMenu(g_menubar, "fileMenu", NULL, 0);
    Widget file_cascade = XtVaCreateManagedWidget(
        "fileCascade",
        xmCascadeButtonWidgetClass, g_menubar,
        XmNlabelString, XmStringCreateLocalized("File"),
        XmNmnemonic, 'F',
        XmNsubMenuId, file_pane,
        NULL
    );
    (void)file_cascade;

    Widget new_item = XtVaCreateManagedWidget(
        "newWindow",
        xmPushButtonWidgetClass, file_pane,
        XmNlabelString, XmStringCreateLocalized("New Window"),
        NULL
    );
    XtAddCallback(new_item, XmNactivateCallback, menu_new_window_cb, NULL);

    XtVaCreateManagedWidget(
        "fileSep",
        xmSeparatorGadgetClass, file_pane,
        NULL
    );

    Widget exit_item = XtVaCreateManagedWidget(
        "exitItem",
        xmPushButtonWidgetClass, file_pane,
        XmNlabelString, XmStringCreateLocalized("Exit"),
        XmNaccelerator, "Alt<Key>F4",
        XmNacceleratorText, XmStringCreateLocalized("Alt+F4"),
        NULL
    );
    XtAddCallback(exit_item, XmNactivateCallback, menu_exit_cb, NULL);

    Widget help_pane = XmCreatePulldownMenu(g_menubar, "helpMenu", NULL, 0);
    Widget help_cascade = XtVaCreateManagedWidget(
        "helpCascade",
        xmCascadeButtonWidgetClass, g_menubar,
        XmNlabelString, XmStringCreateLocalized("Help"),
        XmNmnemonic, 'H',
        XmNsubMenuId, help_pane,
        XmNmenuHelpWidget, NULL,
        NULL
    );
    XtVaSetValues(g_menubar, XmNmenuHelpWidget, help_cascade, NULL);

    Widget about_item = XtVaCreateManagedWidget(
        "aboutCoins",
        xmPushButtonWidgetClass, help_pane,
        XmNlabelString, XmStringCreateLocalized("About"),
        NULL
    );
    XtAddCallback(about_item, XmNactivateCallback, menu_about_cb, NULL);

    Widget content_form = XmCreateForm(g_form, "contentForm", NULL, 0);
    XtVaSetValues(content_form,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  NULL);
    XtManageChild(content_form);

    g_list = XmCreateScrolledList(content_form, "coinList", NULL, 0);
    XtVaSetValues(
        XtParent(g_list),
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_POSITION,
        XmNrightPosition, 48,
        NULL
    );
    XtVaSetValues(
        g_list,
        XmNvisibleItemCount, 10,
        XmNselectionPolicy, XmBROWSE_SELECT,
        NULL
    );
    XtManageChild(g_list);

    Widget sep = XmCreateSeparatorGadget(content_form, "sep", NULL, 0);
    XtVaSetValues(sep,
                  XmNorientation, XmVERTICAL,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_POSITION,
                  XmNleftPosition, 50,
                  XmNrightAttachment, XmATTACH_POSITION,
                  XmNrightPosition, 50,
                  NULL);
    XtManageChild(sep);

    g_info_label = XtVaCreateManagedWidget(
        "info",
        xmLabelWidgetClass, content_form,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_POSITION,
        XmNleftPosition, 52,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNrecomputeSize, True,
        XmNmarginWidth, 8,
        XmNmarginHeight, 8,
        NULL
    );

    Widget bottom_bar = XmCreateForm(g_form, "bottomBar", NULL, 0);
    XtVaSetValues(bottom_bar,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNheight, 52,
                  XmNresizeHeight, False,
                  NULL);
    XtManageChild(bottom_bar);

    Widget bottom_sep = XmCreateSeparatorGadget(g_form, "bottomSep", NULL, 0);
    XtVaSetValues(bottom_sep,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNleftOffset, -12,
                  XmNrightOffset, -12,
                  XmNbottomAttachment, XmATTACH_WIDGET,
                  XmNbottomWidget, bottom_bar,
                  XmNbottomOffset, 8,
                  XmNtopAttachment, XmATTACH_NONE,
                  XmNheight, 2,
                  XmNrecomputeSize, False,
                  NULL);
    XtManageChild(bottom_sep);

    XtVaSetValues(content_form,
                  XmNbottomAttachment, XmATTACH_WIDGET,
                  XmNbottomWidget, bottom_sep,
                  XmNbottomOffset, 8,
                  NULL);

    XmString s_refresh = XmStringCreateLocalized("Refresh");
    g_refresh_btn = XmCreatePushButtonGadget(bottom_bar, "refreshButton", NULL, 0);
    XtVaSetValues(g_refresh_btn,
                  XmNlabelString, s_refresh,
                  XmNmarginWidth, 12,
                  XmNmarginHeight, 6,
                  XmNleftAttachment, XmATTACH_POSITION,
                  XmNleftPosition, 2,
                  XmNrightAttachment, XmATTACH_POSITION,
                  XmNrightPosition, 48,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNtopOffset, 6,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNbottomOffset, 6,
                  NULL);
    XmStringFree(s_refresh);
    XtManageChild(g_refresh_btn);

    XmString s_close = XmStringCreateLocalized("Close");
    g_close_btn = XmCreatePushButtonGadget(bottom_bar, "closeButton", NULL, 0);
    XtVaSetValues(g_close_btn,
                  XmNlabelString, s_close,
                  XmNmarginWidth, 12,
                  XmNmarginHeight, 6,
                  XmNleftAttachment, XmATTACH_POSITION,
                  XmNleftPosition, 52,
                  XmNrightAttachment, XmATTACH_POSITION,
                  XmNrightPosition, 98,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNtopOffset, 6,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNbottomOffset, 6,
                  NULL);
    XmStringFree(s_close);
    XtManageChild(g_close_btn);

    XmMainWindowSetAreas(g_mainw, g_menubar, NULL, NULL, NULL, g_form);

    XtAddCallback(g_refresh_btn, XmNactivateCallback, refresh_btn_cb, NULL);
    XtAddCallback(g_close_btn, XmNactivateCallback, menu_exit_cb, NULL);
    XtAddCallback(g_list, XmNbrowseSelectionCallback, list_sel_cb, NULL);

    XtVaSetValues(g_toplevel,
                  XmNtitle, "Coins",
                  NULL);

    /* restore session */
    if (session_data && session_load(g_toplevel, session_data)) {
        session_apply_geometry(g_toplevel, session_data, "x", "y", "w", "h");
        int idx = session_data_get_int(session_data, "selected_index", 0);
        if (idx < 0) idx = 0;
        if (idx >= g_coin_count) idx = g_coin_count - 1;
        g_selected = idx;
        g_session_loaded = 1;
    }

    /* WM callbacks */
    Atom wm_delete = XmInternAtom(g_display, "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(g_toplevel, wm_delete, wm_delete_callback, (XtPointer)app_context);
    XmActivateWMProtocol(g_toplevel, wm_delete);

    Atom wm_save = XmInternAtom(g_display, "WM_SAVE_YOURSELF", False);
    XmAddWMProtocolCallback(g_toplevel, wm_save, session_save_cb, NULL);
    XmActivateWMProtocol(g_toplevel, wm_save);

    /* apply initial size before realize to avoid full-width flash */
    if (!g_session_loaded) {
        XtVaSetValues(g_toplevel, XmNwidth, 640, XmNheight, 460, NULL);
    }
    XtRealizeWidget(g_toplevel);
    apply_wm_hints();

    /* initial UI + icon placeholder */
    set_label_text(g_info_label,
                   "Loading...\n\n"
                   "Tip: set CK_COINS_IDS env var to override IDs,\n"
                   "e.g. CK_COINS_IDS=\"bitcoin,ethereum,solana\" ck-coins");
    rebuild_list_items();
    update_icon_pixmap_for_selected();

    /* initial fetch quickly, then every 15 min */
    XtAppAddTimeOut(app_context, 250, fetch_timer_cb, NULL);

    XtAppMainLoop(app_context);

    /* cleanup (unreached typically) */
    if (g_icon_pixmap != None) XFreePixmap(g_display, g_icon_pixmap);
    if (g_icon_mask   != None) XFreePixmap(g_display, g_icon_mask);
    if (g_icon_gc != None) XFreeGC(g_display, g_icon_gc);
    if (g_icon_mask_gc != None) XFreeGC(g_display, g_icon_mask_gc);
    for (int i = 0; i < g_xft_font_count; ++i) {
        if (g_xft_fonts[i].font) XftFontClose(g_display, g_xft_fonts[i].font);
    }
    free(g_coins);

    curl_global_cleanup();
    return 0;
}
