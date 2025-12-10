/*
 * load_meters.c
 *
 * Simple Motif app using VerticalMeter to display:
 *   - CPU usage (%)
 *   - RAM usage (%)
 *   - Swap usage (%)
 *   - Load averages (1, 5, 15 min) normalized to CPU count (in %)
 *
 * Layout:
 *   - Top-level XmForm, with 6 child XmForms as "columns"
 *   - In each column: XmLabelGadget at the top, VerticalMeter filling below
 *
 * Columns automatically stretch horizontally with window width,
 * meters fill vertically with height.
 *
 * Compile example (adjust paths as needed):
 *   cc -o load_meters load_meters.c vertical_meter.c \
 *      -lXm -lXt -lX11 -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <X11/Xlib.h>
#include <Xm/Protocols.h>
#include <Dt/Session.h>
#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/LabelG.h>

#include "vertical_meter.h"
#include "../shared/session_utils.h"

#define NUM_METERS 6

enum {
    METER_CPU = 0,
    METER_RAM,
    METER_SWAP,
    METER_LOAD1,
    METER_LOAD5,
    METER_LOAD15
};

/* Update interval in milliseconds */
#define UPDATE_INTERVAL_MS 1000

/* Maxima in "percent" units for all meters */
#define PERCENT_MAX 100
#define LOAD_PERCENT_DEFAULT_MAX 100

static Widget meters[NUM_METERS];
static Widget value_labels[NUM_METERS];
static XtAppContext app_context;
static SessionData *session_data = NULL;
static char g_exec_path[PATH_MAX] = "ck-load";

/* ---------- Helper: CPU usage from /proc/stat ---------- */

static int
read_cpu_usage_percent(int *out_percent)
{
    static unsigned long long prev_user = 0, prev_nice = 0, prev_system = 0;
    static unsigned long long prev_idle = 0, prev_iowait = 0;
    static unsigned long long prev_irq = 0, prev_softirq = 0, prev_steal = 0;
    static int initialized = 0;

    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        return -1;
    }

    char buf[256];
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    /* Example line:
     * cpu  4705 150 1222 1048579 77 0 68 0 0 0
     */
    char cpu_label[5];
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    /* ignore guest, guest_nice */
    int scanned = sscanf(buf, "%4s %llu %llu %llu %llu %llu %llu %llu %llu",
                         cpu_label,
                         &user, &nice, &system, &idle,
                         &iowait, &irq, &softirq, &steal);
    if (scanned < 9) {
        return -1;
    }

    unsigned long long prev_idle_all = prev_idle + prev_iowait;
    unsigned long long idle_all      = idle + iowait;

    unsigned long long prev_non_idle =
        prev_user + prev_nice + prev_system + prev_irq + prev_softirq + prev_steal;
    unsigned long long non_idle =
        user + nice + system + irq + softirq + steal;

    unsigned long long prev_total = prev_idle_all + prev_non_idle;
    unsigned long long total      = idle_all + non_idle;

    if (!initialized) {
        /* First call: initialize and return 0% */
        prev_user = user;
        prev_nice = nice;
        prev_system = system;
        prev_idle = idle;
        prev_iowait = iowait;
        prev_irq = irq;
        prev_softirq = softirq;
        prev_steal = steal;
        initialized = 1;
        *out_percent = 0;
        return 0;
    }

    unsigned long long total_diff = total - prev_total;
    unsigned long long idle_diff  = idle_all - prev_idle_all;

    prev_user = user;
    prev_nice = nice;
    prev_system = system;
    prev_idle = idle;
    prev_iowait = iowait;
    prev_irq = irq;
    prev_softirq = softirq;
    prev_steal = steal;

    if (total_diff == 0) {
        *out_percent = 0;
        return 0;
    }

    double cpu_percent = 100.0 * (double)(total_diff - idle_diff) / (double)total_diff;
    if (cpu_percent < 0.0) cpu_percent = 0.0;
    if (cpu_percent > 100.0) cpu_percent = 100.0;

    *out_percent = (int)(cpu_percent + 0.5);
    return 0;
}

/* ---------- Helper: RAM + swap usage from /proc/meminfo ---------- */

static int
read_mem_and_swap_percent(int *out_ram_percent, int *out_swap_percent,
                          double *out_ram_used_gb, double *out_swap_used_gb)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1;

    char key[64];
    unsigned long value;
    char unit[32];

    unsigned long mem_total = 0;
    unsigned long mem_available = 0;
    unsigned long swap_total = 0;
    unsigned long swap_free = 0;

    while (fscanf(fp, "%63s %lu %31s\n", key, &value, unit) == 3) {
        if (strcmp(key, "MemTotal:") == 0) {
            mem_total = value;
        } else if (strcmp(key, "MemAvailable:") == 0) {
            mem_available = value;
        } else if (strcmp(key, "SwapTotal:") == 0) {
            swap_total = value;
        } else if (strcmp(key, "SwapFree:") == 0) {
            swap_free = value;
        }
    }

    fclose(fp);

    if (out_ram_used_gb)  *out_ram_used_gb  = 0.0;
    if (out_swap_used_gb) *out_swap_used_gb = 0.0;

    if (mem_total > 0 && mem_available > 0) {
        unsigned long mem_used = mem_total - mem_available;
        double ram_percent = 100.0 * (double)mem_used / (double)mem_total;
        if (ram_percent < 0.0) ram_percent = 0.0;
        if (ram_percent > 100.0) ram_percent = 100.0;
        *out_ram_percent = (int)(ram_percent + 0.5);
        if (out_ram_used_gb) {
            *out_ram_used_gb = (double)mem_used / (1024.0 * 1024.0);
        }
    } else {
        *out_ram_percent = 0;
    }

    if (swap_total > 0) {
        unsigned long swap_used = swap_total - swap_free;
        double swap_percent = 100.0 * (double)swap_used / (double)swap_total;
        if (swap_percent < 0.0) swap_percent = 0.0;
        if (swap_percent > 100.0) swap_percent = 100.0;
        *out_swap_percent = (int)(swap_percent + 0.5);
        if (out_swap_used_gb) {
            *out_swap_used_gb = (double)swap_used / (1024.0 * 1024.0);
        }
    } else {
        *out_swap_percent = 0;
    }

    return 0;
}

/* ---------- Helper: load averages from /proc/loadavg ---------- */

static int
read_load_percent(int *out_l1, int *out_l5, int *out_l15,
                  double *out_raw_l1, double *out_raw_l5, double *out_raw_l15)
{
    FILE *fp = fopen("/proc/loadavg", "r");
    if (!fp) return -1;

    double l1, l5, l15;
    if (fscanf(fp, "%lf %lf %lf", &l1, &l5, &l15) != 3) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    if (out_raw_l1) *out_raw_l1 = l1;
    if (out_raw_l5) *out_raw_l5 = l5;
    if (out_raw_l15) *out_raw_l15 = l15;

    long n_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (n_cpus <= 0) n_cpus = 1;

    double scale = 100.0 / (double)n_cpus; /* 1.0 load per core = 100% */

    int p1  = (int)(l1  * scale + 0.5);
    int p5  = (int)(l5  * scale + 0.5);
    int p15 = (int)(l15 * scale + 0.5);

    if (p1  < 0) p1  = 0;
    if (p5  < 0) p5  = 0;
    if (p15 < 0) p15 = 0;

    *out_l1 = p1;
    *out_l5 = p5;
    *out_l15 = p15;
    return 0;
}

/* ---------- Timer callback: update all meters ---------- */

static void
update_meters_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)client_data;
    (void)id;

    int cpu_percent;
    int ram_percent, swap_percent;
    int load1_percent, load5_percent, load15_percent;
    int load_max = LOAD_PERCENT_DEFAULT_MAX;
    double ram_used_gb = 0.0, swap_used_gb = 0.0;
    double load1_raw = 0.0, load5_raw = 0.0, load15_raw = 0.0;

    if (read_cpu_usage_percent(&cpu_percent) == 0) {
        VerticalMeterSetValue(meters[METER_CPU], cpu_percent);
        char buf[32];
        snprintf(buf, sizeof(buf), "%d%%", cpu_percent);
        XmString s = XmStringCreateLocalized(buf);
        XtVaSetValues(value_labels[METER_CPU], XmNlabelString, s, NULL);
        XmStringFree(s);
    }

    if (read_mem_and_swap_percent(&ram_percent, &swap_percent,
                                  &ram_used_gb, &swap_used_gb) == 0) {
        VerticalMeterSetValue(meters[METER_RAM],  ram_percent);
        VerticalMeterSetValue(meters[METER_SWAP], swap_percent);

        char buf_ram[32];
        snprintf(buf_ram, sizeof(buf_ram), "%.1f GB", ram_used_gb);
        XmString s_ram = XmStringCreateLocalized(buf_ram);
        XtVaSetValues(value_labels[METER_RAM], XmNlabelString, s_ram, NULL);
        XmStringFree(s_ram);

        char buf_swap[32];
        snprintf(buf_swap, sizeof(buf_swap), "%.1f GB", swap_used_gb);
        XmString s_swap = XmStringCreateLocalized(buf_swap);
        XtVaSetValues(value_labels[METER_SWAP], XmNlabelString, s_swap, NULL);
        XmStringFree(s_swap);
    }

    if (read_load_percent(&load1_percent, &load5_percent, &load15_percent,
                          &load1_raw, &load5_raw, &load15_raw) == 0) {
        /* Dynamically raise the maximum if any load value exceeds the default.
           Keep all three load meters on the same scale. */
        if (load1_percent > load_max) load_max = load1_percent;
        if (load5_percent > load_max) load_max = load5_percent;
        if (load15_percent > load_max) load_max = load15_percent;

        VerticalMeterSetMaximum(meters[METER_LOAD1],  load_max);
        VerticalMeterSetMaximum(meters[METER_LOAD5],  load_max);
        VerticalMeterSetMaximum(meters[METER_LOAD15], load_max);

        VerticalMeterSetDefaultMaximum(meters[METER_LOAD1],  LOAD_PERCENT_DEFAULT_MAX);
        VerticalMeterSetDefaultMaximum(meters[METER_LOAD5],  LOAD_PERCENT_DEFAULT_MAX);
        VerticalMeterSetDefaultMaximum(meters[METER_LOAD15], LOAD_PERCENT_DEFAULT_MAX);

        VerticalMeterSetValue(meters[METER_LOAD1],  load1_percent);
        VerticalMeterSetValue(meters[METER_LOAD5],  load5_percent);
        VerticalMeterSetValue(meters[METER_LOAD15], load15_percent);

        char buf1[32], buf5[32], buf15[32];
        snprintf(buf1, sizeof(buf1), "%.2f", load1_raw);
        snprintf(buf5, sizeof(buf5), "%.2f", load5_raw);
        snprintf(buf15, sizeof(buf15), "%.2f", load15_raw);

        XmString s1 = XmStringCreateLocalized(buf1);
        XmString s5 = XmStringCreateLocalized(buf5);
        XmString s15 = XmStringCreateLocalized(buf15);
        XtVaSetValues(value_labels[METER_LOAD1],  XmNlabelString, s1,  NULL);
        XtVaSetValues(value_labels[METER_LOAD5],  XmNlabelString, s5,  NULL);
        XtVaSetValues(value_labels[METER_LOAD15], XmNlabelString, s15, NULL);
        XmStringFree(s1);
        XmStringFree(s5);
        XmStringFree(s15);
    }

    /* Re-arm timer */
    XtAppAddTimeOut(app_context, UPDATE_INTERVAL_MS, update_meters_cb, NULL);
}

static void wm_delete_callback(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    XtAppContext app = (XtAppContext)client_data;
    XtAppSetExitFlag(app);
}

/* ---------- Session handling ---------- */

static void init_exec_path(const char *argv0)
{
    ssize_t len = readlink("/proc/self/exe", g_exec_path,
                           sizeof(g_exec_path) - 1);
    if (len > 0) {
        g_exec_path[len] = '\0';
        return;
    }

    if (argv0 && argv0[0]) {
        if (argv0[0] == '/') {
            strncpy(g_exec_path, argv0, sizeof(g_exec_path) - 1);
            g_exec_path[sizeof(g_exec_path) - 1] = '\0';
            return;
        }

        if (strchr(argv0, '/')) {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd))) {
                size_t cwd_len = strlen(cwd);
                size_t argv_len = strlen(argv0);
                size_t needed = cwd_len + 1 + argv_len + 1;
                if (needed <= sizeof(g_exec_path)) {
                    memcpy(g_exec_path, cwd, cwd_len);
                    g_exec_path[cwd_len] = '/';
                    memcpy(g_exec_path + cwd_len + 1, argv0, argv_len);
                    g_exec_path[cwd_len + 1 + argv_len] = '\0';
                    return;
                }
            }
        }

        strncpy(g_exec_path, argv0, sizeof(g_exec_path) - 1);
        g_exec_path[sizeof(g_exec_path) - 1] = '\0';
    }
}

static void session_save_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    (void)call_data;
    if (!session_data) return;

    session_capture_geometry(w, session_data, "x", "y", "w", "h");
    session_save(w, session_data, g_exec_path);
}

/* ---------- Main + UI setup ---------- */

int
main(int argc, char *argv[])
{
    Widget toplevel, main_form;
    XmString xm_title;
    static char *meter_labels[NUM_METERS] = {
        "CPU",
        "RAM",
        "Swap",
        "Load 1",
        "Load 5",
        "Load 15"
    };

    XtSetLanguageProc(NULL, NULL, NULL);

    toplevel = XtVaAppInitialize(
        &app_context,
        "LoadMeters",
        NULL, 0,
        &argc, argv,
        NULL,
        NULL
    );

    /* Session handling: parse -session, remember exec path */
    char *session_id = session_parse_argument(&argc, argv);
    session_data = session_data_create(session_id);
    free(session_id);
    init_exec_path(argv[0]);

    /* Main form, fractional positions used for equal-width columns */
    main_form = XtVaCreateManagedWidget(
        "mainForm",
        xmFormWidgetClass, toplevel,
        XmNfractionBase, NUM_METERS * 10, /* 10 units per column */
        NULL
    );

    for (int i = 0; i < NUM_METERS; ++i) {
        int left_pos  = i * 10;
        int right_pos = (i + 1) * 10;

        /* Column Form */
        Widget col_form = XtVaCreateManagedWidget(
            "colForm",
            xmFormWidgetClass, main_form,
            XmNleftAttachment,   XmATTACH_POSITION,
            XmNleftPosition,     left_pos,
            XmNrightAttachment,  XmATTACH_POSITION,
            XmNrightPosition,    right_pos,
            XmNtopAttachment,    XmATTACH_FORM,
            XmNbottomAttachment, XmATTACH_FORM,
            NULL
        );

        /* Column label */
        xm_title = XmStringCreateLocalized(meter_labels[i]);
        Widget label = XtVaCreateManagedWidget(
            "meterLabel",
            xmLabelGadgetClass, col_form,
            XmNlabelString,    xm_title,
            XmNalignment,      XmALIGNMENT_CENTER,
            XmNtopAttachment,  XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment,XmATTACH_FORM,
            NULL
        );
        XmStringFree(xm_title);

        /* Vertical meter below the label, filling remaining space */
        Arg args[8];
        Cardinal n = 0;
        /* Initial reasonable size; will be overridden by attachments */
        XtSetArg(args[n], XmNwidth,  40); n++;
        XtSetArg(args[n], XmNheight, 150); n++;

        Widget meter = VerticalMeterCreate(col_form, "verticalMeter", args, n);

        XtVaSetValues(
            meter,
            XmNtopAttachment,    XmATTACH_WIDGET,
            XmNtopWidget,        label,
            XmNleftAttachment,   XmATTACH_FORM,
            XmNrightAttachment,  XmATTACH_FORM,
            NULL
        );

        /* Value label at the bottom */
        Widget value_label = XtVaCreateManagedWidget(
            "valueLabel",
            xmLabelGadgetClass, col_form,
            XmNalignment,      XmALIGNMENT_CENTER,
            XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment,XmATTACH_FORM,
            NULL
        );
        XmString initial = XmStringCreateLocalized("-");
        XtVaSetValues(value_label, XmNlabelString, initial, NULL);
        XmStringFree(initial);

        /* Meter sits above the value label */
        XtVaSetValues(
            meter,
            XmNbottomAttachment, XmATTACH_WIDGET,
            XmNbottomWidget,     value_label,
            XmNbottomOffset,     2,
            NULL
        );

        /* Configure meter maxima and cell height */
        if (i == METER_LOAD1 || i == METER_LOAD5 || i == METER_LOAD15) {
            VerticalMeterSetMaximum(meter, LOAD_PERCENT_DEFAULT_MAX);
            VerticalMeterSetDefaultMaximum(meter, LOAD_PERCENT_DEFAULT_MAX);
        } else {
            VerticalMeterSetMaximum(meter, PERCENT_MAX);
        }
        VerticalMeterSetCellHeight(meter, 4); /* 0 = square cells in your implementation */

        meters[i] = meter;
        value_labels[i] = value_label;
    }

    /* Set an application icon or window title if desired */
    XtVaSetValues(toplevel,
                  XmNtitle, "System Load",
                  NULL);

    /* Session restore (geometry) */
    if (session_data && session_load(toplevel, session_data)) {
        session_apply_geometry(toplevel, session_data, "x", "y", "w", "h");
    }

    /* WM protocol handling */
    Atom wm_delete = XmInternAtom(XtDisplay(toplevel),
                                  "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(toplevel, wm_delete,
                            wm_delete_callback, (XtPointer)app_context);
    XmActivateWMProtocol(toplevel, wm_delete);

    Atom wm_save = XmInternAtom(XtDisplay(toplevel),
                                "WM_SAVE_YOURSELF", False);
    XmAddWMProtocolCallback(toplevel, wm_save,
                            session_save_cb, NULL);
    XmActivateWMProtocol(toplevel, wm_save);

    XtRealizeWidget(toplevel);

    /* Start periodic updates */
    XtAppAddTimeOut(app_context, UPDATE_INTERVAL_MS, update_meters_cb, NULL);

    XtAppMainLoop(app_context);
    return 0;
}
