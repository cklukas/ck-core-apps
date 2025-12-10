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

        /* Create GCs */
        XGCValues gcv;
        gcv.foreground = vm->top_shadow;
        vm->gc_top = XtGetGC(w, GCForeground, &gcv);

        gcv.foreground = vm->bottom_shadow;
        vm->gc_bottom = XtGetGC(w, GCForeground, &gcv);

        gcv.foreground = vm->bar_color;
        vm->gc_bar = XtGetGC(w, GCForeground, &gcv);

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
    XSetForeground(dpy, vm->gc_bar, vm->bg);
    XFillRectangle(dpy, win, vm->gc_bar, 0, 0, width, height);
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
