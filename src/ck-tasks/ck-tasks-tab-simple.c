#include "ck-tasks-tabs.h"
#include "ck-tasks-ui-helpers.h"

#include <Xm/LabelG.h>

Widget tasks_ui_create_simple_tab(TasksUi *ui, TasksTab tab, const char *name,
                                  const char *title, const char *description)
{
    (void)description;
    Widget page = tasks_ui_create_page(ui, name, tab, title, description);
    XmString placeholder = tasks_ui_make_string("Content placeholder for this tab.");
    XtVaCreateManagedWidget(
        "tabPlaceholder",
        xmLabelGadgetClass, page,
        XmNlabelString, placeholder,
        XmNalignment, XmALIGNMENT_CENTER,
        XmNtopAttachment, XmATTACH_FORM,
        XmNtopOffset, 14,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftOffset, 8,
        XmNrightOffset, 8,
        XmNbottomOffset, 8,
        NULL);
    XmStringFree(placeholder);
    return page;
}
