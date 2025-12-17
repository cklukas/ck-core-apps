/*
 * ck-nibbles.c  -  Motif/CDE "Nibbles" (QBasic/DOS-inspired) clone
 *
 * Build:
 *   cc -O2 -Wall -Isrc -I/usr/local/CDE/include \
 *      src/games/ck-nibbles/ck-nibbles.c src/shared/about_dialog.c \
 *      -o build/bin/ck-nibbles -L/usr/local/CDE/lib -lXm -lXt -lX11
 *
 * Run:
 *   ./ck-nibbles            # Startup dialog chooses mode
 *   ./ck-nibbles -2         # Defaults to 2-player in dialog
 *
 * Keys:
 *   Player 1: Arrow keys
 *   Player 2: W A S D (only in 2-player mode)
 *   P: pause
 *   R: restart level (current round)
 */

#include <Xm/Xm.h>
#include <Xm/CascadeB.h>
#include <Xm/DialogS.h>
#include <Xm/Protocols.h>
#include <Xm/DrawingA.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/Label.h>
#include <Xm/MenuShell.h>
#include <Xm/MessageB.h>
#include <Xm/Notebook.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/ToggleB.h>
#include <Xm/Text.h>
#include <Xm/TextF.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/xpm.h>

#include "../../shared/about_dialog.h"
#include "../../shared/session_utils.h"

#include "images/ck-nibbles.l.pm"

#include <Dt/Session.h>
#include <Dt/Dt.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>
#include <ctype.h>

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif

/* ---------- Retro-ish config ---------- */

#define GRID_W   80
#define GRID_H   50

#define CELL_PX  18
#define TICK_MS  90

#define MAX_LEN  (GRID_W*GRID_H)
#define DIRQ_CAP 32

typedef enum { DIR_UP=0, DIR_RIGHT=1, DIR_DOWN=2, DIR_LEFT=3 } Dir;

typedef struct {
    Dir q[DIRQ_CAP];
    int head, tail;
} DirQueue;

typedef struct { int x, y; } Pt;

typedef enum {
    DEATH_NONE = 0,
    DEATH_WALL,
    DEATH_OUT_OF_BOUNDS,
    DEATH_BITE_SELF,
    DEATH_HIT_OTHER,
    DEATH_HEAD_ON
} DeathReason;

typedef struct {
    Pt body[MAX_LEN];
    int len;
    Dir dir;
    Dir next_dir;
    int alive;
    unsigned long color;
    int score;
    int grow_pending;

    /* for end-of-round explanation */
    DeathReason death_reason;
} Snake;

typedef struct {
    int two_player;
    int paused;
    int level;
    int tick_ms;

    int cell;
    int offx, offy;

    Snake s1;
    Snake s2;

    Pt food;
    int has_food;

    unsigned char wall[GRID_H][GRID_W];

    /* X / Motif */
    Widget toplevel;
    Widget drawing;
    Widget status;
    Widget menu_bar;
    XtAppContext app;
    Display *dpy;
    Window win;
    Pixmap icon_pixmap;
    Pixmap icon_mask;
    Pixmap about_icon_pixmap;

    char current_status[256];
    int show_grid;
    int auto_pause;

    /* Per-player direction wishlists */
    DirQueue p1q;
    DirQueue p2q;

    /* Auto-repeat suppression: track key-down state by keycode */
    unsigned char key_down[256];

    GC gc_bg;
    GC gc_grid;
    GC gc_wall;
    GC gc_text;
    GC gc_food;
    GC gc_s1;
    GC gc_s2;

    unsigned long col_bg;
    unsigned long col_grid;
    unsigned long col_wall;
    unsigned long col_text;
    unsigned long col_food;
    unsigned long col_s1;
    unsigned long col_s2;

    XtIntervalId timer;

    /* player naming */
    char player1_name[64];
    char player2_name[64];

    /* match/rounds */
    int rounds_total;     /* user-set in New Game dialog */
    int rounds_played;    /* completed rounds */

    /*
     * "points" for match outcome:
     * - Win gives 1 point to winner.
     * - Draw gives 1 point to BOTH players (requested).
     * We still track draw_rounds as a statistic.
     */
    int p1_round_wins;    /* interpreted as match points */
    int p2_round_wins;    /* interpreted as match points */
    int draw_rounds;

    /* New Game dialog (robust FormDialog) */
    Widget newgame_dialog;
    Widget newgame_rb_1p;
    Widget newgame_rb_2p;
    Widget newgame_text_p1;
    Widget newgame_text_p2;
    Widget newgame_label_p2;
    Widget newgame_text_rounds;
    int    newgame_is_startup;
    int    newgame_prev_paused;

    /* Round End dialog */
    Widget roundend_dialog;
    int    roundend_active;
    char   roundend_msg[512];

    /* Validation warning dialog */
    Widget warn_dialog;

    /* Session handling */
    SessionData *session_data;
    char exec_path[PATH_MAX];
    int session_restored;
} Game;

static void init_exec_path(char *exec_path, size_t size, const char *argv0) {
    ssize_t len = readlink("/proc/self/exe", exec_path, size - 1);
    if (len > 0) {
        exec_path[len] = '\0';
        return;
    }
    if (argv0 && argv0[0] == '/') {
        strncpy(exec_path, argv0, size - 1);
        exec_path[size - 1] = '\0';
        return;
    }
    strncpy(exec_path, "ck-nibbles", size - 1);
    exec_path[size - 1] = '\0';
}

static Game G;

/* ---------- Forward declarations ---------- */

static void redraw(void);
static void set_status_text(void);
static void restart_game(int full_reset_scores);
static void apply_queued_dirs(void);
static int  dir_opposite(Dir a, Dir b);

static void show_new_game_dialog(int is_startup);
static void build_new_game_dialog(void);
static void on_new_game_menu(Widget w, XtPointer client, XtPointer call);

static void build_round_end_dialog(void);
static void show_round_end_dialog(const char *msg);
static void on_roundend_next(Widget w, XtPointer client, XtPointer call);
static void on_roundend_end(Widget w, XtPointer client, XtPointer call);

static void on_toplevel_configure(Widget w, XtPointer client, XEvent *ev, Boolean *cont);
static int  startup_dialog_shown = 0;
static int  g_debug_icons = 0;
static float g_about_ui_scale = 1.0f;

static void dbg_icons(const char *fmt, ...) {
    if (!g_debug_icons) return;

    setvbuf(stderr, NULL, _IONBF, 0);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---------- Dialog positioning helper ---------- */

static void center_shell_over_widget(Widget shell, Widget over) {
    if (!shell || !over) return;

    if (!XtIsRealized(over)) return;
    if (!XtIsRealized(shell)) XtRealizeWidget(shell); /* realize (not map) so width/height are valid */

    Position rx = 0, ry = 0;
    XtTranslateCoords(over, 0, 0, &rx, &ry);

    Dimension ow = 0, oh = 0, sw = 0, sh = 0;
    XtVaGetValues(over,  XmNwidth, &ow, XmNheight, &oh, NULL);
    XtVaGetValues(shell, XmNwidth, &sw, XmNheight, &sh, NULL);

    Position nx = rx + (Position)((int)ow - (int)sw) / 2;
    Position ny = ry + (Position)((int)oh - (int)sh) / 2;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;

    XtVaSetValues(shell, XmNx, nx, XmNy, ny, NULL);
}

/* ---------- Direction queue ---------- */

static void dirq_clear(DirQueue *dq) { dq->head = dq->tail = 0; }
static int  dirq_empty(const DirQueue *dq) { return dq->head == dq->tail; }

static int dirq_push(DirQueue *dq, Dir d) {
    int next_tail = (dq->tail + 1) % DIRQ_CAP;
    if (next_tail == dq->head) return 0; /* full */
    dq->q[dq->tail] = d;
    dq->tail = next_tail;
    return 1;
}

static int dirq_pop(DirQueue *dq, Dir *out) {
    if (dirq_empty(dq)) return 0;
    *out = dq->q[dq->head];
    dq->head = (dq->head + 1) % DIRQ_CAP;
    return 1;
}

/* Pop directions until we find one that is not a 180° reversal vs current snake dir. */
static int dirq_pop_valid(DirQueue *dq, const Snake *s, Dir *out) {
    Dir d;
    while (dirq_pop(dq, &d)) {
        if (!dir_opposite(s->dir, d)) {
            *out = d;
            return 1;
        }
        /* drop 180° reversals */
    }
    return 0;
}

/* ---------- Utility ---------- */

static int rnd_int(int lo, int hi) { /* inclusive */
    return lo + (int)(rand() % (unsigned)(hi - lo + 1));
}

static int pt_eq(Pt a, Pt b) { return a.x==b.x && a.y==b.y; }

static int in_bounds(int x, int y) {
    return (x>=0 && x<GRID_W && y>=0 && y<GRID_H);
}

static Pt step_pt(Pt p, Dir d) {
    if (d==DIR_UP)    p.y--;
    if (d==DIR_DOWN)  p.y++;
    if (d==DIR_LEFT)  p.x--;
    if (d==DIR_RIGHT) p.x++;
    return p;
}

static int dir_opposite(Dir a, Dir b) {
    return ((a==DIR_UP && b==DIR_DOWN) ||
            (a==DIR_DOWN && b==DIR_UP) ||
            (a==DIR_LEFT && b==DIR_RIGHT) ||
            (a==DIR_RIGHT && b==DIR_LEFT));
}

static void trim_copy(char *dst, size_t dstsz, const char *src, const char *fallback) {
    if (!dst || dstsz == 0) return;
    dst[0] = '\0';

    if (!src) src = "";
    while (*src==' ' || *src=='\t' || *src=='\n' || *src=='\r') src++;

    size_t n = strlen(src);
    while (n > 0) {
        char c = src[n-1];
        if (c==' ' || c=='\t' || c=='\n' || c=='\r') n--;
        else break;
    }

    if (n == 0) {
        strncpy(dst, fallback ? fallback : "", dstsz-1);
        dst[dstsz-1] = '\0';
        return;
    }

    if (n >= dstsz) n = dstsz-1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* strict: must be a clean integer token and in [minv,maxv] */
static int parse_int_strict_range(const char *s, int minv, int maxv, int *out) {
    if (!s) return 0;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return 0;

    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return 0;
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end != '\0') return 0;
    if (v < minv || v > maxv) return 0;

    if (out) *out = (int)v;
    return 1;
}

static const char* death_reason_str(DeathReason r) {
    switch (r) {
        case DEATH_WALL:          return "hit a wall";
        case DEATH_OUT_OF_BOUNDS: return "left the field";
        case DEATH_BITE_SELF:     return "bit itself";
        case DEATH_HIT_OTHER:     return "crashed into the other snake";
        case DEATH_HEAD_ON:       return "hit head-on";
        default:                  return "died";
    }
}

/* ---------- Validation warning dialog ---------- */

static void build_warn_dialog(void) {
    if (G.warn_dialog) return;

    Arg args[8];
    int n = 0;
    XtSetArg(args[n], XmNdialogStyle, XmDIALOG_PRIMARY_APPLICATION_MODAL); n++;
    XtSetArg(args[n], XmNautoUnmanage, True); n++;

    G.warn_dialog = XmCreateWarningDialog(G.toplevel, "warn_dialog", args, n);

    Widget helpb = XmMessageBoxGetChild(G.warn_dialog, XmDIALOG_HELP_BUTTON);
    if (helpb) XtUnmanageChild(helpb);

    Widget cancelb = XmMessageBoxGetChild(G.warn_dialog, XmDIALOG_CANCEL_BUTTON);
    if (cancelb) XtUnmanageChild(cancelb);

    Widget shell = XtParent(G.warn_dialog);
    XtVaSetValues(shell,
                  XmNtitle, "Invalid Value",
                  XmNdeleteResponse, XmUNMAP,
                  XmNtransientFor, G.toplevel,
                  NULL);
}

static void show_warning(const char *msg) {
    build_warn_dialog();

    XmString xs = XmStringCreateLocalized((char*)(msg ? msg : ""));
    XtVaSetValues(G.warn_dialog, XmNmessageString, xs, NULL);
    XmStringFree(xs);

    XtManageChild(G.warn_dialog);

    Widget shell = XtParent(G.warn_dialog);
    if (XtIsRealized(shell)) {
        XRaiseWindow(XtDisplay(shell), XtWindow(shell));
    }
}

/* ---------- Level generation ---------- */

static void clear_walls(void) { memset(G.wall, 0, sizeof(G.wall)); }

static void add_border(void) {
    for (int x=0;x<GRID_W;x++) {
        G.wall[0][x]=1;
        G.wall[GRID_H-1][x]=1;
    }
    for (int y=0;y<GRID_H;y++) {
        G.wall[y][0]=1;
        G.wall[y][GRID_W-1]=1;
    }
}

static void build_level(int lvl) {
    clear_walls();
    add_border();

    int m = (lvl % 6);

    if (m==0) {
        /* only border */
    } else if (m==1) {
        int box_left = GRID_W * 12 / 40;
        int box_right = GRID_W * 28 / 40;
        int box_top = GRID_H * 8 / 25;
        int box_bottom = GRID_H * 16 / 25;
        int gap_y = GRID_H * 12 / 25;
        for (int x=box_left; x<box_right; x++) { G.wall[box_top][x]=1; G.wall[box_bottom][x]=1; }
        for (int y=box_top; y<=box_bottom; y++) { G.wall[y][box_left]=1; G.wall[y][box_right-1]=1; }
        G.wall[gap_y][box_left]=0; G.wall[gap_y][box_right-1]=0;
    } else if (m==2) {
        int x1 = GRID_W * 13 / 40;
        int x2 = GRID_W * 26 / 40;
        int y_start = GRID_H * 3 / 25;
        int y_end = GRID_H * 22 / 25;
        int skip_y = GRID_H * 12 / 25;
        for (int y=y_start; y<=y_end; y++) {
            if (y != skip_y) { G.wall[y][x1]=1; G.wall[y][x2]=1; }
        }
    } else if (m==3) {
        int zig_top = GRID_H * 6 / 25;
        int zig_bottom = GRID_H * 18 / 25;
        int zig_left = GRID_W * 6 / 40;
        int zig_right = GRID_W * 33 / 40;
        for (int x=4; x<GRID_W-4; x++) {
            if (x%2==0) G.wall[zig_top][x]=1;
            else        G.wall[zig_bottom][x]=1;
        }
        for (int y=8; y<GRID_H-8; y++) {
            if (y%2==0) G.wall[y][zig_left]=1;
            else        G.wall[y][zig_right]=1;
        }
    } else if (m==4) {
        int cx = GRID_W/2;
        int cy = GRID_H/2;
        for (int x=2;x<GRID_W-2;x++) if (x!=cx) G.wall[cy][x]=1;
        for (int y=2;y<GRID_H-2;y++) if (y!=cy) G.wall[y][cx]=1;
        G.wall[cy][cx-4]=0; G.wall[cy][cx+4]=0;
        G.wall[cy-4][cx]=0; G.wall[cy+4][cx]=0;
    } else if (m==5) {
        int box1_left = GRID_W * 5 / 40;
        int box1_right = GRID_W * 18 / 40;
        int box1_top = GRID_H * 5 / 25;
        int box1_bottom = GRID_H * 11 / 25;
        int box2_left = GRID_W * 22 / 40;
        int box2_right = GRID_W * 35 / 40;
        int box2_top = GRID_H * 13 / 25;
        int box2_bottom = GRID_H * 19 / 25;
        int ent1_y = GRID_H * 8 / 25;
        int ent2_y = GRID_H * 16 / 25;
        for (int x=box1_left; x<box1_right; x++) { G.wall[box1_top][x]=1; G.wall[box1_bottom][x]=1; }
        for (int y=box1_top; y<=box1_bottom; y++) { G.wall[y][box1_left]=1; G.wall[y][box1_right-1]=1; }
        for (int x=box2_left; x<box2_right; x++) { G.wall[box2_top][x]=1; G.wall[box2_bottom][x]=1; }
        for (int y=box2_top; y<=box2_bottom; y++) { G.wall[y][box2_left]=1; G.wall[y][box2_right-1]=1; }
        G.wall[ent1_y][box1_left]=0;  G.wall[ent2_y][box2_right-1]=0;
    }
}

/* ---------- Collision & occupancy ---------- */

static int cell_is_wall(int x, int y) {
    if (!in_bounds(x,y)) return 1;
    return G.wall[y][x] ? 1 : 0;
}

static int cell_in_snake(const Snake *s, int x, int y) {
    for (int i=0;i<s->len;i++) {
        if (s->body[i].x==x && s->body[i].y==y) return 1;
    }
    return 0;
}

static int cell_occupied(int x, int y) {
    if (cell_is_wall(x,y)) return 1;
    if (G.s1.alive && cell_in_snake(&G.s1, x,y)) return 1;
    if (G.s2.alive && cell_in_snake(&G.s2, x,y)) return 1;
    return 0;
}

/* ---------- Food placement ---------- */

static void place_food(void) {
    int tries=0;
    while (tries++ < 2000) {
        int x = rnd_int(1, GRID_W-2);
        int y = rnd_int(1, GRID_H-2);
        if (!cell_occupied(x,y)) {
            G.food.x=x; G.food.y=y;
            G.has_food=1;
            return;
        }
    }
    G.has_food=0;
}

/* ---------- Snake init/reset ---------- */

static void init_snake(Snake *s, int x, int y, Dir d, unsigned long col) {
    s->len = 5;
    s->dir = d;
    s->next_dir = d;
    s->alive = 1;
    s->color = col;
    s->grow_pending = 0;
    s->death_reason = DEATH_NONE;

    for (int i=0;i<s->len;i++) {
        s->body[i].x = x - i*(d==DIR_RIGHT ? 1 : d==DIR_LEFT ? -1 : 0);
        s->body[i].y = y - i*(d==DIR_DOWN  ? 1 : d==DIR_UP   ? -1 : 0);
    }
}

static void reset_round(void) {
    build_level(G.level);

    int s1x = GRID_W * 8 / 40;
    int s1y = GRID_H * 12 / 25;
    int s2x = GRID_W * 31 / 40;
    int s2y = GRID_H * 12 / 25;

    init_snake(&G.s1, s1x, s1y, DIR_RIGHT, G.col_s1);
    init_snake(&G.s2, s2x, s2y, DIR_LEFT,  G.col_s2);

    if (!G.two_player) {
        G.s2.alive = 1; /* CPU */
    }

    place_food();
}

/* ---------- UI status ---------- */

static void set_status_text(void) {
    const char *p1 = (G.player1_name[0] ? G.player1_name : "Player 1");
    const char *p2 = (G.two_player ? (G.player2_name[0] ? G.player2_name : "Player 2")
                                   : (G.player2_name[0] ? G.player2_name : "CPU"));

    char buf[256];
    snprintf(buf, sizeof(buf),
             "Level %d   Round %d/%d   Points %d:%d (%dD)   %s:%d   %s:%d   %s   (P pause, R restart)",
             G.level+1,
             (G.rounds_played + 1),
             (G.rounds_total > 0 ? G.rounds_total : 1),
             G.p1_round_wins, G.p2_round_wins, G.draw_rounds,
             p1, G.s1.score,
             p2, G.s2.score,
             (G.paused ? "PAUSED" : "RUN"));

    if (strcmp(buf, G.current_status) != 0) {
        strcpy(G.current_status, buf);
        XmString xs = XmStringCreateLocalized(buf);
        XtVaSetValues(G.status, XmNlabelString, xs, NULL);
        XmStringFree(xs);
    }
}

/* ---------- Drawing ---------- */

static void compute_cell_geometry(void) {
    Dimension w,h;
    XtVaGetValues(G.drawing, XmNwidth, &w, XmNheight, &h, NULL);

    int cw = (int)w / GRID_W;
    int ch = (int)h / GRID_H;
    G.cell = MAX(6, MIN(cw, ch));
    int used_w = G.cell * GRID_W;
    int used_h = G.cell * GRID_H;
    G.offx = ((int)w - used_w) / 2;
    G.offy = ((int)h - used_h) / 2;
}

static void fill_rect_cell(GC gc, int x, int y, int inset) {
    int px = G.offx + x*G.cell + inset;
    int py = G.offy + y*G.cell + inset;
    int sz = G.cell - 2*inset;
    if (sz < 1) sz = 1;
    XFillRectangle(G.dpy, G.win, gc, px, py, (unsigned)sz, (unsigned)sz);
}

static void draw_grid(void) {
    Dimension w,h;
    XtVaGetValues(G.drawing, XmNwidth, &w, XmNheight, &h, NULL);
    XFillRectangle(G.dpy, G.win, G.gc_bg, 0, 0, (unsigned)w, (unsigned)h);

    if (!G.show_grid) return;

    int left = G.offx, top = G.offy;
    int right = G.offx + G.cell*GRID_W;
    int bottom = G.offy + G.cell*GRID_H;

    for (int x=0;x<=GRID_W;x++) {
        int px = left + x*G.cell;
        XDrawLine(G.dpy, G.win, G.gc_grid, px, top, px, bottom);
    }
    for (int y=0;y<=GRID_H;y++) {
        int py = top + y*G.cell;
        XDrawLine(G.dpy, G.win, G.gc_grid, left, py, right, py);
    }
}

static void draw_walls(void) {
    for (int y=0;y<GRID_H;y++) {
        for (int x=0;x<GRID_W;x++) {
            if (G.wall[y][x]) fill_rect_cell(G.gc_wall, x, y, 1);
        }
    }
}

static void draw_food(void) {
    if (!G.has_food) return;
    fill_rect_cell(G.gc_food, G.food.x, G.food.y, G.cell/4);
}

static void draw_snake(const Snake *s, GC gc) {
    if (!s->alive) return;
    for (int i=s->len-1;i>=0;i--) {
        int inset = (i==0) ? 2 : 3;
        fill_rect_cell(gc, s->body[i].x, s->body[i].y, inset);
    }
}

static void redraw(void) {
    if (!XtIsRealized(G.drawing)) return;

    G.dpy = XtDisplay(G.drawing);
    G.win = XtWindow(G.drawing);
    compute_cell_geometry();

    draw_grid();
    draw_walls();
    draw_food();
    draw_snake(&G.s1, G.gc_s1);
    draw_snake(&G.s2, G.gc_s2);
}

static void on_expose(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)client; (void)call;
    redraw();
}

/* ---------- Game mechanics ---------- */

static int will_hit_reason(const Snake *s, Pt next, int include_tail, DeathReason *out_reason) {
    if (!in_bounds(next.x, next.y)) {
        if (out_reason) *out_reason = DEATH_OUT_OF_BOUNDS;
        return 1;
    }
    if (cell_is_wall(next.x, next.y)) {
        if (out_reason) *out_reason = DEATH_WALL;
        return 1;
    }

    /* self collision */
    for (int i=0;i<s->len;i++) {
        if (!include_tail && i==s->len-1) continue;
        if (s->body[i].x==next.x && s->body[i].y==next.y) {
            if (out_reason) *out_reason = DEATH_BITE_SELF;
            return 1;
        }
    }

    /* other snake collision */
    const Snake *o = (s==&G.s1) ? &G.s2 : &G.s1;
    if (o->alive) {
        for (int i=0;i<o->len;i++) {
            if (o->body[i].x==next.x && o->body[i].y==next.y) {
                if (out_reason) *out_reason = DEATH_HIT_OTHER;
                return 1;
            }
        }
    }

    if (out_reason) *out_reason = DEATH_NONE;
    return 0;
}

static void apply_next_dir(Snake *s) {
    if (!dir_opposite(s->dir, s->next_dir)) s->dir = s->next_dir;
}

/*
 * Step BOTH snakes "simultaneously" (same tick) and apply special draw logic:
 * - head-head same target cell => draw
 * - head swap (A moves into B head cell while B moves into A head cell) => draw
 * - any tick where both die => draw (both get 1 match point)
 *
 * Note: we keep collision checks against current bodies (including other head),
 * and explicitly treat head-on / swap as simultaneous.
 */
static void step_both_snakes_simultaneous(void) {
    if (!G.s1.alive && !G.s2.alive) return;

    /* apply next dir constraints */
    if (G.s1.alive) apply_next_dir(&G.s1);
    if (G.s2.alive) apply_next_dir(&G.s2);

    Pt p1_prev = G.s1.body[0];
    Pt p2_prev = G.s2.body[0];

    Pt p1_next = G.s1.alive ? step_pt(p1_prev, G.s1.dir) : p1_prev;
    Pt p2_next = G.s2.alive ? step_pt(p2_prev, G.s2.dir) : p2_prev;

    int p1_include_tail = (G.s1.grow_pending > 0) ? 1 : 0;
    int p2_include_tail = (G.s2.grow_pending > 0) ? 1 : 0;

    DeathReason r1 = DEATH_NONE, r2 = DEATH_NONE;

    /* Special simultaneous head collision cases */
    int head_same_cell = (G.s1.alive && G.s2.alive && pt_eq(p1_next, p2_next));
    int head_swap = (G.s1.alive && G.s2.alive && pt_eq(p1_next, p2_prev) && pt_eq(p2_next, p1_prev));

    int p1_dead = 0, p2_dead = 0;

    if (G.s1.alive) {
        if (head_same_cell || head_swap) {
            p1_dead = 1;
            r1 = DEATH_HEAD_ON;
        } else if (will_hit_reason(&G.s1, p1_next, p1_include_tail, &r1)) {
            p1_dead = 1;
        }
    }

    if (G.s2.alive) {
        if (head_same_cell || head_swap) {
            p2_dead = 1;
            r2 = DEATH_HEAD_ON;
        } else if (will_hit_reason(&G.s2, p2_next, p2_include_tail, &r2)) {
            p2_dead = 1;
        }
    }

    /* Apply deaths */
    if (p1_dead) { G.s1.alive = 0; G.s1.death_reason = r1; }
    if (p2_dead) { G.s2.alive = 0; G.s2.death_reason = r2; }

    /* If dead, do not move that snake */
    if (!G.s1.alive || !G.s2.alive) {
        /* if one is dead, the other may still move this tick (classic feel) */
        /* but if both are dead, no one moves */
    }

    /* Determine food eaten based on target cells (before moving) */
    int p1_ate = (G.s1.alive && G.has_food && pt_eq(p1_next, G.food));
    int p2_ate = (G.s2.alive && G.has_food && pt_eq(p2_next, G.food));

    if (p1_ate) { G.s1.grow_pending += 2; G.s1.score += 10; }
    if (p2_ate) { G.s2.grow_pending += 2; G.s2.score += 10; }

    /* Move snakes that are alive */
    if (G.s1.alive) {
        int new_len = G.s1.len;
        if (G.s1.grow_pending > 0) {
            if (G.s1.len < MAX_LEN) new_len = G.s1.len + 1;
            G.s1.grow_pending--;
        }
        for (int i=new_len-1;i>0;i--) G.s1.body[i] = G.s1.body[i-1];
        G.s1.body[0] = p1_next;
        G.s1.len = new_len;
    }

    if (G.s2.alive) {
        int new_len = G.s2.len;
        if (G.s2.grow_pending > 0) {
            if (G.s2.len < MAX_LEN) new_len = G.s2.len + 1;
            G.s2.grow_pending--;
        }
        for (int i=new_len-1;i>0;i--) G.s2.body[i] = G.s2.body[i-1];
        G.s2.body[0] = p2_next;
        G.s2.len = new_len;
    }

    /* Food placement: if any ate, place a new food (simple) */
    if (p1_ate || p2_ate) place_food();
}

/* safe append helper (keeps NUL, never overflows) */
static void str_append(char *dst, size_t dstsz, const char *src) {
    if (!dst || dstsz == 0 || !src) return;
    size_t used = strlen(dst);
    if (used >= dstsz - 1) return;
    size_t avail = dstsz - 1 - used;
    strncat(dst, src, avail);
}

/* Called when a round ends (someone died). Shows dialog instead of auto-resetting. */
static void end_round_and_show_dialog(void) {
    if (G.roundend_active) return;
    G.roundend_active = 1;
    G.paused = 1;

    const char *p1 = (G.player1_name[0] ? G.player1_name : "Player 1");
    const char *p2 = (G.two_player ? (G.player2_name[0] ? G.player2_name : "Player 2")
                                   : (G.player2_name[0] ? G.player2_name : "CPU"));

    int p1_dead = !G.s1.alive;
    int p2_dead = !G.s2.alive;

    const char *winner = NULL;
    int draw = 0;

    if (p1_dead && p2_dead) { draw = 1; }
    else if (p1_dead && !p2_dead) { winner = p2; }
    else if (!p1_dead && p2_dead) { winner = p1; }

    /* update match stats */
    G.rounds_played++;

    if (draw) {
        /* requested: draw gives BOTH players a point */
        G.draw_rounds++;
        G.p1_round_wins++;
        G.p2_round_wins++;
    } else if (winner == p1) {
        G.p1_round_wins++;
    } else if (winner == p2) {
        G.p2_round_wins++;
    }

    /* build message */
    char line1[128];
    if (draw) snprintf(line1, sizeof(line1), "Round %d/%d: Draw",
                       G.rounds_played, G.rounds_total);
    else snprintf(line1, sizeof(line1), "Round %d/%d: %s wins!",
                  G.rounds_played, G.rounds_total, winner);

    char line2[256];
    if (p1_dead) snprintf(line2, sizeof(line2), "%s %s.", p1, death_reason_str(G.s1.death_reason));
    else         snprintf(line2, sizeof(line2), "%s survived.", p1);

    char line3[256];
    if (p2_dead) snprintf(line3, sizeof(line3), "%s %s.", p2, death_reason_str(G.s2.death_reason));
    else         snprintf(line3, sizeof(line3), "%s survived.", p2);

    char line4[128];
    snprintf(line4, sizeof(line4), "Match points: %d : %d   (Draw rounds: %d)",
             G.p1_round_wins, G.p2_round_wins, G.draw_rounds);

    /* final result line if match finished */
    char line5[160];
    line5[0] = '\0';
    if (G.rounds_played >= G.rounds_total) {
        if (G.p1_round_wins > G.p2_round_wins) {
            snprintf(line5, sizeof(line5), "Final result: %s wins the match.", p1);
        } else if (G.p2_round_wins > G.p1_round_wins) {
            snprintf(line5, sizeof(line5), "Final result: %s wins the match.", p2);
        } else {
            snprintf(line5, sizeof(line5), "Final result: Match draw.");
        }
    }

    G.roundend_msg[0] = '\0';
    str_append(G.roundend_msg, sizeof(G.roundend_msg), line1);
    str_append(G.roundend_msg, sizeof(G.roundend_msg), "\n\n");
    str_append(G.roundend_msg, sizeof(G.roundend_msg), line2);
    str_append(G.roundend_msg, sizeof(G.roundend_msg), "\n");
    str_append(G.roundend_msg, sizeof(G.roundend_msg), line3);
    str_append(G.roundend_msg, sizeof(G.roundend_msg), "\n\n");
    str_append(G.roundend_msg, sizeof(G.roundend_msg), line4);

    if (line5[0]) {
        str_append(G.roundend_msg, sizeof(G.roundend_msg), "\n");
        str_append(G.roundend_msg, sizeof(G.roundend_msg), line5);
    }

    set_status_text();
    redraw();

    show_round_end_dialog(G.roundend_msg);
}

static void handle_deaths_and_progress(void) {
    if (!G.s1.alive || !G.s2.alive) {
        /* Small score penalty on death (optional, keeps original feel) */
        if (!G.s1.alive) G.s1.score = MAX(0, G.s1.score - 25);
        if (!G.s2.alive) G.s2.score = MAX(0, G.s2.score - 25);

        end_round_and_show_dialog();
    }
}

/* ---------- CPU (P2 AI) ---------- */

static Dir cpu_choose_dir(const Snake *cpu) {
    Dir options[4] = { DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT };
    int bestScore = 1<<30;
    Dir bestDir = cpu->dir;

    for (int k=0;k<4;k++) {
        Dir d = options[k];
        if (dir_opposite(cpu->dir, d)) continue;

        Pt next = step_pt(cpu->body[0], d);
        int include_tail = (cpu->grow_pending>0) ? 1 : 0;
        DeathReason reason = DEATH_NONE;
        if (will_hit_reason(cpu, next, include_tail, &reason)) continue;

        int dist = 0;
        if (G.has_food) dist = abs(next.x - G.food.x) + abs(next.y - G.food.y);

        int score = dist * 10;
        if (d != cpu->dir) score += 6;

        score += (cell_is_wall(next.x+1,next.y) + cell_is_wall(next.x-1,next.y)
                + cell_is_wall(next.x,next.y+1) + cell_is_wall(next.x,next.y-1)) * 2;

        if (score < bestScore) { bestScore = score; bestDir = d; }
    }
    return bestDir;
}

/* ---------- Input wishlist application ---------- */

static void apply_queued_dirs(void) {
    Dir d;

    if (dirq_pop_valid(&G.p1q, &G.s1, &d)) {
        G.s1.next_dir = d;
    }

    if (G.two_player) {
        if (dirq_pop_valid(&G.p2q, &G.s2, &d)) {
            G.s2.next_dir = d;
        }
    }
}

/* ---------- Timer tick ---------- */

static void on_tick(XtPointer client, XtIntervalId *id) {
    (void)client; (void)id;

    if (!G.paused) {
        apply_queued_dirs();

        if (!G.two_player) {
            G.s2.next_dir = cpu_choose_dir(&G.s2);
        }

        /* IMPORTANT: step simultaneously so head-on cases become draws correctly */
        step_both_snakes_simultaneous();

        handle_deaths_and_progress();
        set_status_text();
        redraw();
    }

    G.timer = XtAppAddTimeOut(G.app, (unsigned long)G.tick_ms, on_tick, NULL);
}

/* ---------- Restart / focus / key handling ---------- */

static void restart_game(int full_reset_scores) {
    if (full_reset_scores) {
        G.s1.score = 0;
        G.s2.score = 0;
        G.level = 0;

        G.rounds_played = 0;
        G.p1_round_wins = 0;
        G.p2_round_wins = 0;
        G.draw_rounds = 0;
    }

    G.roundend_active = 0;

    dirq_clear(&G.p1q);
    dirq_clear(&G.p2q);
    memset(G.key_down, 0, sizeof(G.key_down));

    reset_round();
    G.paused = 0;

    set_status_text();
    redraw();
}

static void on_click_focus(Widget w, XtPointer client, XEvent *ev, Boolean *cont) {
    (void)client;
    *cont = True;
    if (ev->type == ButtonPress) {
        memset(G.key_down, 0, sizeof(G.key_down));
        XmProcessTraversal(w, XmTRAVERSE_CURRENT);
        XSetInputFocus(XtDisplay(w), XtWindow(w), RevertToParent, CurrentTime);
    }
}

static void on_focus_change(Widget w, XtPointer client, XEvent *ev, Boolean *cont) {
    (void)w; (void)client;
    *cont = True;

    if (ev->type == FocusIn) {
        memset(G.key_down, 0, sizeof(G.key_down));
        if (G.auto_pause) {
            G.paused = 0;
            G.auto_pause = 0;
            set_status_text();
            redraw();
        }
    } else if (ev->type == FocusOut) {
        memset(G.key_down, 0, sizeof(G.key_down));
        if (!G.paused) {
            G.paused = 1;
            G.auto_pause = 1;
            set_status_text();
            redraw();
        }
    }
}

static void enqueue_for_player(DirQueue *dq, int keycode, Dir d) {
    if (keycode < 0) return;
    if (keycode > 255) keycode = 255;

    if (G.key_down[keycode]) return;

    G.key_down[keycode] = 1;
    (void)dirq_push(dq, d);
}

static void on_key(Widget w, XtPointer client, XEvent *ev, Boolean *cont) {
    (void)w; (void)client;
    *cont = True;

    if (ev->type == KeyRelease) {
        int kc = ev->xkey.keycode;
        if (kc < 0) return;
        if (kc > 255) kc = 255;
        G.key_down[kc] = 0;
        return;
    }

    if (ev->type != KeyPress) return;

    /* if round-end dialog open, ignore game keys */
    if (G.roundend_active) return;

    KeySym ks;
    char buf[8];
    XLookupString(&ev->xkey, buf, sizeof(buf), &ks, NULL);

    if (ks == XK_p || ks == XK_P) {
        G.paused = !G.paused;
        set_status_text();
        redraw();
        return;
    }

    if (ks == XK_r || ks == XK_R) {
        /* restart current round (same level, same match) */
        G.roundend_active = 0;
        reset_round();
        G.paused = 0;
        set_status_text();
        redraw();
        return;
    }

    if (ks == XK_Up)    { enqueue_for_player(&G.p1q, ev->xkey.keycode, DIR_UP); return; }
    if (ks == XK_Down)  { enqueue_for_player(&G.p1q, ev->xkey.keycode, DIR_DOWN); return; }
    if (ks == XK_Left)  { enqueue_for_player(&G.p1q, ev->xkey.keycode, DIR_LEFT); return; }
    if (ks == XK_Right) { enqueue_for_player(&G.p1q, ev->xkey.keycode, DIR_RIGHT); return; }

    if (G.two_player) {
        if (ks == XK_w || ks == XK_W) { enqueue_for_player(&G.p2q, ev->xkey.keycode, DIR_UP); return; }
        if (ks == XK_s || ks == XK_S) { enqueue_for_player(&G.p2q, ev->xkey.keycode, DIR_DOWN); return; }
        if (ks == XK_a || ks == XK_A) { enqueue_for_player(&G.p2q, ev->xkey.keycode, DIR_LEFT); return; }
        if (ks == XK_d || ks == XK_D) { enqueue_for_player(&G.p2q, ev->xkey.keycode, DIR_RIGHT); return; }
    }
}

/* ---------- Colors / GC ---------- */

static unsigned long alloc_named_color(Display *dpy, Colormap cmap, const char *name, unsigned long fallback) {
    XColor scr, exact;
    if (XAllocNamedColor(dpy, cmap, name, &scr, &exact)) return scr.pixel;
    return fallback;
}

static GC make_gc(unsigned long pixel) {
    XGCValues v;
    memset(&v, 0, sizeof(v));
    v.foreground = pixel;
    v.background = G.col_bg;
    return XCreateGC(G.dpy, G.win, GCForeground|GCBackground, &v);
}

static void setup_graphics(void) {
    G.dpy = XtDisplay(G.drawing);
    G.win = XtWindow(G.drawing);

    Colormap cmap = DefaultColormap(G.dpy, DefaultScreen(G.dpy));

    G.col_bg   = alloc_named_color(G.dpy, cmap, "black",   BlackPixel(G.dpy, DefaultScreen(G.dpy)));
    G.col_grid = alloc_named_color(G.dpy, cmap, "gray25",  G.col_bg);
    G.col_wall = alloc_named_color(G.dpy, cmap, "gray70",  WhitePixel(G.dpy, DefaultScreen(G.dpy)));
    G.col_text = alloc_named_color(G.dpy, cmap, "white",   WhitePixel(G.dpy, DefaultScreen(G.dpy)));
    G.col_food = alloc_named_color(G.dpy, cmap, "yellow",  WhitePixel(G.dpy, DefaultScreen(G.dpy)));
    G.col_s1   = alloc_named_color(G.dpy, cmap, "cyan",    WhitePixel(G.dpy, DefaultScreen(G.dpy)));
    G.col_s2   = alloc_named_color(G.dpy, cmap, "magenta", WhitePixel(G.dpy, DefaultScreen(G.dpy)));

    G.gc_bg   = make_gc(G.col_bg);
    G.gc_grid = make_gc(G.col_grid);
    G.gc_wall = make_gc(G.col_wall);
    G.gc_text = make_gc(G.col_text);
    G.gc_food = make_gc(G.col_food);
    G.gc_s1   = make_gc(G.col_s1);
    G.gc_s2   = make_gc(G.col_s2);

    G.s1.color = G.col_s1;
    G.s2.color = G.col_s2;
}

static float query_x_dpi(Display *dpy) {
    int scr = DefaultScreen(dpy);
    int px = DisplayWidth(dpy, scr);
    int mm = DisplayWidthMM(dpy, scr);
    if (mm <= 0) return 96.0f;
    return (float)px * 25.4f / (float)mm;
}

static float clamp_f(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static Pixmap create_pixmap_from_xpm_data(const char *source_desc, char **data, Pixmap *mask_out) {
    Display *dpy = XtDisplay(G.toplevel);
    if (!dpy || !data) {
        dbg_icons("[ck-nibbles] xpm: cannot load (%s): dpy=%p data=%p",
                  source_desc ? source_desc : "n/a",
                  (void *)dpy,
                  (void *)data);
        return None;
    }

    Pixmap pixmap = None;
    Pixmap mask = None;
    XpmAttributes attr;
    memset(&attr, 0, sizeof(attr));
    attr.valuemask = XpmSize;

    dbg_icons("[ck-nibbles] xpm: loading from compiled-in data (%s)", source_desc ? source_desc : "n/a");

    int status = XpmCreatePixmapFromData(dpy,
                                         DefaultRootWindow(dpy),
                                         data,
                                         &pixmap,
                                         &mask,
                                         &attr);
    if (g_debug_icons) {
        const char *err = XpmGetErrorString(status);
        dbg_icons("[ck-nibbles] xpm: status=%d (%s) pixmap=0x%lx mask=0x%lx size=%ux%u",
                  status,
                  err ? err : "n/a",
                  (unsigned long)pixmap,
                  (unsigned long)mask,
                  (unsigned)attr.width,
                  (unsigned)attr.height);
    }
    if (status != XpmSuccess) {
        if (pixmap != None) XFreePixmap(dpy, pixmap);
        if (mask != None) XFreePixmap(dpy, mask);
        return None;
    }

    if (mask_out) {
        *mask_out = mask;
    } else if (mask != None) {
        XFreePixmap(dpy, mask);
    }

    return pixmap;
}

static Pixmap scale_pixmap_nearest(Display *dpy,
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
    if (!src_img) {
        dbg_icons("[ck-nibbles] xpm: XGetImage failed for scaling src=0x%lx", (unsigned long)src);
        return src;
    }

    Window root = DefaultRootWindow(dpy);
    Pixmap dst = XCreatePixmap(dpy, root, dst_w, dst_h, depth);
    if (dst == None) {
        dbg_icons("[ck-nibbles] xpm: XCreatePixmap failed for scaling to %ux%u", dst_w, dst_h);
        XDestroyImage(src_img);
        return src;
    }

    Visual *visual = DefaultVisual(dpy, DefaultScreen(dpy));
    XImage *dst_img = XCreateImage(dpy, visual, depth, ZPixmap, 0, NULL, dst_w, dst_h, 32, 0);
    if (!dst_img) {
        dbg_icons("[ck-nibbles] xpm: XCreateImage failed for scaling to %ux%u", dst_w, dst_h);
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
        dbg_icons("[ck-nibbles] xpm: OOM creating scaled image buffer %ux%u", dst_w, dst_h);
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
    dbg_icons("[ck-nibbles] xpm: scaled pixmap to %ux%u (scale=%.2f)", dst_w, dst_h, scale);
    return dst;
}

static Pixmap create_flattened_pixmap_from_xpm_data(const char *source_desc,
                                                    char **data,
                                                    Widget background_widget)
{
    Pixmap mask = None;
    Pixmap src = create_pixmap_from_xpm_data(source_desc, data, &mask);
    if (src == None) return None;

    Display *dpy = XtDisplay(G.toplevel);

    Window root = DefaultRootWindow(dpy);
    Window dummy_root = None;
    int x = 0, y = 0;
    unsigned int w = 0, h = 0, bw = 0, depth = 0;
    if (!XGetGeometry(dpy, src, &dummy_root, &x, &y, &w, &h, &bw, &depth)) {
        dbg_icons("[ck-nibbles] xpm: XGetGeometry failed (%s) src=0x%lx", source_desc ? source_desc : "n/a", (unsigned long)src);
        if (mask != None) XFreePixmap(dpy, mask);
        return src;
    }

    Pixel bg = BlackPixel(dpy, DefaultScreen(dpy));
    if (background_widget) {
        XtVaGetValues(background_widget, XmNbackground, &bg, NULL);
    }
    dbg_icons("[ck-nibbles] xpm: flattening (%s) using bg_pixel=0x%lx size=%ux%u depth=%u mask=0x%lx",
              source_desc ? source_desc : "n/a",
              (unsigned long)bg,
              w, h,
              depth,
              (unsigned long)mask);

    Pixmap dst = XCreatePixmap(dpy, root, w, h, depth);
    if (dst == None) {
        dbg_icons("[ck-nibbles] xpm: XCreatePixmap failed (%s)", source_desc ? source_desc : "n/a");
        if (mask != None) XFreePixmap(dpy, mask);
        return src;
    }

    XGCValues gcv;
    memset(&gcv, 0, sizeof(gcv));
    gcv.foreground = bg;
    gcv.function = GXcopy;
    GC gc = XCreateGC(dpy, dst, GCForeground | GCFunction, &gcv);
    XFillRectangle(dpy, dst, gc, 0, 0, w, h);

    if (mask != None) {
        XSetClipMask(dpy, gc, mask);
        XSetClipOrigin(dpy, gc, 0, 0);
    }
    XCopyArea(dpy, src, dst, gc, 0, 0, w, h, 0, 0);
    XSetClipMask(dpy, gc, None);

    XFreeGC(dpy, gc);
    XFreePixmap(dpy, src);
    if (mask != None) XFreePixmap(dpy, mask);

    return dst;
}

static void setup_window_icon(void) {
    if (!G.toplevel || G.icon_pixmap != None) return;
    Display *dpy = XtDisplay(G.toplevel);
    if (!dpy) return;

    Pixmap mask = None;
    dbg_icons("[ck-nibbles] window icon: requesting icon pixmap from src/games/ck-nibbles/images/ck-nibbles.l.pm (nibbles_l_pm)");
    Pixmap pixmap = create_pixmap_from_xpm_data("src/games/ck-nibbles/images/ck-nibbles.l.pm:nibbles_l_pm", nibbles_l_pm, &mask);
    if (pixmap == None) {
        dbg_icons("[ck-nibbles] window icon: failed to load xpm");
        return;
    }

    G.icon_pixmap = pixmap;
    if (mask != None) {
        G.icon_mask = mask;
    }

    Window win = XtWindow(G.toplevel);
    if (win == None) {
        dbg_icons("[ck-nibbles] window icon: XtWindow(toplevel) is None");
        return;
    }

    XWMHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = IconPixmapHint;
    hints.icon_pixmap = pixmap;
    if (mask != None) {
        hints.flags |= IconMaskHint;
        hints.icon_mask = mask;
    }
    XSetWMHints(dpy, win, &hints);
    dbg_icons("[ck-nibbles] window icon: set WM hints on win=0x%lx pixmap=0x%lx mask=0x%lx",
              (unsigned long)win,
              (unsigned long)pixmap,
              (unsigned long)mask);
}

static Pixmap ensure_about_icon_pixmap(Widget background_widget) {
    if (G.about_icon_pixmap != None) return G.about_icon_pixmap;
    dbg_icons("[ck-nibbles] about icon: base is large icon src/games/ck-nibbles/images/ck-nibbles.l.pm (nibbles_l_pm)");

    Pixmap flat = create_flattened_pixmap_from_xpm_data(
        "src/games/ck-nibbles/images/ck-nibbles.l.pm:nibbles_l_pm",
        nibbles_l_pm,
        background_widget ? background_widget : G.toplevel);

    if (flat == None) {
        dbg_icons("[ck-nibbles] about icon: failed to build flattened base pixmap");
        return None;
    }

    Window dummy_root = None;
    int x = 0, y = 0;
    unsigned int w = 0, h = 0, bw = 0, depth = 0;
    if (!XGetGeometry(XtDisplay(G.toplevel), flat, &dummy_root, &x, &y, &w, &h, &bw, &depth)) {
        dbg_icons("[ck-nibbles] about icon: XGetGeometry failed for flat pixmap=0x%lx", (unsigned long)flat);
        G.about_icon_pixmap = flat;
    } else {
        float s = clamp_f(g_about_ui_scale, 1.0f, 4.0f);
        dbg_icons("[ck-nibbles] about icon: dpi-derived scale=%.2f (base %ux%u)", s, w, h);
        G.about_icon_pixmap = scale_pixmap_nearest(XtDisplay(G.toplevel), flat, w, h, depth, s);
    }

    dbg_icons("[ck-nibbles] about icon: pixmap=0x%lx", (unsigned long)G.about_icon_pixmap);
    return G.about_icon_pixmap;
}

static void add_about_icon_to_page(Widget notebook, Pixmap pixmap) {
    if (!notebook || pixmap == None) return;
    Widget page = XtNameToWidget(notebook, "page_app_about");
    if (!page) {
        dbg_icons("[ck-nibbles] about icon: page_app_about not found");
        return;
    }

    Arg args[12];
    int n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNtopOffset, 12); n++;
    XtSetArg(args[n], XmNleftOffset, 12); n++;
    XtSetArg(args[n], XmNlabelType, XmPIXMAP); n++;
    XtSetArg(args[n], XmNlabelPixmap, pixmap); n++;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_CENTER); n++;
    XtSetArg(args[n], XmNmarginWidth, 0); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    XtSetArg(args[n], XmNtraversalOn, False); n++;
    Widget icon_label = XmCreateLabel(page, "nibbles_page_icon", args, n);
    if (icon_label) XtManageChild(icon_label);
    dbg_icons("[ck-nibbles] about icon: added label=%p with pixmap=0x%lx",
              (void *)icon_label,
              (unsigned long)pixmap);

    /* Avoid overlapping the title/subtitle labels (about_add_title_page anchors them at the form). */
    if (icon_label) {
        Widget title = XtNameToWidget(page, "label_title");
        if (title) {
            XtVaSetValues(title,
                          XmNleftAttachment, XmATTACH_WIDGET,
                          XmNleftWidget, icon_label,
                          XmNleftOffset, 12,
                          XmNalignment, XmALIGNMENT_BEGINNING,
                          NULL);
        }
        Widget subtitle = XtNameToWidget(page, "label_subtitle");
        if (subtitle) {
            XtVaSetValues(subtitle,
                          XmNleftAttachment, XmATTACH_WIDGET,
                          XmNleftWidget, icon_label,
                          XmNleftOffset, 12,
                          XmNalignment, XmALIGNMENT_BEGINNING,
                          NULL);
        }
    }
}

static void on_realize(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)client; (void)call;
    setup_graphics();
    setup_window_icon();

    /*
     * IMPORTANT:
     * - Fresh start: build a clean match state (then we show startup New Game dialog)
     * - Session restore: DO NOT wipe restored values (scores/rounds/level/etc).
     *   Just ensure round state exists for the current level.
     */
    if (G.session_restored) {
        dirq_clear(&G.p1q);
        dirq_clear(&G.p2q);
        memset(G.key_down, 0, sizeof(G.key_down));
        reset_round();          /* keep restored scores/match stats */
        set_status_text();
        redraw();
    } else {
        /* prepared fresh game state; startup dialog will appear after WM ConfigureNotify */
        restart_game(1);
    }
}

/* ---------- Menu callbacks ---------- */

static void on_close(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)client; (void)call;
    exit(0);
}

static void on_toggle_grid(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)client;
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call;
    G.show_grid = cbs->set;
    redraw();
}

static void on_about(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)client; (void)call;

    Widget dialog_shell = NULL;
    Widget notebook = about_dialog_build(G.toplevel, "about_dialog", "About Nibbles", &dialog_shell);
    if (!notebook || !dialog_shell) return;
    dbg_icons("[ck-nibbles] about dialog: shell=%p notebook=%p", (void *)dialog_shell, (void *)notebook);

    about_add_standard_pages(notebook, 1,
                             "Nibbles",
                             "Nibbles",
                             "Snake Game\n\nControls:\nArrows: Move Player 1\nWASD: Move Player 2 (if enabled)\nP: Pause\nR: Restart (round)",
                             True);

    /* Build a "flattened" pixmap so XPM transparency doesn't render as garbage in XmLabel. */
    Widget page = XtNameToWidget(notebook, "page_app_about");
    dbg_icons("[ck-nibbles] about icon: using page=%p as background reference", (void *)page);
    Pixmap about_pixmap = ensure_about_icon_pixmap(page ? page : notebook);
    if (about_pixmap != None) {
        add_about_icon_to_page(notebook, about_pixmap);
    } else {
        dbg_icons("[ck-nibbles] about dialog: about pixmap is None (not adding icon)");
    }

    XtVaSetValues(dialog_shell,
                  XmNwidth, 700,
                  XmNheight, 450,
                  NULL);

    XtPopup(dialog_shell, XtGrabNone);
}

/* ---------- Round End Dialog ---------- */

static void build_round_end_dialog(void) {
    if (G.roundend_dialog) return;

    Arg args[8];
    int n = 0;
    XtSetArg(args[n], XmNdialogStyle, XmDIALOG_FULL_APPLICATION_MODAL); n++;

    G.roundend_dialog = XmCreateMessageDialog(G.toplevel, "round_end_dialog", args, n);

    /* Hide Help button */
    Widget helpb = XmMessageBoxGetChild(G.roundend_dialog, XmDIALOG_HELP_BUTTON);
    if (helpb) XtUnmanageChild(helpb);

    /* Set static title via parent shell */
    Widget shell = XtParent(G.roundend_dialog);
    XtVaSetValues(shell,
                  XmNtitle, "Round Result",
                  XmNdeleteResponse, XmUNMAP,
                  XmNtransientFor, G.toplevel,
                  NULL);

    /* Labels for buttons (default; may change when match finishes) */
    XmString ok = XmStringCreateLocalized("Next Round");
    XmString cancel = XmStringCreateLocalized("End Game");
    XtVaSetValues(G.roundend_dialog,
                  XmNokLabelString, ok,
                  XmNcancelLabelString, cancel,
                  NULL);
    XmStringFree(ok);
    XmStringFree(cancel);

    XtAddCallback(G.roundend_dialog, XmNokCallback, on_roundend_next, NULL);
    XtAddCallback(G.roundend_dialog, XmNcancelCallback, on_roundend_end, NULL);
}

static void show_round_end_dialog(const char *msg) {
    build_round_end_dialog();

    XmString xs = XmStringCreateLocalized((char*)(msg ? msg : ""));
    XtVaSetValues(G.roundend_dialog,
                  XmNmessageString, xs,
                  NULL);
    XmStringFree(xs);

    /* If match is finished, relabel OK button */
    if (G.rounds_played >= G.rounds_total) {
        XmString ok = XmStringCreateLocalized("New Game...");
        XtVaSetValues(G.roundend_dialog, XmNokLabelString, ok, NULL);
        XmStringFree(ok);
    } else {
        XmString ok = XmStringCreateLocalized("Next Round");
        XtVaSetValues(G.roundend_dialog, XmNokLabelString, ok, NULL);
        XmStringFree(ok);
    }

    XtManageChild(G.roundend_dialog);

    /* Raise */
    Widget shell = XtParent(G.roundend_dialog);
    if (XtIsRealized(shell)) {
        XRaiseWindow(XtDisplay(shell), XtWindow(shell));
    }
}

static void on_roundend_next(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)client; (void)call;

    if (G.roundend_dialog) XtUnmanageChild(G.roundend_dialog);

    G.roundend_active = 0;

    /* If match finished -> go back to New Game dialog */
    if (G.rounds_played >= G.rounds_total) {
        show_new_game_dialog(0);
        return;
    }

    /* Advance level per round (simple + predictable) */
    G.level = (G.level + 1) % 12;

    /* Start next round */
    reset_round();
    G.paused = 0;
    set_status_text();
    redraw();
}

static void on_roundend_end(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)client; (void)call;

    if (G.roundend_dialog) XtUnmanageChild(G.roundend_dialog);
    G.roundend_active = 0;

    show_new_game_dialog(0);
}

/* ---------- New Game Dialog (robust FormDialog) ---------- */

static void on_newgame_mode_changed(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)client; (void)call;

    Boolean is_1p = XmToggleButtonGetState(G.newgame_rb_1p);

    XmString xs = XmStringCreateLocalized(is_1p ? "CPU Name:" : "Player 2 Name:");
    XtVaSetValues(G.newgame_label_p2, XmNlabelString, xs, NULL);
    XmStringFree(xs);

    /* keep editable even for CPU (nice) */
    XtSetSensitive(G.newgame_text_p2, True);
}

static void on_newgame_cancel(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)client; (void)call;

    G.paused = 1;
    set_status_text();
    redraw();

    if (G.newgame_dialog) XtUnmanageChild(G.newgame_dialog);
}

/* OK/Start: validate rounds strictly (1..99). If invalid -> warning, do NOT start. */
static void on_newgame_start(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)client; (void)call;

    Boolean is_2p = XmToggleButtonGetState(G.newgame_rb_2p);
    G.two_player = is_2p ? 1 : 0;

    char *t1 = XmTextFieldGetString(G.newgame_text_p1);
    char *t2 = XmTextFieldGetString(G.newgame_text_p2);
    char *tr = XmTextFieldGetString(G.newgame_text_rounds);

    /* Validate rounds BEFORE applying changes / closing dialog */
    int rounds = 0;
    if (!parse_int_strict_range(tr, 1, 99, &rounds)) {
        if (t1) XtFree(t1);
        if (t2) XtFree(t2);
        if (tr) XtFree(tr);

        show_warning("Please enter a valid number of rounds from 1 to 99.");
        return; /* keep dialog open */
    }

    trim_copy(G.player1_name, sizeof(G.player1_name), t1, "Player 1");
    if (G.two_player) trim_copy(G.player2_name, sizeof(G.player2_name), t2, "Player 2");
    else              trim_copy(G.player2_name, sizeof(G.player2_name), t2, "CPU");

    G.rounds_total = rounds;

    if (t1) XtFree(t1);
    if (t2) XtFree(t2);
    if (tr) XtFree(tr);

    /* Start a fresh match */
    restart_game(1);

    if (G.newgame_dialog) XtUnmanageChild(G.newgame_dialog);
}

static void build_new_game_dialog(void) {
    if (G.newgame_dialog) return;

    Arg args[16];
    int n = 0;

    XtSetArg(args[n], XmNdialogStyle, XmDIALOG_FULL_APPLICATION_MODAL); n++;
    XtSetArg(args[n], XmNautoUnmanage, False); n++;
    G.newgame_dialog = XmCreateFormDialog(G.toplevel, "newgame_dialog", args, n);

    Widget shell = XtParent(G.newgame_dialog);
    XtVaSetValues(shell,
                  XmNtitle, "New Game",
                  XmNdeleteResponse, XmUNMAP,
                  XmNtransientFor, G.toplevel,
                  XmNallowShellResize, True,
                  NULL);

    XtVaSetValues(G.newgame_dialog,
                  XmNfractionBase, 100,
                  XmNmarginWidth,  10,
                  XmNmarginHeight, 10,
                  NULL);

    /* Buttons */
    Widget start_button = XtVaCreateManagedWidget("start_button",
                                                  xmPushButtonWidgetClass, G.newgame_dialog,
                                                  XmNlabelString, XmStringCreateLocalized("Start"),
                                                  XmNbottomAttachment, XmATTACH_FORM,
                                                  XmNleftAttachment,   XmATTACH_POSITION,
                                                  XmNleftPosition,     20,
                                                  XmNrightAttachment,  XmATTACH_POSITION,
                                                  XmNrightPosition,    45,
                                                  NULL);
    XtAddCallback(start_button, XmNactivateCallback, on_newgame_start, NULL);

    Widget cancel_button = XtVaCreateManagedWidget("cancel_button",
                                                   xmPushButtonWidgetClass, G.newgame_dialog,
                                                   XmNlabelString, XmStringCreateLocalized("Cancel"),
                                                   XmNbottomAttachment, XmATTACH_FORM,
                                                   XmNleftAttachment,   XmATTACH_POSITION,
                                                   XmNleftPosition,     55,
                                                   XmNrightAttachment,  XmATTACH_POSITION,
                                                   XmNrightPosition,    80,
                                                   NULL);
    XtAddCallback(cancel_button, XmNactivateCallback, on_newgame_cancel, NULL);

    XtVaSetValues(G.newgame_dialog, XmNdefaultButton, start_button, NULL);

    /* Players Frame */
    Widget players_frame = XtVaCreateManagedWidget("players_frame",
                                                   xmFrameWidgetClass, G.newgame_dialog,
                                                   XmNtopAttachment,   XmATTACH_FORM,
                                                   XmNleftAttachment,  XmATTACH_FORM,
                                                   XmNrightAttachment, XmATTACH_FORM,
                                                   XmNshadowType,      XmSHADOW_ETCHED_IN,
                                                   NULL);

    (void)XtVaCreateManagedWidget("players_label",
                                  xmLabelWidgetClass, players_frame,
                                  XmNlabelString, XmStringCreateLocalized("Number of Players"),
                                  XmNframeChildType, XmFRAME_TITLE_CHILD,
                                  NULL);

    Widget radio_box = XmCreateRadioBox(players_frame, "radio_box", NULL, 0);
    XtManageChild(radio_box);

    G.newgame_rb_1p = XtVaCreateManagedWidget("rb_1p",
                                              xmToggleButtonWidgetClass, radio_box,
                                              XmNlabelString, XmStringCreateLocalized("1 - You vs CPU"),
                                              XmNset, True,
                                              NULL);
    XtAddCallback(G.newgame_rb_1p, XmNvalueChangedCallback, on_newgame_mode_changed, NULL);

    G.newgame_rb_2p = XtVaCreateManagedWidget("rb_2p",
                                              xmToggleButtonWidgetClass, radio_box,
                                              XmNlabelString, XmStringCreateLocalized("2 - Two Players"),
                                              XmNset, False,
                                              NULL);
    XtAddCallback(G.newgame_rb_2p, XmNvalueChangedCallback, on_newgame_mode_changed, NULL);

    /* Names + Rounds Frame */
    Widget cfg_frame = XtVaCreateManagedWidget("cfg_frame",
                                               xmFrameWidgetClass, G.newgame_dialog,
                                               XmNtopAttachment,    XmATTACH_WIDGET,
                                               XmNtopWidget,        players_frame,
                                               XmNleftAttachment,   XmATTACH_FORM,
                                               XmNrightAttachment,  XmATTACH_FORM,
                                               XmNbottomAttachment, XmATTACH_WIDGET,
                                               XmNbottomWidget,     start_button,
                                               XmNbottomOffset,     10,
                                               XmNshadowType,       XmSHADOW_ETCHED_IN,
                                               NULL);

    (void)XtVaCreateManagedWidget("cfg_label",
                                  xmLabelWidgetClass, cfg_frame,
                                  XmNlabelString, XmStringCreateLocalized("Players & Match"),
                                  XmNframeChildType, XmFRAME_TITLE_CHILD,
                                  NULL);

    Widget cfg_form = XtVaCreateManagedWidget("cfg_form",
                                              xmFormWidgetClass, cfg_frame,
                                              XmNfractionBase, 100,
                                              XmNmarginWidth,  8,
                                              XmNmarginHeight, 8,
                                              NULL);

    /* Align all text fields to the same X by using a fixed label column */
    enum { LABEL_COL_RIGHT = 35, TEXT_COL_LEFT = 36 };

    (void)XtVaCreateManagedWidget("label_p1",
                                  xmLabelWidgetClass, cfg_form,
                                  XmNlabelString, XmStringCreateLocalized("Player 1 Name:"),
                                  XmNtopAttachment,  XmATTACH_FORM,
                                  XmNleftAttachment, XmATTACH_FORM,
                                  XmNrightAttachment, XmATTACH_POSITION,
                                  XmNrightPosition, LABEL_COL_RIGHT,
                                  XmNalignment,      XmALIGNMENT_END,
                                  NULL);

    G.newgame_text_p1 = XtVaCreateManagedWidget("text_p1",
                                                xmTextFieldWidgetClass, cfg_form,
                                                XmNtopAttachment,   XmATTACH_FORM,
                                                XmNleftAttachment,  XmATTACH_POSITION,
                                                XmNleftPosition,    TEXT_COL_LEFT,
                                                XmNrightAttachment, XmATTACH_FORM,
                                                XmNcolumns,         24,
                                                NULL);

    G.newgame_label_p2 = XtVaCreateManagedWidget("label_p2",
                                                 xmLabelWidgetClass, cfg_form,
                                                 XmNlabelString, XmStringCreateLocalized("CPU Name:"),
                                                 XmNtopAttachment,  XmATTACH_WIDGET,
                                                 XmNtopWidget,      G.newgame_text_p1,
                                                 XmNtopOffset,      8,
                                                 XmNleftAttachment, XmATTACH_FORM,
                                                 XmNrightAttachment, XmATTACH_POSITION,
                                                 XmNrightPosition, LABEL_COL_RIGHT,
                                                 XmNalignment,      XmALIGNMENT_END,
                                                 NULL);

    G.newgame_text_p2 = XtVaCreateManagedWidget("text_p2",
                                                xmTextFieldWidgetClass, cfg_form,
                                                XmNtopAttachment,   XmATTACH_WIDGET,
                                                XmNtopWidget,       G.newgame_text_p1,
                                                XmNtopOffset,       8,
                                                XmNleftAttachment,  XmATTACH_POSITION,
                                                XmNleftPosition,    TEXT_COL_LEFT,
                                                XmNrightAttachment, XmATTACH_FORM,
                                                XmNcolumns,         24,
                                                NULL);

    (void)XtVaCreateManagedWidget("label_rounds",
                                  xmLabelWidgetClass, cfg_form,
                                  XmNlabelString, XmStringCreateLocalized("Rounds (1..99):"),
                                  XmNtopAttachment,  XmATTACH_WIDGET,
                                  XmNtopWidget,      G.newgame_text_p2,
                                  XmNtopOffset,      10,
                                  XmNleftAttachment, XmATTACH_FORM,
                                  XmNrightAttachment, XmATTACH_POSITION,
                                  XmNrightPosition, LABEL_COL_RIGHT,
                                  XmNalignment,      XmALIGNMENT_END,
                                  NULL);

    G.newgame_text_rounds = XtVaCreateManagedWidget("text_rounds",
                                                    xmTextFieldWidgetClass, cfg_form,
                                                    XmNtopAttachment,  XmATTACH_WIDGET,
                                                    XmNtopWidget,      G.newgame_text_p2,
                                                    XmNtopOffset,      10,
                                                    XmNleftAttachment, XmATTACH_POSITION,
                                                    XmNleftPosition,   TEXT_COL_LEFT,
                                                    XmNcolumns,        6,
                                                    NULL);

    /* Optional: give the dialog a reasonable initial size so centering works nicely */
    XtVaSetValues(shell, XmNwidth, 420, XmNheight, 260, NULL);

    on_newgame_mode_changed(NULL, NULL, NULL);
}

static void show_new_game_dialog(int is_startup) {
    build_new_game_dialog();

    G.newgame_is_startup = is_startup;

    if (!is_startup) {
        G.newgame_prev_paused = G.paused;
    } else {
        G.newgame_prev_paused = 1;
    }

    /* pause while dialog is open */
    G.paused = 1;
    set_status_text();
    redraw();

    XmToggleButtonSetState(G.newgame_rb_1p, !G.two_player, False);
    XmToggleButtonSetState(G.newgame_rb_2p,  G.two_player, False);

    XmTextFieldSetString(G.newgame_text_p1, (G.player1_name[0] ? G.player1_name : "Player 1"));
    XmTextFieldSetString(G.newgame_text_p2, (G.player2_name[0] ? G.player2_name : (G.two_player ? "Player 2" : "CPU")));

    {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%d", (G.rounds_total > 0 ? G.rounds_total : 3));
        XmTextFieldSetString(G.newgame_text_rounds, tmp);
    }

    on_newgame_mode_changed(NULL, NULL, NULL);

    /* Center over the main window */
    center_shell_over_widget(XtParent(G.newgame_dialog), G.toplevel);

    XtManageChild(G.newgame_dialog);

    Widget shell = XtParent(G.newgame_dialog);
    if (XtIsRealized(shell)) {
        XRaiseWindow(XtDisplay(shell), XtWindow(shell));
    }
}

static void on_new_game_menu(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)client; (void)call;
    show_new_game_dialog(0);
}

static void cb_wm_delete(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)client; (void)call;
    exit(0);
}

static void cb_wm_save(Widget w, XtPointer client, XtPointer call) {
    (void)client; (void)call;
    session_capture_geometry(G.toplevel, G.session_data, "x", "y", "w", "h");
    session_data_set_int(G.session_data, "show_grid", G.show_grid);
    session_data_set_int(G.session_data, "two_player", G.two_player);
    session_data_set(G.session_data, "player1_name", G.player1_name);
    session_data_set(G.session_data, "player2_name", G.player2_name);
    session_data_set_int(G.session_data, "rounds_total", G.rounds_total);
    session_data_set_int(G.session_data, "rounds_played", G.rounds_played);
    session_data_set_int(G.session_data, "p1_round_wins", G.p1_round_wins);
    session_data_set_int(G.session_data, "p2_round_wins", G.p2_round_wins);
    session_data_set_int(G.session_data, "draw_rounds", G.draw_rounds);
    session_data_set_int(G.session_data, "level", G.level);
    session_data_set_int(G.session_data, "paused", G.paused);
    session_data_set_int(G.session_data, "s1_score", G.s1.score);
    session_data_set_int(G.session_data, "s2_score", G.s2.score);
    /* Serialize more state as needed: snakes, food, walls, etc. */
    /* For simplicity, assuming restart on restore, but extend as per task */
    session_save(G.toplevel, G.session_data, G.exec_path);
}

static void restore_game_state(void) {
    // Parse and restore food
    const char *food_str = session_data_get(G.session_data, "food");
    if (food_str) {
        sscanf(food_str, "%d,%d", &G.food.x, &G.food.y);
        G.has_food = 1;
    }

    // Restore snakes (example: body as string "x1,y1;x2,y2;...")
    const char *s1_body = session_data_get(G.session_data, "s1_body");
    if (s1_body) {
        // Parse string to body array
        // Implement parsing logic
    }
    // Similarly for s2, walls, etc.
}

/* Show startup dialog only after WM has placed/resized the toplevel at least once. */
static void on_toplevel_configure(Widget w, XtPointer client, XEvent *ev, Boolean *cont) {
    (void)client;
    *cont = True;

    if (startup_dialog_shown) return;
    if (ev->type != ConfigureNotify) return;

    /* First real geometry from the WM -> now centering works reliably */
    startup_dialog_shown = 1;
    XtRemoveEventHandler(w, StructureNotifyMask, False, on_toplevel_configure, NULL);

    /* Pause and force startup dialog */
    G.paused = 1;
    set_status_text();
    redraw();
    show_new_game_dialog(1);
}

/* ---------- Main ---------- */

static int arg_is(const char *a, const char *b) { return a && b && strcmp(a,b)==0; }

static int env_truthy(const char *name) {
    const char *v = getenv(name);
    if (!v || !*v) return 0;
    if (strcmp(v, "0") == 0) return 0;
    if (strcasecmp(v, "false") == 0) return 0;
    if (strcasecmp(v, "no") == 0) return 0;
    if (strcasecmp(v, "off") == 0) return 0;
    return 1;
}

int main(int argc, char **argv) {
    memset(&G, 0, sizeof(G));
    G.current_status[0] = '\0';
    dirq_clear(&G.p1q);
    dirq_clear(&G.p2q);
    memset(G.key_down, 0, sizeof(G.key_down));

    strcpy(G.player1_name, "Player 1");
    strcpy(G.player2_name, "CPU");
    G.rounds_total = 3;

    G.two_player = 0;
    for (int i=1;i<argc;i++) {
        if (arg_is(argv[i], "-2") || arg_is(argv[i], "--2") || arg_is(argv[i], "--two")) {
            G.two_player = 1;
            strcpy(G.player2_name, "Player 2");
        }
    }

    init_exec_path(G.exec_path, sizeof(G.exec_path), argv[0]);

    g_debug_icons = env_truthy("CK_NIBBLES_DEBUG_ICONS");

    char *session_id = session_parse_argument(&argc, argv);
    G.session_data = session_data_create(session_id);
    free(session_id);
    G.session_restored = 0;

    srand((unsigned)time(NULL));

    G.level = 0;
    G.tick_ms = TICK_MS;
    G.paused = 0;
    G.show_grid = 0;
    G.auto_pause = 0;

    XtSetLanguageProc(NULL, NULL, NULL);

    G.toplevel = XtVaAppInitialize(&G.app, "CkNibbles",
                                   NULL, 0, &argc, argv,
                                   NULL,
                                   XmNtitle, "Nibbles",
                                   XmNiconName, "Nibbles",
                                   NULL);

    {
        Display *dpy = XtDisplay(G.toplevel);
        float dpi = query_x_dpi(dpy);
        g_about_ui_scale = clamp_f(dpi / 96.0f, 1.0f, 4.0f);
        dbg_icons("[ck-nibbles] about icon: dpi=%.1f -> ui_scale=%.2f", dpi, g_about_ui_scale);
    }

    DtInitialize(XtDisplay(G.toplevel), G.toplevel, "CkNibbles", "CkNibbles");

    if (G.session_data) {
        if (session_load(G.toplevel, G.session_data)) {
            G.show_grid = session_data_get_int(G.session_data, "show_grid", G.show_grid);
            G.two_player = session_data_get_int(G.session_data, "two_player", G.two_player);
            const char *p1_name = session_data_get(G.session_data, "player1_name");
            if (p1_name) strcpy(G.player1_name, p1_name);
            const char *p2_name = session_data_get(G.session_data, "player2_name");
            if (p2_name) strcpy(G.player2_name, p2_name);
            G.rounds_total = session_data_get_int(G.session_data, "rounds_total", G.rounds_total);
            G.rounds_played = session_data_get_int(G.session_data, "rounds_played", G.rounds_played);
            G.p1_round_wins = session_data_get_int(G.session_data, "p1_round_wins", G.p1_round_wins);
            G.p2_round_wins = session_data_get_int(G.session_data, "p2_round_wins", G.p2_round_wins);
            G.draw_rounds = session_data_get_int(G.session_data, "draw_rounds", G.draw_rounds);
            G.level = session_data_get_int(G.session_data, "level", G.level);
            G.paused = session_data_get_int(G.session_data, "paused", G.paused);
            G.s1.score = session_data_get_int(G.session_data, "s1_score", G.s1.score);
            G.s2.score = session_data_get_int(G.session_data, "s2_score", G.s2.score);
            /* Restore more state as needed */
            if (G.session_data->session_id) {
                /* We will treat this as a restore if started with a session id */
                G.session_restored = 1;
                G.paused = 1; /* paused on restore */
                restore_game_state();
            }
            session_apply_geometry(G.toplevel, G.session_data, "x", "y", "w", "h");
        }
    }

    Widget form = XtVaCreateManagedWidget("form",
                                          xmFormWidgetClass, G.toplevel,
                                          XmNfractionBase, 100,
                                          NULL);

    /* menu bar */
    G.menu_bar = XmCreateMenuBar(form, "menu_bar", NULL, 0);
    XtVaSetValues(G.menu_bar,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  NULL);
    XtManageChild(G.menu_bar);

    /* Window menu */
    Widget window_cascade = XtVaCreateManagedWidget("window_cascade",
                                                    xmCascadeButtonWidgetClass, G.menu_bar,
                                                    XmNlabelString, XmStringCreateLocalized("Window"),
                                                    XmNmnemonic, 'W',
                                                    NULL);
    Widget window_menu = XmCreatePulldownMenu(G.menu_bar, "window_menu", NULL, 0);
    XtVaSetValues(window_cascade, XmNsubMenuId, window_menu, NULL);

    Widget newgame_item = XtVaCreateManagedWidget("newgame_item",
                                                  xmPushButtonWidgetClass, window_menu,
                                                  XmNlabelString, XmStringCreateLocalized("New Game..."),
                                                  XmNmnemonic, 'N',
                                                  NULL);
    XtAddCallback(newgame_item, XmNactivateCallback, on_new_game_menu, NULL);

    Widget close_item = XtVaCreateManagedWidget("close_item",
                                                xmPushButtonWidgetClass, window_menu,
                                                XmNlabelString, XmStringCreateLocalized("Close"),
                                                XmNmnemonic, 'C',
                                                XmNaccelerator, "Alt<Key>F4",
                                                XmNacceleratorText, XmStringCreateLocalized("Alt+F4"),
                                                NULL);
    XtAddCallback(close_item, XmNactivateCallback, on_close, NULL);

    /* View menu */
    Widget view_cascade = XtVaCreateManagedWidget("view_cascade",
                                                  xmCascadeButtonWidgetClass, G.menu_bar,
                                                  XmNlabelString, XmStringCreateLocalized("View"),
                                                  XmNmnemonic, 'V',
                                                  NULL);
    Widget view_menu = XmCreatePulldownMenu(G.menu_bar, "view_menu", NULL, 0);
    XtVaSetValues(view_cascade, XmNsubMenuId, view_menu, NULL);

    Widget grid_item = XtVaCreateManagedWidget("grid_item",
                                               xmToggleButtonWidgetClass, view_menu,
                                               XmNlabelString, XmStringCreateLocalized("Show Grid"),
                                               XmNmnemonic, 'G',
                                               XmNset, G.show_grid,
                                               NULL);
    XtAddCallback(grid_item, XmNvalueChangedCallback, on_toggle_grid, NULL);

    /* Help menu */
    Widget help_cascade = XtVaCreateManagedWidget("help_cascade",
                                                  xmCascadeButtonWidgetClass, G.menu_bar,
                                                  XmNlabelString, XmStringCreateLocalized("Help"),
                                                  XmNmnemonic, 'H',
                                                  NULL);
    Widget help_menu = XmCreatePulldownMenu(G.menu_bar, "help_menu", NULL, 0);
    XtVaSetValues(help_cascade, XmNsubMenuId, help_menu, NULL);

    Widget about_item = XtVaCreateManagedWidget("about_item",
                                                xmPushButtonWidgetClass, help_menu,
                                                XmNlabelString, XmStringCreateLocalized("About"),
                                                XmNmnemonic, 'A',
                                                NULL);
    XtAddCallback(about_item, XmNactivateCallback, on_about, NULL);

    XtVaSetValues(G.menu_bar, XmNmenuHelpWidget, help_cascade, NULL);

    /* status bar */
    Widget frame = XtVaCreateManagedWidget("frame",
                                           xmFrameWidgetClass, form,
                                           XmNleftAttachment,   XmATTACH_FORM,
                                           XmNrightAttachment,  XmATTACH_FORM,
                                           XmNbottomAttachment, XmATTACH_FORM,
                                           XmNshadowType,       XmSHADOW_ETCHED_IN,
                                           NULL);

    G.status = XtVaCreateManagedWidget("status",
                                       xmLabelWidgetClass, frame,
                                       XmNalignment,     XmALIGNMENT_BEGINNING,
                                       XmNmarginWidth,   8,
                                       XmNmarginHeight,  4,
                                       XmNrecomputeSize, True,
                                       NULL);

    /* drawing area */
    G.drawing = XtVaCreateManagedWidget("drawing",
                                        xmDrawingAreaWidgetClass, form,
                                        XmNtopAttachment,    XmATTACH_WIDGET,
                                        XmNtopWidget,        G.menu_bar,
                                        XmNleftAttachment,   XmATTACH_FORM,
                                        XmNrightAttachment,  XmATTACH_FORM,
                                        XmNbottomAttachment, XmATTACH_WIDGET,
                                        XmNbottomWidget,     frame,
                                        XmNwidth,            (Dimension)(GRID_W * CELL_PX),
                                        XmNheight,           (Dimension)(GRID_H * CELL_PX),
                                        XmNbackground,       BlackPixelOfScreen(XtScreen(form)),
                                        XmNtraversalOn,      True,
                                        XmNnavigationType,   XmTAB_GROUP,
                                        NULL);

    XtAddCallback(G.drawing, XmNexposeCallback, on_expose, NULL);
    XtAddCallback(G.drawing, XmNresizeCallback, on_expose, NULL);

    XtAddEventHandler(G.drawing, KeyPressMask | KeyReleaseMask, False, on_key, NULL);
    XtAddEventHandler(G.drawing, ButtonPressMask, False, on_click_focus, NULL);
    XtAddEventHandler(G.drawing, FocusChangeMask, False, on_focus_change, NULL);

    XtVaSetValues(G.toplevel,
                  XmNwidth,  (Dimension)(GRID_W * CELL_PX),
                  XmNheight, (Dimension)(GRID_H * CELL_PX + 80),
                  NULL);

    XtRealizeWidget(G.toplevel);

    on_realize(G.drawing, NULL, NULL);

    /*
     * Start the tick loop once (paused state controls actual gameplay updates).
     * Do this regardless of session restore vs fresh start.
     */
    G.timer = XtAppAddTimeOut(G.app, (unsigned long)G.tick_ms, on_tick, NULL);

    /* Defer startup dialog until WM has configured the main window (fresh start only) */
    if (!G.session_restored)
        XtAddEventHandler(G.toplevel, StructureNotifyMask, False, on_toplevel_configure, NULL);

    Atom wm_delete = XmInternAtom(XtDisplay(G.toplevel), "WM_DELETE_WINDOW", False);
    Atom wm_save = XmInternAtom(XtDisplay(G.toplevel), "WM_SAVE_YOURSELF", False);
    XmAddWMProtocolCallback(G.toplevel, wm_delete, cb_wm_delete, NULL);
    XmAddWMProtocolCallback(G.toplevel, wm_save, cb_wm_save, NULL);
    XmActivateWMProtocol(G.toplevel, wm_delete);
    XmActivateWMProtocol(G.toplevel, wm_save);

    XtAppMainLoop(G.app);
    session_data_free(G.session_data);
    return 0;
}
