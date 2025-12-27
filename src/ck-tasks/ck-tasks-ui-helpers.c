#include "ck-tasks-ui-helpers.h"

#include <Xm/Form.h>

#include <stdlib.h>
#include <string.h>

XmString tasks_ui_make_string(const char *text)
{
    return XmStringCreateLocalized((String)(text ? text : ""));
}

XmString *tasks_ui_make_string_array(const char *const strings[], int count)
{
    if (!strings || count <= 0) return NULL;
    XmString *result = (XmString *)calloc(count, sizeof(XmString));
    if (!result) return NULL;
    for (int i = 0; i < count; ++i) {
        result[i] = tasks_ui_make_string(strings[i]);
    }
    return result;
}

void tasks_ui_set_label_text(Widget widget, const char *text)
{
    if (!widget) return;
    XmString label = tasks_ui_make_string(text);
    XtVaSetValues(widget, XmNlabelString, label, NULL);
    XmStringFree(label);
}

Boolean tasks_ui_status_set_label_text(Widget label, char *cache, size_t cache_len, const char *text)
{
    if (!label || !cache || cache_len == 0) return False;
    const char *value = text ? text : "";
    if (strncmp(cache, value, cache_len) == 0) return False;
    strncpy(cache, value, cache_len - 1);
    cache[cache_len - 1] = '\0';
    XmString label_text = tasks_ui_make_string(value);
    XtVaSetValues(label, XmNlabelString, label_text, NULL);
    XmStringFree(label_text);
    return True;
}

Widget tasks_ui_create_page(TasksUi *ui, const char *name, TasksTab tab_number,
                            const char *title, const char *subtitle)
{
    (void)tab_number;
    (void)subtitle;
    XmString tab_label = tasks_ui_make_string(title);
    Widget page = XmCreateForm(ui->tab_stack, (String)(name ? name : "tasksPage"), NULL, 0);
    if (!page) {
        XmStringFree(tab_label);
        return NULL;
    }
    XtVaSetValues(page,
                  XmNtabLabelString, tab_label,
                  XmNfractionBase, 100,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  NULL);
    XmStringFree(tab_label);

    XtManageChild(page);
    return page;
}
