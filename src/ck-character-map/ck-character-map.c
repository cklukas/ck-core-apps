#include <Xm/Xm.h>
#include <Xm/MainW.h>
#include <Xm/Form.h>
#include <Xm/RowColumn.h>
#include <Xm/Label.h>
#include <Xm/PushB.h>
#include <Xm/TextF.h>
#include <Xm/DrawingA.h>
#include <Xm/ScrolledW.h>
#include <Xm/ComboBox.h>
#include <Xm/List.h>
#include <Xm/CascadeB.h>
#include <Xm/Protocols.h>
#include <Xm/MessageB.h>
#include <Xm/SeparatoG.h>
#include <Xm/CutPaste.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <Dt/Dt.h>
#include <Dt/Session.h>

#include "../shared/about_dialog.h"
#include "../shared/session_utils.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <unistd.h>

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif

typedef struct FontSizeEntry {
    int pixel_size;
    int point_size_deci;
    char *label;
    char *xlfd_name;
} FontSizeEntry;

typedef struct FontFace {
    char *key;
    char *display;

    /* XLFD parts (for scalable fonts / generated sizes). */
    char *foundry;
    char *family;
    char *weight;
    char *slant;
    char *setwidth;
    char *addstyle;
    char *spacing;
    char *registry;
    char *encoding;

    bool is_xlfd;
    bool has_scalable;

    FontSizeEntry *sizes;
    int size_count;
    int size_cap;
} FontFace;

typedef struct AppState {
    XtAppContext app_context;
    Widget toplevel;
    Widget mainw;
    Widget menubar;

    Widget work_form;
    Widget control_form;
    Widget font_combo;
    Widget size_combo;
    Widget update_btn;
    int font_combo_items;
    int size_combo_items;

    Widget scrolled;
    Widget drawing;

    Widget bottom_form;
    Widget text_field;
    Widget copy_btn;

    Widget about_shell;

    Display *dpy;

    SessionData *session_data;
    char exec_path[PATH_MAX];

    FontFace *faces;
    int face_count;
    int face_cap;

    int selected_face;
    int selected_size;

    XFontStruct *font;
    bool font_is_two_byte;
    int font_min_byte1;
    int font_max_byte1;
    int font_min_byte2;
    int font_max_byte2;

    unsigned int *glyphs;
    int glyph_count;

    int cell_w;
    int cell_h;
    int ascent;
    int descent;
    int cols;
    int rows;
    int selected_glyph_index;
    Dimension last_viewport_w;

    XtIntervalId reflow_timer;

    GC gc_bg;
    GC gc_grid;
    GC gc_text;
    GC gc_sel_bg;
    GC gc_sel_text;

    Pixel col_bg;
    Pixel col_fg;
    Pixel col_grid;
    Pixel col_sel_bg;
    Pixel col_sel_fg;
} AppState;

static AppState G;

/* -------------------------------------------------------------------------------------------------
 * Small utilities
 * ------------------------------------------------------------------------------------------------- */

static char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n + 1);
    return d;
}

static void init_exec_path(const char *argv0)
{
    G.exec_path[0] = '\0';

    ssize_t len = readlink("/proc/self/exe", G.exec_path, sizeof(G.exec_path) - 1);
    if (len > 0) {
        G.exec_path[len] = '\0';
        return;
    }

    if (!argv0 || !argv0[0]) {
        snprintf(G.exec_path, sizeof(G.exec_path), "ck-character-map");
        return;
    }

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

static void center_shell_on_screen(Widget toplevel)
{
    Dimension w = 0, h = 0;
    XtVaGetValues(toplevel, XmNwidth, &w, XmNheight, &h, NULL);
    if (w == 0 || h == 0) return;

    Display *dpy = XtDisplay(toplevel);
    int screen = DefaultScreen(dpy);
    int sw = DisplayWidth(dpy, screen);
    int sh = DisplayHeight(dpy, screen);

    Position x = (Position)((sw - (int)w) / 2);
    Position y = (Position)((sh - (int)h) / 2);
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    XtVaSetValues(toplevel, XmNx, x, XmNy, y, NULL);
}

static void query_screen_dpi(Display *dpy, int screen, int *out_x, int *out_y)
{
    int px_w = DisplayWidth(dpy, screen);
    int px_h = DisplayHeight(dpy, screen);
    int mm_w = DisplayWidthMM(dpy, screen);
    int mm_h = DisplayHeightMM(dpy, screen);

    int dpi_x = 96;
    int dpi_y = 96;
    if (mm_w > 0) dpi_x = (int)((double)px_w * 25.4 / (double)mm_w + 0.5);
    if (mm_h > 0) dpi_y = (int)((double)px_h * 25.4 / (double)mm_h + 0.5);
    if (dpi_x <= 0) dpi_x = 96;
    if (dpi_y <= 0) dpi_y = 96;

    if (out_x) *out_x = dpi_x;
    if (out_y) *out_y = dpi_y;
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

static int split_preserve_empty(char *s, char delim, char **out, int max_out)
{
    if (!s || !out || max_out <= 0) return 0;
    int n = 0;
    out[n++] = s;
    for (char *p = s; *p && n < max_out; ++p) {
        if (*p == delim) {
            *p = '\0';
            out[n++] = p + 1;
        }
    }
    return n;
}

/* XLFD: 14 fields after the leading '-', so 15 tokens including the leading empty token. */
typedef struct XlfdParts {
    char *dup;
    char *f[15];
    int pixel_size;
    int point_size_deci;
    int resx;
    int resy;
} XlfdParts;

static bool parse_xlfd(const char *name, XlfdParts *out)
{
    if (!name || !out) return false;
    if (name[0] != '-') return false;

    memset(out, 0, sizeof(*out));
    char *dup = xstrdup(name);
    if (!dup) return false;

    char *fields[15];
    int n = split_preserve_empty(dup, '-', fields, 15);
    if (n != 15) {
        free(dup);
        return false;
    }

    out->dup = dup;
    for (int i = 0; i < 15; ++i) out->f[i] = fields[i];

    out->pixel_size = (fields[7] && fields[7][0]) ? atoi(fields[7]) : 0;
    out->point_size_deci = (fields[8] && fields[8][0]) ? atoi(fields[8]) : 0;
    out->resx = (fields[9] && fields[9][0]) ? atoi(fields[9]) : 0;
    out->resy = (fields[10] && fields[10][0]) ? atoi(fields[10]) : 0;

    return true;
}

static char *make_face_key_from_xlfd(const XlfdParts *x)
{
    if (!x) return NULL;
    const char *foundry = x->f[1] ? x->f[1] : "";
    const char *family = x->f[2] ? x->f[2] : "";
    const char *weight = x->f[3] ? x->f[3] : "";
    const char *slant = x->f[4] ? x->f[4] : "";
    const char *setwidth = x->f[5] ? x->f[5] : "";
    const char *addstyle = x->f[6] ? x->f[6] : "";
    const char *spacing = x->f[11] ? x->f[11] : "";
    const char *registry = x->f[13] ? x->f[13] : "";
    const char *encoding = x->f[14] ? x->f[14] : "";

    size_t need = strlen(foundry) + strlen(family) + strlen(weight) + strlen(slant) +
                  strlen(setwidth) + strlen(addstyle) + strlen(spacing) +
                  strlen(registry) + strlen(encoding) + 32;
    char *k = (char *)malloc(need);
    if (!k) return NULL;
    snprintf(k, need, "%s|%s|%s|%s|%s|%s|%s|%s|%s",
             foundry, family, weight, slant, setwidth, addstyle, spacing, registry, encoding);
    return k;
}

static char *make_face_display_from_xlfd(const XlfdParts *x)
{
    if (!x) return NULL;

    const char *family = x->f[2] ? x->f[2] : "";
    const char *weight = x->f[3] ? x->f[3] : "";
    const char *slant = x->f[4] ? x->f[4] : "";
    const char *registry = x->f[13] ? x->f[13] : "";
    const char *encoding = x->f[14] ? x->f[14] : "";

    char style[64];
    style[0] = '\0';
    if (weight[0] && strcmp(weight, "medium") != 0) {
        snprintf(style, sizeof(style), "%s", weight);
    }
    if (slant[0] && strcmp(slant, "r") != 0) {
        if (style[0]) strncat(style, " ", sizeof(style) - strlen(style) - 1);
        if (strcmp(slant, "i") == 0) strncat(style, "italic", sizeof(style) - strlen(style) - 1);
        else if (strcmp(slant, "o") == 0) strncat(style, "oblique", sizeof(style) - strlen(style) - 1);
        else strncat(style, slant, sizeof(style) - strlen(style) - 1);
    }

    char enc[128];
    enc[0] = '\0';
    if (registry[0] || encoding[0]) {
        snprintf(enc, sizeof(enc), " (%s-%s)", registry, encoding);
    }

    size_t need = strlen(family) + strlen(style) + strlen(enc) + 8;
    char *d = (char *)malloc(need);
    if (!d) return NULL;
    if (style[0]) snprintf(d, need, "%s %s%s", family, style, enc);
    else snprintf(d, need, "%s%s", family, enc);
    return d;
}

static void font_face_free(FontFace *f)
{
    if (!f) return;
    free(f->key);
    free(f->display);
    free(f->foundry);
    free(f->family);
    free(f->weight);
    free(f->slant);
    free(f->setwidth);
    free(f->addstyle);
    free(f->spacing);
    free(f->registry);
    free(f->encoding);
    for (int i = 0; i < f->size_count; ++i) {
        free(f->sizes[i].label);
        free(f->sizes[i].xlfd_name);
    }
    free(f->sizes);
    memset(f, 0, sizeof(*f));
}

static int face_find_by_key(const char *key)
{
    if (!key) return -1;
    for (int i = 0; i < G.face_count; ++i) {
        if (G.faces[i].key && strcmp(G.faces[i].key, key) == 0) return i;
    }
    return -1;
}

static bool face_ensure_capacity(void)
{
    if (G.face_count + 1 <= G.face_cap) return true;
    int new_cap = (G.face_cap == 0) ? 64 : (G.face_cap * 2);
    FontFace *nf = (FontFace *)realloc(G.faces, (size_t)new_cap * sizeof(FontFace));
    if (!nf) return false;
    memset(nf + G.face_cap, 0, (size_t)(new_cap - G.face_cap) * sizeof(FontFace));
    G.faces = nf;
    G.face_cap = new_cap;
    return true;
}

static bool size_ensure_capacity(FontFace *f)
{
    if (!f) return false;
    if (f->size_count + 1 <= f->size_cap) return true;
    int new_cap = (f->size_cap == 0) ? 16 : (f->size_cap * 2);
    FontSizeEntry *ns = (FontSizeEntry *)realloc(f->sizes, (size_t)new_cap * sizeof(FontSizeEntry));
    if (!ns) return false;
    memset(ns + f->size_cap, 0, (size_t)(new_cap - f->size_cap) * sizeof(FontSizeEntry));
    f->sizes = ns;
    f->size_cap = new_cap;
    return true;
}

static char *make_size_label(int pixel, int point_deci)
{
    char buf[64];
    if (point_deci > 0) {
        if (point_deci % 10 == 0) snprintf(buf, sizeof(buf), "%d pt", point_deci / 10);
        else snprintf(buf, sizeof(buf), "%.1f pt", (double)point_deci / 10.0);
    } else if (pixel > 0) {
        snprintf(buf, sizeof(buf), "%d px", pixel);
    } else {
        snprintf(buf, sizeof(buf), "default");
    }
    return xstrdup(buf);
}

static bool face_add_size(FontFace *f, int pixel, int point_deci, const char *xlfd_name)
{
    if (!f || !xlfd_name) return false;

    for (int i = 0; i < f->size_count; ++i) {
        if (f->sizes[i].pixel_size == pixel &&
            f->sizes[i].point_size_deci == point_deci &&
            f->sizes[i].xlfd_name &&
            strcmp(f->sizes[i].xlfd_name, xlfd_name) == 0) {
            return true;
        }
        if (f->sizes[i].pixel_size == pixel &&
            f->sizes[i].point_size_deci == point_deci) {
            return true; /* size already represented */
        }
    }

    if (!size_ensure_capacity(f)) return false;

    FontSizeEntry *e = &f->sizes[f->size_count++];
    e->pixel_size = pixel;
    e->point_size_deci = point_deci;
    e->label = make_size_label(pixel, point_deci);
    e->xlfd_name = xstrdup(xlfd_name);
    return (e->label != NULL && e->xlfd_name != NULL);
}

static char *build_xlfd_name(const FontFace *f,
                             int pixel_size,
                             int point_size_deci,
                             int resx,
                             int resy)
{
    if (!f || !f->is_xlfd) return NULL;
    const char *foundry = f->foundry ? f->foundry : "*";
    const char *family = f->family ? f->family : "*";
    const char *weight = f->weight ? f->weight : "*";
    const char *slant = f->slant ? f->slant : "*";
    const char *setwidth = f->setwidth ? f->setwidth : "*";
    const char *addstyle = f->addstyle ? f->addstyle : "";
    const char *spacing = f->spacing ? f->spacing : "*";
    const char *registry = f->registry ? f->registry : "*";
    const char *encoding = f->encoding ? f->encoding : "*";

    char buf[512];
    snprintf(buf, sizeof(buf),
             "-%s-%s-%s-%s-%s-%s-%d-%d-%d-%d-%s-0-%s-%s",
             foundry, family, weight, slant, setwidth, addstyle,
             pixel_size, point_size_deci, resx, resy, spacing,
             registry, encoding);
    return xstrdup(buf);
}

static void face_generate_standard_sizes(FontFace *f)
{
    if (!f || !f->has_scalable || !f->is_xlfd) return;
    if (f->size_count > 0) return;

    int dpi_x = 96, dpi_y = 96;
    query_screen_dpi(G.dpy, DefaultScreen(G.dpy), &dpi_x, &dpi_y);

    static const int pts[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 28, 32, 36, 48, 72 };
    for (size_t i = 0; i < sizeof(pts)/sizeof(pts[0]); ++i) {
        int pt = pts[i];
        int point_deci = pt * 10;
        int pixel = (int)((double)pt * (double)dpi_y / 72.0 + 0.5);
        if (pixel < 1) pixel = 1;
        char *name = build_xlfd_name(f, pixel, point_deci, dpi_x, dpi_y);
        if (!name) continue;
        (void)face_add_size(f, pixel, point_deci, name);
        free(name);
    }
}

static int cmp_faces(const void *a, const void *b)
{
    const FontFace *fa = (const FontFace *)a;
    const FontFace *fb = (const FontFace *)b;
    const char *da = fa->display ? fa->display : "";
    const char *db = fb->display ? fb->display : "";
    return strcasecmp(da, db);
}

static int cmp_sizes(const void *a, const void *b)
{
    const FontSizeEntry *sa = (const FontSizeEntry *)a;
    const FontSizeEntry *sb = (const FontSizeEntry *)b;
    int pa = sa->point_size_deci > 0 ? sa->point_size_deci : sa->pixel_size * 10;
    int pb = sb->point_size_deci > 0 ? sb->point_size_deci : sb->pixel_size * 10;
    if (pa != pb) return (pa < pb) ? -1 : 1;
    if (sa->pixel_size != sb->pixel_size) return (sa->pixel_size < sb->pixel_size) ? -1 : 1;
    return 0;
}

static void load_font_faces(void)
{
    if (!G.dpy) return;

    int count = 0;
    char **names = XListFonts(G.dpy, "*", 100000, &count);
    if (!names || count <= 0) {
        show_error_dialog("Fonts", "No X11 fonts found via XListFonts().");
        if (names) XFreeFontNames(names);
        return;
    }

    for (int i = 0; i < count; ++i) {
        const char *name = names[i];
        if (!name || !name[0]) continue;

        XlfdParts x;
        if (parse_xlfd(name, &x)) {
            char *key = make_face_key_from_xlfd(&x);
            if (!key) {
                free(x.dup);
                continue;
            }

            int idx = face_find_by_key(key);
            if (idx < 0) {
                if (!face_ensure_capacity()) {
                    free(key);
                    free(x.dup);
                    continue;
                }
                idx = G.face_count++;
                FontFace *f = &G.faces[idx];
                memset(f, 0, sizeof(*f));

                f->is_xlfd = true;
                f->key = key;
                f->display = make_face_display_from_xlfd(&x);
                f->foundry = xstrdup(x.f[1]);
                f->family = xstrdup(x.f[2]);
                f->weight = xstrdup(x.f[3]);
                f->slant = xstrdup(x.f[4]);
                f->setwidth = xstrdup(x.f[5]);
                f->addstyle = xstrdup(x.f[6]);
                f->spacing = xstrdup(x.f[11]);
                f->registry = xstrdup(x.f[13]);
                f->encoding = xstrdup(x.f[14]);
            } else {
                free(key);
            }

            FontFace *f = &G.faces[idx];
            if (x.pixel_size <= 0 || x.point_size_deci <= 0) {
                f->has_scalable = true;
            }
            if (x.pixel_size > 0 || x.point_size_deci > 0) {
                (void)face_add_size(f, x.pixel_size, x.point_size_deci, name);
            }

            free(x.dup);
        } else {
            /* Font alias (e.g. "fixed"). Treat as its own face with a single "default" size. */
            int idx = face_find_by_key(name);
            if (idx < 0) {
                if (!face_ensure_capacity()) continue;
                idx = G.face_count++;
                FontFace *f = &G.faces[idx];
                memset(f, 0, sizeof(*f));
                f->is_xlfd = false;
                f->key = xstrdup(name);
                f->display = xstrdup(name);
                (void)face_add_size(f, 0, 0, name);
            }
        }
    }

    XFreeFontNames(names);

    for (int i = 0; i < G.face_count; ++i) {
        FontFace *f = &G.faces[i];
        face_generate_standard_sizes(f);
        if (f->size_count > 1) {
            qsort(f->sizes, (size_t)f->size_count, sizeof(FontSizeEntry), cmp_sizes);
        }
        if (f->size_count == 0) {
            (void)face_add_size(f, 0, 0, f->display ? f->display : "fixed");
        }
    }

    if (G.face_count > 1) {
        qsort(G.faces, (size_t)G.face_count, sizeof(FontFace), cmp_faces);
    }
}

/* -------------------------------------------------------------------------------------------------
 * Clipboard
 * ------------------------------------------------------------------------------------------------- */

static void clipboard_copy_string(const char *text)
{
    if (!text || !text[0]) return;
    if (!G.toplevel || !XtIsRealized(G.toplevel)) return;

    Display *dpy = XtDisplay(G.toplevel);
    Window win = XtWindow(G.toplevel);
    if (!dpy || win == None) return;

    long item_id = 0;
    XmString label = XmStringCreateLocalized("ck-character-map");
    int status = XmClipboardStartCopy(dpy, win, label, CurrentTime, NULL, NULL, &item_id);
    if (label) XmStringFree(label);
    if (status != ClipboardSuccess) return;

    (void)XmClipboardCopy(dpy, win, item_id, "UTF8_STRING", (XtPointer)text, (int)strlen(text), 0, NULL);
    (void)XmClipboardCopy(dpy, win, item_id, "STRING", (XtPointer)text, (int)strlen(text), 0, NULL);
    XmClipboardEndCopy(dpy, win, item_id);
}

/* -------------------------------------------------------------------------------------------------
 * Glyph grid
 * ------------------------------------------------------------------------------------------------- */

static void free_glyphs(void)
{
    free(G.glyphs);
    G.glyphs = NULL;
    G.glyph_count = 0;
    G.selected_glyph_index = -1;
}

static bool glyph_metrics_empty(const XCharStruct *cs)
{
    if (!cs) return true;
    return (cs->lbearing == 0 &&
            cs->rbearing == 0 &&
            cs->width == 0 &&
            cs->ascent == 0 &&
            cs->descent == 0 &&
            cs->attributes == 0);
}

static const XCharStruct *font_char_struct(const XFontStruct *font, unsigned int code)
{
    if (!font || !font->per_char) return NULL;
    int min_b1 = font->min_byte1;
    int max_b1 = font->max_byte1;
    int min_b2 = font->min_char_or_byte2;
    int max_b2 = font->max_char_or_byte2;

    if (max_b1 == 0) {
        if ((int)code < min_b2 || (int)code > max_b2) return NULL;
        return &font->per_char[(int)code - min_b2];
    }

    unsigned int b1 = (code >> 8) & 0xFFu;
    unsigned int b2 = code & 0xFFu;
    if ((int)b1 < min_b1 || (int)b1 > max_b1) return NULL;
    if ((int)b2 < min_b2 || (int)b2 > max_b2) return NULL;
    int b2_count = max_b2 - min_b2 + 1;
    int idx = ((int)b1 - min_b1) * b2_count + ((int)b2 - min_b2);
    return &font->per_char[idx];
}

static void build_glyph_list_from_font(XFontStruct *font)
{
    free_glyphs();
    if (!font) return;

    bool two_byte = (font->max_byte1 > 0);
    int min_b1 = font->min_byte1;
    int max_b1 = font->max_byte1;
    int min_b2 = font->min_char_or_byte2;
    int max_b2 = font->max_char_or_byte2;

    if (min_b2 > max_b2) return;
    if (two_byte && min_b1 > max_b1) return;

    int total = 0;
    if (!two_byte) {
        total = max_b2 - min_b2 + 1;
    } else {
        int b1_count = max_b1 - min_b1 + 1;
        int b2_count = max_b2 - min_b2 + 1;
        if (b1_count <= 0 || b2_count <= 0) return;
        if (b1_count > 65536 / b2_count) {
            /* Defensive: avoid overflow / absurd allocations. */
            return;
        }
        total = b1_count * b2_count;
    }

    unsigned int *codes = (unsigned int *)calloc((size_t)total, sizeof(unsigned int));
    if (!codes) return;

    int out_n = 0;
    if (font->all_chars_exist || !font->per_char) {
        for (int i = 0; i < total; ++i) {
            if (!two_byte) {
                codes[out_n++] = (unsigned int)(min_b2 + i);
            } else {
                int b2_count = max_b2 - min_b2 + 1;
                int b1_off = i / b2_count;
                int b2_off = i % b2_count;
                unsigned int b1 = (unsigned int)(min_b1 + b1_off);
                unsigned int b2 = (unsigned int)(min_b2 + b2_off);
                codes[out_n++] = (b1 << 8) | b2;
            }
        }
    } else {
        for (int i = 0; i < total; ++i) {
            unsigned int code = 0;
            if (!two_byte) {
                code = (unsigned int)(min_b2 + i);
            } else {
                int b2_count = max_b2 - min_b2 + 1;
                int b1_off = i / b2_count;
                int b2_off = i % b2_count;
                unsigned int b1 = (unsigned int)(min_b1 + b1_off);
                unsigned int b2 = (unsigned int)(min_b2 + b2_off);
                code = (b1 << 8) | b2;
            }

            const XCharStruct *cs = font_char_struct(font, code);
            if (cs && !glyph_metrics_empty(cs)) {
                codes[out_n++] = code;
            }
        }
    }

    if (out_n == 0) {
        /* Fall back: show at least the first range even if metrics look empty. */
        out_n = MIN(total, 256);
        for (int i = 0; i < out_n; ++i) {
            if (!two_byte) codes[i] = (unsigned int)(min_b2 + i);
            else codes[i] = (unsigned int)((min_b1 << 8) | (min_b2 + i));
        }
    }

    unsigned int *shrink = (unsigned int *)realloc(codes, (size_t)out_n * sizeof(unsigned int));
    G.glyphs = shrink ? shrink : codes;
    G.glyph_count = out_n;

    G.font_is_two_byte = two_byte;
    G.font_min_byte1 = min_b1;
    G.font_max_byte1 = max_b1;
    G.font_min_byte2 = min_b2;
    G.font_max_byte2 = max_b2;
}

static void ensure_gcs(void)
{
    if (G.gc_text && G.gc_bg && G.gc_grid && G.gc_sel_bg && G.gc_sel_text) return;
    if (!G.drawing || !XtIsRealized(G.drawing)) return;

    Window win = XtWindow(G.drawing);
    if (win == None) return;

    Colormap cmap = DefaultColormap(G.dpy, DefaultScreen(G.dpy));
    XtVaGetValues(G.drawing, XmNbackground, &G.col_bg, XmNforeground, &G.col_fg, XmNcolormap, &cmap, NULL);

    Pixel top = 0, bottom = 0, select = 0;
    XmGetColors(DefaultScreenOfDisplay(G.dpy), cmap, G.col_bg, &G.col_fg, &top, &bottom, &select);
    G.col_grid = bottom;
    G.col_sel_bg = select;

    Pixel sel_fg = 0, sel_top = 0, sel_bottom = 0, sel_select = 0;
    XmGetColors(DefaultScreenOfDisplay(G.dpy), cmap, G.col_sel_bg, &sel_fg, &sel_top, &sel_bottom, &sel_select);
    G.col_sel_fg = sel_fg;

    XGCValues gcv;
    memset(&gcv, 0, sizeof(gcv));

    gcv.foreground = G.col_bg;
    gcv.background = G.col_bg;
    G.gc_bg = XCreateGC(G.dpy, win, GCForeground | GCBackground, &gcv);

    gcv.foreground = G.col_grid;
    gcv.background = G.col_bg;
    G.gc_grid = XCreateGC(G.dpy, win, GCForeground | GCBackground, &gcv);

    gcv.foreground = G.col_fg;
    gcv.background = G.col_bg;
    G.gc_text = XCreateGC(G.dpy, win, GCForeground | GCBackground, &gcv);

    gcv.foreground = G.col_sel_bg;
    gcv.background = G.col_sel_bg;
    G.gc_sel_bg = XCreateGC(G.dpy, win, GCForeground | GCBackground, &gcv);

    gcv.foreground = G.col_sel_fg;
    gcv.background = G.col_sel_bg;
    G.gc_sel_text = XCreateGC(G.dpy, win, GCForeground | GCBackground, &gcv);
}

static Dimension query_viewport_width(void)
{
    Dimension w = 0;
    Widget clip = NULL;
    XtVaGetValues(G.scrolled, XmNclipWindow, &clip, NULL);
    if (clip) {
        XtVaGetValues(clip, XmNwidth, &w, NULL);
        if (w > 0) return w;
    }
    if (G.scrolled) {
        XtVaGetValues(G.scrolled, XmNwidth, &w, NULL);
        if (w > 0) return w;
    }
    if (G.drawing) {
        XtVaGetValues(G.drawing, XmNwidth, &w, NULL);
        if (w > 0) return w;
    }
    return 400;
}

static void recompute_cell_metrics(void)
{
    if (!G.font) {
        G.cell_w = 24;
        G.cell_h = 24;
        G.ascent = 12;
        G.descent = 4;
        return;
    }

    G.ascent = G.font->ascent;
    G.descent = G.font->descent;
    int content_h = MAX(1, G.ascent + G.descent);
    int pad = MAX(2, content_h / 6);

    int max_w = G.font->max_bounds.width;
    if (max_w < 1) max_w = content_h / 2;
    G.cell_w = MAX(16, max_w + pad * 2 + 2);
    G.cell_h = MAX(16, content_h + pad * 2 + 2);
}

static void recompute_grid_geometry(void)
{
    Dimension viewport_w = query_viewport_width();
    if (viewport_w < 1) viewport_w = 1;

    if (G.cell_w < 1) G.cell_w = 24;
    int cols = (int)(viewport_w / (Dimension)G.cell_w);
    if (cols < 1) cols = 1;
    G.cols = cols;

    if (G.glyph_count > 0) G.rows = (G.glyph_count + cols - 1) / cols;
    else G.rows = 1;

    Dimension draw_w = viewport_w;
    Dimension want_w = (Dimension)(cols * G.cell_w);
    if (want_w > 0 && want_w <= viewport_w) draw_w = want_w;
    if (draw_w < 1) draw_w = 1;

    Dimension draw_h = (Dimension)(MAX(1, G.rows) * G.cell_h);
    Dimension cur_w = 0, cur_h = 0;
    XtVaGetValues(G.drawing, XmNwidth, &cur_w, XmNheight, &cur_h, NULL);
    if (cur_w != draw_w || cur_h != draw_h) {
        XtVaSetValues(G.drawing, XmNwidth, draw_w, XmNheight, draw_h, NULL);
    }
    G.last_viewport_w = viewport_w;
}

static void draw_cell(int index, int x0, int y0)
{
    if (!G.dpy || !XtIsRealized(G.drawing)) return;
    Window win = XtWindow(G.drawing);
    if (win == None) return;

    bool selected = (index == G.selected_glyph_index);
    GC bg_gc = selected ? G.gc_sel_bg : G.gc_bg;
    GC text_gc = selected ? G.gc_sel_text : G.gc_text;

    XFillRectangle(G.dpy, win, bg_gc, x0, y0, (unsigned int)G.cell_w, (unsigned int)G.cell_h);
    XDrawRectangle(G.dpy, win, G.gc_grid, x0, y0, (unsigned int)G.cell_w - 1, (unsigned int)G.cell_h - 1);

    if (!G.font || !G.glyphs || index < 0 || index >= G.glyph_count) return;

    unsigned int code = G.glyphs[index];
    int base_y = y0 + (G.cell_h - (G.ascent + G.descent)) / 2 + G.ascent;

    if (!G.font_is_two_byte) {
        char c = (char)(code & 0xFFu);
        int w = XTextWidth(G.font, &c, 1);
        int x = x0 + (G.cell_w - w) / 2;
        XDrawString(G.dpy, win, text_gc, x, base_y, &c, 1);
    } else {
        XChar2b ch;
        ch.byte1 = (unsigned char)((code >> 8) & 0xFFu);
        ch.byte2 = (unsigned char)(code & 0xFFu);
        int w = XTextWidth16(G.font, &ch, 1);
        int x = x0 + (G.cell_w - w) / 2;
        XDrawString16(G.dpy, win, text_gc, x, base_y, &ch, 1);
    }
}

static void redraw_expose_region(const XExposeEvent *ev)
{
    if (!ev) return;
    if (!G.dpy || !XtIsRealized(G.drawing)) return;
    ensure_gcs();
    if (!G.gc_bg) return;

    Window win = XtWindow(G.drawing);
    if (win == None) return;

    XFillRectangle(G.dpy, win, G.gc_bg, ev->x, ev->y, (unsigned int)ev->width, (unsigned int)ev->height);

    if (!G.glyphs || G.glyph_count <= 0 || G.cols <= 0) {
        const char *msg = "Select a font and size, then press Update.";
        int x = 8;
        int y = 24;
        XDrawString(G.dpy, win, G.gc_text, x, y, msg, (int)strlen(msg));
        return;
    }

    int x1 = ev->x + (int)ev->width - 1;
    int y1 = ev->y + (int)ev->height - 1;

    int col0 = ev->x / G.cell_w;
    int col1 = x1 / G.cell_w;
    int row0 = ev->y / G.cell_h;
    int row1 = y1 / G.cell_h;

    if (col0 < 0) col0 = 0;
    if (row0 < 0) row0 = 0;
    if (col1 >= G.cols) col1 = G.cols - 1;
    if (row1 >= G.rows) row1 = G.rows - 1;

    for (int r = row0; r <= row1; ++r) {
        for (int c = col0; c <= col1; ++c) {
            int idx = r * G.cols + c;
            if (idx >= G.glyph_count) continue;
            draw_cell(idx, c * G.cell_w, r * G.cell_h);
        }
    }
}

static void redraw_all(void)
{
    if (!G.dpy || !G.drawing || !XtIsRealized(G.drawing)) return;
    Window win = XtWindow(G.drawing);
    if (win == None) return;
    XClearArea(G.dpy, win, 0, 0, 0, 0, True);
}

static int utf8_encode(uint32_t cp, char out[8])
{
    if (!out) return 0;
    if (cp <= 0x7Fu) {
        out[0] = (char)cp;
        out[1] = '\0';
        return 1;
    }
    if (cp <= 0x7FFu) {
        out[0] = (char)(0xC0 | ((cp >> 6) & 0x1F));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = '\0';
        return 2;
    }
    if (cp <= 0xFFFFu) {
        out[0] = (char)(0xE0 | ((cp >> 12) & 0x0F));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = '\0';
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        out[0] = (char)(0xF0 | ((cp >> 18) & 0x07));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        out[4] = '\0';
        return 4;
    }
    out[0] = '\0';
    return 0;
}

static void append_glyph_to_text(unsigned int code)
{
    if (!G.text_field) return;

    char glyph[8];
    if (utf8_encode(code, glyph) <= 0) return;

    char *prev = XmTextFieldGetString(G.text_field);
    size_t prev_len = prev ? strlen(prev) : 0;
    size_t add_len = strlen(glyph);
    size_t total = prev_len + add_len + 1;

    char *next = (char *)malloc(total);
    if (!next) {
        if (prev) XtFree(prev);
        return;
    }
    if (prev && prev_len > 0) memcpy(next, prev, prev_len);
    memcpy(next + prev_len, glyph, add_len);
    next[prev_len + add_len] = '\0';

    XmTextFieldSetString(G.text_field, next);
    XmTextFieldSetInsertionPosition(G.text_field, (XmTextPosition)(prev_len + add_len));

    free(next);
    if (prev) XtFree(prev);
}

static void drawing_button_press(Widget w, XtPointer client, XEvent *event, Boolean *cont)
{
    (void)w;
    (void)client;
    (void)cont;
    if (!event || event->type != ButtonPress) return;
    if (!G.glyphs || G.glyph_count <= 0) return;

    XButtonEvent *bev = (XButtonEvent *)event;
    int col = bev->x / G.cell_w;
    int row = bev->y / G.cell_h;
    if (col < 0 || row < 0) return;
    int idx = row * G.cols + col;
    if (idx < 0 || idx >= G.glyph_count) return;

    int old = G.selected_glyph_index;
    G.selected_glyph_index = idx;
    append_glyph_to_text(G.glyphs[idx]);

    if (old >= 0 && old < G.glyph_count) {
        int ox = (old % G.cols) * G.cell_w;
        int oy = (old / G.cols) * G.cell_h;
        draw_cell(old, ox, oy);
    }
    int nx = (idx % G.cols) * G.cell_w;
    int ny = (idx / G.cols) * G.cell_h;
    draw_cell(idx, nx, ny);
}

static void drawing_expose_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    XmDrawingAreaCallbackStruct *cbs = (XmDrawingAreaCallbackStruct *)call;
    if (!cbs || !cbs->event) return;
    if (cbs->event->type != Expose) return;

    XExposeEvent *ev = (XExposeEvent *)cbs->event;
    redraw_expose_region(ev);
}

static void reflow_timeout_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)client_data;
    (void)id;
    G.reflow_timer = 0;
    recompute_grid_geometry();
    redraw_all();
}

static void clip_configure_event(Widget w, XtPointer client, XEvent *event, Boolean *cont)
{
    (void)w;
    (void)client;
    (void)cont;
    if (!event || event->type != ConfigureNotify) return;

    XConfigureEvent *cev = (XConfigureEvent *)event;
    if ((Dimension)cev->width == G.last_viewport_w) return;

    if (G.reflow_timer) return;
    G.reflow_timer = XtAppAddTimeOut(G.app_context, 0, reflow_timeout_cb, NULL);
}

/* -------------------------------------------------------------------------------------------------
 * UI callbacks
 * ------------------------------------------------------------------------------------------------- */

static void on_close(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    (void)call;
    XtAppSetExitFlag(G.app_context);
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
    Widget notebook = about_dialog_build(G.toplevel, "about_dialog", "About Character Map", &shell);
    if (!notebook || !shell) return;

    about_add_standard_pages(notebook, 1,
                             "Character Map",
                             "Character Map",
                             "Browse the glyphs available in X11 fonts.\n"
                             "Select a font and size, press Update, then click characters to add them to the field below.",
                             True);

    XtVaSetValues(shell, XmNwidth, 700, XmNheight, 450, NULL);
    XtAddCallback(shell, XmNdestroyCallback, about_destroy_cb, NULL);

    G.about_shell = shell;
    XtPopup(shell, XtGrabNone);
}

static void on_copy(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    (void)call;
    if (!G.text_field) return;
    char *s = XmTextFieldGetString(G.text_field);
    if (!s) return;
    clipboard_copy_string(s);
    XtFree(s);
}

static void combobox_clear_counted(Widget combo, int *io_count)
{
    if (!combo) return;
    if (!io_count || *io_count <= 0) return;
    while (*io_count > 0) {
        XmComboBoxDeletePos(combo, 1);
        (*io_count)--;
    }
    XmComboBoxUpdate(combo);
}

static void populate_size_combo_for_face(int face_index, int prefer_pixel, int prefer_point_deci)
{
    if (face_index < 0 || face_index >= G.face_count) return;
    FontFace *f = &G.faces[face_index];

    combobox_clear_counted(G.size_combo, &G.size_combo_items);

    for (int i = 0; i < f->size_count; ++i) {
        XmString s = XmStringCreateLocalized(f->sizes[i].label ? f->sizes[i].label : "default");
        XmComboBoxAddItem(G.size_combo, s, i + 1, False);
        G.size_combo_items++;
        XmStringFree(s);
    }
    XmComboBoxUpdate(G.size_combo);

    int sel = 1;
    if (prefer_pixel > 0 || prefer_point_deci > 0) {
        for (int i = 0; i < f->size_count; ++i) {
            if ((prefer_pixel > 0 && f->sizes[i].pixel_size == prefer_pixel) ||
                (prefer_point_deci > 0 && f->sizes[i].point_size_deci == prefer_point_deci)) {
                sel = i + 1;
                break;
            }
        }
    } else {
        /* Heuristic: pick ~12pt if available. */
        for (int i = 0; i < f->size_count; ++i) {
            if (f->sizes[i].point_size_deci == 120) {
                sel = i + 1;
                break;
            }
        }
    }

    XtVaSetValues(G.size_combo, XmNselectedPosition, sel, NULL);
    G.selected_size = sel - 1;
}

static void on_font_selected(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    XmComboBoxCallbackStruct *cbs = (XmComboBoxCallbackStruct *)call;
    if (!cbs) return;
    int pos = (int)cbs->item_position;
    if (pos < 1 || pos > G.face_count) return;
    G.selected_face = pos - 1;
    populate_size_combo_for_face(G.selected_face, 0, 0);
}

static void on_size_selected(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    XmComboBoxCallbackStruct *cbs = (XmComboBoxCallbackStruct *)call;
    if (!cbs) return;
    int pos = (int)cbs->item_position;
    int face = G.selected_face;
    if (face < 0 || face >= G.face_count) return;
    if (pos < 1 || pos > G.faces[face].size_count) return;
    G.selected_size = pos - 1;
}

static void apply_selected_font(void)
{
    if (G.selected_face < 0 || G.selected_face >= G.face_count) return;
    FontFace *f = &G.faces[G.selected_face];
    if (f->size_count <= 0) return;

    int sidx = G.selected_size;
    if (sidx < 0 || sidx >= f->size_count) sidx = 0;
    const char *font_name = f->sizes[sidx].xlfd_name;
    if (!font_name || !font_name[0]) return;

    XFontStruct *xf = XLoadQueryFont(G.dpy, font_name);
    if (!xf) {
        char buf[512];
        snprintf(buf, sizeof(buf), "Unable to load font:\n%s", font_name);
        show_error_dialog("Font Load Error", buf);
        return;
    }

    if (G.font) {
        XFreeFont(G.dpy, G.font);
        G.font = NULL;
    }
    G.font = xf;

    recompute_cell_metrics();
    build_glyph_list_from_font(G.font);

    ensure_gcs();
    if (G.gc_text && G.font) {
        XSetFont(G.dpy, G.gc_text, G.font->fid);
        XSetFont(G.dpy, G.gc_sel_text, G.font->fid);
    }

    recompute_grid_geometry();
    redraw_all();
}

static void on_update(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    (void)call;
    apply_selected_font();
}

/* -------------------------------------------------------------------------------------------------
 * Menus and UI building
 * ------------------------------------------------------------------------------------------------- */

static void create_menu(Widget parent)
{
    Widget menubar = XmCreateMenuBar(parent, "menubar", NULL, 0);

    Widget window_pd = XmCreatePulldownMenu(menubar, "windowPD", NULL, 0);
    XtVaCreateManagedWidget("Window", xmCascadeButtonWidgetClass, menubar, XmNsubMenuId, window_pd, NULL);
    Widget mi_close = XtVaCreateManagedWidget("Close", xmPushButtonWidgetClass, window_pd, NULL);
    XtAddCallback(mi_close, XmNactivateCallback, on_close, NULL);

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
                                      NULL);

    create_menu(G.mainw);

    G.work_form = XtVaCreateManagedWidget("workForm",
                                          xmFormWidgetClass, G.mainw,
                                          XmNfractionBase, 100,
                                          XmNmarginWidth, 8,
                                          XmNmarginHeight, 8,
                                          NULL);

    /* Top control bar */
    G.control_form = XtVaCreateManagedWidget("controlForm",
                                             xmFormWidgetClass, G.work_form,
                                             XmNtopAttachment, XmATTACH_FORM,
                                             XmNleftAttachment, XmATTACH_FORM,
                                             XmNrightAttachment, XmATTACH_FORM,
                                             NULL);

    XmString s_font = XmStringCreateLocalized("Font:");
    Widget font_label = XtVaCreateManagedWidget("fontLabel",
                                                xmLabelWidgetClass, G.control_form,
                                                XmNlabelString, s_font,
                                                XmNtopAttachment, XmATTACH_FORM,
                                                XmNleftAttachment, XmATTACH_FORM,
                                                XmNalignment, XmALIGNMENT_BEGINNING,
                                                XmNmarginTop, 4,
                                                XmNmarginBottom, 4,
                                                NULL);
    XmStringFree(s_font);

    G.font_combo = XtVaCreateManagedWidget("fontCombo",
                                           xmComboBoxWidgetClass, G.control_form,
                                           XmNcomboBoxType, XmDROP_DOWN_LIST,
                                           XmNvisibleItemCount, 14,
                                           XmNtopAttachment, XmATTACH_FORM,
                                           XmNleftAttachment, XmATTACH_WIDGET,
                                           XmNleftWidget, font_label,
                                           XmNrightAttachment, XmATTACH_POSITION,
                                           XmNrightPosition, 65,
                                           XmNmarginWidth, 4,
                                           XmNmarginHeight, 4,
                                           NULL);
    XtAddCallback(G.font_combo, XmNselectionCallback, on_font_selected, NULL);

    XmString s_size = XmStringCreateLocalized("Size:");
    Widget size_label = XtVaCreateManagedWidget("sizeLabel",
                                                xmLabelWidgetClass, G.control_form,
                                                XmNlabelString, s_size,
                                                XmNtopAttachment, XmATTACH_FORM,
                                                XmNleftAttachment, XmATTACH_WIDGET,
                                                XmNleftWidget, G.font_combo,
                                                XmNalignment, XmALIGNMENT_BEGINNING,
                                                XmNmarginTop, 4,
                                                XmNmarginBottom, 4,
                                                XmNmarginLeft, 8,
                                                NULL);
    XmStringFree(s_size);

    G.size_combo = XtVaCreateManagedWidget("sizeCombo",
                                           xmComboBoxWidgetClass, G.control_form,
                                           XmNcomboBoxType, XmDROP_DOWN_LIST,
                                           XmNvisibleItemCount, 12,
                                           XmNtopAttachment, XmATTACH_FORM,
                                           XmNleftAttachment, XmATTACH_WIDGET,
                                           XmNleftWidget, size_label,
                                           XmNrightAttachment, XmATTACH_POSITION,
                                           XmNrightPosition, 85,
                                           XmNmarginWidth, 4,
                                           XmNmarginHeight, 4,
                                           NULL);
    XtAddCallback(G.size_combo, XmNselectionCallback, on_size_selected, NULL);

    XmString s_update = XmStringCreateLocalized("Update");
    G.update_btn = XtVaCreateManagedWidget("updateBtn",
                                           xmPushButtonWidgetClass, G.control_form,
                                           XmNlabelString, s_update,
                                           XmNtopAttachment, XmATTACH_FORM,
                                           XmNrightAttachment, XmATTACH_FORM,
                                           XmNmarginWidth, 10,
                                           XmNmarginHeight, 4,
                                           NULL);
    XmStringFree(s_update);
    XtAddCallback(G.update_btn, XmNactivateCallback, on_update, NULL);

    /* Separator */
    Widget sep = XtVaCreateManagedWidget("sep",
                                         xmSeparatorGadgetClass, G.work_form,
                                         XmNtopAttachment, XmATTACH_WIDGET,
                                         XmNtopWidget, G.control_form,
                                         XmNleftAttachment, XmATTACH_FORM,
                                         XmNrightAttachment, XmATTACH_FORM,
                                         XmNtopOffset, 6,
                                         NULL);

    /* Bottom: text field + Copy button */
    G.bottom_form = XtVaCreateManagedWidget("bottomForm",
                                            xmFormWidgetClass, G.work_form,
                                            XmNleftAttachment, XmATTACH_FORM,
                                            XmNrightAttachment, XmATTACH_FORM,
                                            XmNtopAttachment, XmATTACH_NONE,
                                            XmNbottomAttachment, XmATTACH_FORM,
                                            NULL);

    XmString s_chars = XmStringCreateLocalized("Characters to copy:");
    Widget chars_label = XtVaCreateManagedWidget("charsLabel",
                                                 xmLabelWidgetClass, G.bottom_form,
                                                 XmNlabelString, s_chars,
                                                 XmNtopAttachment, XmATTACH_FORM,
                                                 XmNleftAttachment, XmATTACH_FORM,
                                                 XmNalignment, XmALIGNMENT_BEGINNING,
                                                 XmNmarginBottom, 4,
                                                 NULL);
    XmStringFree(s_chars);

    XmString s_copy = XmStringCreateLocalized("Copy to Clipboard");
    G.copy_btn = XtVaCreateManagedWidget("copyBtn",
                                         xmPushButtonWidgetClass, G.bottom_form,
                                         XmNlabelString, s_copy,
                                         XmNtopAttachment, XmATTACH_WIDGET,
                                         XmNtopWidget, chars_label,
                                         XmNrightAttachment, XmATTACH_FORM,
                                         XmNmarginWidth, 10,
                                         XmNmarginHeight, 4,
                                         NULL);
    XmStringFree(s_copy);
    XtAddCallback(G.copy_btn, XmNactivateCallback, on_copy, NULL);

    G.text_field = XtVaCreateManagedWidget("textField",
                                           xmTextFieldWidgetClass, G.bottom_form,
                                           XmNtopAttachment, XmATTACH_WIDGET,
                                           XmNtopWidget, chars_label,
                                           XmNleftAttachment, XmATTACH_FORM,
                                           XmNrightAttachment, XmATTACH_WIDGET,
                                           XmNrightWidget, G.copy_btn,
                                           XmNrightOffset, 8,
                                           XmNmarginHeight, 4,
                                           NULL);

    /* Scrolled glyph area (fills between separator and bottom form) */
    G.scrolled = XtVaCreateManagedWidget("scrolled",
                                         xmScrolledWindowWidgetClass, G.work_form,
                                         XmNscrollingPolicy, XmAUTOMATIC,
                                         XmNvisualPolicy, XmVARIABLE,
                                         XmNscrollBarDisplayPolicy, XmAS_NEEDED,
                                         XmNtopAttachment, XmATTACH_WIDGET,
                                         XmNtopWidget, sep,
                                         XmNtopOffset, 6,
                                         XmNleftAttachment, XmATTACH_FORM,
                                         XmNrightAttachment, XmATTACH_FORM,
                                         XmNbottomAttachment, XmATTACH_WIDGET,
                                         XmNbottomWidget, G.bottom_form,
                                         XmNbottomOffset, 8,
                                         NULL);

    G.drawing = XtVaCreateManagedWidget("drawing",
                                        xmDrawingAreaWidgetClass, G.scrolled,
                                        XmNwidth, 400,
                                        XmNheight, 300,
                                        XmNresizePolicy, XmRESIZE_NONE,
                                        NULL);

    XtVaSetValues(G.scrolled, XmNworkWindow, G.drawing, NULL);

    XtAddCallback(G.drawing, XmNexposeCallback, drawing_expose_cb, NULL);
    XtAddEventHandler(G.drawing, ButtonPressMask, False, drawing_button_press, NULL);

    XmMainWindowSetAreas(G.mainw, G.menubar, NULL, NULL, NULL, G.work_form);
}

/* -------------------------------------------------------------------------------------------------
 * WM protocols and session management
 * ------------------------------------------------------------------------------------------------- */

static void cb_wm_delete(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    (void)call;
    XtAppSetExitFlag(G.app_context);
}

static void cb_wm_save_yourself(Widget w, XtPointer client, XtPointer call)
{
    (void)client;
    (void)call;
    if (!G.session_data) return;

    session_capture_geometry(w, G.session_data, "x", "y", "w", "h");

    if (G.selected_face >= 0 && G.selected_face < G.face_count) {
        FontFace *f = &G.faces[G.selected_face];
        if (f->key) session_data_set(G.session_data, "font_key", f->key);
        if (G.selected_size >= 0 && G.selected_size < f->size_count) {
            session_data_set_int(G.session_data, "pixel_size", f->sizes[G.selected_size].pixel_size);
            session_data_set_int(G.session_data, "point_deci", f->sizes[G.selected_size].point_size_deci);
        }
    }

    session_save(w, G.session_data, G.exec_path);
}

static void apply_session_selection(void)
{
    int face_idx = -1;
    int prefer_pixel = 0;
    int prefer_point = 0;

    if (G.session_data) {
        const char *key = session_data_get(G.session_data, "font_key");
        if (key && key[0]) face_idx = face_find_by_key(key);
        prefer_pixel = session_data_get_int(G.session_data, "pixel_size", 0);
        prefer_point = session_data_get_int(G.session_data, "point_deci", 0);
    }

    if (face_idx < 0 && G.face_count > 0) {
        /* Prefer something sensible if available. */
        for (int i = 0; i < G.face_count; ++i) {
            const FontFace *f = &G.faces[i];
            if (f->family && strcmp(f->family, "fixed") == 0) {
                face_idx = i;
                break;
            }
        }
        if (face_idx < 0) face_idx = 0;
    }

    if (face_idx >= 0 && face_idx < G.face_count) {
        XtVaSetValues(G.font_combo, XmNselectedPosition, face_idx + 1, NULL);
        G.selected_face = face_idx;
        populate_size_combo_for_face(face_idx, prefer_pixel, prefer_point);
    }
}

static void populate_font_combo(void)
{
    combobox_clear_counted(G.font_combo, &G.font_combo_items);

    for (int i = 0; i < G.face_count; ++i) {
        XmString s = XmStringCreateLocalized(G.faces[i].display ? G.faces[i].display : "font");
        XmComboBoxAddItem(G.font_combo, s, i + 1, False);
        G.font_combo_items++;
        XmStringFree(s);
    }
    XmComboBoxUpdate(G.font_combo);

    apply_session_selection();
}

/* -------------------------------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    memset(&G, 0, sizeof(G));
    G.selected_face = -1;
    G.selected_size = -1;
    G.selected_glyph_index = -1;

    char *session_id = session_parse_argument(&argc, argv);
    G.session_data = session_data_create(session_id);
    free(session_id);
    init_exec_path(argv[0]);

    G.toplevel = XtVaAppInitialize(&G.app_context, "CkCharacterMap", NULL, 0,
                                   &argc, argv, NULL,
                                   XmNtitle, "Character Map",
                                   XmNminWidth, 500,
                                   XmNminHeight, 400,
                                   NULL);
    G.dpy = XtDisplay(G.toplevel);

    DtInitialize(G.dpy, G.toplevel, "CkCharacterMap", "CkCharacterMap");

    build_ui();

    Atom wm_delete = XmInternAtom(G.dpy, "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(G.toplevel, wm_delete, cb_wm_delete, NULL);
    XmActivateWMProtocol(G.toplevel, wm_delete);

    Atom wm_save = XmInternAtom(G.dpy, "WM_SAVE_YOURSELF", False);
    XmAddWMProtocolCallback(G.toplevel, wm_save, cb_wm_save_yourself, NULL);
    XmActivateWMProtocol(G.toplevel, wm_save);

    Boolean restored_geom = False;
    if (G.session_data && session_load(G.toplevel, G.session_data)) {
        restored_geom = session_apply_geometry(G.toplevel, G.session_data, "x", "y", "w", "h");
    }

    load_font_faces();
    populate_font_combo();

    XtRealizeWidget(G.toplevel);
    about_set_window_icon_ck_core(G.toplevel);

    /* Hook clip resize to reflow the grid with window width. */
    Widget clip = NULL;
    XtVaGetValues(G.scrolled, XmNclipWindow, &clip, NULL);
    if (clip) {
        XtAddEventHandler(clip, StructureNotifyMask, False, clip_configure_event, NULL);
    }

    if (!restored_geom) center_shell_on_screen(G.toplevel);

    /* Initial render. */
    ensure_gcs();
    apply_selected_font();

    XtAppMainLoop(G.app_context);

    /* Cleanup (mostly for correctness / tooling; the process is exiting anyway). */
    free_glyphs();
    if (G.font) XFreeFont(G.dpy, G.font);
    if (G.gc_bg) XFreeGC(G.dpy, G.gc_bg);
    if (G.gc_grid) XFreeGC(G.dpy, G.gc_grid);
    if (G.gc_text) XFreeGC(G.dpy, G.gc_text);
    if (G.gc_sel_bg) XFreeGC(G.dpy, G.gc_sel_bg);
    if (G.gc_sel_text) XFreeGC(G.dpy, G.gc_sel_text);

    for (int i = 0; i < G.face_count; ++i) {
        font_face_free(&G.faces[i]);
    }
    free(G.faces);
    session_data_free(G.session_data);

    return 0;
}
