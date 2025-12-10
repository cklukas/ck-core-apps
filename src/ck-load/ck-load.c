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
#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/LabelG.h>

#include "vertical_meter.h"

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
/* Allow load to go up to 400% (4x all cores fully loaded) */
#define LOAD_PERCENT_MAX 400

static Widget meters[NUM_METERS];
static XtAppContext app_context;

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
read_mem_and_swap_percent(int *out_ram_percent, int *out_swap_percent)
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

    if (mem_total > 0 && mem_available > 0) {
        unsigned long mem_used = mem_total - mem_available;
        double ram_percent = 100.0 * (double)mem_used / (double)mem_total;
        if (ram_percent < 0.0) ram_percent = 0.0;
        if (ram_percent > 100.0) ram_percent = 100.0;
        *out_ram_percent = (int)(ram_percent + 0.5);
    } else {
        *out_ram_percent = 0;
    }

    if (swap_total > 0) {
        unsigned long swap_used = swap_total - swap_free;
        double swap_percent = 100.0 * (double)swap_used / (double)swap_total;
        if (swap_percent < 0.0) swap_percent = 0.0;
        if (swap_percent > 100.0) swap_percent = 100.0;
        *out_swap_percent = (int)(swap_percent + 0.5);
    } else {
        *out_swap_percent = 0;
    }

    return 0;
}

/* ---------- Helper: load averages from /proc/loadavg ---------- */

static int
read_load_percent(int *out_l1, int *out_l5, int *out_l15)
{
    FILE *fp = fopen("/proc/loadavg", "r");
    if (!fp) return -1;

    double l1, l5, l15;
    if (fscanf(fp, "%lf %lf %lf", &l1, &l5, &l15) != 3) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    long n_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (n_cpus <= 0) n_cpus = 1;

    double scale = 100.0 / (double)n_cpus; /* 1.0 load per core = 100% */

    int p1  = (int)(l1  * scale + 0.5);
    int p5  = (int)(l5  * scale + 0.5);
    int p15 = (int)(l15 * scale + 0.5);

    /* Cap at LOAD_PERCENT_MAX */
    if (p1  < 0) p1  = 0;   if (p1  > LOAD_PERCENT_MAX) p1  = LOAD_PERCENT_MAX;
    if (p5  < 0) p5  = 0;   if (p5  > LOAD_PERCENT_MAX) p5  = LOAD_PERCENT_MAX;
    if (p15 < 0) p15 = 0;   if (p15 > LOAD_PERCENT_MAX) p15 = LOAD_PERCENT_MAX;

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

    if (read_cpu_usage_percent(&cpu_percent) == 0) {
        VerticalMeterSetValue(meters[METER_CPU], cpu_percent);
    }

    if (read_mem_and_swap_percent(&ram_percent, &swap_percent) == 0) {
        VerticalMeterSetValue(meters[METER_RAM],  ram_percent);
        VerticalMeterSetValue(meters[METER_SWAP], swap_percent);
    }

    if (read_load_percent(&load1_percent, &load5_percent, &load15_percent) == 0) {
        VerticalMeterSetValue(meters[METER_LOAD1],  load1_percent);
        VerticalMeterSetValue(meters[METER_LOAD5],  load5_percent);
        VerticalMeterSetValue(meters[METER_LOAD15], load15_percent);
    }

    /* Re-arm timer */
    XtAppAddTimeOut(app_context, UPDATE_INTERVAL_MS, update_meters_cb, NULL);
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
            XmNbottomAttachment, XmATTACH_FORM,
            NULL
        );

        /* Configure meter maxima and cell height */
        if (i == METER_LOAD1 || i == METER_LOAD5 || i == METER_LOAD15) {
            VerticalMeterSetMaximum(meter, LOAD_PERCENT_MAX);
        } else {
            VerticalMeterSetMaximum(meter, PERCENT_MAX);
        }
        VerticalMeterSetCellHeight(meter, 4); /* 0 = square cells in your implementation */

        meters[i] = meter;
    }

    /* Set an application icon or window title if desired */
    XtVaSetValues(toplevel,
                  XmNtitle, "System Load",
                  NULL);

    XtRealizeWidget(toplevel);

    /* Start periodic updates */
    XtAppAddTimeOut(app_context, UPDATE_INTERVAL_MS, update_meters_cb, NULL);

    XtAppMainLoop(app_context);
    return 0;
}
