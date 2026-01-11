#include "tab_manager.h"

#include <Xm/DrawingA.h>
#include <Xm/Form.h>
#include <Xm/TabStack.h>
#include <Xm/Xm.h>
#include <X11/Intrinsic.h>
#include <X11/Xlib.h>

#include <algorithm>
#include <utility>

#include "browser_ui_bridge.h"

extern const char *kInitialBrowserUrl;
extern void on_tab_destroyed(Widget w, XtPointer client_data, XtPointer call_data);
extern void on_browser_resize(Widget w, XtPointer client_data, XtPointer call_data);
extern void on_browser_area_button_press(Widget w, XtPointer client_data, XEvent *event, Boolean *continue_to_dispatch);
extern char *xm_name(const char *name);

TabManager &TabManager::instance()
{
    static TabManager manager;
    return manager;
}

BrowserTab *TabManager::addTab(std::unique_ptr<BrowserTab> tab)
{
    if (!tab) return nullptr;
    BrowserTab *ptr = tab.get();
    tabs_.push_back(std::move(tab));
    return ptr;
}

BrowserTab *TabManager::createTab(Widget tab_stack,
                                  const char *name,
                                  const char *title,
                                  const char *base_title,
                                  const char *initial_url)
{
    if (!tab_stack) return nullptr;
    Widget page = XmCreateForm(tab_stack, (String)name, NULL, 0);
    XtVaSetValues(page,
                  XmNfractionBase, 100,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNmarginWidth, 0,
                  XmNmarginHeight, 0,
                  XmNborderWidth, 0,
                  NULL);
    auto tab = std::make_unique<BrowserTab>();
    BrowserTab *tab_ptr = tab.get();
    tab_ptr->page = page;
    tab_ptr->base_title = base_title ? base_title : "";
    tab_ptr->title_full = title ? title : "";
    tab_ptr->pending_url = normalize_url(initial_url ? initial_url : kInitialBrowserUrl);
    tab_ptr->current_url.clear();
    tab_ptr->status_message.clear();
    tab_ptr->security_status.clear();
    update_tab_security_status(tab_ptr);
    XtVaSetValues(page, XmNuserData, tab_ptr, NULL);
    XtAddCallback(page, XmNdestroyCallback, on_tab_destroyed, tab_ptr);
    update_tab_label(tab_ptr, title);

    Widget browser_area = XmCreateDrawingArea(page, xm_name("browserView"), NULL, 0);
    XtVaSetValues(browser_area,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNresizePolicy, XmRESIZE_ANY,
                  XmNtraversalOn, True,
                  XmNnavigationType, XmTAB_GROUP,
                  XmNleftOffset, 0,
                  XmNrightOffset, 0,
                  XmNbottomOffset, 0,
                  XmNtopOffset, 0,
                  NULL);
    XtAddCallback(browser_area, XmNresizeCallback, on_browser_resize, tab_ptr);
    XtAddEventHandler(browser_area, ButtonPressMask, False, on_browser_area_button_press, tab_ptr);
    XtManageChild(browser_area);
    tab_ptr->browser_area = browser_area;

    XtManageChild(page);
    TabManager::instance().addTab(std::move(tab));
    update_all_tab_labels("create_tab_page");
    return tab_ptr;
}

void TabManager::set_selection_handler(TabSelectionHandler handler)
{
    selection_handler_ = std::move(handler);
}

void TabManager::selectTab(BrowserTab *tab)
{
    BrowserTab *previous = current_tab_;
    current_tab_ = tab;
    if (selection_handler_) {
        selection_handler_(tab, previous);
    }
}

void TabManager::scheduleBrowserCreation(BrowserTab *tab)
{
    schedule_tab_browser_creation(tab);
}

void TabManager::removeTab(BrowserTab *tab)
{
    if (!tab) return;
    auto it = std::find_if(tabs_.begin(), tabs_.end(),
                           [tab](const std::unique_ptr<BrowserTab> &entry) {
                               return entry.get() == tab;
                           });
    if (it != tabs_.end()) {
        detach_tab_clients(tab);
        tabs_.erase(it);
        if (current_tab_ == tab) {
            current_tab_ = nullptr;
        }
    }
}

void TabManager::clearTabs()
{
    for (const auto &entry : tabs_) {
        detach_tab_clients(entry.get());
    }
    tabs_.clear();
    current_tab_ = nullptr;
}

std::vector<std::unique_ptr<BrowserTab>> &TabManager::tabs()
{
    return tabs_;
}

const std::vector<std::unique_ptr<BrowserTab>> &TabManager::tabs() const
{
    return tabs_;
}

BrowserTab *TabManager::currentTab() const
{
    return current_tab_;
}

void TabManager::setCurrentTab(BrowserTab *tab)
{
    current_tab_ = tab;
}

int TabManager::countTabsWithBaseTitle(const char *base_title) const
{
    if (!base_title) return 0;
    int matches = 0;
    for (const auto &entry : tabs_) {
        BrowserTab *tab = entry.get();
        if (tab && tab->base_title == base_title) {
            matches++;
        }
    }
    return matches;
}
