#include "about_dialog.h"

#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/Notebook.h>
#include <Xm/PushB.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <Dt/Dt.h>

static void run_cmd_single_line(const char *cmd, char *buf, size_t size)
{
    buf[0] = '\0';
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        snprintf(buf, size, "n/a");
        return;
    }

    if (!fgets(buf, size, fp)) {
        snprintf(buf, size, "n/a");
        pclose(fp);
        return;
    }
    pclose(fp);

    size_t len = strlen(buf);
    while (len > 0 &&
           (buf[len-1] == '\n' || buf[len-1] == ' ' || buf[len-1] == '\t')) {
        buf[--len] = '\0';
    }
}

int about_get_os_fields(AboutField fields[], int max_fields)
{
    if (max_fields < 4) return 0;

    fields[0].label = "Distributor ID";
    run_cmd_single_line("lsb_release -i -s 2>/dev/null",
                        fields[0].value, sizeof(fields[0].value));

    fields[1].label = "Description";
    run_cmd_single_line("lsb_release -d -s 2>/dev/null",
                        fields[1].value, sizeof(fields[1].value));

    fields[2].label = "Release";
    run_cmd_single_line("lsb_release -r -s 2>/dev/null",
                        fields[2].value, sizeof(fields[2].value));

    fields[3].label = "Codename";
    run_cmd_single_line("lsb_release -c -s 2>/dev/null",
                        fields[3].value, sizeof(fields[3].value));

    return 4;
}

int about_get_cde_fields(AboutField fields[], int max_fields)
{
    if (max_fields < 4) return 0;

    fields[0].label = "Version";
    snprintf(fields[0].value, sizeof(fields[0].value), "%s", DtVERSION_STRING);

    fields[1].label = "Major / Revision / Update";
    snprintf(fields[1].value, sizeof(fields[1].value),
             "%d / %d / %d", DtVERSION, DtREVISION, DtUPDATE_LEVEL);

    fields[2].label = "Version Number";
    snprintf(fields[2].value, sizeof(fields[2].value), "%d", DtVERSION_NUMBER);

    fields[3].label = "DtVersion (runtime)";
    snprintf(fields[3].value, sizeof(fields[3].value), "%d", DtVERSION_NUMBER);

    return 4;
}

void about_add_title_page(Widget notebook, int page_number,
                          const char *page_name,
                          const char *tab_label,
                          const char *title_text,
                          const char *subtitle_text)
{
    Arg args[8];
    int n = 0;
    XtSetArg(args[n], XmNfractionBase, 1); n++;
    Widget page = XmCreateForm(notebook, page_name ? (char *)page_name : "titlePage", args, n);
    XtManageChild(page);

    Display *dpy = XtDisplay(notebook);
    XFontStruct *bold_fs = XLoadQueryFont(dpy, "-*-helvetica-bold-r-normal-*-24-*");
    XmFontList bold_fontlist = NULL;
    if (bold_fs) {
        bold_fontlist = XmFontListCreate(bold_fs, XmFONTLIST_DEFAULT_TAG);
    }

    Widget label_title;
    n = 0;
    XtSetArg(args[n], XmNtopAttachment,    XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNalignment,        XmALIGNMENT_CENTER); n++;
    XtSetArg(args[n], XmNtopOffset,        16); n++;
    label_title = XmCreateLabel(page, "label_title", args, n);

    XtVaSetValues(label_title,
                  XmNlabelString, XmStringCreateLocalized((char *)(title_text ? title_text : "")),
                  NULL);
    if (bold_fontlist) {
        XtVaSetValues(label_title,
                      XmNfontList, bold_fontlist,
                      NULL);
    }
    XtManageChild(label_title);

    if (subtitle_text && subtitle_text[0]) {
        Widget label_copy;
        n = 0;
        XtSetArg(args[n], XmNtopAttachment,    XmATTACH_WIDGET); n++;
        XtSetArg(args[n], XmNtopWidget,        label_title); n++;
        XtSetArg(args[n], XmNtopOffset,        8); n++;
        XtSetArg(args[n], XmNleftAttachment,   XmATTACH_FORM); n++;
        XtSetArg(args[n], XmNrightAttachment,  XmATTACH_FORM); n++;
        XtSetArg(args[n], XmNalignment,        XmALIGNMENT_CENTER); n++;
        label_copy = XmCreateLabel(page, "label_subtitle", args, n);

        XtVaSetValues(label_copy,
                      XmNlabelString, XmStringCreateLocalized((char *)subtitle_text),
                      NULL);
        XtManageChild(label_copy);
    }

    Widget tab = XmCreatePushButton(notebook, tab_label ? (char *)tab_label : "tab", NULL, 0);
    XtVaSetValues(tab,
                  XmNlabelString, XmStringCreateLocalized((char *)(tab_label ? tab_label : "")),
                  NULL);
    XtManageChild(tab);

    XtVaSetValues(page,
                  XmNnotebookChildType, XmPAGE,
                  XmNpageNumber,        page_number,
                  NULL);

    XtVaSetValues(tab,
                  XmNnotebookChildType, XmMAJOR_TAB,
                  XmNpageNumber,        page_number,
                  NULL);
}

void about_add_table_page(Widget notebook, int page_number,
                          const char *page_name,
                          const char *tab_label,
                          AboutField fields[], int field_count)
{
    if (!notebook || !fields || field_count <= 0) return;

    Arg args[8];
    int n = 0;
    XtSetArg(args[n], XmNfractionBase, 100); n++;
    Widget page = XmCreateForm(notebook, page_name ? (char *)page_name : "tablePage", args, n);
    XtManageChild(page);

    Widget prev_row = NULL;
    for (int i = 0; i < field_count; ++i) {
        Widget key_label, value_label;

        n = 0;
        if (prev_row == NULL) {
            XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
            XtSetArg(args[n], XmNtopOffset,     8); n++;
        } else {
            XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
            XtSetArg(args[n], XmNtopWidget,     prev_row); n++;
            XtSetArg(args[n], XmNtopOffset,     4); n++;
        }
        XtSetArg(args[n], XmNleftAttachment,  XmATTACH_POSITION); n++;
        XtSetArg(args[n], XmNleftPosition,    5); n++;
        XtSetArg(args[n], XmNrightAttachment, XmATTACH_POSITION); n++;
        XtSetArg(args[n], XmNrightPosition,   40); n++;
        XtSetArg(args[n], XmNalignment,       XmALIGNMENT_END); n++;
        key_label = XmCreateLabel(page, "table_key", args, n);

        XmString key_xms = XmStringCreateLocalized((char*)fields[i].label);
        XtVaSetValues(key_label,
                      XmNlabelString, key_xms,
                      NULL);
        XmStringFree(key_xms);
        XtManageChild(key_label);

        n = 0;
        XtSetArg(args[n], XmNtopAttachment,   XmATTACH_OPPOSITE_WIDGET); n++;
        XtSetArg(args[n], XmNtopWidget,       key_label); n++;
        XtSetArg(args[n], XmNleftAttachment,  XmATTACH_POSITION); n++;
        XtSetArg(args[n], XmNleftPosition,    45); n++;
        XtSetArg(args[n], XmNrightAttachment, XmATTACH_POSITION); n++;
        XtSetArg(args[n], XmNrightPosition,   95); n++;
        XtSetArg(args[n], XmNalignment,       XmALIGNMENT_BEGINNING); n++;
        value_label = XmCreateLabel(page, "table_value", args, n);

        XmString value_xms = XmStringCreateLocalized(fields[i].value);
        XtVaSetValues(value_label,
                      XmNlabelString, value_xms,
                      NULL);
        XmStringFree(value_xms);
        XtManageChild(value_label);

        prev_row = key_label;
    }

    Widget tab = XmCreatePushButton(notebook, tab_label ? (char *)tab_label : "tab", NULL, 0);
    XtVaSetValues(tab,
                  XmNlabelString, XmStringCreateLocalized((char *)(tab_label ? tab_label : "")),
                  NULL);
    XtManageChild(tab);

    XtVaSetValues(page,
                  XmNnotebookChildType, XmPAGE,
                  XmNpageNumber,        page_number,
                  NULL);

    XtVaSetValues(tab,
                  XmNnotebookChildType, XmMAJOR_TAB,
                  XmNpageNumber,        page_number,
                  NULL);
}

int about_add_standard_pages(Widget notebook, int start_page,
                             const char *app_tab_label,
                             const char *app_title,
                             const char *app_subtitle,
                             Boolean include_ck_core_tab)
{
    int page = start_page;
    if (app_title) {
        about_add_title_page(notebook, page++, "page_app_about",
                             app_tab_label ? app_tab_label : "About",
                             app_title,
                             app_subtitle ? app_subtitle : "");
    }

    if (include_ck_core_tab) {
        about_add_title_page(notebook, page++, "page_ckcore", "CK-Core",
                             "CK-Core",
                             "(c) 2025-2026 by Dr. C. Klukas");
    }

    AboutField cde_fields[4];
    int cde_count = about_get_cde_fields(cde_fields, 4);
    about_add_table_page(notebook, page++, "page_cde", "CDE",
                         cde_fields, cde_count);

    AboutField os_fields[4];
    int os_count = about_get_os_fields(os_fields, 4);
    about_add_table_page(notebook, page++, "page_os", "OS",
                         os_fields, os_count);

    return page;
}
