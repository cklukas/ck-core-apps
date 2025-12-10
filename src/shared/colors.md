# CDE Style Manager Colors

How to consume the desktop’s active palette and “Number Of Colors...” setting instead of inventing new colors.

## Palette basics
- A palette is up to 8 color sets (`XmCO_MAX_NUM_COLORS`).
- Each color set has five pixels: foreground, background, topShadowColor, bottomShadowColor, selectColor.
- The “Number Of Colors...” dialog sets `colorUse`: High=8 sets (`XmCO_HIGH_COLOR`), Medium=4 (`XmCO_MEDIUM_COLOR`), Low=2 (`XmCO_LOW_COLOR`), Black & White=2 (`XmCO_BLACK_WHITE`). Only that many sets are meaningful.
- Default High-color mapping: 1 active frame, 2 inactive frame, 3 unused, 4 text areas, 5 app backgrounds, 6 menus/dialogs, 7 unused, 8 Front Panel background.

## Best practice: read what the desktop uses
- Use Motif’s ColorObj helpers; they reflect the current Style Manager palette and `colorUse`.
- Preferred call: `XmeGetColorObjData(Screen*, int *colorUse, XmPixelSet *pixelSet, unsigned short numSets, short *active, short *inactive, short *primary, short *secondary, short *text);`
- Fallback: `XmeGetPixelData(int screenNum, int *colorUse, XmPixelSet *pixelSet, short *active, short *inactive, short *primary, short *secondary);`
- If you need RGB values, query them with `XQueryColors` on the colormap.
- Re-read on startup; re-read again if you want to honor live changes (e.g., after the user changes “Number Of Colors...” or picks a new palette).

## Drop-in C helper (reads palette + number of colors)
```c
#include <Xm/ColorObjP.h>
#include <X11/Xlib.h>
#include <stdbool.h>

typedef struct { Pixel pixel; XColor rgb; } CdeColorComponent;
typedef struct { CdeColorComponent fg, bg, ts, bs, sc; } CdeColorSet;

typedef struct {
    int colorUse;                 /* XmCO_HIGH_COLOR, etc. */
    short active, inactive, primary, secondary, text; /* color set IDs */
    int count;                    /* meaningful sets for this colorUse */
    CdeColorSet set[XmCO_MAX_NUM_COLORS];
} CdePalette;

static bool cde_read_palette(Display *dpy, int screenNum, Colormap cmap, CdePalette *out) {
    Screen *scr = ScreenOfDisplay(dpy, screenNum);
    XmPixelSet pixels[XmCO_MAX_NUM_COLORS];
    short a=0,i=0,p=0,s=0,t=0;
    int colorUse=0;

    if (!XmeGetColorObjData(scr, &colorUse, pixels, XmCO_MAX_NUM_COLORS, &a, &i, &p, &s, &t))
        return false;

    out->colorUse  = colorUse;
    out->active    = a; out->inactive = i;
    out->primary   = p; out->secondary = s; out->text = t;
    out->count = (colorUse == XmCO_HIGH_COLOR) ? 8 :
                 (colorUse == XmCO_MEDIUM_COLOR) ? 4 : 2; /* Low and BW */

    for (int idx = 0; idx < XmCO_MAX_NUM_COLORS; ++idx) {
        out->set[idx].fg.pixel = pixels[idx].fg;
        out->set[idx].bg.pixel = pixels[idx].bg;
        out->set[idx].ts.pixel = pixels[idx].ts;
        out->set[idx].bs.pixel = pixels[idx].bs;
        out->set[idx].sc.pixel = pixels[idx].sc;

        XColor q[5] = {
            {.pixel = pixels[idx].fg}, {.pixel = pixels[idx].bg},
            {.pixel = pixels[idx].ts}, {.pixel = pixels[idx].bs},
            {.pixel = pixels[idx].sc}
        };
        XQueryColors(dpy, cmap, q, 5);
        out->set[idx].fg.rgb = q[0]; out->set[idx].bg.rgb = q[1];
        out->set[idx].ts.rgb = q[2]; out->set[idx].bs.rgb = q[3];
        out->set[idx].sc.rgb = q[4];
    }
    return true;
}
```

## Using the number-of-colors value
- `colorUse` from the helper is exactly what the dialog sets. Respect it:
  - High: use up to 8 sets.
  - Medium: use only the first 4 sets.
  - Low or Black/White: use only the first 2 sets (treat UI as two-tone; BW still supplies fg/bg pixels).
- Honor `count` when iterating sets; do not assume higher IDs are available.
- For Motif widgets, set `*ColorSetId` resources (active/inactive/primary/secondary/text) using the IDs returned; Motif handles the rest per colorUse.
- For custom drawing, use `out->set[id-1].*` pixels directly; avoid inventing colors and avoid using sets beyond `count`.
- To react to user changes at runtime, call `cde_read_palette` again after a settings change (e.g., on a timer or after handling the color server’s selection change if you wire that up).

## Usage notes
- Call after your Xt/Motif app is initialized so ColorObj is running (after `XtAppInitialize`).
- Pass the app display, screen index, and colormap.
- Link with Motif/CDE libs (e.g., `-lXm -lXt -lX11`).
