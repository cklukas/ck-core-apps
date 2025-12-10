#ifndef VERTICAL_METER_H
#define VERTICAL_METER_H

#include <Xm/Xm.h>

/*
 * vertical_meter.h
 *
 * Simple vertical meter component for Motif.
 *
 * The meter is implemented as an XmDrawingArea with:
 *  - a sunken frame (using top/bottom shadow colors)
 *  - stacked rectangles filling from bottom to top depending on the value
 *
 * Public API:
 *
 *   Widget VerticalMeterCreate(Widget parent, char *name, Arg *args, Cardinal n);
 *   void   VerticalMeterSetValue(Widget w, int value);
 *   void   VerticalMeterSetMaximum(Widget w, int maximum);
 *   void   VerticalMeterSetCellHeight(Widget w, int cell_height); // 0 = square
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Create and manage a vertical meter widget.
 *
 * parent : parent widget (e.g. a Form or RowColumn)
 * name   : widget name
 * args   : standard Motif Arg list
 * n      : number of args
 *
 * Returns the created widget (an XmDrawingArea subclass / wrapper).
 */
Widget VerticalMeterCreate(Widget parent, char *name, Arg *args, Cardinal n);

/* Set the current meter value (0..maximum). */
void VerticalMeterSetValue(Widget w, int value);

/* Set the maximum value (default suggested 100). */
void VerticalMeterSetMaximum(Widget w, int maximum);

/* Set the minimum height of a single "cell" in pixels.
 * If cell_height == 0, cells are drawn square (height ≈ width).
 */
void VerticalMeterSetCellHeight(Widget w, int cell_height);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VERTICAL_METER_H */
