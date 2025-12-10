#ifndef CDE_PALETTE_H
#define CDE_PALETTE_H

#include <X11/Xlib.h>
#include <Xm/Xm.h>
#include <Xm/ColorObjP.h>
#include <stdbool.h>

typedef struct {
    Pixel pixel;
    XColor rgb;
} CdeColorComponent;

typedef struct {
    CdeColorComponent fg;
    CdeColorComponent bg;
    CdeColorComponent ts;
    CdeColorComponent bs;
    CdeColorComponent sc;
} CdeColorSet;

typedef struct {
    int colorUse;                 /* XmCO_HIGH_COLOR, XmCO_MEDIUM_COLOR, XmCO_LOW_COLOR, XmCO_BLACK_WHITE */
    short active, inactive, primary, secondary, text;
    int count;                    /* how many sets are meaningful */
    CdeColorSet set[XmCO_MAX_NUM_COLORS];
} CdePalette;

/* Returns true on success, false otherwise. Requires ColorObj initialized (after XtAppInitialize). */
bool cde_palette_read(Display *dpy, int screen_num, Colormap cmap, CdePalette *out);

/* Helpers to inspect palette meta data */
int  cde_palette_color_use(const CdePalette *p);
int  cde_palette_set_count(const CdePalette *p);
bool cde_palette_is_high_color(const CdePalette *p);

#endif /* CDE_PALETTE_H */
