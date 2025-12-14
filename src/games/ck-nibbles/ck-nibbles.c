/*
 * ck-nibbles.c  -  Motif/CDE "Nibbles" (QBasic/DOS-inspired) clone
 *
 * Build:
 *   cc -O2 -o ck-nibbles ck-nibbles.c -lXm -lXt -lX11
 *
 * Run:
 *   ./ck-nibbles            # 1P vs CPU
 *   ./ck-nibbles -2         # 2-player (P2 uses WASD)
 *
 * Keys:
 *   Player 1: Arrow keys
 *   Player 2: W A S D (only in -2 mode)
 *   P: pause
 *   R: restart level
 *   Esc/Q: quit
 */

#include <Xm/Xm.h>
#include <Xm/CascadeB.h>
#include <Xm/DialogS.h>
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
#include <X11/Xlib.h>
#include <X11/keysym.h>

#include "../../shared/about_dialog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif

/* ---------- Retro-ish config ---------- */

#define GRID_W  80
#define GRID_H  50

#define CELL_PX 18  /* base pixel size; window can scale, we compute cell size from widget */
#define TICK_MS 90  /* speed (lower = faster). QBasic feel is around 70-110ms */

#define MAX_LEN (GRID_W*GRID_H)

typedef enum { DIR_UP=0, DIR_RIGHT=1, DIR_DOWN=2, DIR_LEFT=3 } Dir;

typedef struct {
    int x, y;
} Pt;

typedef struct {
    Pt body[MAX_LEN];
    int len;
    Dir dir;
    Dir next_dir;
    int alive;
    unsigned long color; /* pixel */
    int score;
    int grow_pending;
} Snake;

typedef struct {
    int two_player;
    int paused;
    int level;
    int tick_ms;

    int cell;       /* computed cell size in px */
    int offx, offy; /* centering offset */

    Snake s1;
    Snake s2;

    Pt food;
    int has_food;

    /* Grid: 0 empty, 1 wall */
    unsigned char wall[GRID_H][GRID_W];

    /* X / Motif */
    Widget toplevel;
    Widget drawing;
    Widget status;
    Widget menu_bar;
    XtAppContext app;
    Display *dpy;
    Window win;

    char current_status[256];
    int show_grid;

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
} Game;

static Game G;

/* ---------- Utility / RNG ---------- */

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

/* ---------- Level generation (simple retro patterns) ---------- */

static void clear_walls(void) {
    memset(G.wall, 0, sizeof(G.wall));
}

static void add_border(void) {
    int x,y;
    for (x=0;x<GRID_W;x++) {
        G.wall[0][x]=1;
        G.wall[GRID_H-1][x]=1;
    }
    for (y=0;y<GRID_H;y++) {
        G.wall[y][0]=1;
        G.wall[y][GRID_W-1]=1;
    }
}

/* A few classic-ish obstacles. Keep it simple but fun. Scaled to grid size. */
static void build_level(int lvl) {
    clear_walls();
    add_border();

    int x,y;
    int m = (lvl % 6);

    if (m==0) {
        /* only border */
    } else if (m==1) {
        /* center box */
        int box_left = GRID_W * 12 / 40;
        int box_right = GRID_W * 28 / 40;
        int box_top = GRID_H * 8 / 25;
        int box_bottom = GRID_H * 16 / 25;
        int gap_y = GRID_H * 12 / 25;
        for (x=box_left; x<box_right; x++) { G.wall[box_top][x]=1; G.wall[box_bottom][x]=1; }
        for (y=box_top; y<=box_bottom; y++) { G.wall[y][box_left]=1; G.wall[y][box_right-1]=1; }
        /* gaps */
        G.wall[gap_y][box_left]=0; G.wall[gap_y][box_right-1]=0;
    } else if (m==2) {
        /* two vertical pillars */
        int x1 = GRID_W * 13 / 40;
        int x2 = GRID_W * 26 / 40;
        int y_start = GRID_H * 3 / 25;
        int y_end = GRID_H * 22 / 25;
        int skip_y = GRID_H * 12 / 25;
        for (y=y_start; y<=y_end; y++) {
            if (y != skip_y) {
                G.wall[y][x1]=1;
                G.wall[y][x2]=1;
            }
        }
    } else if (m==3) {
        /* zigzag */
        int zig_top = GRID_H * 6 / 25;
        int zig_bottom = GRID_H * 18 / 25;
        int zig_left = GRID_W * 6 / 40;
        int zig_right = GRID_W * 33 / 40;
        for (x=4; x<GRID_W-4; x++) {
            if (x%2==0) G.wall[zig_top][x]=1;
            else        G.wall[zig_bottom][x]=1;
        }
        for (y=8; y<GRID_H-8; y++) {
            if (y%2==0) G.wall[y][zig_left]=1;
            else        G.wall[y][zig_right]=1;
        }
    } else if (m==4) {
        /* cross with gaps */
        int cx = GRID_W/2;
        int cy = GRID_H/2;
        for (x=2;x<GRID_W-2;x++) if (x!=cx) G.wall[cy][x]=1;
        for (y=2;y<GRID_H-2;y++) if (y!=cy) G.wall[y][cx]=1;
        G.wall[cy][cx-4]=0; G.wall[cy][cx+4]=0;
        G.wall[cy-4][cx]=0; G.wall[cy+4][cx]=0;
    } else if (m==5) {
        /* twin boxes */
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
        for (x=box1_left; x<box1_right; x++) { G.wall[box1_top][x]=1; G.wall[box1_bottom][x]=1; }
        for (y=box1_top; y<=box1_bottom; y++) { G.wall[y][box1_left]=1; G.wall[y][box1_right-1]=1; }
        for (x=box2_left; x<box2_right; x++) { G.wall[box2_top][x]=1; G.wall[box2_bottom][x]=1; }
        for (y=box2_top; y<=box2_bottom; y++) { G.wall[y][box2_left]=1; G.wall[y][box2_right-1]=1; }
        /* entrances */
        G.wall[ent1_y][box1_left]=0;  G.wall[ent2_y][box2_right-1]=0;
    }

    /* ensure corners are walls from border */
}

/* ---------- Collision & occupancy ---------- */

static int cell_is_wall(int x, int y) {
    if (!in_bounds(x,y)) return 1;
    return G.wall[y][x] ? 1 : 0;
}

static int cell_in_snake(const Snake *s, int x, int y) {
    int i;
    for (i=0;i<s->len;i++) {
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
    /* fallback: no food (rare) */
    G.has_food=0;
}

/* ---------- Snake init/reset ---------- */

static void init_snake(Snake *s, int x, int y, Dir d, unsigned long col) {
    int i;
    s->len = 5;
    s->dir = d;
    s->next_dir = d;
    s->alive = 1;
    s->color = col;
    s->grow_pending = 0;
    for (i=0;i<s->len;i++) {
        /* body[0] is head */
        s->body[i].x = x - i*(d==DIR_RIGHT ? 1 : d==DIR_LEFT ? -1 : 0);
        s->body[i].y = y - i*(d==DIR_DOWN  ? 1 : d==DIR_UP   ? -1 : 0);
    }
}

static void reset_round(void) {
    build_level(G.level);

    /* start positions chosen to avoid walls, scaled to grid */
    int s1x = GRID_W * 8 / 40;
    int s1y = GRID_H * 12 / 25;
    int s2x = GRID_W * 31 / 40;
    int s2y = GRID_H * 12 / 25;
    init_snake(&G.s1, s1x, s1y, DIR_RIGHT, G.col_s1);
    init_snake(&G.s2, s2x, s2y, DIR_LEFT,  G.col_s2);

    if (!G.two_player) {
        /* CPU starts alive too */
        G.s2.alive = 1;
    }

    place_food();

    /* keep scores, just reset crash state */
}

/* ---------- UI status ---------- */

static void set_status_text(void) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "Level %d   P1:%d   %s:%d   %s   (P pause, R restart, Esc quit)",
             G.level+1,
             G.s1.score,
             (G.two_player ? "P2" : "CPU"),
             G.s2.score,
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
    G.cell = MAX(6, MIN(cw, ch)); /* minimum so it remains playable */
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
    /* background */
    Dimension w,h;
    XtVaGetValues(G.drawing, XmNwidth, &w, XmNheight, &h, NULL);
    XFillRectangle(G.dpy, G.win, G.gc_bg, 0, 0, (unsigned)w, (unsigned)h);

    if (!G.show_grid) return;

    /* draw subtle grid lines */
    int x,y;
    int left = G.offx, top = G.offy;
    int right = G.offx + G.cell*GRID_W;
    int bottom = G.offy + G.cell*GRID_H;

    for (x=0;x<=GRID_W;x++) {
        int px = left + x*G.cell;
        XDrawLine(G.dpy, G.win, G.gc_grid, px, top, px, bottom);
    }
    for (y=0;y<=GRID_H;y++) {
        int py = top + y*G.cell;
        XDrawLine(G.dpy, G.win, G.gc_grid, left, py, right, py);
    }
}

static void draw_walls(void) {
    int x,y;
    for (y=0;y<GRID_H;y++) {
        for (x=0;x<GRID_W;x++) {
            if (G.wall[y][x]) {
                fill_rect_cell(G.gc_wall, x, y, 1);
            }
        }
    }
}

static void draw_food(void) {
    if (!G.has_food) return;
    /* simple retro "dot" */
    fill_rect_cell(G.gc_food, G.food.x, G.food.y, G.cell/4);
}

static void draw_snake(const Snake *s, GC gc) {
    if (!s->alive) return;
    int i;
    for (i=s->len-1;i>=0;i--) {
        int inset = (i==0) ? 2 : 3; /* head slightly larger */
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

/* expose callback */
static void on_expose(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)client; (void)call;
    redraw();
}

/* ---------- Game mechanics ---------- */

static int will_hit(const Snake *s, Pt next, int include_tail) {
    /* include_tail=0 allows moving into the last cell if tail will move away this tick */
    if (!in_bounds(next.x, next.y)) return 1;
    if (cell_is_wall(next.x, next.y)) return 1;

    /* collide with self */
    for (int i=0;i<s->len;i++) {
        if (!include_tail && i==s->len-1) continue;
        if (s->body[i].x==next.x && s->body[i].y==next.y) return 1;
    }

    /* collide with other snake */
    const Snake *o = (s==&G.s1) ? &G.s2 : &G.s1;
    if (o->alive) {
        for (int i=0;i<o->len;i++) {
            /* similarly, allow if other tail moves away? keep simple: treat as solid */
            if (o->body[i].x==next.x && o->body[i].y==next.y) return 1;
        }
    }

    return 0;
}

static void apply_next_dir(Snake *s) {
    if (!dir_opposite(s->dir, s->next_dir)) {
        s->dir = s->next_dir;
    }
}

/* Move snake by one cell; returns 1 if ate food */
static int step_snake(Snake *s) {
    if (!s->alive) return 0;

    apply_next_dir(s);

    Pt head = s->body[0];
    Pt next = step_pt(head, s->dir);

    int include_tail = 1;
    if (s->grow_pending == 0) include_tail = 0; /* tail will move away */

    if (will_hit(s, next, include_tail)) {
        s->alive = 0;
        return 0;
    }

    int ate = (G.has_food && pt_eq(next, G.food));

    /* shift body */
    if (ate) {
        s->grow_pending += 2; /* classic-ish growth */
        s->score += 10;
    }

    int new_len = s->len;
    if (s->grow_pending > 0) {
        if (s->len < MAX_LEN) {
            new_len = s->len + 1;
        }
        s->grow_pending--;
    }

    /* move segments: from tail to head */
    for (int i=new_len-1;i>0;i--) {
        if (i-1 < s->len) s->body[i] = s->body[i-1];
        else s->body[i] = s->body[i-1]; /* safe */
    }
    s->body[0] = next;
    s->len = new_len;

    if (ate) {
        place_food();
    }
    return ate;
}

/* If either dies, reset the round but keep scores; advance level if P1 ate enough. */
static void handle_deaths_and_progress(void) {
    if (!G.s1.alive || !G.s2.alive) {
        /* retro feel: short “crash” penalty */
        if (!G.s1.alive) G.s1.score = MAX(0, G.s1.score - 25);
        if (!G.s2.alive) G.s2.score = MAX(0, G.s2.score - 25);

        /* advance level slowly based on combined progress */
        static int foods_eaten = 0;
        foods_eaten++;
        if (foods_eaten >= 6) {
            foods_eaten = 0;
            G.level = (G.level + 1) % 12;
        }

        reset_round();
    }
}

/* ---------- CPU (P2 AI) ---------- */

/* Very “retro” AI:
 * - prefer direction that reduces Manhattan distance to food
 * - avoid immediate death
 * - slight bias to keep going straight
 */
static Dir cpu_choose_dir(const Snake *cpu) {
    Dir options[4] = { DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT };

    /* scoring: lower is better */
    int bestScore = 1<<30;
    Dir bestDir = cpu->dir;

    for (int k=0;k<4;k++) {
        Dir d = options[k];
        if (dir_opposite(cpu->dir, d)) continue;

        Pt next = step_pt(cpu->body[0], d);
        int include_tail = (cpu->grow_pending>0) ? 1 : 0;

        if (will_hit(cpu, next, include_tail)) continue;

        int dist = 0;
        if (G.has_food) {
            dist = abs(next.x - G.food.x) + abs(next.y - G.food.y);
        }

        int score = dist * 10;

        /* prefer straight direction a little */
        if (d != cpu->dir) score += 6;

        /* prefer not hugging walls too much */
        score += (cell_is_wall(next.x+1,next.y) + cell_is_wall(next.x-1,next.y)
                + cell_is_wall(next.x,next.y+1) + cell_is_wall(next.x,next.y-1)) * 2;

        if (score < bestScore) {
            bestScore = score;
            bestDir = d;
        }
    }

    /* if all options blocked, keep dir (will die) */
    return bestDir;
}

/* ---------- Timer tick ---------- */

static void on_tick(XtPointer client, XtIntervalId *id) {
    (void)client; (void)id;

    if (!G.paused) {
        if (!G.two_player) {
            /* set CPU move */
            G.s2.next_dir = cpu_choose_dir(&G.s2);
        }

        step_snake(&G.s1);
        step_snake(&G.s2);

        handle_deaths_and_progress();
        set_status_text();
        redraw();
    }

    G.timer = XtAppAddTimeOut(G.app, (unsigned long)G.tick_ms, on_tick, NULL);
}

/* ---------- Input handling ---------- */

static void set_dir_for_player(Snake *s, Dir d) {
    /* no immediate 180° reversal */
    if (!dir_opposite(s->dir, d)) {
        s->next_dir = d;
    }
}

static void restart_game(int full_reset_scores) {
    if (full_reset_scores) {
        G.s1.score = 0;
        G.s2.score = 0;
        G.level = 0;
    }
    reset_round();
    G.paused = 0;
    set_status_text();
    redraw();
}

static void on_key(Widget w, XtPointer client, XEvent *ev, Boolean *cont) {
    (void)w; (void)client;
    *cont = True;

    if (ev->type != KeyPress) return;

    KeySym ks;
    char buf[8];
    XLookupString(&ev->xkey, buf, sizeof(buf), &ks, NULL);

    /* global */
    if (ks == XK_Escape || ks == XK_q || ks == XK_Q) {
        exit(0);
    }
    if (ks == XK_p || ks == XK_P) {
        G.paused = !G.paused;
        set_status_text();
        redraw();
        return;
    }
    if (ks == XK_r || ks == XK_R) {
        restart_game(0);
        return;
    }

    /* player 1 arrows */
    if (ks == XK_Up)    { set_dir_for_player(&G.s1, DIR_UP); return; }
    if (ks == XK_Down)  { set_dir_for_player(&G.s1, DIR_DOWN); return; }
    if (ks == XK_Left)  { set_dir_for_player(&G.s1, DIR_LEFT); return; }
    if (ks == XK_Right) { set_dir_for_player(&G.s1, DIR_RIGHT); return; }

    /* player 2 WASD only if two-player mode */
    if (G.two_player) {
        if (ks == XK_w || ks == XK_W) { set_dir_for_player(&G.s2, DIR_UP); return; }
        if (ks == XK_s || ks == XK_S) { set_dir_for_player(&G.s2, DIR_DOWN); return; }
        if (ks == XK_a || ks == XK_A) { set_dir_for_player(&G.s2, DIR_LEFT); return; }
        if (ks == XK_d || ks == XK_D) { set_dir_for_player(&G.s2, DIR_RIGHT); return; }
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

    /* “CDE-ish retro” palette */
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

    /* store in snakes too */
    G.s1.color = G.col_s1;
    G.s2.color = G.col_s2;
}

/* ---------- Realize callback to init GCs ---------- */

static void on_realize(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)client; (void)call;
    setup_graphics();
    restart_game(1);
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

    /* Create a dialog shell */
    Widget dialog_shell = XmCreateDialogShell(G.toplevel, "about_dialog", NULL, 0);
    XtVaSetValues(dialog_shell,
                  XmNtitle, "About Nibbles",
                  XmNdeleteResponse, XmDESTROY,
                  NULL);

    /* Create a form inside the dialog */
    Widget form = XtVaCreateManagedWidget("form",
                                          xmFormWidgetClass, dialog_shell,
                                          XmNmarginWidth,  10,
                                          XmNmarginHeight, 10,
                                          NULL);

    /* Create notebook */
    Widget notebook = XmCreateNotebook(form, "notebook", NULL, 0);
    XtVaSetValues(notebook,
                  XmNtopAttachment,    XmATTACH_FORM,
                  XmNleftAttachment,   XmATTACH_FORM,
                  XmNrightAttachment,  XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  NULL);
    XtManageChild(notebook);

    /* Add standard pages */
    about_add_standard_pages(notebook, 1,
                             "Nibbles",
                             "Nibbles",
                             "Snake Game\n\nControls:\nArrows: Move P1\nWASD: Move P2 (if enabled)\nP: Pause\nR: Restart\nEsc: Quit",
                             False); /* no CK-Core tab */

    /* Add OK button at bottom */
    Widget ok_button = XtVaCreateManagedWidget("ok_button",
                                               xmPushButtonWidgetClass, form,
                                               XmNlabelString, XmStringCreateLocalized("OK"),
                                               XmNbottomAttachment, XmATTACH_FORM,
                                               XmNleftAttachment,   XmATTACH_POSITION,
                                               XmNrightAttachment,  XmATTACH_POSITION,
                                               XmNleftPosition,     40,
                                               XmNrightPosition,    60,
                                               NULL);

    /* Make notebook sit above the OK button */
    XtVaSetValues(notebook,
                  XmNbottomAttachment, XmATTACH_WIDGET,
                  XmNbottomWidget,     ok_button,
                  XmNbottomOffset,     10,
                  NULL);

    /* OK callback to close dialog */
    XtAddCallback(ok_button, XmNactivateCallback,
                  (XtCallbackProc)XtDestroyWidget, (XtPointer)dialog_shell);

    /* Make OK the default button */
    XtVaSetValues(form,
                  XmNdefaultButton, ok_button,
                  NULL);

    /* Set a reasonable initial size (not min) */
    XtVaSetValues(dialog_shell,
                  XmNwidth,  600,
                  XmNheight, 450,
                  NULL);

    /* Manage the *form* (child), not the shell */
    XtManageChild(form);

    /* Pop it up */
    XtPopup(dialog_shell, XtGrabNone);
}

/* ---------- Main ---------- */

static int arg_is(const char *a, const char *b) { return a && b && strcmp(a,b)==0; }

int main(int argc, char **argv) {
    memset(&G, 0, sizeof(G));
    G.current_status[0] = '\0';

    G.two_player = 0;
    for (int i=1;i<argc;i++) {
        if (arg_is(argv[i], "-2") || arg_is(argv[i], "--2") || arg_is(argv[i], "--two")) {
            G.two_player = 1;
        }
    }

    srand((unsigned)time(NULL));

    G.level = 0;
    G.tick_ms = TICK_MS;
    G.paused = 0;
    G.show_grid = 0;

    XtSetLanguageProc(NULL, NULL, NULL);

    G.toplevel = XtVaAppInitialize(&G.app, "CkNibbles",
                                  NULL, 0, &argc, argv,
                                  NULL,
                                  XmNtitle, "Nibbles",
                                  XmNiconName, "Nibbles",
                                  NULL);

    Widget form = XtVaCreateManagedWidget("form",
                                          xmFormWidgetClass, G.toplevel,
                                          XmNfractionBase, 100,
                                          NULL);

    /* menu bar */
    G.menu_bar = XmCreateMenuBar(form, "menu_bar", NULL, 0);
    XtVaSetValues(G.menu_bar, XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM, NULL);
    XtManageChild(G.menu_bar);

    /* Window menu */
    Widget window_cascade = XtVaCreateManagedWidget("window_cascade",
                                                    xmCascadeButtonWidgetClass, G.menu_bar,
                                                    XmNlabelString, XmStringCreateLocalized("Window"),
                                                    XmNmnemonic, 'W', NULL);
    Widget window_menu = XmCreatePulldownMenu(G.menu_bar, "window_menu", NULL, 0);
    XtVaSetValues(window_cascade, XmNsubMenuId, window_menu, NULL);
    Widget close_item = XtVaCreateManagedWidget("close_item",
                                                xmPushButtonWidgetClass, window_menu,
                                                XmNlabelString, XmStringCreateLocalized("Close"),
                                                XmNmnemonic, 'C',
                                                XmNaccelerator, "Alt<Key>F4",
                                                XmNacceleratorText, XmStringCreateLocalized("Alt+F4"), NULL);
    XtAddCallback(close_item, XmNactivateCallback, on_close, NULL);

    /* View menu */
    Widget view_cascade = XtVaCreateManagedWidget("view_cascade",
                                                  xmCascadeButtonWidgetClass, G.menu_bar,
                                                  XmNlabelString, XmStringCreateLocalized("View"),
                                                  XmNmnemonic, 'V', NULL);
    Widget view_menu = XmCreatePulldownMenu(G.menu_bar, "view_menu", NULL, 0);
    XtVaSetValues(view_cascade, XmNsubMenuId, view_menu, NULL);
    Widget grid_item = XtVaCreateManagedWidget("grid_item",
                                               xmToggleButtonWidgetClass, view_menu,
                                               XmNlabelString, XmStringCreateLocalized("Show Grid"),
                                               XmNmnemonic, 'G',
                                               XmNset, G.show_grid, NULL);
    XtAddCallback(grid_item, XmNvalueChangedCallback, on_toggle_grid, NULL);

    /* Help menu */
    Widget help_cascade = XtVaCreateManagedWidget("help_cascade",
                                                  xmCascadeButtonWidgetClass, G.menu_bar,
                                                  XmNlabelString, XmStringCreateLocalized("Help"),
                                                  XmNmnemonic, 'H', NULL);
    Widget help_menu = XmCreatePulldownMenu(G.menu_bar, "help_menu", NULL, 0);
    XtVaSetValues(help_cascade, XmNsubMenuId, help_menu, NULL);
    Widget about_item = XtVaCreateManagedWidget("about_item",
                                                xmPushButtonWidgetClass, help_menu,
                                                XmNlabelString, XmStringCreateLocalized("About"),
                                                XmNmnemonic, 'A', NULL);
    XtAddCallback(about_item, XmNactivateCallback, on_about, NULL);

    /* Set help menu as right-aligned (CDE standard) */
    XtVaSetValues(G.menu_bar, XmNmenuHelpWidget, help_cascade, NULL);

    /* status bar (natural height) */
    Widget frame = XtVaCreateManagedWidget("frame",
                                           xmFrameWidgetClass, form,
                                           XmNleftAttachment,   XmATTACH_FORM,
                                           XmNrightAttachment,  XmATTACH_FORM,
                                           XmNbottomAttachment, XmATTACH_FORM,
                                           /* optional: make it look like a status line */
                                           XmNshadowType,       XmSHADOW_ETCHED_IN,
                                           NULL);

    G.status = XtVaCreateManagedWidget("status",
                                       xmLabelWidgetClass, frame,
                                       XmNalignment,     XmALIGNMENT_BEGINNING,
                                       XmNmarginWidth,   8,
                                       XmNmarginHeight,  4,
                                       XmNrecomputeSize, True,   /* keep natural */
                                       NULL);

    /* drawing area fills remaining space above status bar */
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
                                        NULL);

    XtAddCallback(G.drawing, XmNexposeCallback, on_expose, NULL);
    XtAddCallback(G.drawing, XmNresizeCallback, on_expose, NULL);

    /* key handling: attach event handler */
    XtAddEventHandler(G.toplevel, KeyPressMask, False, on_key, NULL);

    /* Request a sensible initial size */
    XtVaSetValues(G.toplevel,
                  XmNwidth,  (Dimension)(GRID_W * CELL_PX),
                  XmNheight, (Dimension)(GRID_H * CELL_PX + 80), /* + menu+status headroom */
                  NULL);

    XtRealizeWidget(G.toplevel);

    /* Need realized window before we can create GCs */
    on_realize(G.drawing, NULL, NULL);

    set_status_text();

    /* start timer loop */
    G.timer = XtAppAddTimeOut(G.app, (unsigned long)G.tick_ms, on_tick, NULL);

    XtAppMainLoop(G.app);
    return 0;
}
