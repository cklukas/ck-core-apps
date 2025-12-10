#include "cde_palette.h"

#include <string.h>

bool cde_palette_read(Display *dpy, int screen_num, Colormap cmap, CdePalette *out)
{
    if (!dpy || !out) return false;
    Screen *scr = ScreenOfDisplay(dpy, screen_num);
    if (!scr) return false;

    XmPixelSet pixels[XmCO_MAX_NUM_COLORS];
    short a = 0, i = 0, p = 0, s = 0, t = 0;
    int colorUse = 0;

    if (!XmeGetColorObjData(scr, &colorUse, pixels, XmCO_MAX_NUM_COLORS, &a, &i, &p, &s, &t)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->colorUse = colorUse;
    out->active   = a;
    out->inactive = i;
    out->primary  = p;
    out->secondary= s;
    out->text     = t;
    out->count = (colorUse == XmCO_HIGH_COLOR)   ? 8 :
                 (colorUse == XmCO_MEDIUM_COLOR) ? 4 : 2;

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
        out->set[idx].fg.rgb = q[0];
        out->set[idx].bg.rgb = q[1];
        out->set[idx].ts.rgb = q[2];
        out->set[idx].bs.rgb = q[3];
        out->set[idx].sc.rgb = q[4];
    }

    return true;
}

int cde_palette_color_use(const CdePalette *p)
{
    return p ? p->colorUse : 0;
}

int cde_palette_set_count(const CdePalette *p)
{
    return p ? p->count : 0;
}

bool cde_palette_is_high_color(const CdePalette *p)
{
    return p && p->colorUse == XmCO_HIGH_COLOR;
}
