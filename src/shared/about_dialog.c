#include "about_dialog.h"

#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/Notebook.h>
#include <Xm/PushB.h>
#include <Xm/DialogS.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/xpm.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <Dt/Dt.h>

#include "ck-core.l.pm"

static void run_cmd_single_line(const char *cmd, char *buf, size_t size)
{
    buf[0] = '\0';
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        snprintf(buf, size, "n/a");
        return;
    }

    if (!fgets(buf, size, fp)) {
        snprintf(buf, size, "n/a");
        pclose(fp);
        return;
    }
    pclose(fp);

    size_t len = strlen(buf);
    while (len > 0 &&
           (buf[len-1] == '\n' || buf[len-1] == ' ' || buf[len-1] == '\t')) {
        buf[--len] = '\0';
    }
}

int about_get_os_fields(AboutField fields[], int max_fields)
{
    if (max_fields < 4) return 0;

    fields[0].label = "Distributor ID";
    run_cmd_single_line("lsb_release -i -s 2>/dev/null",
                        fields[0].value, sizeof(fields[0].value));

    fields[1].label = "Description";
    run_cmd_single_line("lsb_release -d -s 2>/dev/null",
                        fields[1].value, sizeof(fields[1].value));

    fields[2].label = "Release";
    run_cmd_single_line("lsb_release -r -s 2>/dev/null",
                        fields[2].value, sizeof(fields[2].value));

    fields[3].label = "Codename";
    run_cmd_single_line("lsb_release -c -s 2>/dev/null",
                        fields[3].value, sizeof(fields[3].value));

    return 4;
}

int about_get_cde_fields(AboutField fields[], int max_fields)
{
    if (max_fields < 4) return 0;

    fields[0].label = "Version";
    snprintf(fields[0].value, sizeof(fields[0].value), "%s", DtVERSION_STRING);

    fields[1].label = "Major / Revision / Update";
    snprintf(fields[1].value, sizeof(fields[1].value),
             "%d / %d / %d", DtVERSION, DtREVISION, DtUPDATE_LEVEL);

    fields[2].label = "Version Number";
    snprintf(fields[2].value, sizeof(fields[2].value), "%d", DtVERSION_NUMBER);

    fields[3].label = "DtVersion (runtime)";
    snprintf(fields[3].value, sizeof(fields[3].value), "%d", DtVERSION_NUMBER);

    return 4;
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

static Pixmap create_flattened_pixmap_from_xpm_data(Display *dpy,
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
    if (background_widget) {
        XtVaGetValues(background_widget, XmNbackground, &bg, NULL);
    }

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
        return scale_pixmap_nearest(dpy, dst, attr.width, attr.height, depth, scale);
    }

    return dst;
}

static void add_icon_to_title_page(Widget page, Pixmap pixmap)
{
    if (!page || pixmap == None) return;

    Arg args[12];
    int n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNtopOffset, 12); n++;
    XtSetArg(args[n], XmNleftOffset, 12); n++;
    XtSetArg(args[n], XmNlabelType, XmPIXMAP); n++;
    XtSetArg(args[n], XmNlabelPixmap, pixmap); n++;
    XtSetArg(args[n], XmNmarginWidth, 0); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    XtSetArg(args[n], XmNtraversalOn, False); n++;
    Widget icon_label = XmCreateLabel(page, "page_icon", args, n);
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

Widget about_add_title_page(Widget notebook, int page_number,
                           const char *page_name,
                           const char *tab_label,
                           const char *title_text,
                           const char *subtitle_text)
{
    Arg args[8];
    int n = 0;
    XtSetArg(args[n], XmNfractionBase, 1); n++;
    Widget page = XmCreateForm(notebook, page_name ? (char *)page_name : "titlePage", args, n);
    XtManageChild(page);

    Display *dpy = XtDisplay(notebook);
    XFontStruct *bold_fs = XLoadQueryFont(dpy, "-*-helvetica-bold-r-normal-*-24-*");
    XmFontList bold_fontlist = NULL;
    if (bold_fs) {
        bold_fontlist = XmFontListCreate(bold_fs, XmFONTLIST_DEFAULT_TAG);
    }

    Widget label_title;
    n = 0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNalignment,        XmALIGNMENT_CENTER); n++;
    XtSetArg(args[n], XmNtopOffset,        16); n++;
    label_title = XmCreateLabel(page, "label_title", args, n);

    XtVaSetValues(label_title,
                  XmNlabelString, XmStringCreateLocalized((char *)(title_text ? title_text : "")),
                  NULL);
    if (bold_fontlist) {
        XtVaSetValues(label_title,
                      XmNfontList, bold_fontlist,
                      NULL);
    }
    XtManageChild(label_title);

    if (subtitle_text && subtitle_text[0]) {
        Widget label_copy;
        n = 0;
        XtSetArg(args[n], XmNtopAttachment,    XmATTACH_WIDGET); n++;
        XtSetArg(args[n], XmNtopWidget,        label_title); n++;
        XtSetArg(args[n], XmNtopOffset,        8); n++;
        XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM); n++;
        XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM); n++;
        XtSetArg(args[n], XmNalignment,        XmALIGNMENT_CENTER); n++;
        label_copy = XmCreateLabel(page, "label_subtitle", args, n);

        XtVaSetValues(label_copy,
                      XmNlabelString, XmStringCreateLocalized((char *)subtitle_text),
                      NULL);
        XtManageChild(label_copy);
    }

    Widget tab = XmCreatePushButton(notebook, tab_label ? (char *)tab_label : "tab", NULL, 0);
    XtVaSetValues(tab,
                  XmNlabelString, XmStringCreateLocalized((char *)(tab_label ? tab_label : "")),
                  NULL);
    XtManageChild(tab);

    XtVaSetValues(page,
                  XmNnotebookChildType, XmPAGE,
                  XmNpageNumber,        page_number,
                  NULL);

    XtVaSetValues(tab,
                  XmNnotebookChildType, XmMAJOR_TAB,
                  XmNpageNumber,        page_number,
                  NULL);

    return page;
}

Widget about_add_ck_core_page(Widget notebook, int page_number,
                              const char *page_name,
                              const char *tab_label)
{
    if (!notebook) return NULL;

    Widget page = about_add_title_page(notebook, page_number,
                                       page_name ? page_name : "page_ckcore",
                                       tab_label ? tab_label : "CK-Core",
                                       "CK-Core",
                                       "(c) 2025-2026 by Dr. C. Klukas");

    Display *dpy = XtDisplay(notebook);
    float dpi = query_x_dpi(dpy);
    float scale = clamp_f(dpi / 96.0f, 1.0f, 4.0f);
    Pixmap icon = create_flattened_pixmap_from_xpm_data(dpy, ck_core_l_pm, page, scale);
    add_icon_to_title_page(page, icon);
    return page;
}

int about_set_window_icon_from_xpm(Widget toplevel, char **xpm_data)
{
    if (!toplevel || !xpm_data) return 0;
    if (!XtIsRealized(toplevel)) return 0;

    Display *dpy = XtDisplay(toplevel);
    Window win = XtWindow(toplevel);
    if (!dpy || win == None) return 0;

    Pixmap pixmap = None;
    Pixmap mask = None;

    int status = XpmCreatePixmapFromData(dpy,
                                         DefaultRootWindow(dpy),
                                         xpm_data,
                                         &pixmap,
                                         &mask,
                                         NULL);
    if (status != XpmSuccess || pixmap == None) {
        if (pixmap != None) XFreePixmap(dpy, pixmap);
        if (mask != None) XFreePixmap(dpy, mask);
        return 0;
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
    return 1;
}

int about_set_window_icon_ck_core(Widget toplevel)
{
    return about_set_window_icon_from_xpm(toplevel, ck_core_l_pm);
}

Widget about_dialog_build(Widget parent,
                          const char *shell_name,
                          const char *title,
                          Widget *out_shell)
{
    if (!parent) return NULL;

    Widget shell = XmCreateDialogShell(parent,
                                       shell_name ? (char *)shell_name : "about_dialog",
                                       NULL, 0);
    if (!shell) return NULL;

    XtVaSetValues(shell,
                  XmNdeleteResponse, XmDESTROY,
                  XmNallowShellResize, True,
                  XmNtransientFor, parent,
                  XmNtitle, title ? title : "About",
                  NULL);

    Widget form = XmCreateForm(shell, "aboutForm", NULL, 0);
    XtManageChild(form);
    XtVaSetValues(form,
                  XmNmarginWidth, 10,
                  XmNmarginHeight, 10,
                  NULL);

    Arg args[8];
    int n = 0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
    Widget notebook = XmCreateNotebook(form, "aboutNotebook", args, n);
    XtVaSetValues(notebook,
                  XmNmarginWidth,  12,
                  XmNmarginHeight, 12,
                  NULL);
    XtManageChild(notebook);

    XmString ok_label = XmStringCreateLocalized("OK");
    Widget ok_button = XtVaCreateManagedWidget("aboutOk",
                                                xmPushButtonWidgetClass, form,
                                                XmNlabelString, ok_label,
                                                XmNbottomAttachment, XmATTACH_FORM,
                                                XmNbottomOffset, 8,
                                                XmNleftAttachment, XmATTACH_POSITION,
                                                XmNrightAttachment, XmATTACH_POSITION,
                                                XmNleftPosition, 40,
                                                XmNrightPosition, 60,
                                                NULL);
    XmStringFree(ok_label);
    XtAddCallback(ok_button, XmNactivateCallback, (XtCallbackProc)XtDestroyWidget, (XtPointer)shell);

    XtVaSetValues(notebook,
                  XmNbottomAttachment, XmATTACH_WIDGET,
                  XmNbottomWidget,     ok_button,
                  XmNbottomOffset,     8,
                  NULL);

    XtVaSetValues(form, XmNdefaultButton, ok_button, NULL);

    if (out_shell) *out_shell = shell;
    return notebook;
}

Widget about_add_table_page(Widget notebook, int page_number,
                           const char *page_name,
                           const char *tab_label,
                           AboutField fields[], int field_count)
{
    if (!notebook || !fields || field_count <= 0) return NULL;

    Arg args[8];
    int n = 0;
    XtSetArg(args[n], XmNfractionBase, 100); n++;
    Widget page = XmCreateForm(notebook, page_name ? (char *)page_name : "tablePage", args, n);
    XtManageChild(page);

    /* Calculate the maximum width needed for labels */
    Display *dpy = XtDisplay(notebook);
    XFontStruct *font_struct = XLoadQueryFont(dpy, "-*-helvetica-bold-r-normal-*-18-*");
    if (!font_struct) {
        font_struct = XLoadQueryFont(dpy, "-*-helvetica-bold-r-normal-*-16-*");
    }
    if (!font_struct) {
        font_struct = XLoadQueryFont(dpy, "-*-helvetica-bold-r-normal-*-14-*");
    }
    if (!font_struct) {
        font_struct = XLoadQueryFont(dpy, "-*-helvetica-bold-r-normal-*-12-*");
    }
    if (!font_struct) {
        font_struct = XLoadQueryFont(dpy, "fixed");
    }

    XmFontList bold_fontlist = NULL;
    if (font_struct) {
        bold_fontlist = XmFontListCreate(font_struct, XmFONTLIST_DEFAULT_TAG);
    }

    int max_label_width = 0;
    if (font_struct) {
        for (int i = 0; i < field_count; ++i) {
            int width = XTextWidth(font_struct, fields[i].label, strlen(fields[i].label));
            if (width > max_label_width) {
                max_label_width = width;
            }
        }
    } else {
        /* Fallback: estimate width based on character count (rough approximation) */
        for (int i = 0; i < field_count; ++i) {
            int width = strlen(fields[i].label) * 8; /* ~8 pixels per character */
            if (width > max_label_width) {
                max_label_width = width;
            }
        }
    }

    /* Get page width to calculate percentage - use notebook width as it's more reliable */
    Dimension page_width = 0;
    XtVaGetValues(notebook, XmNwidth, &page_width, NULL);
    if (page_width == 0) {
        /* Try to get from toplevel */
        Widget toplevel = XtParent(XtParent(notebook)); /* notebook -> mainForm -> toplevel */
        if (toplevel) {
            XtVaGetValues(toplevel, XmNwidth, &page_width, NULL);
        }
        if (page_width == 0) page_width = 550; /* fallback to our max width */
    }

    /* Add more padding (40 pixels) for better spacing */
    int label_column_width = max_label_width + 40;
    int label_column_percent = (label_column_width * 100) / page_width;
    if (label_column_percent < 8) label_column_percent = 8;
    if (label_column_percent > 45) label_column_percent = 45; /* cap at 45% */

    Widget prev_row = NULL;
    for (int i = 0; i < field_count; ++i) {
        Widget key_label, value_label;

        n = 0;
        if (prev_row == NULL) {
            XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
            XtSetArg(args[n], XmNtopOffset,     8); n++;
        } else {
            XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
            XtSetArg(args[n], XmNtopWidget,     prev_row); n++;
            XtSetArg(args[n], XmNtopOffset,     4); n++;
        }
        XtSetArg(args[n], XmNleftAttachment,  XmATTACH_POSITION); n++;
        XtSetArg(args[n], XmNleftPosition,    5); n++;
        XtSetArg(args[n], XmNrightAttachment, XmATTACH_POSITION); n++;
        XtSetArg(args[n], XmNrightPosition,   5 + label_column_percent); n++;
        XtSetArg(args[n], XmNalignment,       XmALIGNMENT_BEGINNING); n++;
        key_label = XmCreateLabel(page, "table_key", args, n);

        XmString key_xms = XmStringCreateLocalized((char*)fields[i].label);
        XtVaSetValues(key_label,
                      XmNlabelString, key_xms,
                      NULL);
        if (bold_fontlist) {
            XtVaSetValues(key_label,
                          XmNfontList, bold_fontlist,
                          NULL);
        }
        XmStringFree(key_xms);
        XtManageChild(key_label);

        n = 0;
        XtSetArg(args[n], XmNtopAttachment,   XmATTACH_OPPOSITE_WIDGET); n++;
        XtSetArg(args[n], XmNtopWidget,       key_label); n++;
        XtSetArg(args[n], XmNleftAttachment,  XmATTACH_POSITION); n++;
        XtSetArg(args[n], XmNleftPosition,    5 + label_column_percent); n++;
        XtSetArg(args[n], XmNrightAttachment, XmATTACH_POSITION); n++;
        XtSetArg(args[n], XmNrightPosition,   95); n++;
        XtSetArg(args[n], XmNalignment,       XmALIGNMENT_BEGINNING); n++;
        value_label = XmCreateLabel(page, "table_value", args, n);

        XmString value_xms = XmStringCreateLocalized(fields[i].value);
        XtVaSetValues(value_label,
                      XmNlabelString, value_xms,
                      NULL);
        XmStringFree(value_xms);
        XtManageChild(value_label);

        prev_row = key_label;
    }

    /* Note: font resources are not freed here for consistency with title_page implementation */
    /* The widgets retain references to the font list */

    Widget tab = XmCreatePushButton(notebook, tab_label ? (char *)tab_label : "tab", NULL, 0);
    XtVaSetValues(tab,
                  XmNlabelString, XmStringCreateLocalized((char *)(tab_label ? tab_label : "")),
                  NULL);
    XtManageChild(tab);

    XtVaSetValues(page,
                  XmNnotebookChildType, XmPAGE,
                  XmNpageNumber,        page_number,
                  NULL);

    XtVaSetValues(tab,
                  XmNnotebookChildType, XmMAJOR_TAB,
                  XmNpageNumber,        page_number,
                  NULL);

    return page;
}

int about_add_standard_pages(Widget notebook, int start_page,
                             const char *app_tab_label,
                             const char *app_title,
                             const char *app_subtitle,
                             Boolean include_ck_core_tab)
{
    int page = start_page;
    if (app_title) {
        about_add_title_page(notebook, page++, "page_app_about",
                             app_tab_label ? app_tab_label : "About",
                             app_title,
                             app_subtitle ? app_subtitle : "");
    }

    if (include_ck_core_tab) {
        about_add_ck_core_page(notebook, page++, "page_ckcore", "CK-Core");
    }

    AboutField cde_fields[4];
    int cde_count = about_get_cde_fields(cde_fields, 4);
    about_add_table_page(notebook, page++, "page_cde", "CDE",
                         cde_fields, cde_count);

    {
        AboutField motif_fields[3];
        motif_fields[0].label = "Version";
        if (strncmp(XmVERSION_STRING, "@(#)", 4) == 0) {
            snprintf(motif_fields[0].value, sizeof(motif_fields[0].value), "%s", XmVERSION_STRING + 4);
        } else {
            snprintf(motif_fields[0].value, sizeof(motif_fields[0].value), "%s", XmVERSION_STRING);
        }
        motif_fields[1].label = "Description";
        snprintf(motif_fields[1].value, sizeof(motif_fields[1].value),
                 "Motif user interface component toolkit");
        motif_fields[2].label = "License";
        snprintf(motif_fields[2].value, sizeof(motif_fields[2].value),
                 "Published under LGPL v2.1");
        about_add_table_page(notebook, page++, "page_motif", "Motif",
                             motif_fields, 3);
    }

    AboutField os_fields[4];
    int os_count = about_get_os_fields(os_fields, 4);
    about_add_table_page(notebook, page++, "page_os", "OS",
                         os_fields, os_count);

    return page;
}
