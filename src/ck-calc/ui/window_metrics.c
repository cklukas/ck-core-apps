#include "window_metrics.h"

#include <stdio.h>

#include <X11/Xlib.h>
#include <Xm/MwmUtil.h>

static Dimension get_desired_width(const AppState *app)
{
    if (!app) return 0;
    int cols = (app->mode == 1) ? 10 : 4;
    return (Dimension)(cols * 60 + 40);
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
    if (!app || !app->shell || !XtIsRealized(app->shell) || !app->main_form) return;

    Dimension shell_w = 0, shell_h = 0;
    Dimension form_w = 0, form_h = 0;
    Dimension pref_h = 0;
    Dimension desired_w = get_desired_width(app);
    XtVaGetValues(app->shell, XmNwidth, &shell_w, XmNheight, &shell_h, NULL);
    XtVaGetValues(app->main_form, XmNwidth, &form_w, XmNheight, &form_h, NULL);

    if (shell_h <= 1 || form_h <= 1) {
        fprintf(stderr,
                "[ck-calc] lock_shell_dimensions deferred shell=%ux%u form=%ux%u\n",
                (unsigned)shell_w, (unsigned)shell_h,
                (unsigned)form_w, (unsigned)form_h);
        XtRealizeWidget(app->shell);
        XtVaSetValues(app->shell, XmNwidth, desired_w, XmNheight, 360, NULL);
        XtVaGetValues(app->shell, XmNwidth, &shell_w, XmNheight, &shell_h, NULL);
        XtVaGetValues(app->main_form, XmNwidth, &form_w, XmNheight, &form_h, NULL);
        fprintf(stderr,
                "[ck-calc] lock_shell_dimensions retry shell=%ux%u form=%ux%u\n",
                (unsigned)shell_w, (unsigned)shell_h,
                (unsigned)form_w, (unsigned)form_h);
        if (shell_h <= 1 || form_h <= 1) return;
    }

    if (!app->chrome_inited && shell_h > form_h) {
        app->chrome_dy = shell_h - form_h;
        app->chrome_inited = True;
    }

    if (form_h == 0 || shell_w == 0) return;

    XtWidgetGeometry pref;
    memset(&pref, 0, sizeof(pref));
    if (XtQueryGeometry(app->main_form, NULL, &pref) == XtGeometryYes && pref.height > 0) {
        pref_h = pref.height;
    }

    Dimension desired_h = pref_h > 0 ? pref_h : form_h;
    if (app->chrome_inited) desired_h += app->chrome_dy;

    /* desired_w already computed above */

    XtVaSetValues(app->shell,
                  XmNwidth,      desired_w,
                  XmNminWidth,   desired_w,
                  XmNmaxWidth,   desired_w,
                  XmNminHeight,  desired_h,
                  XmNmaxHeight,  desired_h,
                  NULL);

    fprintf(stderr,
            "[ck-calc] lock_shell_dimensions shell=%ux%u form=%ux%u pref_h=%u chrome=%u desired_h=%u\n",
            (unsigned)shell_w, (unsigned)shell_h,
            (unsigned)form_w, (unsigned)form_h,
            (unsigned)pref_h, (unsigned)app->chrome_dy,
            (unsigned)desired_h);
}
