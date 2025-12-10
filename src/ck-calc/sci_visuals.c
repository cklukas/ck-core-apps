#include "sci_visuals.h"

#include <stdio.h>
#include <string.h>

#include <Xm/Xm.h>
#include <Xm/PushB.h>
#include <Xm/XmStrDefs.h>
#include <Xm/Label.h>
#include <Xm/DrawingA.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

static Boolean is_second_active_internal(const AppState *app)
{
    if (!app) return False;
    return (app->shift_left_down || app->shift_right_down || app->second_mouse_pressed) ? True : False;
}

Boolean sci_visuals_is_second_active(const AppState *app)
{
    return is_second_active_internal(app);
}

static void set_button_label(Widget btn, const char *label)
{
    if (!btn || !label) return;
    XmString xms = XmStringCreateLocalized((char *)label);
    XtVaSetValues(btn, XmNlabelString, xms, NULL);
    XmStringFree(xms);
}

void sci_visuals_register_button(AppState *app, const char *name, Widget button)
{
    if (!app || !name || !button) return;
    if (strcmp(name, "sciR1bC4") == 0) {
        app->btn_sci_exp = button;
    } else if (strcmp(name, "sciR1bC5") == 0) {
        app->btn_sci_10x = button;
    } else if (strcmp(name, "sciR3C4") == 0) {
        app->btn_sci_ln = button;
    } else if (strcmp(name, "sciR3C5") == 0) {
        app->btn_sci_log10 = button;
    } else if (strcmp(name, "sciR4C1") == 0) {
        app->btn_sci_sin = button;
    } else if (strcmp(name, "sciR4C2") == 0) {
        app->btn_sci_cos = button;
    } else if (strcmp(name, "sciR4C3") == 0) {
        app->btn_sci_tan = button;
    } else if (strcmp(name, "sciR5C1") == 0) {
        app->btn_sci_sinh = button;
    } else if (strcmp(name, "sciR5C2") == 0) {
        app->btn_sci_cosh = button;
    } else if (strcmp(name, "sciR5C3") == 0) {
        app->btn_sci_tanh = button;
    }
}

static void refresh_second_button_labels(AppState *app, Boolean active)
{
    if (!app) return;
    struct {
        Widget      btn;
        const char  *normal;
        const char  *alt;
    } swaps[] = {
        { app->btn_sci_exp,  "e^x",     "y^x"     },
        { app->btn_sci_10x,  "10^x",    "2^x"     },
        { app->btn_sci_ln,   "ln",      "log y"   },
        { app->btn_sci_log10,"log10",   "log 2"   },
        { app->btn_sci_sin,  "sin",     "sin^-1"  },
        { app->btn_sci_cos,  "cos",     "cos^-1"  },
        { app->btn_sci_tan,  "tan",     "tan^-1"  },
        { app->btn_sci_sinh, "sinh",    "sinh^-1" },
        { app->btn_sci_cosh, "cosh",    "cosh^-1" },
        { app->btn_sci_tanh, "tanh",    "tanh^-1" },
    };
    for (size_t i = 0; i < sizeof(swaps)/sizeof(swaps[0]); ++i) {
        Widget btn = swaps[i].btn;
        if (btn) {
            set_button_label(btn, active ? swaps[i].alt : swaps[i].normal);
        }
    }
}

void sci_visuals_update(AppState *app)
{
    if (!app || !app->btn_second) return;
    Boolean active = is_second_active_internal(app);
    if (!app->second_shadow_cached) {
        Pixel top = 0;
        Pixel bottom = 0;
        short thickness = 0;
        XtVaGetValues(app->btn_second,
                      XmNtopShadowColor, &top,
                      XmNbottomShadowColor, &bottom,
                      XmNshadowThickness, &thickness,
                      NULL);
        app->second_shadow_top = top;
        app->second_shadow_bottom = bottom;
        app->second_shadow_thickness = thickness;
        app->second_shadow_cached = True;
        app->second_thickness_cached = True;
    }
    Pixel top = app->second_shadow_top;
    Pixel bottom = app->second_shadow_bottom;
    if (active) {
        Pixel tmp = top;
        top = bottom;
        bottom = tmp;
    }
    if (!app->second_color_cached) {
        Pixel base_bg = 0;
        XtVaGetValues(app->btn_second,
                      XmNbackground, &base_bg,
                      NULL);
        app->second_bg_normal = base_bg;
        Pixel highlight = base_bg;
        if (app->palette_ok) {
            int idx = (app->palette.active >= 0 && app->palette.active < app->palette.count)
                          ? app->palette.active
                          : app->palette.inactive;
            if (idx >= 0 && idx < app->palette.count) {
                highlight = app->palette.set[idx].bg.pixel;
            }
        }
        app->second_bg_active = highlight;
        app->second_color_cached = True;
    }
    Pixel bg = active ? app->second_bg_active : app->second_bg_normal;
    XtVaSetValues(app->btn_second,
                  XmNshadowType, active ? XmSHADOW_IN : XmSHADOW_OUT,
                  XmNtopShadowColor, top,
                  XmNbottomShadowColor, bottom,
                  XmNshadowThickness, app->second_shadow_thickness,
                  XmNbackground, bg,
                  NULL);
    refresh_second_button_labels(app, active);
    if (app->second_border_prev_active != active) {
        fprintf(stderr, "[ck-calc] 2nd border %s (top=%lu bottom=%lu bg=%lu)\n",
                active ? "pressed" : "released",
                (unsigned long)top,
                (unsigned long)bottom,
                (unsigned long)bg);
        app->second_border_prev_active = active;
    }
}

void sci_visuals_handle_shift(AppState *app, KeySym sym, Boolean down)
{
    if (!app) return;
    if (sym == XK_Shift_L) {
        app->shift_left_down = (down != False);
    } else if (sym == XK_Shift_R) {
        app->shift_right_down = (down != False);
    } else {
        return;
    }
    fprintf(stderr, "[ck-calc] shift %s -> active=%d (L=%d R=%d)\n",
            down ? "down" : "up",
            sci_visuals_is_second_active(app),
            app->shift_left_down,
            app->shift_right_down);
    sci_visuals_update(app);
}

void sci_visuals_toggle_button(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = (AppState *)client_data;
    if (!app) return;
    app->second_mouse_pressed = !app->second_mouse_pressed;
    fprintf(stderr, "[ck-calc] 2nd mouse toggle (%s) -> active=%d\n",
            app->second_mouse_pressed ? "press" : "release",
            sci_visuals_is_second_active(app));
    sci_visuals_update(app);
}

void sci_visuals_arm_button(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = (AppState *)client_data;
    if (!app) return;
    if (sci_visuals_is_second_active(app)) {
        fprintf(stderr, "[ck-calc] 2nd arm: keeping border pressed\n");
        sci_visuals_update(app);
    }
}

void sci_visuals_second_button_event(Widget w, XtPointer client_data, XEvent *event, Boolean *cont)
{
    (void)w;
    (void)cont;
    if (!event) return;
    AppState *app = (AppState *)client_data;
    if (!app) return;
    switch (event->type) {
        case ButtonPress:
            fprintf(stderr, "[ck-calc] 2nd mouse down\n");
            if (sci_visuals_is_second_active(app)) {
                fprintf(stderr, "[ck-calc] 2nd already active; keeping border pressed\n");
                sci_visuals_update(app);
            }
            break;
        case ButtonRelease:
            fprintf(stderr, "[ck-calc] 2nd mouse up\n");
            break;
        default:
            break;
    }
}
