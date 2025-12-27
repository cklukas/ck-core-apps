#include "ck-tasks-tabs.h"
#include "ck-tasks-ui-helpers.h"

#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/LabelG.h>
#include <Xm/RowColumn.h>
#include <Xm/ToggleBG.h>

#include <stdio.h>

#include "../ck-load/vertical_meter.h"

static Widget create_meter_column(Widget parent, const char *label_text,
                                  Widget *out_meter, Widget *out_value_label)
{
    Widget col_form = XmCreateForm(parent, "meterColumn", NULL, 0);
    XtVaSetValues(col_form,
                  XmNfractionBase, 100,
                  XmNmarginWidth, 4,
                  XmNmarginHeight, 4,
                  NULL);
    XtManageChild(col_form);

    XmString label = tasks_ui_make_string(label_text);
    Widget label_widget = XtVaCreateManagedWidget(
        "meterLabel",
        xmLabelGadgetClass, col_form,
        XmNlabelString, label,
        XmNalignment, XmALIGNMENT_CENTER,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNtopOffset, 2,
        NULL);
    XmStringFree(label);

    XmString initial = tasks_ui_make_string("-");
    Widget value_label = XtVaCreateManagedWidget(
        "meterValueLabel",
        xmLabelGadgetClass, col_form,
        XmNlabelString, initial,
        XmNalignment, XmALIGNMENT_CENTER,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomOffset, 2,
        NULL);
    XmStringFree(initial);

    Arg args[8];
    int n = 0;
    XtSetArg(args[n], XmNwidth, 40); n++;
    XtSetArg(args[n], XmNheight, 150); n++;
    Widget meter = VerticalMeterCreate(col_form, "verticalMeter", args, n);
    XtVaSetValues(meter,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, label_widget,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_WIDGET,
                  XmNbottomWidget, value_label,
                  XmNbottomOffset, 4,
                  NULL);
    VerticalMeterSetMaximum(meter, 100);
    VerticalMeterSetDefaultMaximum(meter, 100);
    VerticalMeterSetCellHeight(meter, 4);
    if (out_meter) *out_meter = meter;
    if (out_value_label) *out_value_label = value_label;
    XtManageChild(meter);
    return col_form;
}

static void add_performance_tab_content(TasksUi *ui, Widget page)
{
    Widget mode_box = XmCreateRowColumn(page, "cpuModeBox", NULL, 0);
    XtVaSetValues(mode_box,
                  XmNorientation, XmHORIZONTAL,
                  XmNpacking, XmPACK_COLUMN,
                  XmNspacing, 12,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNtopOffset, 12,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNleftOffset, 12,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNrightOffset, 12,
                  XmNradioBehavior, True,
                  NULL);
    XmString overall = tasks_ui_make_string("Overall chart");
    XtVaCreateManagedWidget(
        "cpuOverallMode",
        xmToggleButtonGadgetClass, mode_box,
        XmNlabelString, overall,
        XmNset, True,
        XmNindicatorType, XmONE_OF_MANY,
        NULL);
    XmStringFree(overall);

    XmString divided = tasks_ui_make_string("Divided per-core chart");
    XtVaCreateManagedWidget(
        "cpuPerCoreMode",
        xmToggleButtonGadgetClass, mode_box,
        XmNlabelString, divided,
        XmNindicatorType, XmONE_OF_MANY,
        NULL);
    XmStringFree(divided);
    XtManageChild(mode_box);

    Widget meter_row = XmCreateForm(page, "performanceMeters", NULL, 0);
    XtVaSetValues(meter_row,
                  XmNfractionBase, 100,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, mode_box,
                  XmNtopOffset, 10,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNleftOffset, 12,
                  XmNrightOffset, 12,
                  NULL);
    XtManageChild(meter_row);

    Widget columns[5];
    columns[0] = create_meter_column(meter_row, "CPU", &ui->perf_cpu_meter, &ui->perf_cpu_value_label);
    columns[1] = create_meter_column(meter_row, "Memory", &ui->perf_mem_meter, &ui->perf_mem_value_label);
    columns[2] = create_meter_column(meter_row, "Load 1m", &ui->perf_load1_meter, &ui->perf_load1_value_label);
    columns[3] = create_meter_column(meter_row, "Load 5m", &ui->perf_load5_meter, &ui->perf_load5_value_label);
    columns[4] = create_meter_column(meter_row, "Load 15m", &ui->perf_load15_meter, &ui->perf_load15_value_label);

    for (int i = 0; i < 5; ++i) {
        if (!columns[i]) continue;
        XtVaSetValues(columns[i],
                      XmNtopAttachment, XmATTACH_FORM,
                      XmNbottomAttachment, XmATTACH_FORM,
                      XmNleftAttachment, XmATTACH_POSITION,
                      XmNleftPosition, i * 20,
                      XmNrightAttachment, XmATTACH_POSITION,
                      XmNrightPosition, (i + 1) * 20,
                      NULL);
    }

    Widget history_frame = XmCreateFrame(page, "historyFrame", NULL, 0);
    XtVaSetValues(history_frame,
                  XmNshadowType, XmSHADOW_ETCHED_IN,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, meter_row,
                  XmNtopOffset, 12,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNleftOffset, 10,
                  XmNrightOffset, 10,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNbottomOffset, 10,
                  NULL);
    XtManageChild(history_frame);

    XmString history_text = tasks_ui_make_string("CPU history chart will alternate between overall and per-core views when the user toggles a mode or double-clicks the plot area.");
    XtVaCreateManagedWidget(
        "historyLabel",
        xmLabelGadgetClass, history_frame,
        XmNlabelString, history_text,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNmarginHeight, 8,
        XmNmarginWidth, 8,
        NULL);
    XmStringFree(history_text);
}

Widget tasks_ui_create_performance_tab(TasksUi *ui)
{
    Widget page = tasks_ui_create_page(ui, "performancePage", TASKS_TAB_PERFORMANCE,
                                       "Performance", "CPU / memory meters and graphs will live here.");
    add_performance_tab_content(ui, page);
    return page;
}

void tasks_ui_update_system_stats(TasksUi *ui, const TasksSystemStats *stats)
{
    if (!ui || !stats) return;
    char buffer[32];
    if (ui->perf_cpu_meter) {
        VerticalMeterSetValue(ui->perf_cpu_meter, stats->cpu_percent);
    }
    if (ui->perf_cpu_value_label) {
        snprintf(buffer, sizeof(buffer), "%d%%", stats->cpu_percent);
        tasks_ui_set_label_text(ui->perf_cpu_value_label, buffer);
    }
    if (ui->perf_mem_meter) {
        VerticalMeterSetValue(ui->perf_mem_meter, stats->memory_percent);
    }
    if (ui->perf_mem_value_label) {
        snprintf(buffer, sizeof(buffer), "%d%%", stats->memory_percent);
        tasks_ui_set_label_text(ui->perf_mem_value_label, buffer);
    }
    if (ui->status_cpu_label) {
        snprintf(buffer, sizeof(buffer), "CPU Usage: %d%%", stats->cpu_percent);
        tasks_ui_status_set_label_text(ui->status_cpu_label, ui->status_cpu_text,
                                       sizeof(ui->status_cpu_text), buffer);
    }
    if (ui->status_memory_label) {
        double used_gb = (double)stats->mem_used_kb / (1024.0 * 1024.0);
        double total_gb = (double)stats->mem_total_kb / (1024.0 * 1024.0);
        char mem_buffer[128];
        snprintf(mem_buffer, sizeof(mem_buffer),
                 "Physical Memory: %d%% (%.1f GB/%.1f GB)",
                 stats->memory_percent, used_gb, total_gb);
        tasks_ui_status_set_label_text(ui->status_memory_label, ui->status_memory_text,
                                       sizeof(ui->status_memory_text), mem_buffer);
    }
    int load_max = 100;
    if (stats->load1_percent > load_max) load_max = stats->load1_percent;
    if (stats->load5_percent > load_max) load_max = stats->load5_percent;
    if (stats->load15_percent > load_max) load_max = stats->load15_percent;
    if (ui->perf_load1_meter) {
        VerticalMeterSetMaximum(ui->perf_load1_meter, load_max);
        VerticalMeterSetValue(ui->perf_load1_meter, stats->load1_percent);
    }
    if (ui->perf_load1_value_label) {
        snprintf(buffer, sizeof(buffer), "%d%%", stats->load1_percent);
        tasks_ui_set_label_text(ui->perf_load1_value_label, buffer);
    }
    if (ui->perf_load5_meter) {
        VerticalMeterSetMaximum(ui->perf_load5_meter, load_max);
        VerticalMeterSetValue(ui->perf_load5_meter, stats->load5_percent);
    }
    if (ui->perf_load5_value_label) {
        snprintf(buffer, sizeof(buffer), "%d%%", stats->load5_percent);
        tasks_ui_set_label_text(ui->perf_load5_value_label, buffer);
    }
    if (ui->perf_load15_meter) {
        VerticalMeterSetMaximum(ui->perf_load15_meter, load_max);
        VerticalMeterSetValue(ui->perf_load15_meter, stats->load15_percent);
    }
    if (ui->perf_load15_value_label) {
        snprintf(buffer, sizeof(buffer), "%d%%", stats->load15_percent);
        tasks_ui_set_label_text(ui->perf_load15_value_label, buffer);
    }
}
