/*
 * ck-mines.c  -  Motif/CDE Minesweeper implementation
 *
 * Build:
 *   cc -O2 -Wall -Isrc -I/usr/local/CDE/include \
 *      src/games/ck-mines/ck-mines.c src/shared/session_utils.c src/shared/about_dialog.c \
 *      -o build/bin/ck-mines \
 *      -L/usr/local/CDE/lib -lDtSvc -lDtXinerama -lDtWidget -ltt -lXm -lXt -lSM -lICE -lXinerama -lX11
 *
 * Debug controls:
 *  - CK_MINES_DEBUG=0/1/2/3   (0 off, 1 normal, 2 verbose, 3 insane)
 *  - CK_MINES_BACKBUF=1       (optional pixmap backbuffer)
 *  - CK_MINES_SCALE=2.0       (override dpi scaling)
 */

#include <Xm/Xm.h>
#include <Xm/MainW.h>
#include <Xm/RowColumn.h>
#include <Xm/CascadeB.h>
#include <Xm/PushB.h>
#include <Xm/TextF.h>
#include <Xm/ToggleB.h>
#include <Xm/Label.h>
#include <Xm/Frame.h>
#include <Xm/Form.h>
#include <Xm/DrawingA.h>
#include <Xm/DrawnB.h>
#include <Xm/MessageB.h>
#include <Xm/DialogS.h>
#include <Xm/SeparatoG.h>
#include <Xm/Protocols.h>
#include <Xm/BulletinB.h>
#include <Xm/SelectioB.h>   /* XmCreatePromptDialog, XmSelectionBoxGetChild, XmDIALOG_TEXT, ... */

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/xpm.h>

#include "../../shared/about_dialog.h"
#include "ck-mines.l.pm"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif

/* ---------------------------------------------------------------------------------------
 * Debug/logging
 * --------------------------------------------------------------------------------------- */
static int g_debug = 1; /* 0..3 */
static unsigned long g_log_seq = 0;

static unsigned long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (unsigned long)(ts.tv_sec * 1000ul + ts.tv_nsec / 1000000ul);
}

#define DBG_L(lvl, fmt, ...) do { \
    if (g_debug >= (lvl)) { \
        fprintf(stderr, "[ck-mines] %lu #%lu " fmt "\n", now_ms(), ++g_log_seq, ##__VA_ARGS__); \
    } \
} while(0)

#define DBG(fmt, ...)  DBG_L(1, fmt, ##__VA_ARGS__)
#define DBG2(fmt, ...) DBG_L(2, fmt, ##__VA_ARGS__)
#define DBG3(fmt, ...) DBG_L(3, fmt, ##__VA_ARGS__)

/* forward */
static void redraw_board(void);

typedef struct { const char *name; int cols, rows, mines; } Preset;
static const Preset PRESET_BEGINNER     = { "Beginner",      9,  9, 10 };
static const Preset PRESET_INTERMEDIATE = { "Intermediate", 16, 16, 40 };
static const Preset PRESET_EXPERT       = { "Expert",       30, 16, 99 };

/* ---------------------------------------------------------------------------------------
 * Base sizes (96dpi baseline) and scaling
 * --------------------------------------------------------------------------------------- */
static const int BASE_CELL   = 16;
static const int BASE_PAD    = 8;
static const int BASE_GAP    = 6;

static const int BASE_LED_W   = 13;
static const int BASE_LED_H   = 23;
static const int BASE_LED_GAP = 2;
static const int BASE_LED_PAD = 4;

static const int BASE_FACE_W  = 34;
static const int BASE_FACE_H  = 34;

static float g_ui_scale = 1.0f;

static int S(int v) {
    float f = (float)v * g_ui_scale;
    int r = (int)(f + 0.5f);
    return (r < 1) ? 1 : r;
}
static int CELL_PX(void)    { return S(BASE_CELL); }
static int PAD_PX(void)     { return S(BASE_PAD); }
static int GAP_PX(void)     { return S(BASE_GAP); }
static int LED_W_PX(void)   { return S(BASE_LED_W); }
static int LED_H_PX(void)   { return S(BASE_LED_H); }
static int LED_GAP_PX(void) { return S(BASE_LED_GAP); }
static int LED_PAD_PX(void) { return S(BASE_LED_PAD); }
static int FACE_W_PX(void)  { return S(BASE_FACE_W); }
static int FACE_H_PX(void)  { return S(BASE_FACE_H); }

/* ---------------------------------------------------------------------------------------
 * Game state
 * --------------------------------------------------------------------------------------- */
	typedef struct {
    unsigned char mine, revealed, flagged, question, adj, exploded;
} Cell;

typedef enum { FACE_SMILE=0, FACE_OHNO=1, FACE_DEAD=2, FACE_COOL=3 } FaceState;
typedef enum { DIFF_BEGINNER=0, DIFF_INTERMEDIATE=1, DIFF_EXPERT=2, DIFF_CUSTOM=3 } DiffId;

typedef struct {
    Widget toplevel, mainw, menubar;
    Widget work_form;

    Widget top_frame, top_form;

    Widget mine_frame, mine_led;
    Widget face_button;
    Widget time_frame, time_led;

    Widget board_frame, board_bb, board_da;

    Widget status_frame, status_label;
    Widget marks_toggle;

    Display *dpy;
    GC gc;

    int use_backbuf;
    Pixmap backbuf;
    int bb_w, bb_h;

	    Pixel col_bg, col_fg, col_top, col_bottom, col_select, col_red;
	    Pixel col_yellow;
	    Pixel col_black;
	    Pixel col_led_bg, col_led_on, col_led_off;

    XFontStruct *font;

    int cols, rows, mines;
    DiffId diff;
    int marks_enabled;

    Cell *cells;
    int first_click_done;
    int game_over; /* 0 playing, 1 lost, 2 won */
    int flags_count;
    int revealed_count;
    int exploded_index;

    int left_down, right_down, pressed_cell;
    FaceState face;

    int elapsed, timer_running;
    XtIntervalId timer_id;

    int led_mines_value, led_time_value;

    char cfg_path[512];

    /* geometry targets */
    int want_bw, want_bh;

    unsigned long expose_count;
    unsigned long configure_count;

    int shell_mapped;
    int pending_geom;

    /* Fastest times (for preset difficulties) */
    int  best_time[3];
    char best_name[3][64];
} App;

static App G;
static Widget g_about_shell = NULL;

static void about_destroy_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    (void)call;
    g_about_shell = NULL;
}

static Pixmap mines_scale_pixmap_nearest(Display *dpy,
                                         Pixmap src,
                                         unsigned int src_w,
                                         unsigned int src_h,
                                         unsigned int depth,
                                         float scale)
{
    if (!dpy || src == None) return None;
    if (scale <= 1.0f) return src;
    if (src_w == 0 || src_h == 0) return src;

    unsigned int dst_w = (unsigned int)(src_w * scale + 0.5f);
    unsigned int dst_h = (unsigned int)(src_h * scale + 0.5f);
    if (dst_w < 1) dst_w = 1;
    if (dst_h < 1) dst_h = 1;

    XImage *src_img = XGetImage(dpy, src, 0, 0, src_w, src_h, AllPlanes, ZPixmap);
    if (!src_img) return src;

    Window root = DefaultRootWindow(dpy);
    Pixmap dst = XCreatePixmap(dpy, root, dst_w, dst_h, depth);
    if (dst == None) {
        XDestroyImage(src_img);
        return src;
    }

    Visual *visual = DefaultVisual(dpy, DefaultScreen(dpy));
    XImage *dst_img = XCreateImage(dpy, visual, depth, ZPixmap, 0, NULL, dst_w, dst_h, 32, 0);
    if (!dst_img) {
        XFreePixmap(dpy, dst);
        XDestroyImage(src_img);
        return src;
    }
    if (dst_img->bytes_per_line <= 0) {
        int bpp = dst_img->bits_per_pixel;
        dst_img->bytes_per_line = (int)((dst_w * (unsigned int)bpp + 7u) / 8u);
    }
    dst_img->data = (char *)calloc((size_t)dst_img->bytes_per_line, (size_t)dst_h);
    if (!dst_img->data) {
        XDestroyImage(dst_img);
        XFreePixmap(dpy, dst);
        XDestroyImage(src_img);
        return src;
    }

    for (unsigned int y = 0; y < dst_h; ++y) {
        unsigned int sy = (unsigned int)((float)y / scale);
        if (sy >= src_h) sy = src_h - 1;
        for (unsigned int x = 0; x < dst_w; ++x) {
            unsigned int sx = (unsigned int)((float)x / scale);
            if (sx >= src_w) sx = src_w - 1;
            unsigned long p = XGetPixel(src_img, (int)sx, (int)sy);
            XPutPixel(dst_img, (int)x, (int)y, p);
        }
    }

    GC gc = XCreateGC(dpy, dst, 0, NULL);
    XPutImage(dpy, dst, gc, dst_img, 0, 0, 0, 0, dst_w, dst_h);
    XFreeGC(dpy, gc);

    XDestroyImage(dst_img);
    XDestroyImage(src_img);

    XFreePixmap(dpy, src);
    return dst;
}

static Pixmap mines_create_flattened_pixmap_from_xpm_data(Display *dpy,
                                                          char **xpm_data,
                                                          Widget background_widget,
                                                          float scale)
{
    if (!dpy || !xpm_data) return None;

    Pixmap src = None;
    Pixmap mask = None;
    XpmAttributes attr;
    memset(&attr, 0, sizeof(attr));
    attr.valuemask = XpmSize;
    int status = XpmCreatePixmapFromData(dpy,
                                         DefaultRootWindow(dpy),
                                         xpm_data,
                                         &src,
                                         &mask,
                                         &attr);
    if (status != XpmSuccess || src == None) {
        if (src != None) XFreePixmap(dpy, src);
        if (mask != None) XFreePixmap(dpy, mask);
        return None;
    }

    Pixel bg = BlackPixel(dpy, DefaultScreen(dpy));
    if (background_widget) XtVaGetValues(background_widget, XmNbackground, &bg, NULL);

    Window root = DefaultRootWindow(dpy);
    unsigned int depth = (unsigned int)DefaultDepth(dpy, DefaultScreen(dpy));
    Pixmap dst = XCreatePixmap(dpy, root, attr.width, attr.height, depth);
    if (dst == None) {
        if (mask != None) XFreePixmap(dpy, mask);
        return src;
    }

    XGCValues gcv;
    memset(&gcv, 0, sizeof(gcv));
    gcv.foreground = bg;
    gcv.function = GXcopy;
    GC gc = XCreateGC(dpy, dst, GCForeground | GCFunction, &gcv);
    XFillRectangle(dpy, dst, gc, 0, 0, attr.width, attr.height);
    if (mask != None) {
        XSetClipMask(dpy, gc, mask);
        XSetClipOrigin(dpy, gc, 0, 0);
    }
    XCopyArea(dpy, src, dst, gc, 0, 0, attr.width, attr.height, 0, 0);
    XSetClipMask(dpy, gc, None);

    XFreeGC(dpy, gc);
    XFreePixmap(dpy, src);
    if (mask != None) XFreePixmap(dpy, mask);

    if (scale > 1.0f) {
        return mines_scale_pixmap_nearest(dpy, dst, attr.width, attr.height, depth, scale);
    }
    return dst;
}

static void mines_add_icon_to_about_title_page(Widget notebook)
{
    if (!notebook) return;
    Widget page = XtNameToWidget(notebook, "page_app_about");
    if (!page) return;

    Display *dpy = XtDisplay(notebook);
    Pixmap icon = mines_create_flattened_pixmap_from_xpm_data(dpy, ck_mines_l_pm, page, g_ui_scale);
    if (icon == None) return;

    Arg args[12];
    int n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNtopOffset, 12); n++;
    XtSetArg(args[n], XmNleftOffset, 12); n++;
    XtSetArg(args[n], XmNlabelType, XmPIXMAP); n++;
    XtSetArg(args[n], XmNlabelPixmap, icon); n++;
    XtSetArg(args[n], XmNmarginWidth, 0); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    XtSetArg(args[n], XmNtraversalOn, False); n++;
    Widget icon_label = XmCreateLabel(page, "mines_page_icon", args, n);
    if (icon_label) XtManageChild(icon_label);

    Widget title = XtNameToWidget(page, "label_title");
    if (title && icon_label) {
        XtVaSetValues(title,
                      XmNleftAttachment, XmATTACH_WIDGET,
                      XmNleftWidget, icon_label,
                      XmNleftOffset, 12,
                      XmNalignment, XmALIGNMENT_BEGINNING,
                      NULL);
    }
    Widget subtitle = XtNameToWidget(page, "label_subtitle");
    if (subtitle && icon_label) {
        XtVaSetValues(subtitle,
                      XmNleftAttachment, XmATTACH_WIDGET,
                      XmNleftWidget, icon_label,
                      XmNleftOffset, 12,
                      XmNalignment, XmALIGNMENT_BEGINNING,
                      NULL);
    }
}

/* ---------------------------------------------------------------------------------------
 * Forward decls
 * --------------------------------------------------------------------------------------- */
static void new_game(DiffId diff, int cols, int rows, int mines);
static void redraw_face(void);
static void redraw_leds(void);
static void apply_board_geometry(void);

static void save_config(void);
static void load_config(void);

static void set_status(const char *msg);
static void stop_timer(void);
static void start_timer(void);

static void show_about(void);
static void show_fastest_dialog(void);
static void maybe_record_fastest(void);

static void board_expose_cb(Widget w, XtPointer client, XtPointer call);
static void board_resize_cb(Widget w, XtPointer client, XtPointer call);
static void board_event_handler(Widget w, XtPointer client, XEvent *ev, Boolean *cont);
static void led_expose_cb(Widget w, XtPointer client, XtPointer call);
static void face_expose_cb(Widget w, XtPointer client, XtPointer call);
static void face_activate_cb(Widget w, XtPointer client, XtPointer call);
static void face_arm_cb(Widget w, XtPointer client, XtPointer call);
static void face_disarm_cb(Widget w, XtPointer client, XtPointer call);
static void wm_save_yourself_cb(Widget w, XtPointer client, XtPointer call);

static const char* yes_no(int v) { return v ? "yes" : "no"; }

static void log_xwin(const char *tag, Window win) {
    if (!win) { DBG2("%s: Xwin=None", tag); return; }
    XWindowAttributes a;
    if (!XGetWindowAttributes(G.dpy, win, &a)) {
        DBG("%s: XGetWindowAttributes failed for 0x%lx", tag, (unsigned long)win);
        return;
    }
    DBG2("%s: Xwin=0x%lx map=%s x/y=%d/%d w/h=%d/%d depth=%d",
         tag, (unsigned long)win,
         (a.map_state==IsViewable?"viewable":(a.map_state==IsUnmapped?"unmapped":"unviewable")),
         a.x, a.y, a.width, a.height, a.depth);
}

static void log_widget_geom(const char *tag, Widget w) {
    if (!w) { DBG2("%s: widget=NULL", tag); return; }
    Dimension ww=0, hh=0;
    Position xx=0, yy=0;
    XtVaGetValues(w, XmNwidth, &ww, XmNheight, &hh, XmNx, &xx, XmNy, &yy, NULL);
    DBG2("%s: widget=%p managed=%s realized=%s x/y=%d/%d w/h=%u/%u win=0x%lx",
         tag, (void*)w, yes_no(XtIsManaged(w)), yes_no(XtIsRealized(w)),
         (int)xx, (int)yy, (unsigned)ww, (unsigned)hh,
         (unsigned long)(XtIsRealized(w) ? XtWindow(w) : 0));
    if (XtIsRealized(w)) log_xwin(tag, XtWindow(w));
}

static void log_layout_snapshot(const char *tag) {
    DBG2("layout snapshot: %s", tag);
    log_widget_geom("toplevel", G.toplevel);
    log_widget_geom("mainw", G.mainw);
    log_widget_geom("menubar", G.menubar);
    log_widget_geom("work_form", G.work_form);
    log_widget_geom("top_frame", G.top_frame);
    log_widget_geom("board_frame", G.board_frame);
    log_widget_geom("board_bb", G.board_bb);
    log_widget_geom("board_da", G.board_da);
    log_widget_geom("status_frame", G.status_frame);
}

/* =======================================================================================
 * X error handler
 * ======================================================================================= */
static int xerr_handler(Display *dpy, XErrorEvent *ev) {
    char msg[256];
    XGetErrorText(dpy, ev->error_code, msg, (int)sizeof(msg));
    fprintf(stderr, "[ck-mines] %lu #%lu XError: %s (req=%d.%d res=0x%lx)\n",
            now_ms(), ++g_log_seq, msg, ev->request_code, ev->minor_code, ev->resourceid);
    return 0;
}

/* =======================================================================================
 * DPI / scaling
 * ======================================================================================= */
static float query_x_dpi(Display *dpy) {
    int scr = DefaultScreen(dpy);
    int px = DisplayWidth(dpy, scr);
    int mm = DisplayWidthMM(dpy, scr);
    if (mm <= 0) return 96.0f;
    return (float)px * 25.4f / (float)mm;
}

static int parse_int(const char *s, int defv) {
    if (!s) return defv;
    char *end=NULL;
    long v=strtol(s,&end,10);
    if (end==s) return defv;
    if (v < -2147483647L) v = -2147483647L;
    if (v >  2147483647L) v =  2147483647L;
    return (int)v;
}

static void init_ui_scale_from_env_and_dpi(void) {
    const char *env = getenv("CK_MINES_SCALE");
    if (env && *env) {
        float s = (float)atof(env);
        if (s < 0.5f) s = 0.5f;
        if (s > 4.0f) s = 4.0f;
        g_ui_scale = s;
        DBG("ui_scale override CK_MINES_SCALE=%s -> %.2f", env, g_ui_scale);
        return;
    }
    float dpi = query_x_dpi(G.dpy);
    float s = dpi / 96.0f;
    if (s < 1.0f) s = 1.0f;
    if (s > 4.0f) s = 4.0f;
    g_ui_scale = s;
    DBG("dpi=%.1f -> ui_scale=%.2f", dpi, g_ui_scale);
}

/* =======================================================================================
 * Board pixel size
 * ======================================================================================= */
static void compute_board_pixels(int *out_bw, int *out_bh) {
    int bw = PAD_PX()*2 + G.cols * CELL_PX();
    int bh = PAD_PX()*2 + G.rows * CELL_PX();
    if (out_bw) *out_bw = bw;
    if (out_bh) *out_bh = bh;
}

/* =======================================================================================
 * Safe resize request (Xt)
 * ======================================================================================= */
static int force_widget_size(Widget w, int req_w, int req_h)
{
    XtGeometryResult r;
    Dimension rw = (Dimension)req_w, rh = (Dimension)req_h;
    Dimension rep_w = 0, rep_h = 0;
    int guard = 0;

    if (!w) return 0;

    r = XtMakeResizeRequest(w, rw, rh, &rep_w, &rep_h);

    /* Handle XtGeometryAlmost properly (this is what your log showed). */
    while (r == XtGeometryAlmost && guard++ < 8) {
        if (rep_w == 0) rep_w = rw;
        if (rep_h == 0) rep_h = rh;
        r = XtMakeResizeRequest(w, rep_w, rep_h, &rep_w, &rep_h);
    }

    /* Force widget core size as well (keeps Motif internals consistent). */
    XtVaSetValues(w,
                  XmNwidth,  (Dimension)req_w,
                  XmNheight, (Dimension)req_h,
                  NULL);

    /* Force X window size if realized (keeps widget + Xwin in sync). */
    if (XtIsRealized(w)) {
        XResizeWindow(XtDisplay(w), XtWindow(w), (unsigned)req_w, (unsigned)req_h);
        XSync(XtDisplay(w), False);
    }

    /* Return: 0=Yes, 1=Almost, 2=No (your log convention) */
    {
        int out = (r == XtGeometryYes) ? 0 : (r == XtGeometryAlmost ? 1 : 2);
        DBG2("force_widget_size(%p): req=%dx%d result=%d reply=%ux%u",
             (void*)w, req_w, req_h, out, (unsigned)rep_w, (unsigned)rep_h);
        return out;
    }
}



/* =======================================================================================
 * Config I/O
 * ======================================================================================= */
static void ensure_parent_dir(const char *path) {
    char tmp[512];
    size_t n = strlen(path);
    if (n >= sizeof(tmp)) return;
    strcpy(tmp, path);
    for (int i = (int)strlen(tmp)-1; i >= 0; --i) { if (tmp[i] == '/') { tmp[i]=0; break; } }
    if (tmp[0]==0) return;
    char *p = tmp; if (*p=='/') p++;
    for (; *p; p++) {
        if (*p=='/') { *p=0; (void)mkdir(tmp,0755); *p='/'; }
    }
    (void)mkdir(tmp,0755);
}

static void default_config_path(char *out, size_t outsz) {
    const char *home = getenv("HOME"); if (!home) home=".";
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) snprintf(out,outsz,"%s/ck-core/ck-mines.conf",xdg);
    else snprintf(out,outsz,"%s/.config/ck-core/ck-mines.conf",home);
}

static void trim(char *s) {
    if (!s) return;
    char *p=s; while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n') p++;
    if (p!=s) memmove(s,p,strlen(p)+1);
    size_t n=strlen(s);
    while(n>0 && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\r'||s[n-1]=='\n')) { s[n-1]=0; n--; }
}

static void load_config(void) {
    default_config_path(G.cfg_path,sizeof(G.cfg_path));
    G.diff = DIFF_BEGINNER;
    G.cols = PRESET_BEGINNER.cols;
    G.rows = PRESET_BEGINNER.rows;
    G.mines= PRESET_BEGINNER.mines;
    G.marks_enabled = 1;

    /* Default fastest times */
    for (int i=0;i<3;i++) {
        G.best_time[i] = 0; /* 0 => none recorded yet */
        snprintf(G.best_name[i], sizeof(G.best_name[i]), "%s", "---");
    }

    FILE *f = fopen(G.cfg_path,"r");
    if (!f) {
        const char *home=getenv("HOME");
        if (home) {
            char legacy[512]; snprintf(legacy,sizeof(legacy),"%s/.ck-minesrc",home);
            f = fopen(legacy,"r");
        }
        if (!f) return;
    }
    char line[512];
    while(fgets(line,sizeof(line),f)) {
        trim(line);
        if (!line[0] || line[0]=='#') continue;
        char *eq=strchr(line,'=');
        if(!eq) continue;
        *eq=0;
        char *k=line, *v=eq+1;
        trim(k); trim(v);
        if(!strcmp(k,"diff")) G.diff=(DiffId)parse_int(v,(int)G.diff);
        else if(!strcmp(k,"cols")) G.cols=parse_int(v,G.cols);
        else if(!strcmp(k,"rows")) G.rows=parse_int(v,G.rows);
        else if(!strcmp(k,"mines")) G.mines=parse_int(v,G.mines);
        else if(!strcmp(k,"marks")) G.marks_enabled=parse_int(v,G.marks_enabled);
        else if(!strcmp(k,"best_beginner_time"))      G.best_time[0]=MAX(0,parse_int(v,G.best_time[0]));
        else if(!strcmp(k,"best_intermediate_time"))  G.best_time[1]=MAX(0,parse_int(v,G.best_time[1]));
        else if(!strcmp(k,"best_expert_time"))        G.best_time[2]=MAX(0,parse_int(v,G.best_time[2]));
        else if(!strcmp(k,"best_beginner_name"))      { strncpy(G.best_name[0], v, sizeof(G.best_name[0])-1); G.best_name[0][sizeof(G.best_name[0])-1]=0; }
        else if(!strcmp(k,"best_intermediate_name"))  { strncpy(G.best_name[1], v, sizeof(G.best_name[1])-1); G.best_name[1][sizeof(G.best_name[1])-1]=0; }
        else if(!strcmp(k,"best_expert_name"))        { strncpy(G.best_name[2], v, sizeof(G.best_name[2])-1); G.best_name[2][sizeof(G.best_name[2])-1]=0; }
    }
    fclose(f);

    G.cols = MAX(8, MIN(60, G.cols));
    G.rows = MAX(8, MIN(40, G.rows));
    G.mines = MAX(1, MIN(G.cols*G.rows-1, G.mines));

    if (G.diff != DIFF_BEGINNER && G.diff != DIFF_INTERMEDIATE &&
        G.diff != DIFF_EXPERT && G.diff != DIFF_CUSTOM)
        G.diff = DIFF_BEGINNER;
}

static void save_config(void) {
    ensure_parent_dir(G.cfg_path);
    FILE *f = fopen(G.cfg_path,"w");
    if (!f) {
        const char *home=getenv("HOME"); if(!home) return;
        char legacy[512]; snprintf(legacy,sizeof(legacy),"%s/.ck-minesrc",home);
        f=fopen(legacy,"w"); if(!f) return;
    }
    fprintf(f,"diff=%d\n",(int)G.diff);
    fprintf(f,"cols=%d\n",G.cols);
    fprintf(f,"rows=%d\n",G.rows);
    fprintf(f,"mines=%d\n",G.mines);
    fprintf(f,"marks=%d\n",G.marks_enabled);

    /* Fastest times */
    fprintf(f,"best_beginner_time=%d\n",     MAX(0,G.best_time[0]));
    fprintf(f,"best_beginner_name=%s\n",     G.best_name[0][0] ? G.best_name[0] : "---");
    fprintf(f,"best_intermediate_time=%d\n", MAX(0,G.best_time[1]));
    fprintf(f,"best_intermediate_name=%s\n", G.best_name[1][0] ? G.best_name[1] : "---");
    fprintf(f,"best_expert_time=%d\n",       MAX(0,G.best_time[2]));
    fprintf(f,"best_expert_name=%s\n",       G.best_name[2][0] ? G.best_name[2] : "---");
    fclose(f);
}

/* =======================================================================================
 * Status
 * ======================================================================================= */
static void set_status(const char *msg) {
    DBG2("status: %s", msg ? msg : "(null)");
    if (!G.status_label) return;
    XmString xs = XmStringCreateLocalized((char*)msg);
    XtVaSetValues(G.status_label, XmNlabelString, xs, NULL);
    XmStringFree(xs);
}
/* =======================================================================================
 * Fastest times
 * ======================================================================================= */
static Widget g_fastest_shell = NULL;
static Widget g_fastest_table = NULL;
static Widget g_fastest_note  = NULL;
static Widget g_fastest_mode[3] = {0};
static Widget g_fastest_time[3] = {0};
static Widget g_fastest_name[3] = {0};

static const char* diff_label_from_index(int idx) {
    switch (idx) {
        case 0: return PRESET_BEGINNER.name;
        case 1: return PRESET_INTERMEDIATE.name;
        case 2: return PRESET_EXPERT.name;
        default: return "";
    }
}
static void update_fastest_dialog_text(void) {
    if (!g_fastest_table) return;

    for (int i = 0; i < 3; i++) {
        const char *mode = diff_label_from_index(i);
        const char *name = (G.best_name[i][0] ? G.best_name[i] : "---");
        char timebuf[16];
        if (G.best_time[i] <= 0) snprintf(timebuf, sizeof(timebuf), "---");
        else snprintf(timebuf, sizeof(timebuf), "%d", G.best_time[i]);

        XmString xs_mode = XmStringCreateLocalized((char*)mode);
        XmString xs_time = XmStringCreateLocalized(timebuf);
        XmString xs_name = XmStringCreateLocalized((char*)name);
        XtVaSetValues(g_fastest_mode[i], XmNlabelString, xs_mode, NULL);
        XtVaSetValues(g_fastest_time[i], XmNlabelString, xs_time, NULL);
        XtVaSetValues(g_fastest_name[i], XmNlabelString, xs_name, NULL);
        XmStringFree(xs_mode);
        XmStringFree(xs_time);
        XmStringFree(xs_name);
    }
}

static void fastest_reset_cb(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)client; (void)call;
    for (int i=0;i<3;i++) {
        G.best_time[i] = 0;
        snprintf(G.best_name[i], sizeof(G.best_name[i]), "%s", "---");
    }
    save_config();
    update_fastest_dialog_text();
}

static void fastest_close_cb(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)client; (void)call;
    if (g_fastest_shell) XtUnmanageChild(g_fastest_shell);
}

static void ensure_fastest_dialog(void) {
    if (g_fastest_shell) return;

    /* Use a FormDialog to keep it WM-managed and centered reasonably. */
    g_fastest_shell = XmCreateFormDialog(G.toplevel, "fastestDialog", NULL, 0);
    XtVaSetValues(XtParent(g_fastest_shell),
                  XmNtitle, "Fastest Mine Sweepers",
                  XmNallowShellResize, True,
                  NULL);

    /*
     * The shell already has a window title; don't duplicate it inside the dialog.
     * Build a real 3-column table: three vertical columns packed horizontally.
     * This avoids RowColumn packing quirks (header showing vertically / everything in one row).
     */
    Widget cols = XtVaCreateManagedWidget("fastestTable", xmRowColumnWidgetClass, g_fastest_shell,
                                          XmNtopAttachment, XmATTACH_FORM,
                                          XmNleftAttachment, XmATTACH_FORM,
                                          XmNrightAttachment, XmATTACH_FORM,
                                          XmNorientation, XmHORIZONTAL,
                                          XmNpacking, XmPACK_TIGHT,
                                          XmNspacing, S(18),
                                          XmNmarginHeight, S(10),
                                          XmNmarginWidth,  S(10),
                                          NULL);

    /* Keep using g_fastest_table for attachments below */
    g_fastest_table = cols;

    /* Column containers */
    Widget col_mode = XtVaCreateManagedWidget("colMode", xmRowColumnWidgetClass, cols,
                                              XmNorientation, XmVERTICAL,
                                              XmNpacking, XmPACK_TIGHT,
                                              XmNspacing, S(6),
                                              XmNisAligned, True,
                                              XmNentryAlignment, XmALIGNMENT_BEGINNING,
                                              NULL);

    Widget col_time = XtVaCreateManagedWidget("colTime", xmRowColumnWidgetClass, cols,
                                              XmNorientation, XmVERTICAL,
                                              XmNpacking, XmPACK_TIGHT,
                                              XmNspacing, S(6),
                                              XmNisAligned, True,
                                              XmNentryAlignment, XmALIGNMENT_END,
                                              NULL);

    Widget col_name = XtVaCreateManagedWidget("colName", xmRowColumnWidgetClass, cols,
                                              XmNorientation, XmVERTICAL,
                                              XmNpacking, XmPACK_TIGHT,
                                              XmNspacing, S(6),
                                              XmNisAligned, True,
                                              XmNentryAlignment, XmALIGNMENT_BEGINNING,
                                              NULL);

    /* Headers */
    {
        Widget h_mode = XtVaCreateManagedWidget("hdrMode", xmLabelWidgetClass, col_mode,
                                                XmNalignment, XmALIGNMENT_BEGINNING, NULL);
        Widget h_time = XtVaCreateManagedWidget("hdrTime", xmLabelWidgetClass, col_time,
                                                XmNalignment, XmALIGNMENT_END, NULL);
        Widget h_name = XtVaCreateManagedWidget("hdrName", xmLabelWidgetClass, col_name,
                                                XmNalignment, XmALIGNMENT_BEGINNING, NULL);

        XmString xs1 = XmStringCreateLocalized("Mode");
        XmString xs2 = XmStringCreateLocalized("Time");
        XmString xs3 = XmStringCreateLocalized("Name");
        XtVaSetValues(h_mode, XmNlabelString, xs1, NULL);
        XtVaSetValues(h_time, XmNlabelString, xs2, NULL);
        XtVaSetValues(h_name, XmNlabelString, xs3, NULL);
        XmStringFree(xs1);
        XmStringFree(xs2);
        XmStringFree(xs3);
    }

    /* Data rows (3) */
    for (int i = 0; i < 3; i++) {
        g_fastest_mode[i] = XtVaCreateManagedWidget("mode", xmLabelWidgetClass, col_mode,
                                                    XmNalignment, XmALIGNMENT_BEGINNING,
                                                    NULL);
        g_fastest_time[i] = XtVaCreateManagedWidget("time", xmLabelWidgetClass, col_time,
                                                    XmNalignment, XmALIGNMENT_END,
                                                    NULL);
        g_fastest_name[i] = XtVaCreateManagedWidget("name", xmLabelWidgetClass, col_name,
                                                    XmNalignment, XmALIGNMENT_BEGINNING,
                                                    NULL);
    }

    g_fastest_note = XtVaCreateManagedWidget("fastestNote", xmLabelWidgetClass, g_fastest_shell,
                                             XmNtopAttachment, XmATTACH_WIDGET,
                                             XmNtopWidget, g_fastest_table,
                                             XmNleftAttachment, XmATTACH_FORM,
                                             XmNrightAttachment, XmATTACH_FORM,
                                             XmNalignment, XmALIGNMENT_BEGINNING,
                                             XmNmarginHeight, S(0),
                                             XmNmarginWidth,  S(10),
                                             NULL);
    {
        XmString xs = XmStringCreateLocalized("(Time in seconds)");
        XtVaSetValues(g_fastest_note, XmNlabelString, xs, NULL);
        XmStringFree(xs);
    }

    Widget btn_rc = XtVaCreateManagedWidget("fastestButtons", xmRowColumnWidgetClass, g_fastest_shell,
                                            XmNorientation, XmHORIZONTAL,
                                            XmNpacking, XmPACK_TIGHT,
                                            XmNspacing, S(8),
                                            XmNtopAttachment, XmATTACH_WIDGET,
                                            XmNtopWidget, g_fastest_note,
                                            XmNleftAttachment, XmATTACH_FORM,
                                            XmNrightAttachment, XmATTACH_FORM,
                                            XmNbottomAttachment, XmATTACH_FORM,
                                            XmNmarginHeight, S(8),
                                            XmNmarginWidth,  S(10),
                                            NULL);

    Widget reset = XtVaCreateManagedWidget("Reset Scores", xmPushButtonWidgetClass, btn_rc, NULL);
    XtAddCallback(reset, XmNactivateCallback, fastest_reset_cb, NULL);

    Widget ok = XtVaCreateManagedWidget("OK", xmPushButtonWidgetClass, btn_rc, NULL);
    XtAddCallback(ok, XmNactivateCallback, fastest_close_cb, NULL);

    update_fastest_dialog_text();
}


static void show_fastest_dialog(void) {
    ensure_fastest_dialog();
    update_fastest_dialog_text();
    XtManageChild(g_fastest_shell);
}

typedef struct {
    int idx;   /* 0..2 */
    int time;  /* seconds */
    Widget dialog;
} FastestNameCtx;

static void fastest_name_ok_cb(Widget w, XtPointer client, XtPointer call) {
    (void)w;
    FastestNameCtx *ctx = (FastestNameCtx*)client;
    if (!ctx) return;
    (void)call;

    Widget text = XmSelectionBoxGetChild(ctx->dialog, XmDIALOG_TEXT);
    char *name = XmTextFieldGetString(text);
    if (!name) name = XtNewString("Anonymous");
    trim(name);
    if (!name[0]) {
        XtFree(name);
        name = XtNewString("Anonymous");
    }

    G.best_time[ctx->idx] = ctx->time;
    strncpy(G.best_name[ctx->idx], name, sizeof(G.best_name[ctx->idx])-1);
    G.best_name[ctx->idx][sizeof(G.best_name[ctx->idx])-1] = 0;
    save_config();

    XtFree(name);

    XtUnmanageChild(ctx->dialog);
    XtDestroyWidget(ctx->dialog);

    /* After confirming name, show the scoreboard. */
    show_fastest_dialog();

    free(ctx);
}

static void fastest_name_cancel_cb(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)call;
    FastestNameCtx *ctx = (FastestNameCtx*)client;
    if (!ctx) return;
    XtUnmanageChild(ctx->dialog);
    XtDestroyWidget(ctx->dialog);
    free(ctx);
}

static void prompt_for_fastest_name(int idx, int time_seconds) {
    FastestNameCtx *ctx = (FastestNameCtx*)calloc(1, sizeof(FastestNameCtx));
    if (!ctx) return;
    ctx->idx = idx;
    ctx->time = time_seconds;

    Widget d = XmCreatePromptDialog(G.toplevel, "fastestName", NULL, 0);
    ctx->dialog = d;

    /* Title */
    XtVaSetValues(XtParent(d), XmNtitle, "New Record", NULL);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "Congratulations!\n\nYou have the fastest time for %s.\nPlease enter your name:",
             diff_label_from_index(idx));
    XmString xmsg = XmStringCreateLocalized(msg);
    XtVaSetValues(d, XmNselectionLabelString, xmsg, NULL);
    XmStringFree(xmsg);

    /* Default to existing name (or ---). */
    Widget text = XmSelectionBoxGetChild(d, XmDIALOG_TEXT);
    XmTextFieldSetString(text, G.best_name[idx][0] ? G.best_name[idx] : "---");
    XmTextFieldSetSelection(text, 0, XmTextFieldGetLastPosition(text), CurrentTime);

    /* Hide help button (not used) */
    Widget help = XmSelectionBoxGetChild(d, XmDIALOG_HELP_BUTTON);
    if (help) XtUnmanageChild(help);

    XtAddCallback(d, XmNokCallback, fastest_name_ok_cb, (XtPointer)ctx);
    XtAddCallback(d, XmNcancelCallback, fastest_name_cancel_cb, (XtPointer)ctx);

    XtManageChild(d);
}

static int diff_to_best_index(DiffId d) {
    if (d == DIFF_BEGINNER) return 0;
    if (d == DIFF_INTERMEDIATE) return 1;
    if (d == DIFF_EXPERT) return 2;
    return -1;
}

static void maybe_record_fastest(void) {
    int idx = diff_to_best_index(G.diff);
    if (idx < 0 || idx > 2) return; /* only preset difficulties */
    if (G.elapsed <= 0) return;

    int best = G.best_time[idx];
    if (best == 0 || G.elapsed < best) {
        prompt_for_fastest_name(idx, G.elapsed);
    }
}


/* =======================================================================================
 * Model helpers
 * ======================================================================================= */
static int idx_of(int x,int y){ return y*G.cols+x; }
static int in_bounds(int x,int y){ return x>=0 && x<G.cols && y>=0 && y<G.rows; }
static Cell* cell_at(int x,int y){ return &G.cells[idx_of(x,y)]; }
static int rand_u32(void){ return rand() & 0x7fffffff; }

static void compute_adjacency(void){
    for(int y=0;y<G.rows;y++) for(int x=0;x<G.cols;x++){
        Cell *c=cell_at(x,y);
        if(c->mine){ c->adj=0; continue; }
        int n=0;
        for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++){
            if(dx==0 && dy==0) continue;
            int xx=x+dx, yy=y+dy;
            if(!in_bounds(xx,yy)) continue;
            if(cell_at(xx,yy)->mine) n++;
        }
        c->adj=(unsigned char)n;
    }
}

static void place_mines_avoiding(int avoid_index){
    int total=G.cols*G.rows;
    int placed=0;
    while(placed<G.mines){
        int r=rand_u32()%total;
        if(r==avoid_index) continue;
        Cell *c=&G.cells[r];
        if(c->mine) continue;
        c->mine=1;
        placed++;
    }
    compute_adjacency();
}

static void reveal_flood(int start){
    int total=G.cols*G.rows;
    int *stack=(int*)malloc((size_t)total*sizeof(int));
    int sp=0;
    stack[sp++]=start;
    while(sp>0){
        int i=stack[--sp];
        Cell *c=&G.cells[i];
        if(c->revealed || c->flagged) continue;
        c->revealed=1;
        c->question=0;
        G.revealed_count++;
        int x=i%G.cols, y=i/G.cols;
        if(c->adj==0 && !c->mine){
            for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++){
                if(dx==0 && dy==0) continue;
                int xx=x+dx, yy=y+dy;
                if(!in_bounds(xx,yy)) continue;
                int ni=idx_of(xx,yy);
                Cell *n=&G.cells[ni];
                if(!n->revealed && !n->flagged) stack[sp++]=ni;
            }
        }
    }
    free(stack);
}

static int count_flagged_neighbors(int x,int y){
    int n=0;
    for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++){
        if(dx==0 && dy==0) continue;
        int xx=x+dx, yy=y+dy;
        if(!in_bounds(xx,yy)) continue;
        if(cell_at(xx,yy)->flagged) n++;
    }
    return n;
}

static void check_win(void){
    int total=G.cols*G.rows;
    int target=total-G.mines;
    if(G.game_over) return;
    if(G.revealed_count>=target){
        G.game_over=2;
        G.face=FACE_COOL;
        stop_timer();
        for(int i=0;i<total;i++){
            if(G.cells[i].mine && !G.cells[i].flagged){
                G.cells[i].flagged=1;
                G.flags_count++;
            }
        }
        set_status("You win!");
        redraw_leds();

        /* If this is a new record for the current difficulty, ask for a name. */
        maybe_record_fastest();
    }
}

static void reveal_cell(int index){
    if(G.game_over) return;
    Cell *c=&G.cells[index];
    if(c->revealed || c->flagged) return;

    if(!G.first_click_done){
        place_mines_avoiding(index);
        G.first_click_done=1;
        start_timer();
        set_status("Good luck!");
    }
    if(c->mine){
        c->exploded=1;
        G.exploded_index=index;
        G.game_over=1;
        G.face=FACE_DEAD;
        stop_timer();
        set_status("BOOM! You hit a mine.");
        for(int i=0;i<G.cols*G.rows;i++) G.cells[i].revealed=1;
        return;
    }
    reveal_flood(index);
    check_win();
}

static void chord_open(int x,int y){
    Cell *c=cell_at(x,y);
    if(!c->revealed || c->adj==0) return;
    int f=count_flagged_neighbors(x,y);
    if(f!=c->adj) return;

    for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++){
        if(dx==0 && dy==0) continue;
        int xx=x+dx, yy=y+dy;
        if(!in_bounds(xx,yy)) continue;
        Cell *n=cell_at(xx,yy);
        if(n->flagged || n->revealed) continue;
        int ni=idx_of(xx,yy);

        if(!G.first_click_done){
            place_mines_avoiding(ni);
            G.first_click_done=1;
            start_timer();
        }
        if(n->mine){
            n->exploded=1;
            G.exploded_index=ni;
            G.game_over=1;
            G.face=FACE_DEAD;
            stop_timer();
            set_status("BOOM! You hit a mine.");
            for(int i=0;i<G.cols*G.rows;i++) G.cells[i].revealed=1;
            return;
        } else {
            reveal_flood(ni);
        }
    }
    check_win();
}

static void toggle_flag_question(int index){
    Cell *c=&G.cells[index];
    if(c->revealed) return;

    if(!G.marks_enabled){
        if(c->flagged){ c->flagged=0; G.flags_count--; }
        else { c->flagged=1; c->question=0; G.flags_count++; }
        redraw_leds();
        return;
    }
    if(!c->flagged && !c->question){
        c->flagged=1; c->question=0; G.flags_count++;
    } else if(c->flagged){
        c->flagged=0; c->question=1; G.flags_count--;
    } else {
        c->question=0;
    }
    redraw_leds();
}

/* =======================================================================================
 * Palette/drawing primitives  (PURE CDE/Motif palette, NO hardcoded black)
 * ======================================================================================= */
	static void palette_from_widget(Widget w)
	{
	    Pixel bg;
	    XtVaGetValues(w, XmNbackground, &bg, NULL);

	    Pixel fg, top, bottom, select;
	    XmGetColors(XtScreen(w),
	                DefaultColormapOfScreen(XtScreen(w)),
	                bg, &fg, &top, &bottom, &select);

    G.col_bg     = bg;
    G.col_fg     = fg;
	    G.col_top    = top;
	    G.col_bottom = bottom;
	    G.col_select = select;
	    G.col_black  = BlackPixel(G.dpy, DefaultScreen(G.dpy));

    /* Everything below stays within palette logic:
       - red: try allocate a *named* red; if unavailable, fall back to fg (still readable)
       - LED on/off: derive from palette so it matches CDE theme and stays readable
    */
    {
        Colormap cmap = DefaultColormap(G.dpy, DefaultScreen(G.dpy));
        XColor scr, exact;

        /* Mine/flag red (optional, but nice). Still NOT black, and not #RRGGBBFF. */
        if (XAllocNamedColor(G.dpy, cmap, "red3", &scr, &exact) ||
            XAllocNamedColor(G.dpy, cmap, "firebrick3", &scr, &exact) ||
            XAllocNamedColor(G.dpy, cmap, "red", &scr, &exact))
        {
            G.col_red = scr.pixel;
        }
        else {
            G.col_red = fg;
        }

	        /* LED colors (pure palette):
	           - ON: use red if available, otherwise fg.
	           - OFF: use a shadow-ish palette color (bottom) so segments "ghost" but remain readable.
	           - Background of LED area will be constant dark gray (see led_draw_3digits()).
	        */
	        G.col_led_on  = G.col_red ? G.col_red : fg;
	        /* Keep LED background stable across color schemes for consistent contrast. */
	        if (XAllocNamedColor(G.dpy, cmap, "#101010", &scr, &exact) ||
	            XAllocNamedColor(G.dpy, cmap, "gray10", &scr, &exact) ||
	            XAllocNamedColor(G.dpy, cmap, "grey10", &scr, &exact))
	        {
	            G.col_led_bg = scr.pixel;
	        } else {
	            G.col_led_bg = G.col_black;
	        }

	        if (XAllocNamedColor(G.dpy, cmap, "#2a2a2a", &scr, &exact) ||
	            XAllocNamedColor(G.dpy, cmap, "gray20", &scr, &exact) ||
	            XAllocNamedColor(G.dpy, cmap, "grey20", &scr, &exact))
	        {
	            G.col_led_off = scr.pixel;
	        } else {
	            G.col_led_off = G.col_led_bg;
	        }

        /* Yellow for smiley face */
        if (XAllocNamedColor(G.dpy, cmap, "yellow", &scr, &exact) ||
            XAllocNamedColor(G.dpy, cmap, "gold", &scr, &exact)) {
            G.col_yellow = scr.pixel;
        } else {
            G.col_yellow = select; // Fallback
        }
    }

	    DBG2("palette: bg=%lu fg=%lu top=%lu bottom=%lu select=%lu red=%lu yellow=%lu led_on=%lu led_off=%lu",
	         (unsigned long)G.col_bg, (unsigned long)G.col_fg,
	         (unsigned long)G.col_top, (unsigned long)G.col_bottom,
	         (unsigned long)G.col_select, (unsigned long)G.col_red,
	         (unsigned long)G.col_yellow,
	         (unsigned long)G.col_led_on, (unsigned long)G.col_led_off);
	}


static void fill_rect(Drawable d,int x,int y,int w,int h,Pixel col){
    XSetForeground(G.dpy,G.gc,col);
    XFillRectangle(G.dpy,d,G.gc,x,y,(unsigned)w,(unsigned)h);
}

static void draw_3d_rect(Drawable d,int x,int y,int w,int h,int sunken){
    Pixel tl = sunken ? G.col_bottom : G.col_top;
    Pixel br = sunken ? G.col_top : G.col_bottom;

    XSetForeground(G.dpy,G.gc,tl);
    XDrawLine(G.dpy,d,G.gc,x,y,x+w-1,y);
    XDrawLine(G.dpy,d,G.gc,x,y,x,y+h-1);

    XSetForeground(G.dpy,G.gc,br);
    XDrawLine(G.dpy,d,G.gc,x,y+h-1,x+w-1,y+h-1);
    XDrawLine(G.dpy,d,G.gc,x+w-1,y,x+w-1,y+h-1);
}

/* =======================================================================================
 * Optional backbuffer
 * ======================================================================================= */
static void ensure_backbuffer(int w,int h){
    if(!G.use_backbuf) return;
    if(!G.board_da || !XtIsRealized(G.board_da)) return;
    if(w<=2 || h<=2) return;
    if(G.backbuf && G.bb_w==w && G.bb_h==h) return;

    if(G.backbuf) XFreePixmap(G.dpy,G.backbuf);

    Window win = XtWindow(G.board_da);
    XWindowAttributes a;
    XGetWindowAttributes(G.dpy, win, &a);

    G.backbuf = XCreatePixmap(G.dpy, win, (unsigned)w,(unsigned)h,(unsigned)a.depth);
    G.bb_w=w; G.bb_h=h;
    DBG("backbuffer created %dx%d depth=%d", w,h,a.depth);
}

/* =======================================================================================
 * LED drawing
 * ======================================================================================= */
static unsigned char segmap_for_digit(int d){
    static const unsigned char map[10]={0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F};
    if(d<0||d>9) return 0;
    return map[d];
}
static unsigned char segmap_for_minus(void){ return 0x40; }

static void led_draw_segment_rect(Drawable dr,int x,int y,int w,int h,int enabled)
{
    /* OFF should not be an arbitrary gray. Use theme shadow (bottom) so it looks native. */
    fill_rect(dr, x, y, w, h, enabled ? G.col_led_on : G.col_led_off);
}


static void led_draw_one(Drawable dr,int x,int y,unsigned char segs){
    int t=MAX(2,S(3));
    int w=LED_W_PX();
    int h=LED_H_PX();
    led_draw_segment_rect(dr,x+t,y+0,      w-2*t,t,           (segs&(1<<0))!=0);
    led_draw_segment_rect(dr,x+w-t,y+t,    t,(h/2)-t-1,       (segs&(1<<1))!=0);
    led_draw_segment_rect(dr,x+w-t,y+(h/2)+1,t,(h/2)-t-1,     (segs&(1<<2))!=0);
    led_draw_segment_rect(dr,x+t,y+h-t,    w-2*t,t,           (segs&(1<<3))!=0);
    led_draw_segment_rect(dr,x+0,y+(h/2)+1,t,(h/2)-t-1,       (segs&(1<<4))!=0);
    led_draw_segment_rect(dr,x+0,y+t,      t,(h/2)-t-1,       (segs&(1<<5))!=0);
    led_draw_segment_rect(dr,x+t,y+(h/2)-1,w-2*t,t,           (segs&(1<<6))!=0);
}

	static void led_draw_3digits(Drawable dr,int value,int total_w,int total_h)
	{
	    /* Fixed LED background for consistent contrast across color schemes. */
	    fill_rect(dr, 0, 0, total_w, total_h, G.col_led_bg);
	    draw_3d_rect(dr, 0, 0, total_w, total_h, 1);

    int x = LED_PAD_PX();
    int y = LED_PAD_PX();

    unsigned char s2, s1, s0;
    if (value < 0) {
        int v = -value; if (v > 99) v = 99;
        s2 = segmap_for_minus();
        s1 = segmap_for_digit((v/10) % 10);
        s0 = segmap_for_digit(v % 10);
    } else {
        if (value > 999) value = 999;
        s2 = segmap_for_digit((value/100) % 10);
        s1 = segmap_for_digit((value/10) % 10);
        s0 = segmap_for_digit(value % 10);
    }

    led_draw_one(dr, x, y, s2); x += LED_W_PX() + LED_GAP_PX();
    led_draw_one(dr, x, y, s1); x += LED_W_PX() + LED_GAP_PX();
    led_draw_one(dr, x, y, s0);
}


static void led_expose_cb(Widget w, XtPointer client, XtPointer call){
    (void)call;
    int which=(int)(intptr_t)client;
    if(!XtIsRealized(w)) return;

    Dimension ww=0, hh=0;
    XtVaGetValues(w, XmNwidth,&ww, XmNheight,&hh, NULL);
    if(ww<10 || hh<10) return;

    Pixmap pm = XCreatePixmap(G.dpy, XtWindow(w), (unsigned)ww,(unsigned)hh,
                              (unsigned)DefaultDepthOfScreen(XtScreen(w)));

    int val = (which==0) ? G.led_mines_value : G.led_time_value;
    led_draw_3digits(pm, val, (int)ww,(int)hh);

    XCopyArea(G.dpy, pm, XtWindow(w), G.gc, 0,0,(unsigned)ww,(unsigned)hh,0,0);
    XFreePixmap(G.dpy, pm);
    XFlush(G.dpy);
}

static void redraw_leds(void){
    int remaining = G.mines - G.flags_count;
    G.led_mines_value = remaining;
    G.led_time_value  = G.elapsed;

    if(G.mine_led && XtIsRealized(G.mine_led)) led_expose_cb(G.mine_led,(XtPointer)(intptr_t)0,NULL);
    if(G.time_led && XtIsRealized(G.time_led)) led_expose_cb(G.time_led,(XtPointer)(intptr_t)1,NULL);
}

/* =======================================================================================
 * Board drawing helpers (IMPORTANT: use X window size)
 * ======================================================================================= */
static int get_board_xwin_size(int *out_w, int *out_h, int *out_viewable) {
    if(!G.board_da || !XtIsRealized(G.board_da)) return 0;
    Window win = XtWindow(G.board_da);
    if(!win) return 0;
    XWindowAttributes a;
    if(!XGetWindowAttributes(G.dpy, win, &a)) return 0;
    if(out_w) *out_w = a.width;
    if(out_h) *out_h = a.height;
    if(out_viewable) *out_viewable = (a.map_state==IsViewable) ? 1 : 0;
    return 1;
}

static void draw_text_center(Drawable d,int cx,int cy,const char *s){
    if(!G.font || !s) return;
    int dir,asc,desc;
    XCharStruct overall;
    XTextExtents(G.font, s, (int)strlen(s), &dir,&asc,&desc,&overall);
    int x = cx - overall.width/2;
    int y = cy + (asc - desc)/2;
    XSetForeground(G.dpy, G.gc, G.col_fg);
    XDrawString(G.dpy, d, G.gc, x,y, s, (int)strlen(s));
}

static void draw_cell(Drawable d,int gx,int gy,int index){
    int cell=CELL_PX();
    int pad =PAD_PX();
    int px=pad+gx*cell;
    int py=pad+gy*cell;

    Cell *c=&G.cells[index];
    int pressed=(G.pressed_cell==index) && G.left_down && !G.game_over;

    if(c->revealed || pressed || G.game_over){
        fill_rect(d,px,py,cell,cell,G.col_bg);
        draw_3d_rect(d,px,py,cell,cell,1);

	        if(c->mine){
	            int cx=px+cell/2, cy=py+cell/2;
	            int inset=MAX(1,S(1));
	            if(c->exploded) fill_rect(d,px+inset,py+inset,cell-2*inset,cell-2*inset,G.col_red);

	            /* Always draw mines in black, independent of theme. */
	            XSetForeground(G.dpy, G.gc, G.col_black);

	            int r=(int)(MAX(3,S(4))*1.5f);
	            XFillArc(G.dpy,d,G.gc,cx-r,cy-r,(unsigned)(2*r),(unsigned)(2*r),0,360*64);

	            int arm=MAX(6,S(7));
	            for (int i=0; i<=1; i++) {
	                XDrawLine(G.dpy,d,G.gc,cx-arm+i,cy,cx+arm+i,cy);
	                XDrawLine(G.dpy,d,G.gc,cx-i,cy-arm,cx,cy+arm-i);
	                XDrawLine(G.dpy,d,G.gc,cx-(arm-1),cy-(arm-1)-i,cx+(arm-1),cy+(arm-1)-i);
	                XDrawLine(G.dpy,d,G.gc,cx-(arm-1)+i,cy+(arm-1),cx+(arm-1)+i,cy-(arm-1));
	            }
            // draw white rectangle in top-left quarter of mine as highlight
            XSetForeground(G.dpy,G.gc,G.col_select);
            XFillRectangle(G.dpy,d,G.gc,cx-r/2+1,cy-r/2+1,(unsigned)g_ui_scale,(unsigned)g_ui_scale);
        } else if(c->adj>0){
            char buf[4]; snprintf(buf,sizeof(buf),"%d",(int)c->adj);
            draw_text_center(d,px+cell/2,py+cell/2,buf);
        }
    } else {
        fill_rect(d,px,py,cell,cell,G.col_select);
        draw_3d_rect(d,px,py,cell,cell,0);

        if(c->flagged){
            int fx=px+cell/2 - MAX(2,S(2));
            int fy=py+MAX(3,S(4));
            XSetForeground(G.dpy,G.gc,G.col_fg);
            XDrawLine(G.dpy,d,G.gc,fx,fy,fx,py+cell-MAX(3,S(4)));

            XSetForeground(G.dpy,G.gc,G.col_red);
            XFillRectangle(G.dpy,d,G.gc,fx+1,fy,(unsigned)MAX(7,S(7)),(unsigned)MAX(5,S(5)));

            XSetForeground(G.dpy,G.gc,G.col_fg);
            XDrawRectangle(G.dpy,d,G.gc,fx+1,fy,(unsigned)MAX(7,S(7)),(unsigned)MAX(5,S(5)));
        } else if(c->question){
            draw_text_center(d,px+cell/2,py+cell/2,"?");
        }
    }
}

static void draw_board_to(Drawable d,int w,int h){
    fill_rect(d,0,0,w,h,G.col_bg);

    int bw,bh;
    compute_board_pixels(&bw,&bh);

    fill_rect(d,0,0,bw,bh,G.col_bg);
    draw_3d_rect(d,0,0,bw,bh,1);

    if(!G.cells) return;

    for(int y=0;y<G.rows;y++) for(int x=0;x<G.cols;x++){
        draw_cell(d,x,y,idx_of(x,y));
    }
}

static void redraw_board(void)
{
    int xw = 0, xh = 0, view = 0;

    if (!get_board_xwin_size(&xw, &xh, &view))
        return;

    DBG2("redraw_board: xwin=%dx%d map=%d want=%dx%d", xw, xh, view, G.want_bw, G.want_bh);

    if (!view || xw <= 0 || xh <= 0)
        return;

    /* If backbuffer enabled, ensure it matches and draw to it; else draw direct. */
    if (G.use_backbuf) {
        ensure_backbuffer(xw, xh);
        if (!G.backbuf) return;

        XSetForeground(G.dpy, G.gc, G.col_bg);
        XFillRectangle(G.dpy, G.backbuf, G.gc, 0, 0, (unsigned)xw, (unsigned)xh);

        draw_board_to(G.backbuf, xw, xh);

        XCopyArea(G.dpy, G.backbuf, XtWindow(G.board_da), G.gc,
                  0, 0, (unsigned)xw, (unsigned)xh, 0, 0);
    } else {
        Drawable dst = XtWindow(G.board_da);

        XSetForeground(G.dpy, G.gc, G.col_bg);
        XFillRectangle(G.dpy, dst, G.gc, 0, 0, (unsigned)xw, (unsigned)xh);

        draw_board_to(dst, xw, xh);
    }

    XFlush(G.dpy);
}


/* =======================================================================================
 * Face drawing
 * ======================================================================================= */
	static void draw_face_to(Drawable d,int w,int h){
    
    fill_rect(d,0,0,w,h,G.col_bg);

    int cx = w / 2, cy = h / 2;

    // Tunable parameters (fractions of w/h, easy to fine-tune)
    float radius_frac = 0.4f; // Radius as fraction of min(w,h)
    float eye_size_frac = 0.1f; // Eye size fraction of min(w,h)
    float eye_x_offset_frac = 0.25f; // Horizontal offset from center (symmetric left/right)
    float eye_y_frac = 0.4f; // Vertical position from top (0.0 top, 1.0 bottom; higher = down)
    float mouth_y_frac = 0.55f; // Vertical position from top (lower = up)
    float mouth_width_frac = 0.5f; // Mouth width fraction of w
    float mouth_height_frac = 0.3f; // Mouth height fraction of h
    float thickness = g_ui_scale; // Line thickness for multi-line draws

    int min_dim = MIN(w, h);
    int R = (int)(radius_frac * min_dim);
    int eye_size = (int)(eye_size_frac * min_dim);
    int eye_x_offset = (int)(eye_x_offset_frac * (w / 2.0f));
    int eye_y = (int)(eye_y_frac * h);
    int mouth_y = (int)(mouth_y_frac * h);
    int mouth_width = (int)(mouth_width_frac * w);
    int mouth_height = (int)(mouth_height_frac * h);

    /* Fill the circle with yellow */
	    XSetForeground(G.dpy, G.gc, G.col_yellow);
	    XFillArc(G.dpy, d, G.gc, cx - R, cy - R, (unsigned)(2 * R), (unsigned)(2 * R), 0, 360 * 64);

	    /* Always draw face features in black, independent of theme. */
	    XSetForeground(G.dpy, G.gc, G.col_black);
	    XDrawArc(G.dpy, d, G.gc, cx - R, cy - R, (unsigned)(2 * R), (unsigned)(2 * R), 0, 360 * 64);

    if (G.face == FACE_DEAD) {
        // Left eye (X)
        for (int i = 0; i <= thickness; i++) {
            XDrawLine(G.dpy, d, G.gc, cx - eye_x_offset - eye_size, eye_y + i - eye_size, cx - eye_x_offset + eye_size, eye_y + i + eye_size);
            XDrawLine(G.dpy, d, G.gc, cx - eye_x_offset - eye_size, eye_y + i + eye_size, cx - eye_x_offset + eye_size, eye_y + i - eye_size);
        }
        // Right eye (X, symmetric)
        for (int i = 0; i <= thickness; i++) {
            XDrawLine(G.dpy, d, G.gc, cx + eye_x_offset - eye_size, eye_y + i - eye_size, cx + eye_x_offset + eye_size, eye_y + i + eye_size);
            XDrawLine(G.dpy, d, G.gc, cx + eye_x_offset - eye_size, eye_y + i + eye_size, cx + eye_x_offset + eye_size, eye_y + i - eye_size);
        }
        // Frown mouth
        for (int i = 0; i <= thickness; i++) {
            XDrawArc(G.dpy, d, G.gc, cx - mouth_width / 2, mouth_y + mouth_height/2 + i, (unsigned)mouth_width, (unsigned)mouth_height, 45 * 64, 90 * 64);
        }
    } else if (G.face == FACE_OHNO) {
        // Left eye (small circle)
        XFillArc(G.dpy, d, G.gc, cx - eye_x_offset - eye_size / 2, eye_y - eye_size / 2, (unsigned)eye_size, (unsigned)eye_size, 0, 360 * 64);
        // Right eye (symmetric)
        XFillArc(G.dpy, d, G.gc, cx + eye_x_offset - eye_size / 2, eye_y - eye_size / 2, (unsigned)eye_size, (unsigned)eye_size, 0, 360 * 64);
        // Surprised mouth (O)
        int o_size = MIN(mouth_width, mouth_height) / 3;
        for (int i = 0; i <= thickness; i++) {
            XDrawArc(G.dpy, d, G.gc, cx - o_size + i, mouth_y  + i, (unsigned)(2 * o_size - 2 * i), (unsigned)(2 * o_size - 2 * i), 0, 360 * 64);
        }
    } else if (G.face == FACE_COOL) {
        // Sunglasses line
        int glass_y = eye_y;
        for (int i = 0; i <= thickness; i++) {
            XDrawLine(G.dpy, d, G.gc, cx - w / 3, glass_y + i, cx + w / 3, glass_y + i);
        }

        for (int i = 0; i <= thickness; i++) {
            // draw rectangles for glasses below the line
            XFillRectangle(G.dpy, d, G.gc, cx - eye_x_offset - eye_size, glass_y + i, (unsigned)eye_size*2, (unsigned)(eye_size + thickness));
            XFillRectangle(G.dpy, d, G.gc, cx + eye_x_offset - eye_size, glass_y + i, (unsigned)eye_size*2, (unsigned)(eye_size + thickness));
        }

        for (int i = -thickness; i <= thickness; i++) {
            XDrawArc(G.dpy, d, G.gc, cx - mouth_width / 2, mouth_y + i - mouth_height / 2, (unsigned)mouth_width, (unsigned)mouth_height, 225 * 64, 90 * 64);
        }
    } else {
        // Left eye
        XFillArc(G.dpy, d, G.gc, cx - eye_x_offset - eye_size / 2, eye_y - eye_size / 2, (unsigned)eye_size, (unsigned)eye_size, 0, 360 * 64);
        // Right eye (symmetric)
        XFillArc(G.dpy, d, G.gc, cx + eye_x_offset - eye_size / 2, eye_y - eye_size / 2, (unsigned)eye_size, (unsigned)eye_size, 0, 360 * 64);
        // Smile mouth
        for (int i = 0; i <= thickness; i++) {
            XDrawArc(G.dpy, d, G.gc, cx - mouth_width / 2, mouth_y + i - mouth_height / 2, (unsigned)mouth_width, (unsigned)mouth_height, 225 * 64, 90 * 64);
        }
    }
}

static void redraw_face(void){
    if(!G.face_button || !XtIsRealized(G.face_button)) return;
    Dimension w=0,h=0;
    XtVaGetValues(G.face_button, XmNwidth,&w, XmNheight,&h, NULL);
    if(w==0 || h==0) return;

    Pixmap pm=XCreatePixmap(G.dpy,XtWindow(G.face_button),(unsigned)w,(unsigned)h,
                            (unsigned)DefaultDepthOfScreen(XtScreen(G.face_button)));
    draw_face_to(pm,(int)w,(int)h);
    XCopyArea(G.dpy,pm,XtWindow(G.face_button),G.gc,0,0,(unsigned)w,(unsigned)h,0,0);
    XFreePixmap(G.dpy,pm);
    XFlush(G.dpy);
}

/* =======================================================================================
 * Geometry: apply board size + then ask Motif for preferred mainw size
 * ======================================================================================= */
static int clamp_to_screen_w(int w) {
    int sw = DisplayWidth(G.dpy, DefaultScreen(G.dpy));
    int maxw = MAX(200, sw - S(40));
    if(w < 200) w = 200;
    if(w > maxw) w = maxw;
    return w;
}
static int clamp_to_screen_h(int h) {
    int sh = DisplayHeight(G.dpy, DefaultScreen(G.dpy));
    int maxh = MAX(200, sh - S(80));
    if(h < 200) h = 200;
    if(h > maxh) h = maxh;
    return h;
}

static void apply_board_geometry(void)
{
    int bw = 0, bh = 0;

    compute_board_pixels(&bw, &bh);
    if (bw < 16) bw = 16;
    if (bh < 16) bh = 16;

    G.want_bw = bw;
    G.want_bh = bh;

    DBG2("apply_board_geometry: cols=%d rows=%d mines=%d -> board=%dx%d",
         G.cols, G.rows, G.mines, bw, bh);

    DBG2("layout snapshot: apply_board_geometry BEFORE");
    log_layout_snapshot("apply_board_geometry BEFORE");

    /* Account for borders/shadows so the *inside* ends up big enough. */
    Dimension bb_bw = 0, bb_sh = 0;
    Dimension fr_bw = 0, fr_sh = 0;

    if (G.board_bb) {
        XtVaGetValues(G.board_bb,
                      XmNborderWidth, &bb_bw,
                      XmNshadowThickness, &bb_sh,
                      NULL);
    }
    if (G.board_frame) {
        XtVaGetValues(G.board_frame,
                      XmNborderWidth, &fr_bw,
                      XmNshadowThickness, &fr_sh,
                      NULL);
    }

    /* Desired sizes */
    int da_w = bw, da_h = bh;
    int bb_w = bw + 2 * (int)(bb_bw + bb_sh);
    int bb_h = bh + 2 * (int)(bb_bw + bb_sh);
    int fr_w = bb_w + 2 * (int)(fr_bw + fr_sh);
    int fr_h = bb_h + 2 * (int)(fr_bw + fr_sh);

    /* IMPORTANT ORDER: inner -> outer */
    force_widget_size(G.board_da, da_w, da_h);
    force_widget_size(G.board_bb, bb_w, bb_h);
    force_widget_size(G.board_frame, fr_w, fr_h);

    /* Also pin drawing area position inside BB (prevents "cut off" after shrink/grow). */
    if (G.board_da) {
        XtVaSetValues(G.board_da, XmNx, 0, XmNy, 0, NULL);
    }

    /* Let Motif relayout and flush. */
    if (XtIsRealized(G.toplevel)) {
        XSync(XtDisplay(G.toplevel), False);
    }

    DBG2("layout snapshot: apply_board_geometry AFTER");
    log_layout_snapshot("apply_board_geometry AFTER");
}



/* =======================================================================================
 * Events / expose / configure tracking
 * ======================================================================================= */
static int cell_from_xy(int x,int y,int *out_cx,int *out_cy){
    int bx=x-PAD_PX();
    int by=y-PAD_PX();
    if(bx<0 || by<0) return -1;
    int cx=bx/CELL_PX();
    int cy=by/CELL_PX();
    if(!in_bounds(cx,cy)) return -1;
    if(out_cx) *out_cx=cx;
    if(out_cy) *out_cy=cy;
    return idx_of(cx,cy);
}

static void set_pressed_cell(int idx){
    if(G.pressed_cell==idx) return;
    G.pressed_cell=idx;
    redraw_board();
}

static void board_expose_cb(Widget w, XtPointer client, XtPointer call){
    (void)w;(void)client;(void)call;
    G.expose_count++;
    DBG2("board_expose_cb: count=%lu", G.expose_count);
    redraw_board();
}

static void board_resize_cb(Widget w, XtPointer client, XtPointer call)
{
    int xw = 0, xh = 0, view = 0;
    (void)w; (void)client; (void)call;

    if (!get_board_xwin_size(&xw, &xh, &view))
        return;

    DBG2("board_resize_cb: xwin now %dx%d view=%s (want=%dx%d)",
         xw, xh, view ? "yes" : "no", G.want_bw, G.want_bh);

    /* Keep backbuffer in sync (if enabled) */
    ensure_backbuffer(xw, xh);

    redraw_board();
}


/* shell map handler to enable deferred geometry */
static void toplevel_event_handler(Widget w, XtPointer client, XEvent *ev, Boolean *cont){
    (void)w;(void)client;(void)cont;
    if(ev->type == MapNotify) {
        G.shell_mapped = 1;
        DBG("toplevel MapNotify: shell_mapped=yes pending_geom=%s", yes_no(G.pending_geom));
        if(G.pending_geom) {
            G.pending_geom = 0;
            apply_board_geometry();
        }
    }
}

static void board_event_handler(Widget w, XtPointer client, XEvent *ev, Boolean *cont){
    (void)w;(void)client;(void)cont;

    if (ev->type == ConfigureNotify) {
        XConfigureEvent *ce = &ev->xconfigure;
        G.configure_count++;
        DBG2("board ConfigureNotify: count=%lu x/y=%d/%d w/h=%d/%d",
             G.configure_count, ce->x, ce->y, ce->width, ce->height);
        redraw_board();
        return;
    }

    if(ev->type==ButtonPress){
        XButtonEvent *be=&ev->xbutton;
        int cx,cy;
        int idx=cell_from_xy(be->x,be->y,&cx,&cy);

        if(be->button==Button1) G.left_down=1;
        if(be->button==Button3) G.right_down=1;

        if(G.game_over) return;

        if(idx>=0){
            if(G.left_down && G.right_down){
                set_status("Chording...");
            } else if(G.left_down){
                Cell *c=&G.cells[idx];
                if(!c->revealed && !c->flagged){
                    G.face=FACE_OHNO;
                    redraw_face();
                    set_pressed_cell(idx);
                }
            }
        }
    } else if(ev->type==MotionNotify){
        if(!G.left_down || G.game_over) return;
        XMotionEvent *me=&ev->xmotion;
        int idx=cell_from_xy(me->x,me->y,NULL,NULL);
        if(idx>=0){
            Cell *c=&G.cells[idx];
            if(!c->revealed && !c->flagged) set_pressed_cell(idx);
            else set_pressed_cell(-1);
        } else {
            set_pressed_cell(-1);
        }
    } else if(ev->type==ButtonRelease){
        XButtonEvent *be=&ev->xbutton;
        int cx,cy;
        int idx=cell_from_xy(be->x,be->y,&cx,&cy);

        int was_left  = (be->button==Button1);
        int was_right = (be->button==Button3);

        if(was_left)  G.left_down=0;
        if(was_right) G.right_down=0;

        if(G.game_over){
            set_pressed_cell(-1);
            redraw_face();
            redraw_board();
            return;
        }

        if(idx>=0){
            if(was_right && !was_left && !G.left_down){
                toggle_flag_question(idx);
                set_status("Mark updated.");
            } else if(was_left && !was_right && !G.right_down){
                if(G.pressed_cell==idx) reveal_cell(idx);
                set_pressed_cell(-1);
            } else {
                chord_open(cx,cy);
                set_pressed_cell(-1);
            }
        } else {
            set_pressed_cell(-1);
        }

        if(G.game_over==2) G.face=FACE_COOL;
        else if(G.game_over==1) G.face=FACE_DEAD;
        else G.face=FACE_SMILE;
        redraw_face();
        check_win();
        redraw_board();
    }
}

/* =======================================================================================
 * Timer
 * ======================================================================================= */
static void timer_tick(XtPointer client, XtIntervalId *id){
    (void)client;(void)id;
    if(!G.timer_running) return;
    if(G.game_over){ G.timer_running=0; return; }
    G.elapsed++;
    redraw_leds();
    G.timer_id = XtAppAddTimeOut(XtWidgetToApplicationContext(G.toplevel), 1000, timer_tick, NULL);
}

static void start_timer(void){
    if(G.timer_running) return;
    G.timer_running=1;
    G.timer_id = XtAppAddTimeOut(XtWidgetToApplicationContext(G.toplevel), 1000, timer_tick, NULL);
}

static void stop_timer(void){
    G.timer_running=0;
}

/* =======================================================================================
 * Clear/init
 * ======================================================================================= */
static void clear_board_state(void){
    free(G.cells);
    G.cells = (Cell*)calloc((size_t)G.cols*(size_t)G.rows, sizeof(Cell));

    G.first_click_done=0;
    G.game_over=0;
    G.flags_count=0;
    G.revealed_count=0;
    G.exploded_index=-1;

    G.left_down=0;
    G.right_down=0;
    G.pressed_cell=-1;

    G.face=FACE_SMILE;
    G.elapsed=0;
    stop_timer();

    set_status("Ready");
    redraw_leds();
}

/* =======================================================================================
 * Menu callbacks
 * ======================================================================================= */
static void cb_new(Widget w,XtPointer c,XtPointer call){ (void)w;(void)c;(void)call;
    new_game(G.diff,G.cols,G.rows,G.mines);
}
static void cb_beginner(Widget w,XtPointer c,XtPointer call){ (void)w;(void)c;(void)call;
    G.diff=DIFF_BEGINNER; G.cols=PRESET_BEGINNER.cols; G.rows=PRESET_BEGINNER.rows; G.mines=PRESET_BEGINNER.mines;
    save_config(); new_game(G.diff,G.cols,G.rows,G.mines);
}
static void cb_intermediate(Widget w,XtPointer c,XtPointer call){ (void)w;(void)c;(void)call;
    G.diff=DIFF_INTERMEDIATE; G.cols=PRESET_INTERMEDIATE.cols; G.rows=PRESET_INTERMEDIATE.rows; G.mines=PRESET_INTERMEDIATE.mines;
    save_config(); new_game(G.diff,G.cols,G.rows,G.mines);
}
static void cb_expert(Widget w,XtPointer c,XtPointer call){ (void)w;(void)c;(void)call;
    G.diff=DIFF_EXPERT; G.cols=PRESET_EXPERT.cols; G.rows=PRESET_EXPERT.rows; G.mines=PRESET_EXPERT.mines;
    save_config(); new_game(G.diff,G.cols,G.rows,G.mines);
}
static void cb_exit(Widget w,XtPointer c,XtPointer call){ (void)w;(void)c;(void)call;
    save_config(); exit(0);
}
static void cb_about(Widget w,XtPointer c,XtPointer call){ (void)w;(void)c;(void)call;
    show_about();
}
static void cb_marks(Widget w,XtPointer client,XtPointer call){
    (void)w;(void)client;
    XmToggleButtonCallbackStruct *cs=(XmToggleButtonCallbackStruct*)call;
    G.marks_enabled = cs->set ? 1 : 0;
    save_config();
    set_status(G.marks_enabled ? "Marks (?) enabled." : "Marks (?) disabled.");
}
static void wm_save_yourself_cb(Widget w,XtPointer client,XtPointer call){
    (void)w;(void)client;(void)call;
    save_config();
}
static void cb_fastest(Widget w,XtPointer c,XtPointer call){ (void)w;(void)c;(void)call;
    show_fastest_dialog();
}

/* =======================================================================================
 * UI building
 * ======================================================================================= */
static void led_area_size(Dimension *out_w, Dimension *out_h){
    *out_w = (Dimension)(LED_PAD_PX()*2 + 3*LED_W_PX() + 2*LED_GAP_PX());
    *out_h = (Dimension)(LED_PAD_PX()*2 + LED_H_PX());
}

static void create_menu(Widget parent){
    Widget menubar = XmCreateMenuBar(parent,"menubar",NULL,0);

    Widget game_pd = XmCreatePulldownMenu(menubar,"gamePD",NULL,0);
    XtVaCreateManagedWidget("Game", xmCascadeButtonWidgetClass, menubar, XmNsubMenuId, game_pd, NULL);

    Widget mi_new = XtVaCreateManagedWidget("New", xmPushButtonWidgetClass, game_pd, NULL);
    XtAddCallback(mi_new, XmNactivateCallback, cb_new, NULL);

    XtVaCreateManagedWidget("sep1", xmSeparatorGadgetClass, game_pd, NULL);

    Widget mi_b = XtVaCreateManagedWidget("Beginner", xmPushButtonWidgetClass, game_pd, NULL);
    Widget mi_i = XtVaCreateManagedWidget("Intermediate", xmPushButtonWidgetClass, game_pd, NULL);
    Widget mi_e = XtVaCreateManagedWidget("Expert", xmPushButtonWidgetClass, game_pd, NULL);
    XtAddCallback(mi_b, XmNactivateCallback, cb_beginner, NULL);
    XtAddCallback(mi_i, XmNactivateCallback, cb_intermediate, NULL);
    XtAddCallback(mi_e, XmNactivateCallback, cb_expert, NULL);

    XtVaCreateManagedWidget("sep3", xmSeparatorGadgetClass, game_pd, NULL);

    Widget mi_fast = XtVaCreateManagedWidget("Fastest Mine Sweepers...", xmPushButtonWidgetClass, game_pd, NULL);
    XtAddCallback(mi_fast, XmNactivateCallback, cb_fastest, NULL);

    XtVaCreateManagedWidget("sep4", xmSeparatorGadgetClass, game_pd, NULL);

    Widget mi_exit = XtVaCreateManagedWidget("Exit", xmPushButtonWidgetClass, game_pd, NULL);
    XtAddCallback(mi_exit, XmNactivateCallback, cb_exit, NULL);

    Widget opt_pd = XmCreatePulldownMenu(menubar,"optPD",NULL,0);
    XtVaCreateManagedWidget("Options", xmCascadeButtonWidgetClass, menubar, XmNsubMenuId, opt_pd, NULL);

    G.marks_toggle = XtVaCreateManagedWidget("Enable Question Marks", xmToggleButtonWidgetClass, opt_pd,
                                             XmNset, (Boolean)(G.marks_enabled?True:False), NULL);
    XtAddCallback(G.marks_toggle, XmNvalueChangedCallback, cb_marks, NULL);

    Widget help_pd = XmCreatePulldownMenu(menubar,"helpPD",NULL,0);
    Widget help_cas = XtVaCreateManagedWidget("Help", xmCascadeButtonWidgetClass, menubar,
                                              XmNsubMenuId, help_pd, NULL);
    Widget mi_about = XtVaCreateManagedWidget("About", xmPushButtonWidgetClass, help_pd, NULL);
    XtAddCallback(mi_about, XmNactivateCallback, cb_about, NULL);
    XtVaSetValues(menubar, XmNmenuHelpWidget, help_cas, NULL);

    XtManageChild(menubar);
    G.menubar = menubar;
}


static void build_ui(void) {
    G.mainw = XtVaCreateManagedWidget("mainw",
                                      xmMainWindowWidgetClass, G.toplevel,
                                      XmNshadowThickness, 0,
                                      NULL);

    create_menu(G.mainw);

    /* Work area: vertical stack */
    G.work_form = XtVaCreateManagedWidget("workRC",
                                         xmRowColumnWidgetClass, G.mainw,
                                         XmNorientation, XmVERTICAL,
                                         XmNpacking, XmPACK_TIGHT,
                                         XmNspacing, (Dimension)GAP_PX(),
                                         XmNmarginWidth, (Dimension)PAD_PX(),
                                         XmNmarginHeight, (Dimension)PAD_PX(),
                                         NULL);

    /* ---------- Top panel frame ---------- */
    G.top_frame = XtVaCreateManagedWidget("topFrame",
                                          xmFrameWidgetClass, G.work_form,
                                          XmNshadowType, XmSHADOW_IN,
                                          NULL);

    G.top_form = XtVaCreateManagedWidget("topForm",
                                         xmFormWidgetClass, G.top_frame,
                                         XmNmarginWidth, (Dimension)PAD_PX(),
                                         XmNmarginHeight, (Dimension)PAD_PX(),
                                         NULL);

    Dimension ledw, ledh;
    led_area_size(&ledw, &ledh);

    G.mine_frame = XtVaCreateManagedWidget("mineFrame", xmFrameWidgetClass, G.top_form,
                                           XmNshadowType, XmSHADOW_IN, NULL);
    G.mine_led = XtVaCreateManagedWidget("mineLED", xmDrawingAreaWidgetClass, G.mine_frame,
                                         XmNwidth, ledw, XmNheight, ledh,
                                         XmNresizePolicy, XmRESIZE_NONE,
                                         NULL);
    XtAddCallback(G.mine_led, XmNexposeCallback, led_expose_cb, (XtPointer)(intptr_t)0);

    G.face_button = XtVaCreateManagedWidget("face", xmDrawnButtonWidgetClass, G.top_form,
        XmNwidth,  (Dimension)FACE_W_PX(),
        XmNheight, (Dimension)FACE_H_PX(),
        XmNshadowThickness, 0,      /* <- remove the frame line */
        XmNhighlightThickness, 0,
        XmNtraversalOn, False,
        XmNnavigationType, XmNONE,
        XmNrecomputeSize, False,
        NULL);

    XtAddCallback(G.face_button, XmNexposeCallback,   face_expose_cb, NULL);
    XtAddCallback(G.face_button, XmNactivateCallback, face_activate_cb, NULL);
    XtAddCallback(G.face_button, XmNarmCallback,      face_arm_cb, NULL);
    XtAddCallback(G.face_button, XmNdisarmCallback,   face_disarm_cb, NULL);

    G.time_frame = XtVaCreateManagedWidget("timeFrame", xmFrameWidgetClass, G.top_form,
                                           XmNshadowType, XmSHADOW_IN, NULL);
    G.time_led = XtVaCreateManagedWidget("timeLED", xmDrawingAreaWidgetClass, G.time_frame,
                                         XmNwidth, ledw, XmNheight, ledh,
                                         XmNresizePolicy, XmRESIZE_NONE,
                                         NULL);
    XtAddCallback(G.time_led, XmNexposeCallback, led_expose_cb, (XtPointer)(intptr_t)1);

    // Attachments for top_form children
    XtVaSetValues(G.mine_frame,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  NULL);

    XtVaSetValues(G.time_frame,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  NULL);

    XtVaSetValues(G.face_button,
                  XmNleftAttachment, XmATTACH_POSITION,
                  XmNleftPosition, 50,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftOffset, -FACE_W_PX() / 2,
                  NULL);

    /* ---------- Board frame ---------- */
    G.board_frame = XtVaCreateManagedWidget("boardFrame",
                                            xmFrameWidgetClass, G.work_form,
                                            XmNshadowType, XmSHADOW_IN,
                                            NULL);

    int bw0, bh0;
    compute_board_pixels(&bw0, &bh0);

    G.board_bb = XtVaCreateManagedWidget("boardBB", xmBulletinBoardWidgetClass, G.board_frame,
                                         XmNresizePolicy, XmRESIZE_NONE,
                                         XmNmarginWidth, 0,
                                         XmNmarginHeight, 0,
                                         XmNwidth,  (Dimension)bw0,
                                         XmNheight, (Dimension)bh0,
                                         NULL);

    G.board_da = XtVaCreateManagedWidget("boardDA", xmDrawingAreaWidgetClass, G.board_bb,
                                         XmNresizePolicy, XmRESIZE_NONE,
                                         XmNwidth,  (Dimension)bw0,
                                         XmNheight, (Dimension)bh0,
                                         XmNx, 0,
                                         XmNy, 0,
                                         NULL);

    XtAddCallback(G.board_da, XmNexposeCallback, board_expose_cb, NULL);
    XtAddCallback(G.board_da, XmNresizeCallback, board_resize_cb, NULL);

    XtAddEventHandler(G.board_da,
                      StructureNotifyMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                      False, board_event_handler, NULL);

    /* ---------- Status bar ---------- */
    G.status_frame = XtVaCreateManagedWidget("statusFrame", xmFrameWidgetClass, G.work_form,
                                             XmNshadowType, XmSHADOW_ETCHED_IN,
                                             NULL);
    G.status_label = XtVaCreateManagedWidget("statusLabel", xmLabelWidgetClass, G.status_frame,
                                             XmNalignment, XmALIGNMENT_BEGINNING,
                                             XmNmarginLeft, 8,
                                             XmNmarginRight, 8,
                                             XmNmarginTop, 2,
                                             XmNmarginBottom, 2,
                                             NULL);

    DBG("XmMainWindowSetAreas: menubar=%p command=NULL work=%p",
        (void*)G.menubar, (void*)G.work_form);

    XmMainWindowSetAreas(G.mainw, G.menubar, NULL, NULL, NULL, G.work_form);

    DBG("build_ui done: board_bb=%p board_da=%p (initial board %dx%d)",
        (void*)G.board_bb, (void*)G.board_da, bw0, bh0);
}

/* =======================================================================================
 * About
 * ======================================================================================= */
static void show_about(void){
    if (g_about_shell && XtIsWidget(g_about_shell)) {
        XtPopup(g_about_shell, XtGrabNone);
        if (XtIsRealized(g_about_shell)) {
            XRaiseWindow(XtDisplay(g_about_shell), XtWindow(g_about_shell));
        }
        return;
    }

    Widget shell = NULL;
    Widget notebook = about_dialog_build(G.toplevel, "about_mines", "About Mines", &shell);
    if (!notebook || !shell) return;

    g_about_shell = shell;
    XtAddCallback(g_about_shell, XmNdestroyCallback, about_destroy_cb, NULL);

    about_add_standard_pages(notebook, 1,
                             "Mines",
                             "Mines Game for CK-Core",
                             " (c) 2025 C. Klukas\n\nDebug:\n  CK_MINES_DEBUG=0..3\n  CK_MINES_BACKBUF=1\n  CK_MINES_SCALE=2.0",
                             True);
    mines_add_icon_to_about_title_page(notebook);

    XtVaSetValues(shell,
                  XmNwidth, 700,
                  XmNheight, 550,
                  NULL);

    XtPopup(shell, XtGrabNone);
}

/* =======================================================================================
 * Face callbacks
 * ======================================================================================= */
static void face_expose_cb(Widget w,XtPointer client,XtPointer call){ (void)w;(void)client;(void)call; redraw_face(); }
static void face_activate_cb(Widget w,XtPointer client,XtPointer call){ (void)w;(void)client;(void)call; new_game(G.diff,G.cols,G.rows,G.mines); }
static void face_arm_cb(Widget w,XtPointer client,XtPointer call){ (void)w;(void)client;(void)call; if(!G.game_over){ G.face=FACE_OHNO; redraw_face(); } }
static void face_disarm_cb(Widget w,XtPointer client,XtPointer call){ (void)w;(void)client;(void)call;
    if(G.game_over==2) G.face=FACE_COOL; else if(G.game_over==1) G.face=FACE_DEAD; else G.face=FACE_SMILE;
    redraw_face();
}

/* =======================================================================================
 * Game control
 * ======================================================================================= */
static void new_game(DiffId diff,int cols,int rows,int mines){
    DBG("new_game(diff=%d cols=%d rows=%d mines=%d)", (int)diff, cols, rows, mines);
    G.diff=diff; G.cols=cols; G.rows=rows; G.mines=mines;

    clear_board_state();
    if(G.marks_toggle) XtVaSetValues(G.marks_toggle, XmNset, (Boolean)(G.marks_enabled?True:False), NULL);

    apply_board_geometry();
    redraw_leds();
    redraw_board();
    redraw_face();

    log_layout_snapshot("after new_game");
}

/* =======================================================================================
 * Init X stuff
 * ======================================================================================= */
static void init_x_stuff(void){
    G.dpy = XtDisplay(G.toplevel);

    Window gw = None;
    if (G.board_da && XtIsRealized(G.board_da)) gw = XtWindow(G.board_da);
    if (gw == None) gw = XtWindow(G.toplevel);

    DBG("init_x_stuff: choosing GC drawable=0x%lx", (unsigned long)gw);
    log_xwin("gc_drawable", gw);

    G.gc = XCreateGC(G.dpy, gw, 0, NULL);
    XSetGraphicsExposures(G.dpy, G.gc, False);

    int pt = (int)(12.0f * g_ui_scale + 0.5f);
    if(pt<12) pt=12;
    if(pt>28) pt=28;
    char pat[256];
    snprintf(pat,sizeof(pat),"-*-helvetica-bold-r-normal--%d-*-*-*-*-*-iso8859-1", pt);
    G.font = XLoadQueryFont(G.dpy, pat);
    if(!G.font) G.font = XLoadQueryFont(G.dpy, "fixed");
    if(G.font) XSetFont(G.dpy, G.gc, G.font->fid);
    DBG("font: requested pt=%d -> %s", pt, G.font ? "loaded" : "none");

    palette_from_widget(G.toplevel);

    DBG("init_x_stuff done: dpy=%p gc=%lu", (void*)G.dpy, (unsigned long)G.gc);
}

/* =======================================================================================
 * Main
 * ======================================================================================= */
static void normalize_config_to_diff(void){
    if(G.diff==DIFF_BEGINNER){
        G.cols=PRESET_BEGINNER.cols; G.rows=PRESET_BEGINNER.rows; G.mines=PRESET_BEGINNER.mines;
    } else if(G.diff==DIFF_INTERMEDIATE){
        G.cols=PRESET_INTERMEDIATE.cols; G.rows=PRESET_INTERMEDIATE.rows; G.mines=PRESET_INTERMEDIATE.mines;
    } else if(G.diff==DIFF_EXPERT){
        G.cols=PRESET_EXPERT.cols; G.rows=PRESET_EXPERT.rows; G.mines=PRESET_EXPERT.mines;
    } else {
        G.diff=DIFF_CUSTOM;
    }
}

int main(int argc,char **argv){
    memset(&G,0,sizeof(G));
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    const char *dbg=getenv("CK_MINES_DEBUG");
    if(dbg && *dbg) g_debug=parse_int(dbg,1);

    G.use_backbuf = (getenv("CK_MINES_BACKBUF") && getenv("CK_MINES_BACKBUF")[0]) ? 1 : 0;

    DBG("starting... pid=%d debug=%d backbuf=%d", (int)getpid(), g_debug, G.use_backbuf);

    load_config();
    normalize_config_to_diff();

    DBG("config: diff=%d cols=%d rows=%d mines=%d marks=%d",
        (int)G.diff,G.cols,G.rows,G.mines,G.marks_enabled);

    XtSetLanguageProc(NULL,NULL,NULL);

    G.toplevel = XtVaAppInitialize(NULL,"ck-mines",NULL,0,&argc,argv,NULL,
                                   XmNallowShellResize, True,
                                   NULL);

    XtVaSetValues(G.toplevel,
        XmNtitle,    "Mines",
        XmNiconName, "Mines",
        NULL);


    G.dpy = XtDisplay(G.toplevel);
    XSetErrorHandler(xerr_handler);

    init_ui_scale_from_env_and_dpi();

    /* initial size request (prevents WM maximizing) */
    {
        int bw,bh; compute_board_pixels(&bw,&bh);
        int init_w=bw + PAD_PX()*6 + S(80);
        int init_h=bh + S(240);
        XtVaSetValues(G.toplevel, XmNwidth,(Dimension)init_w, XmNheight,(Dimension)init_h, NULL);
        DBG("initial shell size request: %dx%d", init_w, init_h);
    }

    build_ui();

    /* watch for shell MapNotify so geometry can be applied safely */
    XtAddEventHandler(G.toplevel, StructureNotifyMask, False, toplevel_event_handler, NULL);

    XtRealizeWidget(G.toplevel);
    about_set_window_icon_from_xpm(G.toplevel, ck_mines_l_pm);

    init_x_stuff();

    /* WM_SAVE_YOURSELF */
    {
        Atom wm_save = XInternAtom(XtDisplay(G.toplevel), "WM_SAVE_YOURSELF", False);
        XmAddWMProtocolCallback(G.toplevel, wm_save, wm_save_yourself_cb, NULL);
    }

    /* after realize, we may already be mapped; if not, it will run on MapNotify */
    G.shell_mapped = 0;
    {
        XWindowAttributes a;
        if(XGetWindowAttributes(G.dpy, XtWindow(G.toplevel), &a) && a.map_state==IsViewable) {
            G.shell_mapped = 1;
        }
    }

    if(G.shell_mapped) {
        DBG("startup: shell already viewable -> apply initial geometry");
        apply_board_geometry();
    } else {
        DBG("startup: shell not viewable yet -> geometry will apply on MapNotify");
        G.pending_geom = 1;
    }

    new_game(G.diff,G.cols,G.rows,G.mines);

    DBG("entering main loop");
    XtAppMainLoop(XtWidgetToApplicationContext(G.toplevel));
    return 0;
}
