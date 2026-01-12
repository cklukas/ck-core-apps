#include "ui_builder.h"

#include <Xm/ArrowB.h>
#include <Xm/CascadeBG.h>
#include <Xm/Frame.h>
#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/MenuShell.h>
#include <Xm/PushBG.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/SeparatoG.h>
#include <Xm/TextF.h>
#include <Xm/ToggleB.h>

#include "tab_manager.h"
#include "browser_ui_bridge.h"

extern void on_new_window(Widget, XtPointer, XtPointer);
extern void on_new_tab(Widget, XtPointer, XtPointer);
extern void on_close_tab(Widget, XtPointer, XtPointer);
extern void on_open_file_menu(Widget, XtPointer, XtPointer);
extern void on_add_bookmark_menu(Widget, XtPointer, XtPointer);
extern void on_open_bookmark_manager_menu(Widget, XtPointer, XtPointer);
extern void on_help_view(Widget, XtPointer, XtPointer);
extern void on_help_about(Widget, XtPointer, XtPointer);
extern void on_back(Widget, XtPointer, XtPointer);
extern void on_forward(Widget, XtPointer, XtPointer);
extern void on_reload(Widget, XtPointer, XtPointer);
extern void on_home(Widget, XtPointer, XtPointer);
extern void on_home_button_press(Widget, XtPointer, XEvent *, Boolean *);
extern void on_url_activate(Widget, XtPointer, XtPointer);
extern void on_url_focus(Widget, XtPointer, XtPointer);
extern void on_url_button_press(Widget, XtPointer, XEvent *, Boolean *);
extern void on_zoom_in(Widget, XtPointer, XtPointer);
extern void on_zoom_out(Widget, XtPointer, XtPointer);
extern void on_zoom_reset(Widget, XtPointer, XtPointer);
extern void on_menu_exit(Widget, XtPointer, XtPointer);
extern void on_enter_url(Widget, XtPointer, XtPointer);
extern void on_go_back_menu(Widget, XtPointer, XtPointer);
extern void on_go_forward_menu(Widget, XtPointer, XtPointer);
extern void on_reload_menu(Widget, XtPointer, XtPointer);
extern void on_restore_session(Widget, XtPointer, XtPointer);
extern char *xm_name(const char *name);
extern const char *kInitialBrowserUrl;

namespace {
static XmString make_string_internal(const char *text)
{
    return XmStringCreateLocalized((String)(text ? text : ""));
}

static Widget create_menu_item_internal(Widget parent, const char *name, const char *label, Pixmap icon)
{
    if (!parent) return NULL;
    XmString xm_label = make_string_internal(label);
    Widget item = XtVaCreateManagedWidget(name,
                                          xmPushButtonGadgetClass,
                                          parent,
                                          XmNlabelString, xm_label,
                                          NULL);
    XmStringFree(xm_label);
    if (icon != XmUNSPECIFIED_PIXMAP) {
        XtVaSetValues(item, XmNlabelType, XmPIXMAP, XmNlabelPixmap, icon, NULL);
    }
    return item;
}

static Widget create_status_segment(Widget parent, const char *name, const char *text, Widget *out_label)
{
    Widget frame = XmCreateFrame(parent, (String)name, NULL, 0);
    XtVaSetValues(frame,
                  XmNshadowType, XmSHADOW_IN,
                  XmNmarginWidth, 4,
                  XmNmarginHeight, 2,
                  NULL);
    XtManageChild(frame);

    XmString xm_text = make_string_internal(text);
    Widget label = XtVaCreateManagedWidget(
        "statusLabel",
        xmLabelGadgetClass, frame,
        XmNlabelString, xm_text,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNmarginLeft, 4,
        XmNmarginRight, 4,
        NULL);
    XmStringFree(xm_text);
    if (out_label) {
        *out_label = label;
    }
    return frame;
}

static Widget create_zoom_segment(Widget parent)
{
    Widget frame = XmCreateFrame(parent, (String)"statusZoom", NULL, 0);
    XtVaSetValues(frame,
                  XmNshadowType, XmSHADOW_IN,
                  XmNmarginWidth, 4,
                  XmNmarginHeight, 2,
                  NULL);
    XtManageChild(frame);

    Widget row = XmCreateRowColumn(frame, xm_name("statusZoomRow"), NULL, 0);
    XtVaSetValues(row,
                  XmNorientation, XmHORIZONTAL,
                  XmNpacking, XmPACK_TIGHT,
                  XmNspacing, 6,
                  XmNmarginWidth, 4,
                  XmNmarginHeight, 2,
                  NULL);
    XtManageChild(row);

    XmString minus_label = make_string_internal("-");
    Widget minus_btn = XtVaCreateManagedWidget("zoomMinus", xmPushButtonWidgetClass, row,
                                               XmNlabelString, minus_label,
                                               XmNmarginWidth, 4,
                                               XmNmarginHeight, 0,
                                               NULL);
    XmStringFree(minus_label);
    XtAddCallback(minus_btn, XmNactivateCallback, on_zoom_out, NULL);

    XmString zoom_label = make_string_internal("Zoom: 100%");
    Widget zoom_btn = XtVaCreateManagedWidget("zoomReset", xmPushButtonWidgetClass, row,
                                              XmNlabelString, zoom_label,
                                              XmNshadowThickness, 0,
                                              XmNmarginWidth, 4,
                                              XmNmarginHeight, 0,
                                              NULL);
    XmStringFree(zoom_label);
    XtAddCallback(zoom_btn, XmNactivateCallback, on_zoom_reset, NULL);

    XmString plus_label = make_string_internal("+");
    Widget plus_btn = XtVaCreateManagedWidget("zoomPlus", xmPushButtonWidgetClass, row,
                                              XmNlabelString, plus_label,
                                              XmNmarginWidth, 4,
                                              XmNmarginHeight, 0,
                                              NULL);
    XmStringFree(plus_label);
    XtAddCallback(plus_btn, XmNactivateCallback, on_zoom_in, NULL);

    TabManager::instance().registerZoomControls(zoom_btn, minus_btn, plus_btn);

    return frame;
}
} // namespace

namespace UiBuilder {

XmString make_string(const char *text)
{
    return make_string_internal(text);
}

Widget create_menu_item(Widget parent, const char *name, const char *label, Pixmap icon)
{
    return create_menu_item_internal(parent, name, label, icon);
}

Widget create_cascade_menu(Widget menu_bar, const char *label, const char *name, char mnemonic)
{
    XmString xm_label = make_string(label);
    Widget menu = XmCreatePulldownMenu(menu_bar, const_cast<String>(name), NULL, 0);
    XtVaCreateManagedWidget(
        name,
        xmCascadeButtonGadgetClass,
        menu_bar,
        XmNlabelString, xm_label,
        XmNmnemonic, mnemonic,
        XmNsubMenuId, menu,
        NULL);
    XmStringFree(xm_label);
    return menu;
}

void set_menu_accelerator(Widget menu_item, const char *accel, const char *accel_text)
{
    if (!menu_item || (!accel && !accel_text)) return;
    XmString accel_label = accel_text ? make_string(accel_text) : XmStringCreateLocalized(const_cast<String>(accel));
    XtVaSetValues(menu_item, XmNaccelerator, const_cast<char *>(accel), XmNacceleratorText, accel_label, NULL);
    XmStringFree(accel_label);
}

Widget createMenuBar(Widget parent, MenuHandles *handles_out)
{
    Widget menu_bar = XmCreateMenuBar(parent, xm_name("browserMenuBar"), NULL, 0);
    XtVaSetValues(menu_bar,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  NULL);
    XtManageChild(menu_bar);

    Widget file_menu = create_cascade_menu(menu_bar, "File", "fileMenu", 'F');
    Widget file_new_window = create_menu_item(file_menu, "fileNewWindow", "New Window");
    Widget file_new_tab = create_menu_item(file_menu, "fileNewTab", "New Tab");
    Widget file_close_tab = create_menu_item(file_menu, "fileCloseTab", "Close Tab");
    Widget file_open_file = create_menu_item(file_menu, "fileOpenFile", "Open File...");
    XtVaCreateManagedWidget("fileSep", xmSeparatorGadgetClass, file_menu, NULL);
    Widget file_exit = create_menu_item(file_menu, "fileExit", "Exit");

    Widget nav_menu = create_cascade_menu(menu_bar, "Navigate", "navigateMenu", 'N');
    Widget nav_back = create_menu_item(nav_menu, "navBack", "Go Back");
    Widget nav_forward = create_menu_item(nav_menu, "navForward", "Go Forward");
    Widget nav_reload = create_menu_item(nav_menu, "navReload", "Reload Page");
    Widget nav_open_url = create_menu_item(nav_menu, "navOpenUrl", "Open URL");
    Widget nav_restore = create_menu_item(nav_menu, "navRestoreSession", "Restore Session");

    Widget bookmarks_menu = create_cascade_menu(menu_bar, "Bookmarks", "bookmarksMenu", 'B');
    Widget bookmark_add = create_menu_item(bookmarks_menu, "bookmarkAdd", "Add Page...");
    Widget bookmark_open_manager = create_menu_item(bookmarks_menu, "bookmarkOpenManager", "Open Bookmark Manager...");
    XtVaCreateManagedWidget("bookmarkFavoritesSep", xmSeparatorGadgetClass, bookmarks_menu, NULL);

    Widget view_menu = create_cascade_menu(menu_bar, "View", "viewMenu", 'V');
    Widget view_zoom_in = create_menu_item(view_menu, "viewZoomIn", "Zoom In");
    Widget view_zoom_out = create_menu_item(view_menu, "viewZoomOut", "Zoom Out");
    Widget view_zoom_reset = create_menu_item(view_menu, "viewZoomReset", "Reset Zoom");
    set_menu_accelerator(view_zoom_in, "Ctrl<Key>plus", "Ctrl+Plus");
    set_menu_accelerator(view_zoom_out, "Ctrl<Key>minus", "Ctrl+Minus");
    set_menu_accelerator(view_zoom_reset, "Ctrl<Key>0", "Ctrl+0");
    XtVaCreateManagedWidget("viewSep1", xmSeparatorGadgetClass, view_menu, NULL);

    Widget help_menu = XmCreatePulldownMenu(menu_bar, xm_name("helpMenu"), NULL, 0);
    XmString help_label = make_string("Help");
    Widget help_cascade = XtVaCreateManagedWidget(
        "helpCascade",
        xmCascadeButtonGadgetClass,
        menu_bar,
        XmNlabelString, help_label,
        XmNmnemonic, 'H',
        XmNsubMenuId, help_menu,
        NULL);
    XmStringFree(help_label);
    XtVaSetValues(menu_bar, XmNmenuHelpWidget, help_cascade, NULL);

    Widget help_view = create_menu_item(help_menu, "helpView", "View Help");
    Widget help_about = create_menu_item(help_menu, "helpAbout", "About");

    set_menu_accelerator(file_new_window, "Ctrl<Key>N", "Ctrl+N");
    set_menu_accelerator(file_new_tab, "Ctrl<Key>T", "Ctrl+T");
    set_menu_accelerator(file_close_tab, "Ctrl<Key>W", "Ctrl+W");
    set_menu_accelerator(file_open_file, "Ctrl<Key>O", "Ctrl+O");
    set_menu_accelerator(file_exit, "Alt<Key>F4", "Alt+F4");
    set_menu_accelerator(nav_back, "Alt<Key>osfLeft", "Alt+Left");
    set_menu_accelerator(nav_forward, "Alt<Key>osfRight", "Alt+Right");
    set_menu_accelerator(nav_reload, "Ctrl<Key>R", "Ctrl+R");
    set_menu_accelerator(nav_open_url, "Ctrl<Key>L", "Ctrl+L");

    XtVaSetValues(file_new_window, XmNmnemonic, 'N', NULL);
    XtVaSetValues(file_new_tab, XmNmnemonic, 'T', NULL);
    XtVaSetValues(file_close_tab, XmNmnemonic, 'C', NULL);
    XtVaSetValues(file_open_file, XmNmnemonic, 'O', NULL);
    XtVaSetValues(file_exit, XmNmnemonic, 'X', NULL);
    XtVaSetValues(nav_back, XmNmnemonic, 'B', NULL);
    XtVaSetValues(nav_forward, XmNmnemonic, 'F', NULL);
    XtVaSetValues(nav_reload, XmNmnemonic, 'R', NULL);
    XtVaSetValues(nav_open_url, XmNmnemonic, 'O', NULL);

    XtAddCallback(file_new_window, XmNactivateCallback, on_new_window, NULL);
    XtAddCallback(file_new_tab, XmNactivateCallback, on_new_tab, NULL);
    XtAddCallback(file_close_tab, XmNactivateCallback, on_close_tab, NULL);
    XtAddCallback(file_open_file, XmNactivateCallback, on_open_file_menu, NULL);
    XtAddCallback(bookmark_add, XmNactivateCallback, on_add_bookmark_menu, NULL);
    XtAddCallback(bookmark_open_manager, XmNactivateCallback, on_open_bookmark_manager_menu, NULL);
    XtAddCallback(help_view, XmNactivateCallback, on_help_view, NULL);
    XtAddCallback(help_about, XmNactivateCallback, on_help_about, NULL);
    XtAddCallback(nav_open_url, XmNactivateCallback, on_enter_url, NULL);
    XtAddCallback(file_exit, XmNactivateCallback, on_menu_exit, NULL);
    XtAddCallback(nav_back, XmNactivateCallback, on_go_back_menu, NULL);
    XtAddCallback(nav_forward, XmNactivateCallback, on_go_forward_menu, NULL);
    XtAddCallback(nav_reload, XmNactivateCallback, on_reload_menu, NULL);
    XtAddCallback(nav_restore, XmNactivateCallback, on_restore_session, NULL);
    XtAddCallback(view_zoom_in, XmNactivateCallback, on_zoom_in, NULL);
    XtAddCallback(view_zoom_out, XmNactivateCallback, on_zoom_out, NULL);
    XtAddCallback(view_zoom_reset, XmNactivateCallback, on_zoom_reset, NULL);

    if (handles_out) {
        handles_out->bookmarks_menu = bookmarks_menu;
        handles_out->nav_back = nav_back;
        handles_out->nav_forward = nav_forward;
    }

    return menu_bar;
}

Widget createToolbar(Widget parent, Widget attach_top, const MenuHandles *menu_handles, ToolbarHandles *handles_out)
{
    Widget toolbar = XmCreateForm(parent, xm_name("browserToolbar"), NULL, 0);
    XtVaSetValues(toolbar,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, attach_top,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNtopOffset, 6,
                  XmNleftOffset, 10,
                  XmNrightOffset, 10,
                  NULL);
    XtManageChild(toolbar);

    Widget button_row = XmCreateRowColumn(toolbar, xm_name("toolbarButtons"), NULL, 0);
    XtVaSetValues(button_row,
                  XmNorientation, XmHORIZONTAL,
                  XmNpacking, XmPACK_TIGHT,
                  XmNspacing, 12,
                  XmNmarginWidth, 6,
                  XmNmarginHeight, 4,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  NULL);
    XtManageChild(button_row);

    XmString back_label = make_string("Back");
    Widget back_button = XtVaCreateManagedWidget("backButton", xmPushButtonWidgetClass, button_row,
                                                  XmNlabelString, back_label, NULL);
    XmStringFree(back_label);
    XtAddCallback(back_button, XmNactivateCallback, on_back, NULL);

    XmString forward_label = make_string("Forward");
    Widget forward_button = XtVaCreateManagedWidget("forwardButton", xmPushButtonWidgetClass, button_row,
                                                    XmNlabelString, forward_label, NULL);
    XmStringFree(forward_label);
    XtAddCallback(forward_button, XmNactivateCallback, on_forward, NULL);

    Widget nav_back_widget = NULL;
    Widget nav_forward_widget = NULL;
    if (menu_handles) {
        nav_back_widget = menu_handles->nav_back;
        nav_forward_widget = menu_handles->nav_forward;
    }
    TabManager::instance().registerNavigationWidgets(back_button,
                                                     forward_button,
                                                     nav_back_widget,
                                                     nav_forward_widget);

    XmString reload_label = make_string("Reload");
    Widget reload_button = XtVaCreateManagedWidget("reloadButton", xmPushButtonWidgetClass, button_row,
                                                   XmNlabelString, reload_label, NULL);
    XmStringFree(reload_label);
    XtAddCallback(reload_button, XmNactivateCallback, on_reload, NULL);
    TabManager::instance().registerReloadButton(reload_button);

    XmString home_label = make_string("Home");
    Widget home_button = XtVaCreateManagedWidget("homeButton", xmPushButtonWidgetClass, button_row,
                                                 XmNlabelString, home_label, NULL);
    XmStringFree(home_label);
    XtAddCallback(home_button, XmNactivateCallback, on_home, NULL);
    XtAddEventHandler(home_button, ButtonPressMask, False, on_home_button_press, NULL);

    int icon_size = desired_favicon_size();
    Widget favicon = XtVaCreateManagedWidget(
        "favicon",
        xmLabelGadgetClass, toolbar,
        XmNlabelType, XmPIXMAP,
        XmNlabelPixmap, XmUNSPECIFIED_PIXMAP,
        XmNrecomputeSize, False,
        XmNwidth, icon_size,
        XmNheight, icon_size,
        XmNalignment, XmALIGNMENT_CENTER,
        XmNleftAttachment, XmATTACH_WIDGET,
        XmNleftWidget, button_row,
        XmNleftOffset, 6,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        NULL);
    TabManager::instance().registerFaviconLabel(favicon);

    Widget url_field = XtVaCreateManagedWidget(
        "urlField",
        xmTextFieldWidgetClass, toolbar,
        XmNvalue, kInitialBrowserUrl,
        XmNresizable, True,
        XmNcolumns, 80,
        XmNeditable, True,
        XmNleftAttachment, XmATTACH_WIDGET,
        XmNleftWidget, favicon,
        XmNleftOffset, 8,
        XmNrightAttachment, XmATTACH_FORM,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        NULL);
    XtVaSetValues(url_field, XmNcursorPositionVisible, True, NULL);
    XtAddCallback(url_field, XmNactivateCallback, on_url_activate, NULL);
    XtAddCallback(url_field, XmNfocusCallback, on_url_focus, NULL);
    XtAddEventHandler(url_field, ButtonPressMask, False, on_url_button_press, NULL);
    XmProcessTraversal(url_field, XmTRAVERSE_CURRENT);
    TabManager::instance().registerUrlField(url_field);

    if (handles_out) {
        handles_out->url_field = url_field;
        handles_out->home_button = home_button;
    }

    return toolbar;
}

Widget createStatusBar(Widget parent, StatusBarHandles *handles_out)
{
    Widget status_form = XmCreateForm(parent, xm_name("browserStatusBar"), NULL, 0);
    XtVaSetValues(status_form,
                  XmNfractionBase, 100,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNbottomOffset, 6,
                  XmNleftOffset, 10,
                  XmNrightOffset, 10,
                  NULL);
    XtManageChild(status_form);

    Widget status_left_label = NULL;
    Widget status_left = create_status_segment(status_form, "statusMain", "", &status_left_label);
    TabManager::instance().registerStatusLabel(status_left_label);
    Widget status_center = create_status_segment(status_form, "statusSecurity", "Security: None",
                                                 handles_out ? &handles_out->security_label : NULL);
    Widget status_right = create_zoom_segment(status_form);

    XtVaSetValues(status_left,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_POSITION,
                  XmNrightPosition, 60,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  NULL);
    XtVaSetValues(status_center,
                  XmNleftAttachment, XmATTACH_POSITION,
                  XmNleftPosition, 60,
                  XmNrightAttachment, XmATTACH_POSITION,
                  XmNrightPosition, 85,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftOffset, 6,
                  NULL);
    XtVaSetValues(status_right,
                  XmNleftAttachment, XmATTACH_POSITION,
                  XmNleftPosition, 85,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftOffset, 6,
                  NULL);

    return status_form;
}

void UiBuilderInit()
{
}

} // namespace UiBuilder
