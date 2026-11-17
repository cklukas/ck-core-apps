#include "window_metrics.h"

#include <stdio.h>
#include <string.h>

#include <X11/Xlib.h>
#include <Xm/MwmUtil.h>

static Dimension get_desired_width(const AppState *app)
{
    if (!app) return 0;
    int cols = (app->mode == 1) ? 10 : 4;
    return (Dimension)(cols * 60 + 40);
}

static Dimension get_widget_natural_height(Widget w)
{
    if (!w) return 0;
    Dimension h = 0, bw = 0;
    XtVaGetValues(w, XmNheight, &h, XmNborderWidth, &bw, NULL);

    XtWidgetGeometry pref;
    memset(&pref, 0, sizeof(pref));
    XtGeometryResult res = XtQueryGeometry(w, NULL, &pref);
    if ((res == XtGeometryYes || res == XtGeometryAlmost) &&
        (pref.request_mode & CWHeight) && pref.height > h) {
        h = pref.height;
    }
    return h > 1 ? (Dimension)(h + 2 * bw) : 0;
}

static Dimension get_desired_height(const AppState *app)
{
    if (!app) return 0;

    Dimension menu_h = get_widget_natural_height(app->menubar);
    if (menu_h == 0) menu_h = 28;

    Dimension disp_h = get_widget_natural_height(app->display_label);
    if (disp_h == 0) disp_h = 36;

    Dimension content_margin_h = 8;
    if (app->content_form) {
        XtVaGetValues(app->content_form, XmNmarginHeight, &content_margin_h, NULL);
    }

    Dimension trig_h = 0;
    Dimension keypad_top_offset = 8;
    if (app->mode == 1 && app->trig_mode_row && XtIsManaged(app->trig_mode_row)) {
        trig_h = get_widget_natural_height(app->trig_mode_row);
        if (trig_h == 0) trig_h = 18;
        keypad_top_offset = 4;
    }

    const Widget row_btns[5] = {
        app->btn_back,
        app->btn_digits[7],
        app->btn_digits[4],
        app->btn_digits[1],
        app->btn_sign,
    };
    Dimension keypad_h = 0;
    for (int r = 0; r < 5; ++r) {
        Dimension bh = get_widget_natural_height(row_btns[r]);
        if (bh == 0) bh = 26;
        keypad_h += (Dimension)(6 + bh); /* 6px XmNtopOffset per row in create_key_button() */
    }
    keypad_h += 4; /* Match 4px XmNleftOffset / XmNrightOffset around keypad buttons */

    /* main_form: menubar + 6px topOffset + content_form (2*marginHeight + display + trig + keypad) */
    Dimension desired_h = (Dimension)(menu_h + 6 + 2 * content_margin_h +
                                      disp_h + trig_h + keypad_top_offset + keypad_h);
    if (app->chrome_inited) {
        desired_h = (Dimension)(desired_h + app->chrome_dy);
    }
    return desired_h;
}

void ck_calc_log_mode_width(AppState *app, const char *context)
{
    if (!app || !context) return;
    Dimension desired = get_desired_width(app);
    Dimension current = 0;
    if (app->shell) {
        XtVaGetValues(app->shell, XmNwidth, &current, NULL);
    }
    fprintf(stderr,
            "[ck-calc] %s mode=%d desired_width=%u current_width=%u\n",
            context,
            app->mode,
            (unsigned int)desired,
            (unsigned int)current);
}

void ck_calc_apply_current_mode_width(AppState *app)
{
    if (!app || !app->shell) return;
    Dimension desired_w = get_desired_width(app);
    XtVaSetValues(app->shell,
                  XmNwidth,     desired_w,
                  XmNminWidth,  desired_w,
                  XmNmaxWidth,  desired_w,
                  NULL);
    ck_calc_log_mode_width(app, "apply_current_mode_width");
}

void ck_calc_apply_wm_hints(AppState *app)
{
    if (!app || !app->shell) return;
    unsigned int decor = MWM_DECOR_ALL ^ (MWM_DECOR_RESIZEH | MWM_DECOR_MAXIMIZE);
    unsigned int funcs = MWM_FUNC_ALL ^ (MWM_FUNC_RESIZE | MWM_FUNC_MAXIMIZE);
    XtVaSetValues(app->shell,
                  XmNmwmDecorations, decor,
                  XmNmwmFunctions,   funcs,
                  XmNallowShellResize, False,
                  NULL);
}

void ck_calc_lock_shell_dimensions(AppState *app)
{
    if (!app || !app->shell || !app->main_form) return;

    Dimension desired_w = get_desired_width(app);
    Dimension desired_h = get_desired_height(app);
    if (desired_w == 0 || desired_h == 0) return;

    Dimension cur_w = 0, cur_h = 0;
    XtVaGetValues(app->shell, XmNwidth, &cur_w, XmNheight, &cur_h, NULL);

    if (cur_w != desired_w || cur_h != desired_h) {
        XtVaSetValues(app->shell,
                      XmNallowShellResize, True,
                      XmNminWidth,         1,
                      XmNmaxWidth,         32767,
                      XmNminHeight,        1,
                      XmNmaxHeight,        32767,
                      NULL);
        XtVaSetValues(app->main_form,
                      XmNwidth,  desired_w,
                      XmNheight, desired_h,
                      NULL);
        XtVaSetValues(app->shell,
                      XmNwidth,            desired_w,
                      XmNheight,           desired_h,
                      XmNminWidth,         desired_w,
                      XmNmaxWidth,         desired_w,
                      XmNminHeight,        desired_h,
                      XmNmaxHeight,        desired_h,
                      XmNallowShellResize, False,
                      NULL);
        if (XtIsRealized(app->shell) && XtWindow(app->shell) != None) {
            XResizeWindow(XtDisplay(app->shell), XtWindow(app->shell), desired_w, desired_h);
        }
    } else {
        XtVaSetValues(app->shell,
                      XmNminWidth,         desired_w,
                      XmNmaxWidth,         desired_w,
                      XmNminHeight,        desired_h,
                      XmNmaxHeight,        desired_h,
                      XmNallowShellResize, False,
                      NULL);
    }

    fprintf(stderr,
            "[ck-calc] lock_shell_dimensions shell=%ux%u desired=%ux%u\n",
            (unsigned)cur_w, (unsigned)cur_h,
            (unsigned)desired_w, (unsigned)desired_h);
}
