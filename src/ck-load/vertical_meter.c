/*
 * vertical_meter.c
 *
 * Simple vertical meter component for Motif.
 *
 * - Implemented as a wrapper around XmDrawingArea.
 * - Draws a sunken frame (indented look) using Motif top/bottom shadow colors.
 * - Inside the frame, draws stacked rectangles (cells) from bottom to top.
 * - Each cell is square by default (height == width), unless a specific
 *   cell height is set.
 * - Value in [0..maximum] determines how many cells are filled.
 *
 * Public API (declare in a header if you like):
 *
 *   Widget VerticalMeterCreate(Widget parent, char *name, Arg *args, Cardinal n);
 *   void   VerticalMeterSetValue(Widget w, int value);
 *   void   VerticalMeterSetMaximum(Widget w, int maximum);
 *   void   VerticalMeterSetCellHeight(Widget w, int cell_height); // 0 = square
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <Xm/Xm.h>
#include <Xm/DrawingA.h>

/* -------------------------------------------------------------------------
 * Internal state
 * ------------------------------------------------------------------------- */

typedef struct {
    int   value;          /* current value */
    int   maximum;        /* maximum value (default 100) */
    int   default_max;    /* threshold for filled vs outlined cells */

    int   cell_height;    /* 0 = square (height == width), >0 = fixed height */
    int   cell_gap;       /* vertical gap between cells (pixels) */
    int   padding;        /* inner padding from frame to cell area */

    /* Colors */
    Pixel bg;
    Pixel top_shadow;
    Pixel bottom_shadow;
    Pixel bar_color;
    Pixel bg_adapted;
    GC    gc_bg;
    double bar_brightness;
    double bg_brightness;
    Boolean has_adapted_bg;

    /* GCs */
    GC    gc_top;
    GC    gc_bottom;
    GC    gc_bar;
} VerticalMeterData;

/* -------------------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------------------- */

static VerticalMeterData *vm_get_data(Widget w, Boolean create_if_missing);
static void vm_destroy_cb(Widget w, XtPointer client_data, XtPointer call_data);
static void vm_expose_cb(Widget w, XtPointer client_data, XtPointer call_data);
static void vm_resize_cb(Widget w, XtPointer client_data, XtPointer call_data);
static void vm_draw(Widget w, VerticalMeterData *vm);
static double vm_pixel_brightness(Display *dpy, Colormap cmap, Pixel pixel);
static Pixel vm_mix_with_white(Display *dpy, Colormap cmap, XColor *base, double factor);

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static VerticalMeterData *vm_get_data(Widget w, Boolean create_if_missing)
{
    VerticalMeterData *vm = NULL;
    XtVaGetValues(w, XmNuserData, &vm, NULL);

    if (!vm && create_if_missing) {
        vm = (VerticalMeterData *)calloc(1, sizeof(VerticalMeterData));
        if (!vm) {
            return NULL;
        }

        vm->value       = 0;
        vm->maximum     = 100;
        vm->default_max = 100;
        vm->cell_height = 0;   /* 0 = square */
        vm->cell_gap    = 1;
        vm->padding     = 2;

        /* Get colors from widget */
        XtVaGetValues(w,
                      XmNbackground,        &vm->bg,
                      XmNtopShadowColor,    &vm->top_shadow,
                      XmNbottomShadowColor, &vm->bottom_shadow,
                      XmNforeground,        &vm->bar_color,
                      NULL);

        /* If selectColor exists, prefer it for the bar color */
        Pixel select;
        if (XtIsWidget(w) &&
            XtIsManaged(w) /* just to avoid warnings; not strictly needed */) {
            if (XtVaGetValues(w, XmCHighlightColor, &select, NULL), 1) {
                /* This may not be present for all widget classes */
        vm->bar_color = select;
        }
    }

        Display *dpy = XtDisplay(w);
        Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
        vm->bar_brightness = vm_pixel_brightness(dpy, cmap, vm->bar_color);
        vm->bg_brightness = vm_pixel_brightness(dpy, cmap, vm->bg);
        XColor bar_color_info = {0};
        XColor bg_color_info = {0};
        bar_color_info.pixel = vm->bar_color;
        bg_color_info.pixel = vm->bg;
        XQueryColor(dpy, cmap, &bar_color_info);
        XQueryColor(dpy, cmap, &bg_color_info);
        double diff = fabs(vm->bar_brightness - vm->bg_brightness);
        vm->bg_adapted = vm->bg;
        vm->has_adapted_bg = False;
        if (diff < 0.5) {
            XColor bg_color = {0};
            bg_color.pixel = vm->bg;
            if (XQueryColor(dpy, cmap, &bg_color)) {
                double factor = (0.5 - diff) / 0.5;
                if (factor > 1.0) factor = 1.0;
                vm->bg_adapted = vm_mix_with_white(dpy, cmap, &bg_color, factor);
                vm->has_adapted_bg = True;
                XColor adapted_info = {0};
                adapted_info.pixel = vm->bg_adapted;
                XQueryColor(dpy, cmap, &adapted_info);
                double adapted_brightness = vm_pixel_brightness(dpy, cmap, vm->bg_adapted);
                fprintf(stderr,
                        "[vertical_meter] contrast low: bar=(%.3f;%u,%u,%u) bg=(%.3f;%u,%u,%u) diff=%.3f adapt=(%.3f;%u,%u,%u)\n",
                        vm->bar_brightness,
                        (unsigned)bar_color_info.red,
                        (unsigned)bar_color_info.green,
                        (unsigned)bar_color_info.blue,
                        vm->bg_brightness,
                        (unsigned)bg_color_info.red,
                        (unsigned)bg_color_info.green,
                        (unsigned)bg_color_info.blue,
                        diff,
                        adapted_brightness,
                        (unsigned)adapted_info.red,
                        (unsigned)adapted_info.green,
                        (unsigned)adapted_info.blue);
            }
        } else {
            fprintf(stderr,
                    "[vertical_meter] contrast OK: bar=(%.3f;%u,%u,%u) bg=(%.3f;%u,%u,%u) diff=%.3f\n",
                    vm->bar_brightness,
                    (unsigned)bar_color_info.red,
                    (unsigned)bar_color_info.green,
                    (unsigned)bar_color_info.blue,
                    vm->bg_brightness,
                    (unsigned)bg_color_info.red,
                    (unsigned)bg_color_info.green,
                    (unsigned)bg_color_info.blue,
                    diff);
        }

        /* Create GCs */
        XGCValues gcv;
        gcv.foreground = vm->top_shadow;
        vm->gc_top = XtGetGC(w, GCForeground, &gcv);

        gcv.foreground = vm->bottom_shadow;
        vm->gc_bottom = XtGetGC(w, GCForeground, &gcv);

        gcv.foreground = vm->bar_color;
        vm->gc_bar = XtGetGC(w, GCForeground, &gcv);

        gcv.foreground = vm->bg_adapted;
        vm->gc_bg = XtGetGC(w, GCForeground, &gcv);

        XtVaSetValues(w, XmNuserData, vm, NULL);

        /* Add destroy callback to free data */
        XtAddCallback(w, XmNdestroyCallback, vm_destroy_cb, NULL);
    }

    return vm;
}

static void vm_destroy_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    (void)call_data;

    VerticalMeterData *vm = NULL;
    XtVaGetValues(w, XmNuserData, &vm, NULL);
    if (!vm) return;

    /* Release GCs */
    if (vm->gc_top)    XtReleaseGC(w, vm->gc_top);
    if (vm->gc_bottom) XtReleaseGC(w, vm->gc_bottom);
    if (vm->gc_bar)    XtReleaseGC(w, vm->gc_bar);
    if (vm->gc_bg)     XtReleaseGC(w, vm->gc_bg);

    free(vm);
    XtVaSetValues(w, XmNuserData, NULL, NULL);
}

/* -------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------- */

static void vm_expose_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    XmDrawingAreaCallbackStruct *cbs = (XmDrawingAreaCallbackStruct *)call_data;

    if (!cbs || cbs->reason != XmCR_EXPOSE) return;

    VerticalMeterData *vm = vm_get_data(w, True);
    if (!vm) return;

    vm_draw(w, vm);
}

static void vm_resize_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    (void)call_data;

    VerticalMeterData *vm = vm_get_data(w, False);
    if (!vm) return;

    vm_draw(w, vm);
}

static void vm_draw(Widget w, VerticalMeterData *vm)
{
    if (!XtIsRealized(w)) return;

    Display *dpy = XtDisplay(w);
    Window   win = XtWindow(w);

    Dimension width, height;
    XtVaGetValues(w,
                  XmNwidth,  &width,
                  XmNheight, &height,
                  NULL);

    if (width < 4 || height < 4) {
        return;
    }

    /* Clear background */
    if (vm->gc_bg) {
        XFillRectangle(dpy, win, vm->gc_bg, 0, 0, width, height);
    } else {
        XSetForeground(dpy, vm->gc_bar, vm->bg);
        XFillRectangle(dpy, win, vm->gc_bar, 0, 0, width, height);
    }
    XSetForeground(dpy, vm->gc_bar, vm->bar_color);  /* restore bar color */

    /* Draw sunken frame around whole widget */
    /* Outer rectangle border */
    int x0 = 0;
    int y0 = 0;
    int x1 = (int)width - 1;
    int y1 = (int)height - 1;

    /* Sunken: top/left dark (bottomShadow), bottom/right bright (topShadow) */
    /* Top */
    XDrawLine(dpy, win, vm->gc_bottom, x0, y0, x1, y0);
    /* Left */
    XDrawLine(dpy, win, vm->gc_bottom, x0, y0, x0, y1);
    /* Bottom */
    XDrawLine(dpy, win, vm->gc_top, x0, y1, x1, y1);
    /* Right */
    XDrawLine(dpy, win, vm->gc_top, x1, y0, x1, y1);

    /* Inner content area (inside the frame + padding) */
    int pad = vm->padding;
    int inner_x0 = x0 + 1 + pad;
    int inner_y0 = y0 + 1 + pad;
    int inner_x1 = x1 - 1 - pad;
    int inner_y1 = y1 - 1 - pad;

    if (inner_x1 <= inner_x0 || inner_y1 <= inner_y0) return;

    int inner_w = inner_x1 - inner_x0 + 1;
    int inner_h = inner_y1 - inner_y0 + 1;

    /* Determine cell geometry */
    int cell_gap = vm->cell_gap;
    int cell_width  = (inner_w > 4) ? (inner_w - 2) : inner_w; /* leave tiny margins */
    if (cell_width < 1) cell_width = 1;

    int cell_height;
    if (vm->cell_height > 0) {
        cell_height = vm->cell_height;
    } else {
        /* Square by default: height = width */
        cell_height = cell_width;
    }

    if (cell_height < 1) cell_height = 1;

    /* Compute how many cells fit vertically */
    int cell_total = cell_height + cell_gap;
    if (cell_total <= 0) return;

    int max_cells = inner_h / cell_total;
    if (max_cells <= 0) {
        max_cells = 1;
    }

    /* Determine how many cells to fill based on value/maximum */
    int value  = vm->value;
    int maxval = (vm->maximum > 0) ? vm->maximum : 100;
    int default_max = (vm->default_max > 0) ? vm->default_max : maxval;
    if (default_max > maxval) default_max = maxval;
    if (value < 0) value = 0;
    if (value > maxval) value = maxval;

    int filled_cells = (max_cells * value + maxval / 2) / maxval;
    int default_cells = (max_cells * default_max + maxval / 2) / maxval;
    if (default_cells > max_cells) default_cells = max_cells;

    int solid_cells = filled_cells;
    if (solid_cells > default_cells) solid_cells = default_cells;
    int outlined_cells = filled_cells - solid_cells;

    /* Draw filled cells from bottom up */
    int cx = inner_x0 + (inner_w - cell_width) / 2;
    int cy_bottom = inner_y1;

    for (int i = 0; i < max_cells; ++i) {
        int cell_y1 = cy_bottom - i * cell_total;
        int cell_y0 = cell_y1 - cell_height + 1;

        if (cell_y0 < inner_y0) break;

        if (i < solid_cells) {
            /* Filled cell */
            XFillRectangle(dpy, win, vm->gc_bar,
                           cx, cell_y0,
                           (unsigned int)cell_width,
                           (unsigned int)cell_height);
        } else if (i < solid_cells + outlined_cells) {
            /* Outlined cell */
            XDrawRectangle(dpy, win, vm->gc_bar,
                           cx, cell_y0,
                           (unsigned int)cell_width,
                           (unsigned int)cell_height);
        } else {
            /* Empty cell – optionally draw outline, or skip for clean look */
            /* Here we just leave it blank so the meter is clean. */
        }
    }
}

static double vm_pixel_brightness(Display *dpy, Colormap cmap, Pixel pixel)
{
    if (!dpy) return 0.0;
    XColor color;
    color.pixel = pixel;
    if (!XQueryColor(dpy, cmap, &color)) return 0.0;
    double r = color.red / 65535.0;
    double g = color.green / 65535.0;
    double b = color.blue / 65535.0;
    return 0.299 * r + 0.587 * g + 0.114 * b;
}

static Pixel vm_mix_with_white(Display *dpy, Colormap cmap, XColor *base, double factor)
{
    if (!dpy || !base) return base ? base->pixel : 0;
    if (factor <= 0.0) return base->pixel;
    if (factor > 1.0) factor = 1.0;
    XColor target = *base;
    target.red   = (unsigned short)(base->red   + (unsigned short)((65535 - base->red)   * factor));
    target.green = (unsigned short)(base->green + (unsigned short)((65535 - base->green) * factor));
    target.blue  = (unsigned short)(base->blue  + (unsigned short)((65535 - base->blue)  * factor));
    target.flags = DoRed | DoGreen | DoBlue;
    if (XAllocColor(dpy, cmap, &target)) {
        return target.pixel;
    }
    return base->pixel;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/*
 * Create a VerticalMeter inside the given parent.
 *
 * Usage example:
 *
 *   Widget meter = VerticalMeterCreate(parent, "levelMeter", NULL, 0);
 *   VerticalMeterSetMaximum(meter, 100);
 *   VerticalMeterSetValue(meter, 75);
 */
Widget VerticalMeterCreate(Widget parent, char *name, Arg *args, Cardinal n)
{
    Widget w = XmCreateDrawingArea(parent, name ? name : "verticalMeter",
                                   args, n);

    /* Attach callbacks */
    XtAddCallback(w, XmNexposeCallback, vm_expose_cb, NULL);
    XtAddCallback(w, XmNresizeCallback, vm_resize_cb, NULL);
    XtAddCallback(w, XmNdestroyCallback, vm_destroy_cb, NULL);

    /* Ensure data is created and colors are initialized */
    (void)vm_get_data(w, True);

    XtManageChild(w);
    return w;
}

void VerticalMeterSetValue(Widget w, int value)
{
    VerticalMeterData *vm = vm_get_data(w, False);
    if (!vm) return;

    if (value < 0) value = 0;
    if (value > vm->maximum && vm->maximum > 0) value = vm->maximum;

    if (vm->value != value) {
        vm->value = value;
        vm_draw(w, vm);
    }
}

void VerticalMeterSetMaximum(Widget w, int maximum)
{
    VerticalMeterData *vm = vm_get_data(w, False);
    if (!vm) return;

    if (maximum <= 0) maximum = 100;
    vm->maximum = maximum;
    if (vm->default_max > vm->maximum) vm->default_max = vm->maximum;

    if (vm->value > vm->maximum) vm->value = vm->maximum;

    vm_draw(w, vm);
}

void VerticalMeterSetDefaultMaximum(Widget w, int default_max)
{
    VerticalMeterData *vm = vm_get_data(w, False);
    if (!vm) return;

    if (default_max <= 0) default_max = vm->maximum;
    if (default_max > vm->maximum) default_max = vm->maximum;
    vm->default_max = default_max;

    vm_draw(w, vm);
}

/*
 * Set a fixed cell height. If cell_height == 0, the meter uses square cells
 * (height == width) by default.
 */
void VerticalMeterSetCellHeight(Widget w, int cell_height)
{
    VerticalMeterData *vm = vm_get_data(w, False);
    if (!vm) return;

    if (cell_height < 0) cell_height = 0;
    vm->cell_height = cell_height;

    vm_draw(w, vm);
}
