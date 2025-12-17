#include <Xm/Xm.h>
#include <Xm/MainW.h>
#include <Xm/Form.h>
#include <Xm/RowColumn.h>
#include <Xm/Label.h>
#include <Xm/PushB.h>
#include <Xm/ToggleB.h>
#include <Xm/TextF.h>
#include <Xm/DrawingA.h>
#include <Xm/ScrolledW.h>
#include <Xm/ComboBox.h>
#include <Xm/List.h>
#include <Xm/CascadeB.h>
#include <Xm/Protocols.h>
#include <Xm/MessageB.h>
#include <Xm/SelectioB.h>
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
#include <sys/types.h>
#include <unistd.h>

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif

#define DEFAULT_SAMPLE_TEXT "The quick brown fox jumps over the lazy dog 1234567890"

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

typedef struct FontEncoding {
    char *key;       /* e.g. "iso10646-1" */
    char *display;   /* user-visible */
    int *face_indices;
    int face_count;
    int face_cap;
} FontEncoding;

typedef struct FontGroup {
    char *key;       /* e.g. "misc|fixed" or "alias|fixed" */
    char *display;   /* e.g. "fixed" or "fixed (misc)" */
    char *foundry;
    char *family;
    bool is_alias;

    FontEncoding *encodings;
    int enc_count;
    int enc_cap;
} FontGroup;

typedef struct AppState {
    XtAppContext app_context;
    Widget toplevel;
    Widget mainw;
    Widget menubar;

    Widget work_form;
    Widget control_form;
    Widget group_combo;
    Widget encoding_combo;
    Widget bold_toggle;
    Widget italic_toggle;
    Widget size_combo;
    int group_combo_items;
    int encoding_combo_items;
    int size_combo_items;
    bool updating_controls;

    Widget font_file_label;
    Widget sample_preview;
    XtIntervalId sample_reflow_timer;
    Dimension last_sample_w;
    char *sample_text;
    char **sample_lines;
    int sample_line_count;
    int sample_line_cap;
    Widget sample_dialog;
    Widget selected_char_label;
    XtIntervalId apply_timer;

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

    FontGroup *groups;
    int group_count;
    int group_cap;

    int selected_group;
    int selected_encoding;
    bool want_bold;
    bool want_italic;

    int selected_face; /* index into faces (resolved from group+encoding+style) */
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

static void update_selected_char_label_none(void);
static void update_selected_char_label_code(unsigned int code);
static void sample_preview_update_and_draw(void);
static void apply_selected_font(void);
static void schedule_apply_selected_font(void);

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

static bool str_contains_ci(const char *s, const char *sub);

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

typedef struct AliasFontParts {
    char *base;
    char *weight;
    char *slant;
    int point_size;
} AliasFontParts;

static void alias_font_parts_free(AliasFontParts *p)
{
    if (!p) return;
    free(p->base);
    free(p->weight);
    free(p->slant);
    memset(p, 0, sizeof(*p));
}

static bool token_all_digits(const char *s)
{
    if (!s || !s[0]) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (!isdigit(*p)) return false;
    }
    return true;
}

static bool token_all_alpha(const char *s)
{
    if (!s || !s[0]) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (!isalpha(*p)) return false;
    }
    return true;
}

typedef struct AliasStyleFlags {
    bool bold;
    bool demi;
    bool black;
    bool italic;
    bool oblique;
    bool regular;
} AliasStyleFlags;

static bool alias_style_token_flags(const char *t, AliasStyleFlags *f)
{
    if (!t || !t[0]) return false;
    if (!f) return false;
    if (!token_all_alpha(t)) return false;

    bool is_style = false;
    if (str_contains_ci(t, "italic")) { f->italic = true; is_style = true; }
    if (str_contains_ci(t, "oblique")) { f->oblique = true; is_style = true; }
    if (str_contains_ci(t, "bold")) { f->bold = true; is_style = true; }
    if (str_contains_ci(t, "demi")) { f->demi = true; is_style = true; }
    if (str_contains_ci(t, "black")) { f->black = true; is_style = true; }

    if (strcasecmp(t, "roman") == 0 ||
        strcasecmp(t, "regular") == 0 ||
        strcasecmp(t, "medium") == 0 ||
        strcasecmp(t, "book") == 0 ||
        strcasecmp(t, "normal") == 0) {
        is_style = true;
        f->regular = true;
    }

    if (!is_style) return false;

    return true;
}

static char *join_tokens(char **tokens, int count, char sep)
{
    if (!tokens || count <= 0) return NULL;

    size_t need = 1;
    int real = 0;
    for (int i = 0; i < count; ++i) {
        if (!tokens[i] || !tokens[i][0]) continue;
        need += strlen(tokens[i]) + 1;
        real++;
    }
    if (real == 0) return NULL;

    char *out = (char *)malloc(need);
    if (!out) return NULL;
    out[0] = '\0';

    bool first = true;
    for (int i = 0; i < count; ++i) {
        const char *t = tokens[i];
        if (!t || !t[0]) continue;
        if (!first) {
            size_t len = strlen(out);
            out[len] = sep;
            out[len + 1] = '\0';
        }
        strncat(out, t, need - strlen(out) - 1);
        first = false;
    }
    return out;
}

static bool parse_alias_font_name(const char *name, AliasFontParts *out)
{
    if (!name || !name[0] || !out) return false;
    memset(out, 0, sizeof(*out));

    char *dup = xstrdup(name);
    if (!dup) return false;

    char *tokens[32];
    int n = split_preserve_empty(dup, '-', tokens, 32);
    if (n <= 0) {
        free(dup);
        return false;
    }

    int pt = 0;
    if (n >= 2 && token_all_digits(tokens[n - 1])) {
        int v = atoi(tokens[n - 1]);
        if (v >= 4 && v <= 200) {
            pt = v;
            n--;
        }
    }

    AliasStyleFlags flags;
    memset(&flags, 0, sizeof(flags));
    char *base_tokens[32];
    int base_n = 0;
    for (int i = 0; i < n; ++i) {
        if (alias_style_token_flags(tokens[i], &flags)) continue;
        base_tokens[base_n++] = tokens[i];
    }

    const char *weight_s = "medium";
    if (flags.black) weight_s = "black";
    else if (flags.demi && flags.bold) weight_s = "demibold";
    else if (flags.demi) weight_s = "demi";
    else if (flags.bold) weight_s = "bold";
    else if (flags.regular) weight_s = "medium";

    char slant_c = 'r';
    if (flags.italic) slant_c = 'i';
    else if (flags.oblique) slant_c = 'o';

    char *weight = xstrdup(weight_s);
    if (!weight) {
        free(dup);
        return false;
    }

    char *base = (base_n > 0) ? join_tokens(base_tokens, base_n, '-') : join_tokens(tokens, n, '-');
    if (!base) {
        free(weight);
        free(dup);
        return false;
    }

    out->base = base;
    out->weight = weight;
    out->slant = (char *)malloc(2);
    if (!out->slant) {
        alias_font_parts_free(out);
        free(dup);
        return false;
    }
    out->slant[0] = slant_c;
    out->slant[1] = '\0';
    out->point_size = pt;

    free(dup);
    return true;
}

static char *make_alias_face_key(const AliasFontParts *p)
{
    if (!p || !p->base) return NULL;
    const char *w = (p->weight && p->weight[0]) ? p->weight : "medium";
    const char *s = (p->slant && p->slant[0]) ? p->slant : "r";
    size_t need = strlen("aliasface|") + strlen(p->base) + strlen(w) + strlen(s) + 4;
    char *k = (char *)malloc(need);
    if (!k) return NULL;
    snprintf(k, need, "aliasface|%s|%s|%s", p->base, w, s);
    return k;
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

static void font_encoding_free(FontEncoding *e)
{
    if (!e) return;
    free(e->key);
    free(e->display);
    free(e->face_indices);
    memset(e, 0, sizeof(*e));
}

static void font_group_free(FontGroup *g)
{
    if (!g) return;
    free(g->key);
    free(g->display);
    free(g->foundry);
    free(g->family);
    for (int i = 0; i < g->enc_count; ++i) {
        font_encoding_free(&g->encodings[i]);
    }
    free(g->encodings);
    memset(g, 0, sizeof(*g));
}

static void free_font_groups(void)
{
    for (int i = 0; i < G.group_count; ++i) {
        font_group_free(&G.groups[i]);
    }
    free(G.groups);
    G.groups = NULL;
    G.group_count = 0;
    G.group_cap = 0;
}

static bool group_ensure_capacity(void)
{
    if (G.group_count + 1 <= G.group_cap) return true;
    int new_cap = (G.group_cap == 0) ? 64 : (G.group_cap * 2);
    FontGroup *ng = (FontGroup *)realloc(G.groups, (size_t)new_cap * sizeof(FontGroup));
    if (!ng) return false;
    memset(ng + G.group_cap, 0, (size_t)(new_cap - G.group_cap) * sizeof(FontGroup));
    G.groups = ng;
    G.group_cap = new_cap;
    return true;
}

static bool encoding_ensure_capacity(FontGroup *g)
{
    if (!g) return false;
    if (g->enc_count + 1 <= g->enc_cap) return true;
    int new_cap = (g->enc_cap == 0) ? 8 : (g->enc_cap * 2);
    FontEncoding *ne = (FontEncoding *)realloc(g->encodings, (size_t)new_cap * sizeof(FontEncoding));
    if (!ne) return false;
    memset(ne + g->enc_cap, 0, (size_t)(new_cap - g->enc_cap) * sizeof(FontEncoding));
    g->encodings = ne;
    g->enc_cap = new_cap;
    return true;
}

static bool encoding_faces_ensure_capacity(FontEncoding *e)
{
    if (!e) return false;
    if (e->face_count + 1 <= e->face_cap) return true;
    int new_cap = (e->face_cap == 0) ? 32 : (e->face_cap * 2);
    int *nf = (int *)realloc(e->face_indices, (size_t)new_cap * sizeof(int));
    if (!nf) return false;
    e->face_indices = nf;
    e->face_cap = new_cap;
    return true;
}

static int group_find_by_key(const char *key)
{
    if (!key) return -1;
    for (int i = 0; i < G.group_count; ++i) {
        if (G.groups[i].key && strcmp(G.groups[i].key, key) == 0) return i;
    }
    return -1;
}

static int encoding_find_by_key(const FontGroup *g, const char *key)
{
    if (!g || !key) return -1;
    for (int i = 0; i < g->enc_count; ++i) {
        if (g->encodings[i].key && strcmp(g->encodings[i].key, key) == 0) return i;
    }
    return -1;
}

static char *make_group_key_for_face(const FontFace *f)
{
    if (!f) return NULL;
    if (!f->is_xlfd) {
        const char *name = f->display ? f->display : (f->key ? f->key : "font");
        size_t need = strlen("alias|") + strlen(name) + 1;
        char *k = (char *)malloc(need);
        if (!k) return NULL;
        snprintf(k, need, "alias|%s", name);
        return k;
    }

    const char *foundry = f->foundry ? f->foundry : "*";
    const char *family = f->family ? f->family : "*";
    size_t need = strlen(foundry) + strlen(family) + 2;
    char *k = (char *)malloc(need);
    if (!k) return NULL;
    snprintf(k, need, "%s|%s", foundry, family);
    return k;
}

static char *make_encoding_key_for_face(const FontFace *f)
{
    if (!f) return NULL;
    if (!f->is_xlfd) return xstrdup("default");
    const char *reg = f->registry ? f->registry : "*";
    const char *enc = f->encoding ? f->encoding : "*";
    size_t need = strlen(reg) + strlen(enc) + 2;
    char *k = (char *)malloc(need);
    if (!k) return NULL;
    snprintf(k, need, "%s-%s", reg, enc);
    return k;
}

static int cmp_groups(const void *a, const void *b)
{
    const FontGroup *ga = (const FontGroup *)a;
    const FontGroup *gb = (const FontGroup *)b;
    const char *da = ga->display ? ga->display : "";
    const char *db = gb->display ? gb->display : "";
    return strcasecmp(da, db);
}

static int cmp_encodings(const void *a, const void *b)
{
    const FontEncoding *ea = (const FontEncoding *)a;
    const FontEncoding *eb = (const FontEncoding *)b;
    const char *ka = ea->key ? ea->key : "";
    const char *kb = eb->key ? eb->key : "";

    if (strcmp(ka, "default") == 0 && strcmp(kb, "default") != 0) return -1;
    if (strcmp(ka, "default") != 0 && strcmp(kb, "default") == 0) return 1;
    return strcasecmp(ka, kb);
}

static void build_font_groups(void)
{
    free_font_groups();

    for (int i = 0; i < G.face_count; ++i) {
        FontFace *f = &G.faces[i];

        char *gkey = make_group_key_for_face(f);
        if (!gkey) continue;
        int gidx = group_find_by_key(gkey);
        if (gidx < 0) {
            if (!group_ensure_capacity()) {
                free(gkey);
                continue;
            }
            gidx = G.group_count++;
            FontGroup *g = &G.groups[gidx];
            memset(g, 0, sizeof(*g));
            g->key = gkey;
            g->is_alias = !f->is_xlfd;
            g->foundry = f->is_xlfd ? xstrdup(f->foundry ? f->foundry : "*") : NULL;
            g->family = f->is_xlfd ? xstrdup(f->family ? f->family : "font")
                                   : xstrdup(f->display ? f->display : (f->key ? f->key : "font"));
        } else {
            free(gkey);
        }

        FontGroup *g = &G.groups[gidx];

        char *ekey = make_encoding_key_for_face(f);
        if (!ekey) continue;
        int eidx = encoding_find_by_key(g, ekey);
        if (eidx < 0) {
            if (!encoding_ensure_capacity(g)) {
                free(ekey);
                continue;
            }
            eidx = g->enc_count++;
            FontEncoding *e = &g->encodings[eidx];
            memset(e, 0, sizeof(*e));
            e->key = ekey;
            if (strcmp(ekey, "default") == 0) {
                e->display = xstrdup("(default)");
            } else {
                e->display = xstrdup(ekey);
            }
        } else {
            free(ekey);
        }

        FontEncoding *e = &g->encodings[eidx];
        if (!encoding_faces_ensure_capacity(e)) continue;
        e->face_indices[e->face_count++] = i;
    }

    /* Compute display names: show foundry only if needed to disambiguate. */
    for (int i = 0; i < G.group_count; ++i) {
        FontGroup *g = &G.groups[i];
        free(g->display);
        g->display = NULL;
        if (g->is_alias) {
            g->display = xstrdup(g->family ? g->family : "font");
            continue;
        }

        bool dup_family = false;
        for (int j = 0; j < G.group_count; ++j) {
            if (i == j) continue;
            FontGroup *h = &G.groups[j];
            if (h->is_alias) continue;
            if (!g->family || !h->family) continue;
            if (strcmp(g->family, h->family) == 0) {
                dup_family = true;
                break;
            }
        }

        const char *fam = g->family ? g->family : "font";
        const char *fnd = g->foundry ? g->foundry : "*";
        if (dup_family) {
            size_t need = strlen(fam) + strlen(fnd) + 4;
            g->display = (char *)malloc(need);
            if (g->display) snprintf(g->display, need, "%s (%s)", fam, fnd);
        } else {
            g->display = xstrdup(fam);
        }
    }

    /* Sort encodings inside each group for stable UI. */
    for (int i = 0; i < G.group_count; ++i) {
        FontGroup *g = &G.groups[i];
        if (g->enc_count > 1) {
            qsort(g->encodings, (size_t)g->enc_count, sizeof(FontEncoding), cmp_encodings);
        }
    }

    if (G.group_count > 1) {
        qsort(G.groups, (size_t)G.group_count, sizeof(FontGroup), cmp_groups);
    }
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
            /* Font alias (e.g. "fixed", "lucidasans-italic-12"). */
            AliasFontParts ap;
            if (!parse_alias_font_name(name, &ap)) {
                continue;
            }

            char *key = make_alias_face_key(&ap);
            if (!key) {
                alias_font_parts_free(&ap);
                continue;
            }

            int idx = face_find_by_key(key);
            if (idx < 0) {
                if (!face_ensure_capacity()) {
                    free(key);
                    alias_font_parts_free(&ap);
                    continue;
                }
                idx = G.face_count++;
                FontFace *f = &G.faces[idx];
                memset(f, 0, sizeof(*f));
                f->is_xlfd = false;
                f->key = key;
                f->display = xstrdup(ap.base);
                f->family = xstrdup(ap.base);
                f->weight = xstrdup(ap.weight);
                f->slant = xstrdup(ap.slant);
            } else {
                free(key);
            }

            FontFace *f = &G.faces[idx];
            int pt_deci = (ap.point_size > 0) ? (ap.point_size * 10) : 0;
            (void)face_add_size(f, 0, pt_deci, name);

            alias_font_parts_free(&ap);
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

    build_font_groups();
}

/* -------------------------------------------------------------------------------------------------
 * Font file lookup
 * ------------------------------------------------------------------------------------------------- */

static char *xfont_get_property_string(Display *dpy, const XFontStruct *font, const char *prop_name)
{
    if (!dpy || !font || !prop_name || !prop_name[0]) return NULL;

    Atom a = XInternAtom(dpy, prop_name, False);
    if (a == None) return NULL;

    for (int i = 0; i < font->n_properties; ++i) {
        if (font->properties[i].name != a) continue;
        char *name = XGetAtomName(dpy, (Atom)font->properties[i].card32);
        if (!name) return NULL;
        char *out = xstrdup(name);
        XFree(name);
        return out;
    }
    return NULL;
}

static bool xfont_get_property_card32(Display *dpy, const XFontStruct *font, const char *prop_name, unsigned long *out_val)
{
    if (out_val) *out_val = 0;
    if (!dpy || !font || !prop_name || !prop_name[0] || !out_val) return false;

    Atom a = XInternAtom(dpy, prop_name, False);
    if (a == None) return false;

    for (int i = 0; i < font->n_properties; ++i) {
        if (font->properties[i].name != a) continue;
        *out_val = font->properties[i].card32;
        return true;
    }
    return false;
}

static char *skip_ws(char *s)
{
    if (!s) return NULL;
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static void rstrip_ws(char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[n - 1] = '\0';
        n--;
    }
}

static void rstrip_newline(char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

static bool parse_fonts_dir_line(char *line, char **out_file, char **out_name)
{
    if (out_file) *out_file = NULL;
    if (out_name) *out_name = NULL;
    if (!line || !out_file || !out_name) return false;

    rstrip_newline(line);
    char *p = skip_ws(line);
    if (!p || !p[0]) return false;

    char *file = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    if (!*p) return false;
    *p++ = '\0';

    p = skip_ws(p);
    if (!p || !p[0]) return false;
    rstrip_ws(p);

    *out_file = file;
    *out_name = p;
    return true;
}

static char *join_dir_file(const char *dir, const char *file)
{
    if (!dir || !dir[0] || !file || !file[0]) return NULL;
    if (file[0] == '/') return xstrdup(file);

    size_t dlen = strlen(dir);
    bool slash = (dir[dlen - 1] == '/');
    size_t need = dlen + strlen(file) + (slash ? 1 : 2);
    char *out = (char *)malloc(need);
    if (!out) return NULL;
    if (slash) snprintf(out, need, "%s%s", dir, file);
    else snprintf(out, need, "%s/%s", dir, file);
    return out;
}

static char *font_path_entry_dir(const char *entry)
{
    if (!entry || entry[0] != '/') return NULL;

    const char *colon = strchr(entry, ':');
    size_t len = colon ? (size_t)(colon - entry) : strlen(entry);
    while (len > 1 && entry[len - 1] == '/') len--;

    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, entry, len);
    out[len] = '\0';
    return out;
}

static char *find_font_file_in_dir_index(const char *dir, const char *index_name, const char *font_name)
{
    if (!dir || !dir[0] || !index_name || !index_name[0] || !font_name || !font_name[0]) return NULL;
    if (dir[0] != '/') return NULL;

    char path[PATH_MAX];
    size_t dlen = strlen(dir);
    bool slash = (dir[dlen - 1] == '/');
    snprintf(path, sizeof(path), "%s%s%s", dir, slash ? "" : "/", index_name);

    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    char line[4096];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return NULL;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *file = NULL;
        char *name = NULL;
        if (!parse_fonts_dir_line(line, &file, &name)) continue;
        if (strcmp(name, font_name) != 0) continue;
        fclose(fp);
        return join_dir_file(dir, file);
    }

    fclose(fp);
    return NULL;
}

static char *find_font_file_in_dir(const char *dir, const char *font_name)
{
    char *p = find_font_file_in_dir_index(dir, "fonts.dir", font_name);
    if (p) return p;
    return find_font_file_in_dir_index(dir, "fonts.scale", font_name);
}

static char *parse_quoted_or_token(char **io_p)
{
    if (!io_p || !*io_p) return NULL;
    char *p = skip_ws(*io_p);
    if (!p || !p[0]) {
        *io_p = p;
        return NULL;
    }

    char *out = NULL;
    if (*p == '"') {
        p++;
        out = p;
        while (*p && *p != '"') p++;
        if (*p == '"') {
            *p = '\0';
            p++;
        }
    } else {
        out = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) {
            *p = '\0';
            p++;
        }
    }

    *io_p = p;
    return out;
}

static char *resolve_font_alias_in_dir(const char *dir, const char *alias_name)
{
    if (!dir || !dir[0] || !alias_name || !alias_name[0]) return NULL;
    if (dir[0] != '/') return NULL;

    char path[PATH_MAX];
    size_t dlen = strlen(dir);
    bool slash = (dir[dlen - 1] == '/');
    snprintf(path, sizeof(path), "%s%sfonts.alias", dir, slash ? "" : "/");

    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        rstrip_newline(line);
        char *p = skip_ws(line);
        if (!p || !p[0]) continue;
        if (*p == '!' || *p == '#') continue;

        char *alias = parse_quoted_or_token(&p);
        char *target = parse_quoted_or_token(&p);
        if (!alias || !target) continue;
        if (strcmp(alias, alias_name) != 0) continue;

        fclose(fp);
        return xstrdup(target);
    }

    fclose(fp);
    return NULL;
}

static char *make_xlfd_zero_size_name(const char *font_name)
{
    XlfdParts x;
    if (!parse_xlfd(font_name, &x)) return NULL;

    char buf[512];
    snprintf(buf, sizeof(buf),
             "-%s-%s-%s-%s-%s-%s-%d-%d-%d-%d-%s-0-%s-%s",
             x.f[1] ? x.f[1] : "*",
             x.f[2] ? x.f[2] : "*",
             x.f[3] ? x.f[3] : "*",
             x.f[4] ? x.f[4] : "*",
             x.f[5] ? x.f[5] : "*",
             x.f[6] ? x.f[6] : "",
             0, 0, 0, 0,
             x.f[11] ? x.f[11] : "*",
             x.f[13] ? x.f[13] : "*",
             x.f[14] ? x.f[14] : "*");

    free(x.dup);
    return xstrdup(buf);
}

static char *x11_find_font_file_path(Display *dpy, const char *font_name)
{
    if (!dpy || !font_name || !font_name[0]) return NULL;

    int npaths = 0;
    char **paths = XGetFontPath(dpy, &npaths);
    if (!paths || npaths <= 0) {
        if (paths) XFreeFontPath(paths);
        return NULL;
    }

    char *out = NULL;
    char *cur = xstrdup(font_name);
    if (!cur) {
        XFreeFontPath(paths);
        return NULL;
    }

    for (int depth = 0; depth < 4 && cur && cur[0] && !out; ++depth) {
        for (int i = 0; i < npaths && !out; ++i) {
            char *dir = font_path_entry_dir(paths[i]);
            if (!dir) continue;
            out = find_font_file_in_dir(dir, cur);
            free(dir);
        }

        if (!out && cur[0] == '-') {
            char *zero = make_xlfd_zero_size_name(cur);
            if (zero) {
                for (int i = 0; i < npaths && !out; ++i) {
                    char *dir = font_path_entry_dir(paths[i]);
                    if (!dir) continue;
                    out = find_font_file_in_dir(dir, zero);
                    free(dir);
                }
                free(zero);
            }
        }

        if (out) break;

        char *resolved = NULL;
        for (int i = 0; i < npaths && !resolved; ++i) {
            char *dir = font_path_entry_dir(paths[i]);
            if (!dir) continue;
            resolved = resolve_font_alias_in_dir(dir, cur);
            free(dir);
        }
        if (!resolved || strcmp(resolved, cur) == 0) {
            free(resolved);
            break;
        }
        free(cur);
        cur = resolved;
    }

    free(cur);
    XFreeFontPath(paths);
    return out;
}

static void update_font_file_label_for_loaded_font(void)
{
    if (!G.font_file_label) return;

    char *loaded_name = NULL;
    if (G.dpy && G.font) {
        loaded_name = xfont_get_property_string(G.dpy, G.font, "FONT");
    }

    const char *name = loaded_name;
    if (!name || !name[0]) {
        if (G.selected_face >= 0 && G.selected_face < G.face_count) {
            FontFace *f = &G.faces[G.selected_face];
            if (f && f->size_count > 0) {
                int sidx = G.selected_size;
                if (sidx < 0 || sidx >= f->size_count) sidx = 0;
                name = f->sizes[sidx].xlfd_name;
            }
        }
    }

    char *path = (name && name[0]) ? x11_find_font_file_path(G.dpy, name) : NULL;
    if (path && path[0] && access(path, R_OK) != 0) {
        free(path);
        path = NULL;
    }

    char *charset_reg = (G.dpy && G.font) ? xfont_get_property_string(G.dpy, G.font, "CHARSET_REGISTRY") : NULL;
    char *charset_enc = (G.dpy && G.font) ? xfont_get_property_string(G.dpy, G.font, "CHARSET_ENCODING") : NULL;
    char *weight = (G.dpy && G.font) ? xfont_get_property_string(G.dpy, G.font, "WEIGHT_NAME") : NULL;
    char *slant = (G.dpy && G.font) ? xfont_get_property_string(G.dpy, G.font, "SLANT") : NULL;
    char *spacing = (G.dpy && G.font) ? xfont_get_property_string(G.dpy, G.font, "SPACING") : NULL;

    unsigned long pixel_size = 0, point_deci = 0, avg_width = 0, res_x = 0, res_y = 0;
    bool have_px = (G.dpy && G.font) ? xfont_get_property_card32(G.dpy, G.font, "PIXEL_SIZE", &pixel_size) : false;
    bool have_pt = (G.dpy && G.font) ? xfont_get_property_card32(G.dpy, G.font, "POINT_SIZE", &point_deci) : false;
    bool have_aw = (G.dpy && G.font) ? xfont_get_property_card32(G.dpy, G.font, "AVERAGE_WIDTH", &avg_width) : false;
    bool have_rx = (G.dpy && G.font) ? xfont_get_property_card32(G.dpy, G.font, "RESOLUTION_X", &res_x) : false;
    bool have_ry = (G.dpy && G.font) ? xfont_get_property_card32(G.dpy, G.font, "RESOLUTION_Y", &res_y) : false;

    char buf[2048];
    const char *n = (name && name[0]) ? name : "(unknown)";
    const char *p = (path && path[0]) ? path : "(unknown)";
    snprintf(buf, sizeof(buf), "Loaded font: %s\nFont file: %s", n, p);

    if (weight || slant) {
        strncat(buf, "\nStyle: ", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, weight ? weight : "?", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, ", ", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, slant ? slant : "?", sizeof(buf) - strlen(buf) - 1);
    }

    if (charset_reg || charset_enc) {
        strncat(buf, "\nCharset: ", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, charset_reg ? charset_reg : "?", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, "-", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, charset_enc ? charset_enc : "?", sizeof(buf) - strlen(buf) - 1);
    }

    if (have_px || have_pt || (have_rx && have_ry)) {
        strncat(buf, "\nSize:", sizeof(buf) - strlen(buf) - 1);
        if (have_px) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), " %lu px", pixel_size);
            strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
        }
        if (have_pt) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), " %.1f pt", (double)point_deci / 10.0);
            strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
        }
        if (have_rx && have_ry) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), " @ %lux%lu dpi", res_x, res_y);
            strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
        }
    }

    if (spacing || have_aw) {
        strncat(buf, "\nSpacing:", sizeof(buf) - strlen(buf) - 1);
        if (spacing) {
            strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
            strncat(buf, spacing, sizeof(buf) - strlen(buf) - 1);
        }
        if (have_aw) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%sAvg width: %lu", spacing ? ", " : " ", avg_width);
            strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
        }
    }

    if (G.font) {
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "\nGrid glyphs: %d  Range: b1 %d..%d, b2 %d..%d  Ascent/Descent: %d/%d",
                 G.glyph_count,
                 G.font->min_byte1, G.font->max_byte1,
                 G.font->min_char_or_byte2, G.font->max_char_or_byte2,
                 G.font->ascent, G.font->descent);
        strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
    }

    XmString s = XmStringCreateLocalized(buf);
    XtVaSetValues(G.font_file_label, XmNlabelString, s, NULL);
    XmStringFree(s);

    free(charset_reg);
    free(charset_enc);
    free(weight);
    free(slant);
    free(spacing);
    free(path);
    free(loaded_name);
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
        const char *msg = "Select a font and size to browse glyphs.";
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

static bool utf8_decode_next(const char **io_p, uint32_t *out_cp)
{
    if (!io_p || !*io_p || !out_cp) return false;
    const unsigned char *s = (const unsigned char *)(*io_p);
    if (!s[0]) return false;

    unsigned char c0 = s[0];
    if (c0 < 0x80) {
        *out_cp = (uint32_t)c0;
        *io_p = (const char *)(s + 1);
        return true;
    }

    if ((c0 & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(c0 & 0x1Fu) << 6) | (uint32_t)(s[1] & 0x3Fu);
        if (cp < 0x80) cp = 0xFFFDu;
        *out_cp = cp;
        *io_p = (const char *)(s + 2);
        return true;
    }

    if ((c0 & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(c0 & 0x0Fu) << 12) | ((uint32_t)(s[1] & 0x3Fu) << 6) | (uint32_t)(s[2] & 0x3Fu);
        if (cp < 0x800) cp = 0xFFFDu;
        *out_cp = cp;
        *io_p = (const char *)(s + 3);
        return true;
    }

    if ((c0 & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(c0 & 0x07u) << 18) |
                      ((uint32_t)(s[1] & 0x3Fu) << 12) |
                      ((uint32_t)(s[2] & 0x3Fu) << 6) |
                      (uint32_t)(s[3] & 0x3Fu);
        if (cp < 0x10000 || cp > 0x10FFFFu) cp = 0xFFFDu;
        *out_cp = cp;
        *io_p = (const char *)(s + 4);
        return true;
    }

    *out_cp = 0xFFFDu;
    *io_p = (const char *)(s + 1);
    return true;
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
    unsigned int code = G.glyphs[idx];
    append_glyph_to_text(code);
    update_selected_char_label_code(code);

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
 * Sample preview + selected character info
 * ------------------------------------------------------------------------------------------------- */

static void update_selected_char_label_none(void)
{
    if (!G.selected_char_label) return;
    XmString s = XmStringCreateLocalized("Selected character: (none)");
    XtVaSetValues(G.selected_char_label, XmNlabelString, s, NULL);
    XmStringFree(s);
}

static void update_selected_char_label_code(unsigned int code)
{
    if (!G.selected_char_label) return;

    char glyph[8];
    glyph[0] = '\0';
    if (utf8_encode(code, glyph) <= 0) snprintf(glyph, sizeof(glyph), "?");

    char shown[64];
    shown[0] = '\0';
    if (code == (unsigned int)' ') snprintf(shown, sizeof(shown), "' '");
    else if (code == (unsigned int)'\n') snprintf(shown, sizeof(shown), "'\\\\n'");
    else if (code == (unsigned int)'\t') snprintf(shown, sizeof(shown), "'\\\\t'");
    else if (code < 0x20u || code == 0x7Fu) snprintf(shown, sizeof(shown), "(control)");
    else snprintf(shown, sizeof(shown), "%s", glyph);

    char buf[256];
    snprintf(buf, sizeof(buf), "Selected character %u (0x%X): %s", code, code, shown);

    XmString s = XmStringCreateLocalized(buf);
    XtVaSetValues(G.selected_char_label, XmNlabelString, s, NULL);
    XmStringFree(s);
}

static void sample_lines_clear(void)
{
    for (int i = 0; i < G.sample_line_count; ++i) free(G.sample_lines[i]);
    free(G.sample_lines);
    G.sample_lines = NULL;
    G.sample_line_count = 0;
    G.sample_line_cap = 0;
}

static bool sample_lines_ensure_capacity(void)
{
    if (G.sample_line_count + 1 <= G.sample_line_cap) return true;
    int new_cap = (G.sample_line_cap == 0) ? 8 : (G.sample_line_cap * 2);
    char **nl = (char **)realloc(G.sample_lines, (size_t)new_cap * sizeof(char *));
    if (!nl) return false;
    memset(nl + G.sample_line_cap, 0, (size_t)(new_cap - G.sample_line_cap) * sizeof(char *));
    G.sample_lines = nl;
    G.sample_line_cap = new_cap;
    return true;
}

static bool sample_lines_push(const char *line)
{
    if (!sample_lines_ensure_capacity()) return false;
    G.sample_lines[G.sample_line_count++] = xstrdup(line ? line : "");
    return (G.sample_lines[G.sample_line_count - 1] != NULL);
}

static bool utf8_to_single_byte(const char *utf8, char **out_bytes, int *out_len)
{
    if (out_bytes) *out_bytes = NULL;
    if (out_len) *out_len = 0;
    if (!utf8 || !out_bytes || !out_len) return false;

    size_t max = strlen(utf8);
    char *buf = (char *)malloc(max + 1);
    if (!buf) return false;

    int n = 0;
    const char *p = utf8;
    while (p && *p) {
        uint32_t cp = 0;
        if (!utf8_decode_next(&p, &cp)) break;
        if (cp <= 0xFFu) buf[n++] = (char)(cp & 0xFFu);
        else buf[n++] = '?';
    }
    buf[n] = '\0';
    *out_bytes = buf;
    *out_len = n;
    return true;
}

static bool utf8_to_char2b(const char *utf8, XChar2b **out_chars, int *out_len)
{
    if (out_chars) *out_chars = NULL;
    if (out_len) *out_len = 0;
    if (!utf8 || !out_chars || !out_len) return false;

    size_t max = strlen(utf8);
    XChar2b *buf = (XChar2b *)malloc(max * sizeof(XChar2b));
    if (!buf) return false;

    int n = 0;
    const char *p = utf8;
    while (p && *p) {
        uint32_t cp = 0;
        if (!utf8_decode_next(&p, &cp)) break;
        if (cp > 0xFFFFu) cp = (uint32_t)'?';
        buf[n].byte1 = (unsigned char)((cp >> 8) & 0xFFu);
        buf[n].byte2 = (unsigned char)(cp & 0xFFu);
        n++;
    }
    *out_chars = buf;
    *out_len = n;
    return true;
}

static int sample_measure_pixels(const char *utf8)
{
    if (!utf8 || !utf8[0]) return 0;
    if (!G.font) return (int)strlen(utf8) * 8;

    if (!G.font_is_two_byte) {
        char *bytes = NULL;
        int n = 0;
        if (!utf8_to_single_byte(utf8, &bytes, &n) || !bytes) return (int)strlen(utf8) * 8;
        int w = XTextWidth(G.font, bytes, n);
        free(bytes);
        return w;
    }

    XChar2b *chs = NULL;
    int n = 0;
    if (!utf8_to_char2b(utf8, &chs, &n) || !chs) return (int)strlen(utf8) * 8;
    int w = XTextWidth16(G.font, chs, n);
    free(chs);
    return w;
}

static void sample_rewrap_for_width(Dimension widget_w)
{
    sample_lines_clear();

    const char *text = (G.sample_text && G.sample_text[0]) ? G.sample_text : DEFAULT_SAMPLE_TEXT;
    if (!text || !text[0]) {
        (void)sample_lines_push("");
        return;
    }

    int pad = 8;
    int max_w = (int)widget_w - pad * 2 - 2;
    if (max_w < 1) max_w = 1;

    char *cur = NULL;

    const char *p = text;
    while (p && *p) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p) break;

        if (*p == '\n') {
            (void)sample_lines_push(cur ? cur : "");
            free(cur);
            cur = NULL;
            p++;
            continue;
        }

        const char *w0 = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        size_t wlen = (size_t)(p - w0);
        if (wlen == 0) continue;

        char *word = (char *)malloc(wlen + 1);
        if (!word) break;
        memcpy(word, w0, wlen);
        word[wlen] = '\0';

        if (!cur || !cur[0]) {
            free(cur);
            cur = word;
            continue;
        }

        size_t cur_len = strlen(cur);
        size_t need = cur_len + 1 + wlen + 1;
        char *cand = (char *)malloc(need);
        if (!cand) {
            free(word);
            break;
        }
        snprintf(cand, need, "%s %s", cur, word);

        if (sample_measure_pixels(cand) <= max_w) {
            free(cur);
            cur = cand;
            free(word);
        } else {
            (void)sample_lines_push(cur);
            free(cur);
            cur = word;
            free(cand);
        }
    }

    if (cur) {
        (void)sample_lines_push(cur);
        free(cur);
    }

    if (G.sample_line_count == 0) (void)sample_lines_push(text);
}

static void sample_preview_draw(void)
{
    if (!G.dpy || !G.sample_preview || !XtIsRealized(G.sample_preview)) return;

    Window win = XtWindow(G.sample_preview);
    if (win == None) return;
    ensure_gcs();
    if (!G.gc_text) return;

    Dimension w = 0, h = 0;
    XtVaGetValues(G.sample_preview, XmNwidth, &w, XmNheight, &h, NULL);
    if (w < 1 || h < 1) return;

    XClearArea(G.dpy, win, 0, 0, 0, 0, False);

    int pad = 8;
    int asc = (G.font) ? G.font->ascent : 12;
    int des = (G.font) ? G.font->descent : 4;
    int line_h = MAX(1, asc + des + 2);

    int y = pad + asc;
    for (int i = 0; i < G.sample_line_count; ++i) {
        const char *line = G.sample_lines[i] ? G.sample_lines[i] : "";
        if (!line[0]) {
            y += line_h;
            continue;
        }
        int x = pad;
        if (!G.font || !G.font_is_two_byte) {
            char *bytes = NULL;
            int n = 0;
            if (utf8_to_single_byte(line, &bytes, &n) && bytes) {
                XDrawString(G.dpy, win, G.gc_text, x, y, bytes, n);
                free(bytes);
            }
        } else {
            XChar2b *chs = NULL;
            int n = 0;
            if (utf8_to_char2b(line, &chs, &n) && chs) {
                XDrawString16(G.dpy, win, G.gc_text, x, y, chs, n);
                free(chs);
            }
        }
        y += line_h;
    }
}

static void sample_preview_update_layout(void)
{
    if (!G.sample_preview) return;
    Dimension w = 0, cur_h = 0;
    XtVaGetValues(G.sample_preview, XmNwidth, &w, XmNheight, &cur_h, NULL);
    if (w < 1) w = 1;

    sample_rewrap_for_width(w);

    int pad = 8;
    int asc = (G.font) ? G.font->ascent : 12;
    int des = (G.font) ? G.font->descent : 4;
    int line_h = MAX(1, asc + des + 2);
    int lines = MAX(1, G.sample_line_count);
    Dimension need_h = (Dimension)(pad + pad + lines * line_h);
    if (need_h < 30) need_h = 30;

    if (cur_h != need_h) {
        XtVaSetValues(G.sample_preview, XmNheight, need_h, NULL);
    }

    G.last_sample_w = w;
}

static void sample_preview_update_and_draw(void)
{
    sample_preview_update_layout();
    sample_preview_draw();
}

static void sample_expose_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    XmDrawingAreaCallbackStruct *cbs = (XmDrawingAreaCallbackStruct *)call;
    if (!cbs || !cbs->event) return;
    if (cbs->event->type != Expose) return;

    if (G.sample_line_count == 0) sample_preview_update_layout();
    sample_preview_draw();
}

static void sample_reflow_timeout_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)client_data;
    (void)id;
    G.sample_reflow_timer = 0;
    sample_preview_update_and_draw();
}

static void sample_configure_event(Widget w, XtPointer client, XEvent *event, Boolean *cont)
{
    (void)w;
    (void)client;
    (void)cont;
    if (!event || event->type != ConfigureNotify) return;

    XConfigureEvent *cev = (XConfigureEvent *)event;
    if ((Dimension)cev->width == G.last_sample_w) return;

    if (G.sample_reflow_timer) return;
    G.sample_reflow_timer = XtAppAddTimeOut(G.app_context, 0, sample_reflow_timeout_cb, NULL);
}

static void sample_dialog_destroy_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    (void)call;
    G.sample_dialog = NULL;
}

static Widget sample_dialog_text_widget(Widget dialog)
{
    if (!dialog) return NULL;
    return XmSelectionBoxGetChild(dialog, XmDIALOG_TEXT);
}

static void sample_dialog_set_text(Widget dialog, const char *text)
{
    Widget t = sample_dialog_text_widget(dialog);
    if (!t) return;
    XmTextFieldSetString(t, (char *)(text ? text : ""));
    XmTextFieldSetInsertionPosition(t, (XmTextPosition)strlen(text ? text : ""));
}

static void sample_dialog_default_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    (void)call;
    if (!G.sample_dialog) return;
    sample_dialog_set_text(G.sample_dialog, DEFAULT_SAMPLE_TEXT);
}

static void sample_dialog_cancel_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    (void)call;
    if (G.sample_dialog) XtUnmanageChild(G.sample_dialog);
}

static void sample_dialog_ok_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    (void)call;
    if (!G.sample_dialog) return;
    Widget t = sample_dialog_text_widget(G.sample_dialog);
    if (!t) {
        XtUnmanageChild(G.sample_dialog);
        return;
    }
    char *s = XmTextFieldGetString(t);
    if (s) {
        free(G.sample_text);
        G.sample_text = xstrdup(s);
        XtFree(s);
    }
    sample_preview_update_and_draw();
    XtUnmanageChild(G.sample_dialog);
}

static void show_sample_dialog(void)
{
    if (!G.toplevel) return;
    if (!G.sample_dialog || !XtIsWidget(G.sample_dialog)) {
        Arg args[4];
        Cardinal n = 0;
        XtSetArg(args[n], XmNautoUnmanage, False); n++;
        XtSetArg(args[n], XmNdialogStyle, XmDIALOG_PRIMARY_APPLICATION_MODAL); n++;
        G.sample_dialog = XmCreatePromptDialog(G.toplevel, "sampleTextDialog", args, n);
        if (!G.sample_dialog) return;

        Widget shell = XtParent(G.sample_dialog);
        if (shell) {
            XtVaSetValues(shell, XmNtitle, "Sample Text", XmNdeleteResponse, XmUNMAP, NULL);
        }

        Widget helpb = XmSelectionBoxGetChild(G.sample_dialog, XmDIALOG_HELP_BUTTON);
        if (helpb) {
            XmString s_def = XmStringCreateLocalized("Default");
            XtVaSetValues(helpb, XmNlabelString, s_def, NULL);
            XmStringFree(s_def);
            XtAddCallback(helpb, XmNactivateCallback, sample_dialog_default_cb, NULL);
        }

        Widget okb = XmSelectionBoxGetChild(G.sample_dialog, XmDIALOG_OK_BUTTON);
        if (okb) XtAddCallback(okb, XmNactivateCallback, sample_dialog_ok_cb, NULL);

        Widget cancelb = XmSelectionBoxGetChild(G.sample_dialog, XmDIALOG_CANCEL_BUTTON);
        if (cancelb) XtAddCallback(cancelb, XmNactivateCallback, sample_dialog_cancel_cb, NULL);

        XmString s_label = XmStringCreateLocalized("Edit sample text:");
        XtVaSetValues(G.sample_dialog, XmNselectionLabelString, s_label, NULL);
        XmStringFree(s_label);

        XtAddCallback(XtParent(G.sample_dialog), XmNdestroyCallback, sample_dialog_destroy_cb, NULL);
    }

    sample_dialog_set_text(G.sample_dialog, (G.sample_text && G.sample_text[0]) ? G.sample_text : DEFAULT_SAMPLE_TEXT);
    XtManageChild(G.sample_dialog);
}

static void sample_button_press(Widget w, XtPointer client, XEvent *event, Boolean *cont)
{
    (void)w;
    (void)client;
    (void)cont;
    if (!event || event->type != ButtonPress) return;
    show_sample_dialog();
}

/* -------------------------------------------------------------------------------------------------
 * UI callbacks
 * ------------------------------------------------------------------------------------------------- */

static void on_new_window(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    (void)call;

    pid_t pid = fork();
    if (pid == 0) {
        execl(G.exec_path, G.exec_path, (char *)NULL);
        _exit(1);
    }
}

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
                             "Select a font and size, then click characters to add them to the field below.",
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

static void combobox_set_selected_position_and_text(Widget combo, int pos, const char *label)
{
    if (!combo) return;
    if (pos < 1) pos = 1;

    bool prev = G.updating_controls;
    G.updating_controls = true;
    XtVaSetValues(combo, XmNselectedPosition, pos, NULL);
    if (label && label[0]) {
        XmString s = XmStringCreateLocalized((char *)label);
        XmComboBoxSetItem(combo, s);
        XmStringFree(s);
    }
    XmComboBoxUpdate(combo);
    G.updating_controls = prev;
}

static void apply_timeout_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)client_data;
    (void)id;
    G.apply_timer = 0;
    apply_selected_font();
}

static void schedule_apply_selected_font(void)
{
    if (!G.app_context) return;
    if (G.apply_timer) return;
    G.apply_timer = XtAppAddTimeOut(G.app_context, 0, apply_timeout_cb, NULL);
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

    const char *label = (sel >= 1 && sel <= f->size_count && f->sizes[sel - 1].label) ? f->sizes[sel - 1].label : NULL;
    combobox_set_selected_position_and_text(G.size_combo, sel, label ? label : "default");
    G.selected_size = sel - 1;
}

static void current_size_preference(int *out_pixel, int *out_point_deci)
{
    if (out_pixel) *out_pixel = 0;
    if (out_point_deci) *out_point_deci = 0;
    if (G.selected_face < 0 || G.selected_face >= G.face_count) return;
    FontFace *f = &G.faces[G.selected_face];
    if (!f || f->size_count <= 0) return;
    int sidx = G.selected_size;
    if (sidx < 0 || sidx >= f->size_count) sidx = 0;
    if (out_pixel) *out_pixel = f->sizes[sidx].pixel_size;
    if (out_point_deci) *out_point_deci = f->sizes[sidx].point_size_deci;
}

static bool str_contains_ci(const char *s, const char *sub)
{
    if (!s || !sub || !sub[0]) return false;
    size_t n = strlen(sub);
    for (size_t i = 0; s[i]; ++i) {
        size_t j = 0;
        for (; j < n; ++j) {
            char a = s[i + j];
            if (!a) break;
            char b = sub[j];
            if (tolower((unsigned char)a) != tolower((unsigned char)b)) break;
        }
        if (j == n) return true;
    }
    return false;
}

static bool weight_is_bold(const char *w)
{
    if (!w || !w[0]) return false;
    if (str_contains_ci(w, "bold")) return true;
    if (str_contains_ci(w, "demi")) return true;
    if (str_contains_ci(w, "black")) return true;
    return false;
}

static bool slant_is_italic(const char *s)
{
    if (!s || !s[0]) return false;
    char c = (char)tolower((unsigned char)s[0]);
    return (c == 'i' || c == 'o');
}

static int weight_rank(const char *w, bool want_bold)
{
    if (!w || !w[0]) return 100;
    if (want_bold) {
        if (strcasecmp(w, "bold") == 0) return 0;
        if (str_contains_ci(w, "bold")) return 1;
        if (str_contains_ci(w, "demi")) return 2;
        if (str_contains_ci(w, "black")) return 3;
        return 20;
    }
    if (strcasecmp(w, "medium") == 0) return 0;
    if (strcasecmp(w, "regular") == 0) return 1;
    if (strcasecmp(w, "book") == 0) return 2;
    return 20;
}

static int slant_rank(const char *s, bool want_italic)
{
    if (!s || !s[0]) return 100;
    char c = (char)tolower((unsigned char)s[0]);
    if (want_italic) {
        if (c == 'i') return 0;
        if (c == 'o') return 1;
        return 20;
    }
    if (c == 'r' || c == 'n') return 0;
    return 20;
}

static void encoding_style_availability(const FontEncoding *e, bool has[2][2])
{
    has[0][0] = has[0][1] = has[1][0] = has[1][1] = false;
    if (!e) return;
    for (int i = 0; i < e->face_count; ++i) {
        int fi = e->face_indices[i];
        if (fi < 0 || fi >= G.face_count) continue;
        FontFace *f = &G.faces[fi];
        bool b = weight_is_bold(f->weight);
        bool it = slant_is_italic(f->slant);
        has[b ? 1 : 0][it ? 1 : 0] = true;
    }
}

static int find_best_face_for_encoding(const FontEncoding *e, bool want_bold, bool want_italic)
{
    if (!e) return -1;
    int best = -1;
    int best_score = INT_MAX;
    for (int i = 0; i < e->face_count; ++i) {
        int fi = e->face_indices[i];
        if (fi < 0 || fi >= G.face_count) continue;
        FontFace *f = &G.faces[fi];
        bool b = weight_is_bold(f->weight);
        bool it = slant_is_italic(f->slant);
        if (b != want_bold || it != want_italic) continue;
        int score = weight_rank(f->weight, want_bold) * 100 + slant_rank(f->slant, want_italic);
        if (score < best_score) {
            best_score = score;
            best = fi;
        }
    }
    return best;
}

static void populate_encoding_combo_for_group(int group_index, const char *prefer_key)
{
    combobox_clear_counted(G.encoding_combo, &G.encoding_combo_items);
    G.selected_encoding = -1;
    if (group_index < 0 || group_index >= G.group_count) return;
    FontGroup *g = &G.groups[group_index];
    if (!g || g->enc_count <= 0) return;

    for (int i = 0; i < g->enc_count; ++i) {
        const char *label = g->encodings[i].display ? g->encodings[i].display : g->encodings[i].key;
        XmString s = XmStringCreateLocalized((char *)(label ? label : "encoding"));
        XmComboBoxAddItem(G.encoding_combo, s, i + 1, False);
        G.encoding_combo_items++;
        XmStringFree(s);
    }
    XmComboBoxUpdate(G.encoding_combo);

    int sel = 1;
    if (prefer_key && prefer_key[0]) {
        for (int i = 0; i < g->enc_count; ++i) {
            if (g->encodings[i].key && strcmp(g->encodings[i].key, prefer_key) == 0) {
                sel = i + 1;
                break;
            }
        }
    } else {
        for (int i = 0; i < g->enc_count; ++i) {
            if (g->encodings[i].key && strcmp(g->encodings[i].key, "iso10646-1") == 0) {
                sel = i + 1;
                break;
            }
        }
    }

    const char *label = (sel >= 1 && sel <= g->enc_count)
                            ? (g->encodings[sel - 1].display ? g->encodings[sel - 1].display : g->encodings[sel - 1].key)
                            : NULL;
    combobox_set_selected_position_and_text(G.encoding_combo, sel, label ? label : "encoding");
    G.selected_encoding = sel - 1;
}

typedef enum {
    CHANGE_NONE = 0,
    CHANGE_GROUP,
    CHANGE_ENCODING,
    CHANGE_BOLD,
    CHANGE_ITALIC
} ChangeReason;

static void update_variant_from_controls(ChangeReason reason, int prefer_pixel, int prefer_point_deci)
{
    if (G.selected_group < 0 || G.selected_group >= G.group_count) return;
    FontGroup *g = &G.groups[G.selected_group];
    if (!g || g->enc_count <= 0) return;

    if (G.selected_encoding < 0 || G.selected_encoding >= g->enc_count) {
        G.selected_encoding = 0;
    }
    FontEncoding *e = &g->encodings[G.selected_encoding];

    bool has[2][2];
    encoding_style_availability(e, has);

    bool can_bold = has[1][0] || has[1][1];
    bool can_italic = has[0][1] || has[1][1];

    bool bold = G.want_bold;
    bool italic = G.want_italic;
    if (!can_bold) bold = false;
    if (!can_italic) italic = false;

    if (!has[bold ? 1 : 0][italic ? 1 : 0]) {
        if (reason == CHANGE_BOLD) {
            if (bold) {
                if (has[1][italic ? 1 : 0]) {
                    /* keep italic as-is */
                } else if (italic && has[1][0]) {
                    italic = false;
                } else if (!italic && has[1][1]) {
                    italic = true;
                }
            } else {
                if (has[0][italic ? 1 : 0]) {
                    /* keep italic as-is */
                } else if (italic && has[0][0]) {
                    italic = false;
                } else if (!italic && has[0][1]) {
                    italic = true;
                }
            }
        } else if (reason == CHANGE_ITALIC) {
            if (italic) {
                if (has[bold ? 1 : 0][1]) {
                    /* keep bold as-is */
                } else if (bold && has[0][1]) {
                    bold = false;
                } else if (!bold && has[1][1]) {
                    bold = true;
                }
            } else {
                if (has[bold ? 1 : 0][0]) {
                    /* keep bold as-is */
                } else if (bold && has[0][0]) {
                    bold = false;
                } else if (!bold && has[1][0]) {
                    bold = true;
                }
            }
        } else {
            if (has[0][0]) {
                bold = false;
                italic = false;
            } else if (has[0][1]) {
                bold = false;
                italic = true;
            } else if (has[1][0]) {
                bold = true;
                italic = false;
            } else if (has[1][1]) {
                bold = true;
                italic = true;
            }
        }
        if (!has[bold ? 1 : 0][italic ? 1 : 0]) {
            for (int b = 0; b <= 1; ++b) {
                for (int it = 0; it <= 1; ++it) {
                    if (has[b][it]) {
                        bold = (b != 0);
                        italic = (it != 0);
                        b = 2;
                        break;
                    }
                }
            }
        }
    }

    G.want_bold = bold;
    G.want_italic = italic;

    G.updating_controls = true;
    XtSetSensitive(G.bold_toggle, can_bold ? True : False);
    XtSetSensitive(G.italic_toggle, can_italic ? True : False);
    XmToggleButtonSetState(G.bold_toggle, bold ? True : False, False);
    XmToggleButtonSetState(G.italic_toggle, italic ? True : False, False);
    G.updating_controls = false;

    int face = find_best_face_for_encoding(e, bold, italic);
    if (face < 0 && e->face_count > 0) {
        face = e->face_indices[0];
    }
    G.selected_face = face;
    if (face >= 0) {
        populate_size_combo_for_face(face, prefer_pixel, prefer_point_deci);
    }
    schedule_apply_selected_font();
}

static void on_group_selected(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    XmComboBoxCallbackStruct *cbs = (XmComboBoxCallbackStruct *)call;
    if (!cbs) return;
    if (G.updating_controls) return;
    int pos = (int)cbs->item_position;
    if (pos < 1 || pos > G.group_count) return;

    const char *prev_enc_key = NULL;
    if (G.selected_group >= 0 && G.selected_group < G.group_count) {
        FontGroup *prev_g = &G.groups[G.selected_group];
        if (prev_g && G.selected_encoding >= 0 && G.selected_encoding < prev_g->enc_count) {
            prev_enc_key = prev_g->encodings[G.selected_encoding].key;
        }
    }

    G.selected_group = pos - 1;
    populate_encoding_combo_for_group(G.selected_group, prev_enc_key);

    int prefer_pixel = 0, prefer_point = 0;
    current_size_preference(&prefer_pixel, &prefer_point);
    update_variant_from_controls(CHANGE_GROUP, prefer_pixel, prefer_point);
}

static void on_encoding_selected(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    XmComboBoxCallbackStruct *cbs = (XmComboBoxCallbackStruct *)call;
    if (!cbs) return;
    if (G.updating_controls) return;
    int pos = (int)cbs->item_position;
    if (G.selected_group < 0 || G.selected_group >= G.group_count) return;
    FontGroup *g = &G.groups[G.selected_group];
    if (!g || pos < 1 || pos > g->enc_count) return;
    G.selected_encoding = pos - 1;

    int prefer_pixel = 0, prefer_point = 0;
    current_size_preference(&prefer_pixel, &prefer_point);
    update_variant_from_controls(CHANGE_ENCODING, prefer_pixel, prefer_point);
}

static void on_bold_toggled(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    if (G.updating_controls) return;
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call;
    G.want_bold = (cbs && cbs->set) ? true : false;
    int prefer_pixel = 0, prefer_point = 0;
    current_size_preference(&prefer_pixel, &prefer_point);
    update_variant_from_controls(CHANGE_BOLD, prefer_pixel, prefer_point);
}

static void on_italic_toggled(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    if (G.updating_controls) return;
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call;
    G.want_italic = (cbs && cbs->set) ? true : false;
    int prefer_pixel = 0, prefer_point = 0;
    current_size_preference(&prefer_pixel, &prefer_point);
    update_variant_from_controls(CHANGE_ITALIC, prefer_pixel, prefer_point);
}

static void on_size_selected(Widget w, XtPointer client, XtPointer call)
{
    (void)w;
    (void)client;
    XmComboBoxCallbackStruct *cbs = (XmComboBoxCallbackStruct *)call;
    if (!cbs) return;
    if (G.updating_controls) return;
    int pos = (int)cbs->item_position;
    int face = G.selected_face;
    if (face < 0 || face >= G.face_count) return;
    if (pos < 1 || pos > G.faces[face].size_count) return;
    G.selected_size = pos - 1;
    schedule_apply_selected_font();
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
    update_selected_char_label_none();

    ensure_gcs();
    if (G.gc_text && G.font) {
        XSetFont(G.dpy, G.gc_text, G.font->fid);
        XSetFont(G.dpy, G.gc_sel_text, G.font->fid);
    }

    sample_preview_update_and_draw();
    update_font_file_label_for_loaded_font();

    recompute_grid_geometry();
    redraw_all();
}

/* -------------------------------------------------------------------------------------------------
 * Menus and UI building
 * ------------------------------------------------------------------------------------------------- */

static void create_menu(Widget parent)
{
    Widget menubar = XmCreateMenuBar(parent, "menubar", NULL, 0);

    Widget window_pd = XmCreatePulldownMenu(menubar, "windowPD", NULL, 0);
    XtVaCreateManagedWidget("Window", xmCascadeButtonWidgetClass, menubar, XmNsubMenuId, window_pd, NULL);

    XmString s_new_acc = XmStringCreateLocalized("Ctrl+N");
    Widget mi_new = XtVaCreateManagedWidget("New",
                                            xmPushButtonWidgetClass, window_pd,
                                            XmNaccelerator, "Ctrl<Key>N",
                                            XmNacceleratorText, s_new_acc,
                                            NULL);
    XmStringFree(s_new_acc);
    XtAddCallback(mi_new, XmNactivateCallback, on_new_window, NULL);

    XmString s_acc = XmStringCreateLocalized("Alt+F4");
    Widget mi_close = XtVaCreateManagedWidget("Close",
                                              xmPushButtonWidgetClass, window_pd,
                                              XmNaccelerator, "Alt<Key>F4",
                                              XmNacceleratorText, s_acc,
                                              NULL);
    XmStringFree(s_acc);
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
    G.control_form = XtVaCreateManagedWidget("controlBar",
                                             xmRowColumnWidgetClass, G.work_form,
                                             XmNtopAttachment, XmATTACH_FORM,
                                             XmNleftAttachment, XmATTACH_FORM,
                                             XmNrightAttachment, XmATTACH_FORM,
                                             XmNorientation, XmVERTICAL,
                                             XmNpacking, XmPACK_TIGHT,
                                             XmNspacing, 6,
                                             XmNmarginWidth, 0,
                                             XmNmarginHeight, 0,
                                             NULL);

    Widget row1 = XtVaCreateManagedWidget("controlRow1",
                                          xmRowColumnWidgetClass, G.control_form,
                                          XmNorientation, XmHORIZONTAL,
                                          XmNpacking, XmPACK_TIGHT,
                                          XmNspacing, 6,
                                          NULL);

    XmString s_font = XmStringCreateLocalized("Font:");
    XtVaCreateManagedWidget("fontLabel",
                            xmLabelWidgetClass, row1,
                            XmNlabelString, s_font,
                            XmNalignment, XmALIGNMENT_BEGINNING,
                            NULL);
    XmStringFree(s_font);

    G.group_combo = XtVaCreateManagedWidget("fontCombo",
                                            xmComboBoxWidgetClass, row1,
                                            XmNcomboBoxType, XmDROP_DOWN_LIST,
                                            XmNvisibleItemCount, 16,
                                            XmNmarginWidth, 4,
                                            XmNmarginHeight, 4,
                                            XmNwidth, 320,
                                            NULL);
    XtAddCallback(G.group_combo, XmNselectionCallback, on_group_selected, NULL);

    XmString s_bold = XmStringCreateLocalized("Bold");
    G.bold_toggle = XtVaCreateManagedWidget("boldToggle",
                                            xmToggleButtonWidgetClass, row1,
                                            XmNlabelString, s_bold,
                                            XmNset, False,
                                            NULL);
    XmStringFree(s_bold);
    XtAddCallback(G.bold_toggle, XmNvalueChangedCallback, on_bold_toggled, NULL);

    XmString s_italic = XmStringCreateLocalized("Italic");
    G.italic_toggle = XtVaCreateManagedWidget("italicToggle",
                                              xmToggleButtonWidgetClass, row1,
                                              XmNlabelString, s_italic,
                                              XmNset, False,
                                              NULL);
    XmStringFree(s_italic);
    XtAddCallback(G.italic_toggle, XmNvalueChangedCallback, on_italic_toggled, NULL);

    Widget row2 = XtVaCreateManagedWidget("controlRow2",
                                          xmRowColumnWidgetClass, G.control_form,
                                          XmNorientation, XmHORIZONTAL,
                                          XmNpacking, XmPACK_TIGHT,
                                          XmNspacing, 6,
                                          NULL);

    XmString s_enc = XmStringCreateLocalized("Encoding:");
    XtVaCreateManagedWidget("encLabel",
                            xmLabelWidgetClass, row2,
                            XmNlabelString, s_enc,
                            XmNalignment, XmALIGNMENT_BEGINNING,
                            NULL);
    XmStringFree(s_enc);

    G.encoding_combo = XtVaCreateManagedWidget("encodingCombo",
                                               xmComboBoxWidgetClass, row2,
                                               XmNcomboBoxType, XmDROP_DOWN_LIST,
                                               XmNvisibleItemCount, 12,
                                               XmNmarginWidth, 4,
                                               XmNmarginHeight, 4,
                                               XmNwidth, 180,
                                               NULL);
    XtAddCallback(G.encoding_combo, XmNselectionCallback, on_encoding_selected, NULL);

    XmString s_size = XmStringCreateLocalized("Size:");
    XtVaCreateManagedWidget("sizeLabel",
                            xmLabelWidgetClass, row2,
                            XmNlabelString, s_size,
                            XmNalignment, XmALIGNMENT_BEGINNING,
                            NULL);
    XmStringFree(s_size);

    G.size_combo = XtVaCreateManagedWidget("sizeCombo",
                                           xmComboBoxWidgetClass, row2,
                                           XmNcomboBoxType, XmDROP_DOWN_LIST,
                                           XmNvisibleItemCount, 12,
                                           XmNmarginWidth, 4,
                                           XmNmarginHeight, 4,
                                           XmNwidth, 120,
                                           NULL);
    XtAddCallback(G.size_combo, XmNselectionCallback, on_size_selected, NULL);

    /* Sample text preview (click to edit). */
    G.sample_preview = XtVaCreateManagedWidget("samplePreview",
                                               xmDrawingAreaWidgetClass, G.work_form,
                                               XmNtopAttachment, XmATTACH_WIDGET,
                                               XmNtopWidget, G.control_form,
                                               XmNtopOffset, 6,
                                               XmNleftAttachment, XmATTACH_FORM,
                                               XmNrightAttachment, XmATTACH_FORM,
                                               XmNheight, 50,
                                               XmNresizePolicy, XmRESIZE_NONE,
                                               XmNshadowThickness, 1,
                                               XmNshadowType, XmSHADOW_IN,
                                               NULL);

    XtAddCallback(G.sample_preview, XmNexposeCallback, sample_expose_cb, NULL);
    XtAddEventHandler(G.sample_preview, ButtonPressMask, False, sample_button_press, NULL);
    XtAddEventHandler(G.sample_preview, StructureNotifyMask, False, sample_configure_event, NULL);

    /* Separator */
    Widget sep = XtVaCreateManagedWidget("sep",
                                         xmSeparatorGadgetClass, G.work_form,
                                         XmNtopAttachment, XmATTACH_WIDGET,
                                         XmNtopWidget, G.sample_preview,
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

    XmString s_sel = XmStringCreateLocalized("Selected character: (none)");
    G.selected_char_label = XtVaCreateManagedWidget("selectedCharLabel",
                                                    xmLabelWidgetClass, G.bottom_form,
                                                    XmNlabelString, s_sel,
                                                    XmNtopAttachment, XmATTACH_FORM,
                                                    XmNleftAttachment, XmATTACH_FORM,
                                                    XmNrightAttachment, XmATTACH_FORM,
                                                    XmNalignment, XmALIGNMENT_BEGINNING,
                                                    XmNmarginBottom, 4,
                                                    NULL);
    XmStringFree(s_sel);

    XmString s_file = XmStringCreateLocalized("Font file: (unknown)");
    G.font_file_label = XtVaCreateManagedWidget("fontFileLabel",
                                                xmLabelWidgetClass, G.bottom_form,
                                                XmNlabelString, s_file,
                                                XmNtopAttachment, XmATTACH_WIDGET,
                                                XmNtopWidget, G.selected_char_label,
                                                XmNtopOffset, 2,
                                                XmNleftAttachment, XmATTACH_FORM,
                                                XmNrightAttachment, XmATTACH_FORM,
                                                XmNalignment, XmALIGNMENT_BEGINNING,
                                                XmNmarginBottom, 6,
                                                NULL);
    XmStringFree(s_file);

    XmString s_chars = XmStringCreateLocalized("Characters to copy:");
    Widget chars_label = XtVaCreateManagedWidget("charsLabel",
                                                 xmLabelWidgetClass, G.bottom_form,
                                                 XmNlabelString, s_chars,
                                                 XmNtopAttachment, XmATTACH_WIDGET,
                                                 XmNtopWidget, G.font_file_label,
                                                 XmNtopOffset, 2,
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

    if (G.selected_group >= 0 && G.selected_group < G.group_count) {
        FontGroup *g = &G.groups[G.selected_group];
        if (g->key) session_data_set(G.session_data, "group_key", g->key);
        if (G.selected_encoding >= 0 && G.selected_encoding < g->enc_count) {
            FontEncoding *e = &g->encodings[G.selected_encoding];
            if (e->key) session_data_set(G.session_data, "encoding_key", e->key);
        }
    }

    session_data_set_int(G.session_data, "bold", G.want_bold ? 1 : 0);
    session_data_set_int(G.session_data, "italic", G.want_italic ? 1 : 0);

    if (G.selected_face >= 0 && G.selected_face < G.face_count) {
        FontFace *f = &G.faces[G.selected_face];
        if (f->key) session_data_set(G.session_data, "font_key", f->key); /* backward-compatible */
        if (G.selected_size >= 0 && G.selected_size < f->size_count) {
            session_data_set_int(G.session_data, "pixel_size", f->sizes[G.selected_size].pixel_size);
            session_data_set_int(G.session_data, "point_deci", f->sizes[G.selected_size].point_size_deci);
        }
    }

    session_save(w, G.session_data, G.exec_path);
}

static void apply_session_selection(void)
{
    int group_idx = -1;
    int enc_idx = -1;
    bool bold = false;
    bool italic = false;
    int prefer_pixel = 0;
    int prefer_point = 0;

    if (G.session_data) {
        const char *gkey = session_data_get(G.session_data, "group_key");
        const char *ekey = session_data_get(G.session_data, "encoding_key");
        if (gkey && gkey[0]) group_idx = group_find_by_key(gkey);
        if (group_idx >= 0 && ekey && ekey[0]) {
            enc_idx = encoding_find_by_key(&G.groups[group_idx], ekey);
        }

        bold = session_data_get_int(G.session_data, "bold", 0) ? true : false;
        italic = session_data_get_int(G.session_data, "italic", 0) ? true : false;

        prefer_pixel = session_data_get_int(G.session_data, "pixel_size", 0);
        prefer_point = session_data_get_int(G.session_data, "point_deci", 0);

        /* Upgrade path: older sessions saved a full face key. */
        if (group_idx < 0) {
            const char *face_key = session_data_get(G.session_data, "font_key");
            int face_idx = (face_key && face_key[0]) ? face_find_by_key(face_key) : -1;
            if (face_idx >= 0 && face_idx < G.face_count) {
                FontFace *f = &G.faces[face_idx];
                char *kg = make_group_key_for_face(f);
                if (kg) {
                    group_idx = group_find_by_key(kg);
                    free(kg);
                }
                if (group_idx >= 0) {
                    char *ke = make_encoding_key_for_face(f);
                    if (ke) {
                        enc_idx = encoding_find_by_key(&G.groups[group_idx], ke);
                        free(ke);
                    }
                }
                bold = weight_is_bold(f->weight);
                italic = slant_is_italic(f->slant);
            } else if (face_key && face_key[0] && !strchr(face_key, '|') && face_key[0] != '-') {
                /* Older builds stored raw alias names like "lucidasans-italic-12". */
                AliasFontParts ap;
                if (parse_alias_font_name(face_key, &ap)) {
                    char gk[512];
                    snprintf(gk, sizeof(gk), "alias|%s", ap.base ? ap.base : face_key);
                    group_idx = group_find_by_key(gk);
                    if (group_idx >= 0) {
                        enc_idx = encoding_find_by_key(&G.groups[group_idx], "default");
                        if (enc_idx < 0) enc_idx = 0;
                    }
                    bold = weight_is_bold(ap.weight);
                    italic = slant_is_italic(ap.slant);
                    if (prefer_point <= 0 && ap.point_size > 0) prefer_point = ap.point_size * 10;
                    alias_font_parts_free(&ap);
                }
            }
        }
    }

    if (group_idx < 0 && G.group_count > 0) {
        /* Prefer something sensible if available. */
        for (int i = 0; i < G.group_count; ++i) {
            const FontGroup *g = &G.groups[i];
            if (g->family && strcmp(g->family, "fixed") == 0) {
                group_idx = i;
                break;
            }
        }
        if (group_idx < 0) group_idx = 0;
    }

    if (group_idx >= 0 && group_idx < G.group_count) {
        G.selected_group = group_idx;
        G.selected_encoding = -1;

        const char *glabel = G.groups[group_idx].display ? G.groups[group_idx].display : "font";
        combobox_set_selected_position_and_text(G.group_combo, group_idx + 1, glabel);

        const char *prefer_enc_key = NULL;
        if (enc_idx >= 0 && enc_idx < G.groups[group_idx].enc_count) {
            prefer_enc_key = G.groups[group_idx].encodings[enc_idx].key;
        }
        populate_encoding_combo_for_group(group_idx, prefer_enc_key);

        G.want_bold = bold;
        G.want_italic = italic;
        update_variant_from_controls(CHANGE_NONE, prefer_pixel, prefer_point);
    }
}

static void populate_group_combo(void)
{
    combobox_clear_counted(G.group_combo, &G.group_combo_items);

    for (int i = 0; i < G.group_count; ++i) {
        XmString s = XmStringCreateLocalized(G.groups[i].display ? G.groups[i].display : "font");
        XmComboBoxAddItem(G.group_combo, s, i + 1, False);
        G.group_combo_items++;
        XmStringFree(s);
    }
    XmComboBoxUpdate(G.group_combo);

    apply_session_selection();
}

/* -------------------------------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    memset(&G, 0, sizeof(G));
    G.selected_group = -1;
    G.selected_encoding = -1;
    G.selected_face = -1;
    G.selected_size = -1;
    G.selected_glyph_index = -1;
    G.sample_text = xstrdup(DEFAULT_SAMPLE_TEXT);

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
    populate_group_combo();

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
    if (G.apply_timer) {
        XtRemoveTimeOut(G.apply_timer);
        G.apply_timer = 0;
    }
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
    free_font_groups();
    sample_lines_clear();
    free(G.sample_text);
    session_data_free(G.session_data);

    return 0;
}
