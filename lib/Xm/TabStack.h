#ifndef XM_TABSTACK_H
#define XM_TABSTACK_H

#include <Xm/Xm.h>

Widget XmCreateTabStack(Widget parent, String name, ArgList args, Cardinal argCount);
Widget XmTabStackAddPage(Widget tabstack, String name, XmString label, Boolean selected);
void XmTabStackSetCurrentPage(Widget tabstack, int page);
int XmTabStackGetCurrentPage(Widget tabstack);

#endif /* XM_TABSTACK_H */
