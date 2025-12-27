#include "ck-tasks-tabs.h"
#include "ck-tasks-ui-helpers.h"

#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/List.h>
#include <Xm/RowColumn.h>
#include <Xm/ScrolledW.h>

#include <stdlib.h>

static const char *network_samples[] = {
    "eth0: 192.168.1.12 -> 10.0.0.5 (UDP)  10 MB/s",
    "eth0: 192.168.1.12 -> 172.16.0.3 (TCP)  2 MB/s",
    "wlan0: 10.0.0.52 -> 52.15.5.1 (HTTPS) 1.2 MB/s",
    "wlan0: adapter idle",
};

static void add_networking_tab_content(TasksUi *ui, Widget page)
{
    (void)ui;
    Widget summary_box = XmCreateRowColumn(page, "networkSummary", NULL, 0);
    XtVaSetValues(summary_box,
                  XmNorientation, XmHORIZONTAL,
                  XmNspacing, 14,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNtopOffset, 12,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNleftOffset, 12,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNrightOffset, 12,
                  XmNpacking, XmPACK_TIGHT,
                  NULL);
    XtManageChild(summary_box);

    XmString tx = tasks_ui_make_string("Tx: 12 MB/s");
    XtVaCreateManagedWidget(
        "netTxLabel",
        xmLabelGadgetClass, summary_box,
        XmNlabelString, tx,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL);
    XmStringFree(tx);

    XmString rx = tasks_ui_make_string("Rx: 3.6 MB/s");
    XtVaCreateManagedWidget(
        "netRxLabel",
        xmLabelGadgetClass, summary_box,
        XmNlabelString, rx,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL);
    XmStringFree(rx);

    Arg scroll_args[10];
    int sn = 0;
    XtSetArg(scroll_args[sn], XmNtopAttachment, XmATTACH_WIDGET); sn++;
    XtSetArg(scroll_args[sn], XmNtopWidget, summary_box); sn++;
    XtSetArg(scroll_args[sn], XmNtopOffset, 10); sn++;
    XtSetArg(scroll_args[sn], XmNbottomAttachment, XmATTACH_FORM); sn++;
    XtSetArg(scroll_args[sn], XmNbottomOffset, 10); sn++;
    XtSetArg(scroll_args[sn], XmNleftAttachment, XmATTACH_FORM); sn++;
    XtSetArg(scroll_args[sn], XmNrightAttachment, XmATTACH_FORM); sn++;
    XtSetArg(scroll_args[sn], XmNleftOffset, 12); sn++;
    XtSetArg(scroll_args[sn], XmNrightOffset, 12); sn++;
    XtSetArg(scroll_args[sn], XmNscrollingPolicy, XmAUTOMATIC); sn++;
    Widget scroll = XmCreateScrolledWindow(page, "networkListScroll", scroll_args, sn);
    XtManageChild(scroll);

    Widget list = XmCreateScrolledList(scroll, "networkEntries", NULL, 0);
    XtVaSetValues(list,
                  XmNvisibleItemCount, 5,
                  XmNselectionPolicy, XmSINGLE_SELECT,
                  XmNscrollBarDisplayPolicy, XmAS_NEEDED,
                  NULL);

    size_t sample_count = sizeof(network_samples) / sizeof(network_samples[0]);
    XmString *items = tasks_ui_make_string_array(network_samples, (int)sample_count);
    if (items) {
        XmListAddItems(list, items, (Cardinal)sample_count, 0);
        for (size_t i = 0; i < sample_count; ++i) {
            XmStringFree(items[i]);
        }
        free(items);
    }

    XtManageChild(list);
}

Widget tasks_ui_create_networking_tab(TasksUi *ui)
{
    Widget page = tasks_ui_create_page(ui, "networkingPage", TASKS_TAB_NETWORKING,
                                       "Networking", "Adapter status, throughput logs, and connection list.");
    add_networking_tab_content(ui, page);
    return page;
}
