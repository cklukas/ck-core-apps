#ifndef CK_BROWSER_UI_BUILDER_H
#define CK_BROWSER_UI_BUILDER_H

#include <Xm/Xm.h>

#include "browser_tab.h"

namespace UiBuilder {

struct ToolbarHandles {
    Widget url_field = NULL;
    Widget home_button = NULL;
};

struct StatusBarHandles {
    Widget security_label = NULL;
};

struct MenuHandles {
    Widget bookmarks_menu = NULL;
};

XmString make_string(const char *text);

Widget create_menu_item(Widget parent, const char *name, const char *label, Pixmap icon = XmUNSPECIFIED_PIXMAP);
Widget create_cascade_menu(Widget menu_bar, const char *label, const char *name, char mnemonic);
void set_menu_accelerator(Widget menu_item, const char *accel, const char *accel_text);

Widget createMenuBar(Widget parent, MenuHandles *handles_out);
Widget createToolbar(Widget parent, Widget attach_top, ToolbarHandles *handles_out);
Widget createStatusBar(Widget parent, StatusBarHandles *handles_out);

void UiBuilderInit();

} // namespace UiBuilder

#endif // CK_BROWSER_UI_BUILDER_H
