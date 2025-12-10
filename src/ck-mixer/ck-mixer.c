/*
 * ck-mixer.c
 *
 * Win95-style ALSA mixer frontend using Motif.
 *
 * Layout:
 *  - Top-left: device selection combo box
 *  - Next to it: "Use this device as default" checkbox
 *  - Below: horizontal row of vertical mixer columns:
 *      [Label]
 *      [Current value label]
 *      [Vertical XmScale]
 *      [Mute/Select toggles]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>
#include <unistd.h>

#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/RowColumn.h>
#include <Xm/Label.h>
#include <Xm/Scale.h>
#include <Xm/ToggleB.h>
#include <Xm/PushB.h>
#include <Xm/ComboBox.h>      /* If not available on your system, we can switch to OptionMenu later */
#include <Xm/SeparatoG.h>
#include <Xm/Protocols.h>
#include <X11/Xlib.h>
#include <Dt/Session.h>

#include <alsa/asoundlib.h>
#include "../shared/session_utils.h"

/* -------------------------------------------------------------------------
 * Types
 * ------------------------------------------------------------------------- */

/* Forward declarations */
struct MixerControl;
struct MixerDevice;
struct AppState;

/* Represents one simple mixer control (one column in the UI). */
typedef struct MixerControl {
    snd_mixer_elem_t *elem;

    /* Flags and metadata */
    char              name[64];
    long              vol_min;
    long              vol_max;
    bool              has_playback_volume;
    bool              has_playback_switch;
    bool              has_capture_volume;
    bool              has_capture_switch;
    bool              is_enum;
    bool              is_stereo;

    /* Widgets for this control */
    Widget            column_form;    /* container for this control’s column */
    Widget            label_widget;
    Widget            value_label_widget;
    Widget            scale_widget;
    Widget            mute_toggle_widget;
    Widget            capture_toggle_widget;
    /* TODO: enum option menu widget, if needed later */

    /* Optional: cached current values (for update logic) */
    long              cur_playback_value;
    int               cur_mute_state;
} MixerControl;

/* Represents one ALSA mixer device/card. */
typedef struct MixerDevice {
    char             display_name[128];   /* Human readable, e.g. "hw:0 - HDA Intel PCH" */
    char             alsa_name[64];       /* ALSA name, e.g. "hw:0" */
    snd_mixer_t     *handle;

    MixerControl    *controls;
    size_t           num_controls;
} MixerDevice;

/* Global application state. */
typedef struct AppState {
    XtAppContext     app_context;
    Widget           top_level_shell;
    Widget           main_form;

    /* Top bar widgets */
    Widget           device_combo;

    /* Area that holds all control columns */
    Widget           controls_container;      /* e.g. a horizontal RowColumn */

    /* Status bar (optional) */
    Widget           status_label;

    /* ALSA / mixer devices */
    MixerDevice     *devices;
    size_t           num_devices;
    int              current_device_index;    /* -1 if none selected */

    /* ALSA poll integration */
    XtInputId       *mixer_input_ids;
    int              mixer_input_count;

    /* Prevent callback recursion when updating UI from ALSA events */
    bool             updating_from_alsa;

    /* Session handling */
    SessionData     *session_data;
    char             exec_path[PATH_MAX];
} AppState;

/* Simple global pointer so callbacks can see AppState without reshaping everything */
static AppState *g_app = NULL;
static Dimension g_mute_control_height = 0; /* cache height to keep spacer consistent */

#define MIN_COLUMN_WIDTH 90
#define MIN_WINDOW_HEIGHT 320

/* Normalize column widths and center inner widgets after creation */
static void normalize_column_layout(AppState *app, MixerDevice *dev)
{
    if (!app || !dev) return;

    Dimension max_w = 0;

    /* First pass: compute needed width per column based on text + mute + slider */
    for (size_t i = 0; i < dev->num_controls; ++i) {
        MixerControl *ctrl = &dev->controls[i];
        Dimension label_w = 0, value_w = 0, mute_w = 0, scale_w = 24;

        XtVaGetValues(ctrl->label_widget, XmNwidth, &label_w, NULL);
        XtVaGetValues(ctrl->value_label_widget, XmNwidth, &value_w, NULL);
        if (ctrl->mute_toggle_widget) {
            XtVaGetValues(ctrl->mute_toggle_widget, XmNwidth, &mute_w, NULL);
        }
        XtVaGetValues(ctrl->scale_widget, XmNwidth, &scale_w, NULL);
        if (scale_w == 0) scale_w = 24;

        Dimension text_w = label_w;
        if (value_w > text_w) text_w = value_w;
        if (mute_w > text_w)  text_w = mute_w;

        Dimension candidate = text_w + 16; /* padding around text */
        if (candidate < scale_w + 16) candidate = scale_w + 16;
        if (candidate < MIN_COLUMN_WIDTH) candidate = MIN_COLUMN_WIDTH;

        if (candidate > max_w) max_w = candidate;
    }

    /* Second pass: enforce width and center slider/mute widgets */
    for (size_t i = 0; i < dev->num_controls; ++i) {
        MixerControl *ctrl = &dev->controls[i];
        XtVaSetValues(ctrl->column_form, XmNwidth, max_w, NULL);

        Dimension scale_w = 0;
        XtVaGetValues(ctrl->scale_widget, XmNwidth, &scale_w, NULL);
        if (scale_w == 0) scale_w = 24;
        Dimension pad = (max_w > scale_w) ? (max_w - scale_w) / 2 : 0;

        XtVaSetValues(ctrl->scale_widget,
                      XmNleftAttachment,  XmATTACH_FORM,
                      XmNrightAttachment, XmATTACH_FORM,
                      XmNleftOffset,      pad,
                      XmNrightOffset,     pad,
                      NULL);

        if (ctrl->mute_toggle_widget) {
            Dimension mute_w = 0;
            XtVaGetValues(ctrl->mute_toggle_widget, XmNwidth, &mute_w, NULL);
            if (mute_w == 0) mute_w = scale_w;
            Dimension mpad = (max_w > mute_w) ? (max_w - mute_w) / 2 : pad;
            XtVaSetValues(ctrl->mute_toggle_widget,
                          XmNleftAttachment,  XmATTACH_FORM,
                          XmNrightAttachment, XmATTACH_FORM,
                          XmNleftOffset,      mpad,
                          XmNrightOffset,     mpad,
                          NULL);
        }
    }

    /* Ask container/shell to widen to fit all columns */
    if (max_w > 0 && dev->num_controls > 0) {
        Dimension total_width = (Dimension)(dev->num_controls * max_w);
        /* add spacing (6 between columns) and margins (approx) */
        total_width += (Dimension)((dev->num_controls - 1) * 6 + 16);

        XtVaSetValues(app->controls_container, XmNwidth, total_width, NULL);

        Dimension shell_w = 0, shell_h = 0;
        XtVaGetValues(app->top_level_shell,
                      XmNwidth, &shell_w,
                      XmNheight,&shell_h,
                      NULL);
        if (shell_w < total_width + 32 || shell_h < MIN_WINDOW_HEIGHT) {
            Dimension desired_w = (shell_w < total_width + 32) ? total_width + 32 : shell_w;
            Dimension desired_h = (shell_h < MIN_WINDOW_HEIGHT) ? MIN_WINDOW_HEIGHT : shell_h;
            XtMakeResizeRequest(app->top_level_shell,
                                desired_w,
                                desired_h,
                                NULL, NULL);
        }
    }
}

/* -------------------------------------------------------------------------
 * Function prototypes
 * ------------------------------------------------------------------------- */

/* Entry / app setup */
static void app_init_state(AppState *app);
/* ALSA-related functions */
static int  alsa_enumerate_devices(AppState *app);
static int  alsa_open_device(MixerDevice *dev);
static void alsa_close_device(MixerDevice *dev);
static int  alsa_build_controls_for_device(MixerDevice *dev);
static void alsa_refresh_device_controls(AppState *app, MixerDevice *dev);

/* UI creation */
static void ui_create_main_window(AppState *app, int *argc, char **argv);
static void ui_build_device_list(AppState *app);
static void ui_select_initial_device(AppState *app);
static void ui_rebuild_controls_for_current_device(AppState *app);
static void ui_register_wm_protocols(AppState *app);

/* Callbacks */
static void cb_device_selection(Widget w, XtPointer client_data, XtPointer call_data);
static void cb_scale_value_changed(Widget w, XtPointer client_data, XtPointer call_data);
static void cb_mute_toggled(Widget w, XtPointer client_data, XtPointer call_data);
static void cb_wm_delete(Widget w, XtPointer client_data, XtPointer call_data);
static void cb_wm_save(Widget w, XtPointer client_data, XtPointer call_data);

/* ALSA monitoring & integration with Xt */
static void alsa_register_poll_descriptors(AppState *app);
static void cb_mixer_fd_input(XtPointer client_data, int *source, XtInputId *id);

/* Utility */
static void app_set_status(AppState *app, const char *text);
static MixerDevice *app_get_current_device(AppState *app);
static int  volume_to_percent(const MixerControl *ctrl, long v);
static long percent_to_volume(const MixerControl *ctrl, int percent);
static void init_exec_path(AppState *app, const char *argv0);

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    AppState app;
    app_init_state(&app);
    g_app = &app;

    /* Session handling */
    char *session_id = session_parse_argument(&argc, argv);
    app.session_data = session_data_create(session_id);
    free(session_id);
    init_exec_path(&app, argv[0]);

    /* Enumerate mixer devices via ALSA */
    if (alsa_enumerate_devices(&app) <= 0) {
        fprintf(stderr, "ck-mixer: no ALSA mixer devices found.\n");
        return EXIT_FAILURE;
    }

    /* Initialize Motif UI */
    ui_create_main_window(&app, &argc, argv);

    /* Build device list (top-left combo box) */
    ui_build_device_list(&app);

    /* Select initial device (from session if available, else first) */
    ui_select_initial_device(&app);

    /* Register ALSA poll descriptors with Xt event loop */
    alsa_register_poll_descriptors(&app);

    /* Realize and enter main loop */
    XtRealizeWidget(app.top_level_shell);
    XtAppMainLoop(app.app_context);

    /* Cleanup — in practice this will be on exit only */
    for (size_t i = 0; i < app.num_devices; ++i) {
        alsa_close_device(&app.devices[i]);
        free(app.devices[i].controls);
    }
    free(app.devices);
    free(app.mixer_input_ids);

    return EXIT_SUCCESS;
}

/* -------------------------------------------------------------------------
 * App state & config
 * ------------------------------------------------------------------------- */

static void app_init_state(AppState *app)
{
    memset(app, 0, sizeof(*app));
    app->current_device_index = -1;
    app->mixer_input_ids = NULL;
    app->mixer_input_count = 0;
    app->updating_from_alsa = false;
}

static void init_exec_path(AppState *app, const char *argv0)
{
    if (!app) return;
    memset(app->exec_path, 0, sizeof(app->exec_path));

    ssize_t len = readlink("/proc/self/exe", app->exec_path,
                           sizeof(app->exec_path) - 1);
    if (len > 0) {
        app->exec_path[len] = '\0';
        return;
    }

    if (argv0 && argv0[0]) {
        if (argv0[0] == '/') {
            strncpy(app->exec_path, argv0, sizeof(app->exec_path) - 1);
            app->exec_path[sizeof(app->exec_path) - 1] = '\0';
            return;
        }

        if (strchr(argv0, '/')) {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd))) {
                size_t cwd_len = strlen(cwd);
                size_t argv_len = strlen(argv0);
                size_t needed = cwd_len + 1 + argv_len + 1;
                if (needed <= sizeof(app->exec_path)) {
                    memcpy(app->exec_path, cwd, cwd_len);
                    app->exec_path[cwd_len] = '/';
                    memcpy(app->exec_path + cwd_len + 1, argv0, argv_len);
                    app->exec_path[cwd_len + 1 + argv_len] = '\0';
                    return;
                }
            }
        }

        strncpy(app->exec_path, argv0, sizeof(app->exec_path) - 1);
        app->exec_path[sizeof(app->exec_path) - 1] = '\0';
    }
}

/* No config file needed; session data holds geometry and last device index */

/* -------------------------------------------------------------------------
 * ALSA device & controls
 * ------------------------------------------------------------------------- */

static int alsa_enumerate_devices(AppState *app)
{
    void **hints = NULL;
    int err = snd_device_name_hint(-1, "ctl", &hints);
    if (err < 0) {
        fprintf(stderr, "ck-mixer: snd_device_name_hint failed: %s\n",
                snd_strerror(err));
        app->devices     = NULL;
        app->num_devices = 0;
        return 0;
    }

    /* First count how many hints we have */
    int max_devices = 0;
    void **p = hints;
    while (*p != NULL) {
        max_devices++;
        p++;
    }

    if (max_devices == 0) {
        snd_device_name_free_hint(hints);
        app->devices     = NULL;
        app->num_devices = 0;
        return 0;
    }

    app->devices = (MixerDevice *)calloc((size_t)max_devices, sizeof(MixerDevice));
    if (!app->devices) {
        fprintf(stderr, "ck-mixer: out of memory allocating devices\n");
        snd_device_name_free_hint(hints);
        app->num_devices = 0;
        return 0;
    }

    app->num_devices = 0;

    /* Now iterate and fill MixerDevice entries */
    for (p = hints; *p != NULL; ++p) {
        char *name = snd_device_name_get_hint(*p, "NAME");
        char *desc = snd_device_name_get_hint(*p, "DESC");
        char *ioid = snd_device_name_get_hint(*p, "IOID"); /* may be NULL */

        if (!name) {
            if (desc) free(desc);
            if (ioid) free(ioid);
            continue;
        }

        /* Skip "null" pseudo-device */
        if (strcmp(name, "null") == 0) {
            free(name);
            if (desc) free(desc);
            if (ioid) free(ioid);
            continue;
        }

        MixerDevice *dev = &app->devices[app->num_devices];
        memset(dev, 0, sizeof(*dev));

        /* ALSA name (e.g. "hw:0", "default", "pulse", etc.) */
        strncpy(dev->alsa_name, name, sizeof(dev->alsa_name) - 1);
        dev->alsa_name[sizeof(dev->alsa_name) - 1] = '\0';

        /* Build a human-readable display name */
        if (desc && desc[0] != '\0') {
            /* DESC may be multi-line; use first line only */
            char *first_line = desc;
            char *newline = strchr(first_line, '\n');
            if (newline) {
                *newline = '\0';
            }
            snprintf(dev->display_name,
                     sizeof(dev->display_name),
                     "%s (%s)",
                     name, first_line);
        } else {
            strncpy(dev->display_name, name, sizeof(dev->display_name) - 1);
            dev->display_name[sizeof(dev->display_name) - 1] = '\0';
        }

        fprintf(stderr, "ck-mixer: found ALSA mixer device %zu: %s\n",
                app->num_devices, dev->display_name);

        app->num_devices++;

        free(name);
        if (desc) free(desc);
        if (ioid) free(ioid);
    }

    snd_device_name_free_hint(hints);

    if (app->num_devices == 0) {
        free(app->devices);
        app->devices = NULL;
    }

    return (int)app->num_devices;
}

static int alsa_open_device(MixerDevice *dev)
{
    if (!dev) return -1;

    int err;

    /* Close previous handle if any */
    if (dev->handle) {
        snd_mixer_close(dev->handle);
        dev->handle = NULL;
    }

    if ((err = snd_mixer_open(&dev->handle, 0)) < 0) {
        fprintf(stderr, "ck-mixer: snd_mixer_open(%s) failed: %s\n",
                dev->alsa_name, snd_strerror(err));
        return err;
    }

    if ((err = snd_mixer_attach(dev->handle, dev->alsa_name)) < 0) {
        fprintf(stderr, "ck-mixer: snd_mixer_attach(%s) failed: %s\n",
                dev->alsa_name, snd_strerror(err));
        snd_mixer_close(dev->handle);
        dev->handle = NULL;
        return err;
    }

    if ((err = snd_mixer_selem_register(dev->handle, NULL, NULL)) < 0) {
        fprintf(stderr, "ck-mixer: snd_mixer_selem_register(%s) failed: %s\n",
                dev->alsa_name, snd_strerror(err));
        snd_mixer_close(dev->handle);
        dev->handle = NULL;
        return err;
    }

    if ((err = snd_mixer_load(dev->handle)) < 0) {
        fprintf(stderr, "ck-mixer: snd_mixer_load(%s) failed: %s\n",
                dev->alsa_name, snd_strerror(err));
        snd_mixer_close(dev->handle);
        dev->handle = NULL;
        return err;
    }

    /* Build our list of controls for this device */
    err = alsa_build_controls_for_device(dev);
    if (err < 0) {
        fprintf(stderr, "ck-mixer: alsa_build_controls_for_device(%s) failed\n",
                dev->alsa_name);
    }

    return err;
}

static void alsa_close_device(MixerDevice *dev)
{
    if (!dev) return;
    if (dev->handle) {
        snd_mixer_close(dev->handle);
        dev->handle = NULL;
    }
}

/* Map ALSA elements -> MixerControl structs (playback volume only for now) */
static int alsa_build_controls_for_device(MixerDevice *dev)
{
    if (!dev || !dev->handle) return -1;

    /* Free old controls if any */
    free(dev->controls);
    dev->controls = NULL;
    dev->num_controls = 0;

    snd_mixer_elem_t *elem;
    size_t count = 0;

    /* First pass: count interesting controls */
    for (elem = snd_mixer_first_elem(dev->handle);
         elem;
         elem = snd_mixer_elem_next(elem)) {

        if (!snd_mixer_selem_is_active(elem))
            continue;

        if (snd_mixer_selem_has_playback_volume(elem)) {
            count++;
        }
    }

    if (count == 0) {
        fprintf(stderr, "ck-mixer: device %s has no playback volume controls\n",
                dev->alsa_name);
        return 0;
    }

    dev->controls = (MixerControl *)calloc(count, sizeof(MixerControl));
    if (!dev->controls) {
        fprintf(stderr, "ck-mixer: out of memory allocating controls\n");
        dev->num_controls = 0;
        return -1;
    }

    /* Second pass: fill controls */
    size_t idx = 0;
    for (elem = snd_mixer_first_elem(dev->handle);
         elem && idx < count;
         elem = snd_mixer_elem_next(elem)) {

        if (!snd_mixer_selem_is_active(elem))
            continue;

        if (!snd_mixer_selem_has_playback_volume(elem))
            continue;

        MixerControl *ctrl = &dev->controls[idx];
        memset(ctrl, 0, sizeof(*ctrl));

        ctrl->elem = elem;

        const char *name = snd_mixer_selem_get_name(elem);
        if (!name) name = "Unknown";

        strncpy(ctrl->name, name, sizeof(ctrl->name) - 1);
        ctrl->name[sizeof(ctrl->name) - 1] = '\0';

        snd_mixer_selem_get_playback_volume_range(elem,
                                                  &ctrl->vol_min, &ctrl->vol_max);
        ctrl->has_playback_volume  = true;
        ctrl->has_playback_switch  = snd_mixer_selem_has_playback_switch(elem);
        ctrl->has_capture_volume   = snd_mixer_selem_has_capture_volume(elem);
        ctrl->has_capture_switch   = snd_mixer_selem_has_capture_switch(elem);
        ctrl->is_enum              = snd_mixer_selem_is_enumerated(elem);
        ctrl->is_stereo            =
            snd_mixer_selem_has_playback_channel(elem, SND_MIXER_SCHN_FRONT_LEFT) &&
            snd_mixer_selem_has_playback_channel(elem, SND_MIXER_SCHN_FRONT_RIGHT);

        /* Initialize cached current values */
        long v = 0;
        if (snd_mixer_selem_get_playback_volume(elem,
                                                SND_MIXER_SCHN_FRONT_LEFT, &v) >= 0) {
            ctrl->cur_playback_value = v;
        }

        int sw = 1;
        if (ctrl->has_playback_switch) {
            if (snd_mixer_selem_get_playback_switch(elem,
                                                    SND_MIXER_SCHN_FRONT_LEFT, &sw) >= 0) {
                ctrl->cur_mute_state = sw; /* 0 = off, 1 = on (unmuted) */
            } else {
                ctrl->cur_mute_state = 1;
            }
        }

        idx++;
    }

    dev->num_controls = idx;
    return 0;
}

/* Refresh controls from ALSA, e.g. after snd_mixer_handle_events */
static void alsa_refresh_device_controls(AppState *app, MixerDevice *dev)
{
    if (!app || !dev || !dev->controls || dev->num_controls == 0) return;

    app->updating_from_alsa = true;

    for (size_t i = 0; i < dev->num_controls; ++i) {
        MixerControl *ctrl = &dev->controls[i];
        if (!ctrl->elem) continue;

        long v = 0;
        if (snd_mixer_selem_get_playback_volume(ctrl->elem,
                                                SND_MIXER_SCHN_FRONT_LEFT, &v) >= 0) {
            ctrl->cur_playback_value = v;
            int percent = volume_to_percent(ctrl, v);

            if (ctrl->scale_widget) {
                XmScaleSetValue(ctrl->scale_widget, percent);
            }

            if (ctrl->value_label_widget) {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d%%", percent);
                XmString s = XmStringCreateLocalized(buf);
                XtVaSetValues(ctrl->value_label_widget,
                              XmNlabelString, s,
                              NULL);
                XmStringFree(s);
            }
        }

        if (ctrl->has_playback_switch && ctrl->mute_toggle_widget) {
            int sw = 1;
            if (snd_mixer_selem_get_playback_switch(ctrl->elem,
                                                    SND_MIXER_SCHN_FRONT_LEFT, &sw) >= 0) {
                ctrl->cur_mute_state = sw;
                /* UI is "Mute": ON if ALSA switch == 0 */
                Boolean mute = (sw == 0) ? True : False;
                XmToggleButtonSetState(ctrl->mute_toggle_widget, mute, False);
            }
        }
    }

    app->updating_from_alsa = false;
}

/* -------------------------------------------------------------------------
 * UI creation
 * ------------------------------------------------------------------------- */

static void ui_create_main_window(AppState *app, int *argc, char **argv)
{
    XtSetLanguageProc(NULL, NULL, NULL);

    app->top_level_shell = XtVaAppInitialize(
        &app->app_context,
        "CkMixer",          /* application class */
        NULL, 0,            /* command line options */
        argc, argv,         /* argc / argv */
        NULL,               /* fallback resources */
        NULL
    );

    XtVaSetValues(app->top_level_shell,
              XmNtitle,    "Volume Control",
              XmNiconName, "Volume",
              NULL);

    /* Restore geometry from session, if available */
    if (app->session_data && session_load(app->top_level_shell, app->session_data)) {
        session_apply_geometry(app->top_level_shell, app->session_data, "x", "y", "w", "h");
    }

    app->main_form = XtVaCreateManagedWidget(
        "mainForm",
        xmFormWidgetClass,
        app->top_level_shell,
        NULL
    );

    /* Top part: device selection + "use as default" checkbox + separator */

    /* Device combo (left, top) */
    app->device_combo = XtVaCreateManagedWidget(
        "deviceCombo",
        xmComboBoxWidgetClass,
        app->main_form,
        XmNtopAttachment,    XmATTACH_FORM,
        XmNleftAttachment,   XmATTACH_FORM,
        XmNmarginWidth,      4,
        XmNmarginHeight,     4,
        XmNcomboBoxType,     XmDROP_DOWN_LIST,
        XmNvisibleItemCount, 10,   /* how many items visible when dropped */
        NULL
    );

    /* Separator below top bar */
    Widget sep = XtVaCreateManagedWidget(
        "topSeparator",
        xmSeparatorGadgetClass,
        app->main_form,
        XmNtopAttachment,    XmATTACH_WIDGET,
        XmNtopWidget,        app->device_combo,
        XmNleftAttachment,   XmATTACH_FORM,
        XmNrightAttachment,  XmATTACH_FORM,
        NULL
    );

    /* Controls container: tight horizontal RowColumn so columns size to content */
    app->controls_container = XtVaCreateManagedWidget(
        "controlsContainer",
        xmRowColumnWidgetClass,
        app->main_form,
        XmNtopAttachment,    XmATTACH_WIDGET,
        XmNtopWidget,        sep,
        XmNleftAttachment,   XmATTACH_FORM,
        XmNrightAttachment,  XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNorientation,      XmHORIZONTAL,
        XmNpacking,          XmPACK_TIGHT,
        XmNspacing,          6,
        XmNmarginWidth,      8,
        XmNmarginHeight,     8,
        NULL
    );

    /* Optional: status bar at bottom in future (for now, just stderr) */
    app->status_label = NULL;

    /* Device combo callbacks will be set after ui_build_device_list() */

    ui_register_wm_protocols(app);
}

static void ui_register_wm_protocols(AppState *app)
{
    if (!app || !app->top_level_shell) return;

    Atom wm_delete = XmInternAtom(XtDisplay(app->top_level_shell),
                                  "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(app->top_level_shell, wm_delete,
                            cb_wm_delete, (XtPointer)app);
    XmActivateWMProtocol(app->top_level_shell, wm_delete);

    Atom wm_save = XmInternAtom(XtDisplay(app->top_level_shell),
                                "WM_SAVE_YOURSELF", False);
    XmAddWMProtocolCallback(app->top_level_shell, wm_save,
                            cb_wm_save, (XtPointer)app);
    XmActivateWMProtocol(app->top_level_shell, wm_save);
}

static void ui_build_device_list(AppState *app)
{
    if (!app || !app->devices || app->num_devices == 0) return;

    for (size_t i = 0; i < app->num_devices; ++i) {
        MixerDevice *dev = &app->devices[i];
        XmString s = XmStringCreateLocalized(dev->display_name);
        /* Positions in ComboBox are 1-based */
        XmComboBoxAddItem(app->device_combo, s, (int)i + 1, False);
        XmStringFree(s);
    }

    XtAddCallback(app->device_combo, XmNselectionCallback,
                  cb_device_selection, (XtPointer)app);
}

static void ui_select_initial_device(AppState *app)
{
    if (!app || app->num_devices == 0) return;

    int index = 0;
    if (app->session_data) {
        int saved = session_data_get_int(app->session_data, "device_index", 0);
        if (saved >= 0 && (size_t)saved < app->num_devices) {
            index = saved;
        }
    }

    if (index < 0 || (size_t)index >= app->num_devices) {
        index = 0;
    }

    app->current_device_index = index;

    MixerDevice *dev = &app->devices[index];
    if (alsa_open_device(dev) < 0) {
        app_set_status(app, "Failed to open ALSA mixer device");
        return;
    }

    /* Reflect selection in combo box */
    int pos = index + 1;  /* ComboBox positions are 1-based */
    XtVaSetValues(app->device_combo,
              XmNselectedPosition, pos,
              NULL);

    ui_rebuild_controls_for_current_device(app);
}

static void ui_rebuild_controls_for_current_device(AppState *app)
{
    MixerDevice *dev = app_get_current_device(app);
    if (!dev) {
        return;
    }

    /* Destroy all existing control column widgets */
    Widget *children = NULL;
    Cardinal num_children = 0;
    XtVaGetValues(app->controls_container,
                  XmNchildren, &children,
                  XmNnumChildren, &num_children,
                  NULL);
    for (Cardinal i = 0; i < num_children; ++i) {
        XtDestroyWidget(children[i]);
    }

    if (!dev->controls || dev->num_controls == 0) {
        app_set_status(app, "No playback controls for this device");
        return;
    }

    /* Build one column per MixerControl */
    for (size_t i = 0; i < dev->num_controls; ++i) {
        MixerControl *ctrl = &dev->controls[i];

        /* Column container: Form to let the scale stretch vertically */
        Widget col = XtVaCreateManagedWidget(
            "controlColumn",
            xmFormWidgetClass,
            app->controls_container,
            XmNmarginWidth,   4,
            XmNmarginHeight,  4,
            NULL
        );

        ctrl->column_form = col;

        /* Top: label with control name */
        XmString s_name = XmStringCreateLocalized(ctrl->name);
        ctrl->label_widget = XtVaCreateManagedWidget(
            "controlLabel",
            xmLabelWidgetClass,
            col,
            XmNlabelString, s_name,
            XmNtopAttachment,    XmATTACH_FORM,
            XmNleftAttachment,   XmATTACH_FORM,
            XmNrightAttachment,  XmATTACH_FORM,
            XmNalignment,        XmALIGNMENT_CENTER,
            NULL
        );
        XmStringFree(s_name);

        Widget mute_toggle = NULL;
        Widget bottom_anchor = NULL;
        if (ctrl->has_playback_switch) {
            mute_toggle = XtVaCreateManagedWidget(
                "muteToggle",
                xmToggleButtonWidgetClass,
                col,
                XmNlabelString, XmStringCreateLocalized("Mute"),
                XmNbottomAttachment, XmATTACH_FORM,
                XmNleftAttachment,   XmATTACH_FORM,
                XmNrightAttachment,  XmATTACH_FORM,
                NULL
            );
            ctrl->mute_toggle_widget = mute_toggle;
            /* ALSA switch: 1 = on (unmuted), 0 = off (muted) */
            Boolean mute_state = (ctrl->cur_mute_state == 0) ? True : False;
            XmToggleButtonSetState(ctrl->mute_toggle_widget, mute_state, False);

            XtAddCallback(ctrl->mute_toggle_widget, XmNvalueChangedCallback,
                          cb_mute_toggled, (XtPointer)ctrl);

            XtVaGetValues(mute_toggle, XmNheight, &g_mute_control_height, NULL);
            if (g_mute_control_height == 0) {
                g_mute_control_height = 28; /* fallback if theme reports zero now */
            }
            bottom_anchor = mute_toggle;
        } else {
            /* Spacer toggle to reserve identical height even with custom fonts */
            Dimension spacer_h = (g_mute_control_height > 0) ? g_mute_control_height : 28;
            Widget spacer = XtVaCreateManagedWidget(
                "muteSpacer",
                xmLabelWidgetClass,
                col,
                XmNlabelString,      XmStringCreateLocalized(""),
                XmNrecomputeSize,    False,
                XmNheight,           spacer_h,
                XmNbottomAttachment, XmATTACH_FORM,
                XmNleftAttachment,   XmATTACH_FORM,
                XmNrightAttachment,  XmATTACH_FORM,
                NULL
            );
            bottom_anchor = spacer;
        }

        /* Value label: e.g. "75%" placed above mute/spacer */
        long v = ctrl->cur_playback_value;
        int percent = volume_to_percent(ctrl, v);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", percent);
        XmString s_val = XmStringCreateLocalized(buf);
        ctrl->value_label_widget = XtVaCreateManagedWidget(
            "valueLabel",
            xmLabelWidgetClass,
            col,
            XmNlabelString,     s_val,
            XmNalignment,       XmALIGNMENT_CENTER,
            XmNleftAttachment,  XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM,
            XmNbottomAttachment, bottom_anchor ? XmATTACH_WIDGET : XmATTACH_FORM,
            XmNbottomWidget,     bottom_anchor ? bottom_anchor : NULL,
            XmNbottomOffset,     bottom_anchor ? 2 : 0,
            NULL
        );
        XmStringFree(s_val);

        /* Vertical slider */
        ctrl->scale_widget = XtVaCreateManagedWidget(
            "volumeScale",
            xmScaleWidgetClass,
            col,
            XmNorientation,         XmVERTICAL,
            XmNminimum,             0,
            XmNmaximum,             100,
            XmNvalue,               percent,
            XmNshowValue,           False,
            XmNprocessingDirection, XmMAX_ON_TOP,
            XmNscaleWidth,          24,   /* adjust to taste */
            XmNtopAttachment,       XmATTACH_WIDGET,
            XmNtopWidget,           ctrl->label_widget,
            XmNtopOffset,           4,
            XmNleftAttachment,      XmATTACH_FORM,
            XmNrightAttachment,     XmATTACH_FORM,
            XmNbottomAttachment,    XmATTACH_WIDGET,
            XmNbottomWidget,        ctrl->value_label_widget,
            XmNbottomOffset,        4,
            NULL
        );

        XtAddCallback(ctrl->scale_widget, XmNvalueChangedCallback,
                      cb_scale_value_changed, (XtPointer)ctrl);
        /* Optional: drag callback for smoother updates */
        XtAddCallback(ctrl->scale_widget, XmNdragCallback,
                      cb_scale_value_changed, (XtPointer)ctrl);

        /* Mute toggle callback already set above if created */

    }

    normalize_column_layout(app, dev);

    app_set_status(app, dev->display_name);
}

/* -------------------------------------------------------------------------
 * Callbacks
 * ------------------------------------------------------------------------- */

static void cb_wm_delete(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = (AppState *)client_data;
    if (!app) return;
    XtAppSetExitFlag(app->app_context);
}

static void cb_wm_save(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    AppState *app = (AppState *)client_data;
    if (!app || !app->session_data) return;

    session_capture_geometry(app->top_level_shell, app->session_data, "x", "y", "w", "h");
    if (app->current_device_index >= 0) {
        session_data_set_int(app->session_data, "device_index", app->current_device_index);
    }
    session_save(app->top_level_shell, app->session_data, app->exec_path);
}

static void cb_device_selection(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    AppState *app = (AppState *)client_data;
    if (!app) return;

    XmComboBoxCallbackStruct *cbs = (XmComboBoxCallbackStruct *)call_data;
    int pos = cbs->item_position; /* 1-based */

    int new_index = pos - 1;
    if (new_index < 0 || (size_t)new_index >= app->num_devices) {
        return;
    }

    if (new_index == app->current_device_index) {
        return; /* no change */
    }

    /* Close old device */
    MixerDevice *old_dev = app_get_current_device(app);
    if (old_dev) {
        alsa_close_device(old_dev);
        free(old_dev->controls);
        old_dev->controls = NULL;
        old_dev->num_controls = 0;
    }

    app->current_device_index = new_index;
    MixerDevice *new_dev = app_get_current_device(app);
    if (!new_dev) return;

    if (alsa_open_device(new_dev) < 0) {
        app_set_status(app, "Failed to open selected device");
        return;
    }

    ui_rebuild_controls_for_current_device(app);

    /* Re-register ALSA poll descriptors for the new device */
    alsa_register_poll_descriptors(app);
}

static void cb_scale_value_changed(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;

    MixerControl *ctrl = (MixerControl *)client_data;
    if (!ctrl || !ctrl->elem) return;
    if (!g_app) return;

    /* Avoid feedback loop when we update widgets from ALSA events */
    if (g_app->updating_from_alsa) return;

    int percent = 0;
    XmScaleGetValue(ctrl->scale_widget, &percent);

    long raw = percent_to_volume(ctrl, percent);

    /* Set same volume on all playback channels */
    snd_mixer_selem_set_playback_volume_all(ctrl->elem, raw);
    ctrl->cur_playback_value = raw;

    if (ctrl->value_label_widget) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", percent);
        XmString s = XmStringCreateLocalized(buf);
        XtVaSetValues(ctrl->value_label_widget,
                      XmNlabelString, s,
                      NULL);
        XmStringFree(s);
    }
}

static void cb_mute_toggled(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)call_data;

    MixerControl *ctrl = (MixerControl *)client_data;
    if (!ctrl || !ctrl->elem) return;
    if (!g_app) return;

    if (g_app->updating_from_alsa) return;

    Boolean mute_state = XmToggleButtonGetState(w);
    /* UI: mute_state == True means "Muted"
     * ALSA switch: 1 = on (unmuted), 0 = off (muted)
     */
    int alsa_switch = mute_state ? 0 : 1;

    snd_mixer_selem_set_playback_switch_all(ctrl->elem, alsa_switch);
    ctrl->cur_mute_state = alsa_switch;
}

/* -------------------------------------------------------------------------
 * ALSA monitoring / Xt integration
 * ------------------------------------------------------------------------- */

static void alsa_register_poll_descriptors(AppState *app)
{
    /* Remove old inputs if any */
    if (app->mixer_input_ids && app->mixer_input_count > 0) {
        for (int i = 0; i < app->mixer_input_count; ++i) {
            XtRemoveInput(app->mixer_input_ids[i]);
        }
        free(app->mixer_input_ids);
        app->mixer_input_ids = NULL;
        app->mixer_input_count = 0;
    }

    MixerDevice *dev = app_get_current_device(app);
    if (!dev || !dev->handle) return;

    int count = snd_mixer_poll_descriptors_count(dev->handle);
    if (count <= 0) return;

    struct pollfd *pfds = (struct pollfd *)calloc((size_t)count, sizeof(struct pollfd));
    if (!pfds) return;

    if (snd_mixer_poll_descriptors(dev->handle, pfds, count) < 0) {
        free(pfds);
        return;
    }

    app->mixer_input_ids = (XtInputId *)calloc((size_t)count, sizeof(XtInputId));
    if (!app->mixer_input_ids) {
        free(pfds);
        return;
    }

    app->mixer_input_count = 0;

    for (int i = 0; i < count; ++i) {
        app->mixer_input_ids[i] = XtAppAddInput(
            app->app_context,
            pfds[i].fd,
            (XtPointer)XtInputReadMask,
            cb_mixer_fd_input,
            (XtPointer)app
        );
        app->mixer_input_count++;
    }

    free(pfds);
}

static void cb_mixer_fd_input(XtPointer client_data, int *source, XtInputId *id)
{
    (void)source;
    (void)id;

    AppState *app = (AppState *)client_data;
    if (!app) return;

    MixerDevice *dev = app_get_current_device(app);
    if (!dev || !dev->handle) {
        return;
    }

    /* Handle ALSA events (volume changes done elsewhere) */
    snd_mixer_handle_events(dev->handle);
    alsa_refresh_device_controls(app, dev);
}

/* -------------------------------------------------------------------------
 * Utility
 * ------------------------------------------------------------------------- */

static void app_set_status(AppState *app, const char *text)
{
    /* For now, just print to stderr.
     * Later, we can add a status_label widget and set its text here.
     */
    fprintf(stderr, "STATUS: %s\n", text ? text : "");
    (void)app;
}

static MixerDevice *app_get_current_device(AppState *app)
{
    if (!app || app->current_device_index < 0) {
        return NULL;
    }
    if ((size_t)app->current_device_index >= app->num_devices) {
        return NULL;
    }
    return &app->devices[app->current_device_index];
}

static int volume_to_percent(const MixerControl *ctrl, long v)
{
    if (!ctrl) return 0;
    long min = ctrl->vol_min;
    long max = ctrl->vol_max;
    if (max <= min) return 0;

    if (v < min) v = min;
    if (v > max) v = max;

    long range = max - min;
    long rel = v - min;

    /* Round to nearest integer */
    int percent = (int)((rel * 100 + range / 2) / range);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    return percent;
}

static long percent_to_volume(const MixerControl *ctrl, int percent)
{
    if (!ctrl) return 0;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    long min = ctrl->vol_min;
    long max = ctrl->vol_max;
    if (max <= min) return min;

    long range = max - min;
    long raw = min + (percent * range + 50) / 100; /* rounding */

    if (raw < min) raw = min;
    if (raw > max) raw = max;

    return raw;
}
