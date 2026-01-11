#include "tab_manager.h"

#include <Xm/DrawingA.h>
#include <Xm/Form.h>
#include <Xm/TabStack.h>
#include <Xm/Xm.h>
#include <X11/Intrinsic.h>
#include <X11/Xlib.h>

#include <cstdio>

#include <algorithm>

#include "browser_ui_bridge.h"

namespace {
int zoom_percent_from_level(double level)
{
    const double kStepFactor = 1.0954451150103321;  // sqrt(1.2)
    int steps = (int)(level * 2.0 + (level >= 0 ? 0.5 : -0.5));
    double factor = 1.0;
    if (steps > 0) {
        for (int i = 0; i < steps; ++i) {
            factor *= kStepFactor;
        }
    } else if (steps < 0) {
        for (int i = 0; i < -steps; ++i) {
            factor /= kStepFactor;
        }
    }
    int percent = (int)(factor * 100.0 + 0.5);
    if (percent < 25) percent = 25;
    if (percent > 500) percent = 500;
    return percent;
}
}  // namespace

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

void TabManager::registerNavigationWidgets(Widget back_button,
                                          Widget forward_button,
                                          Widget nav_back,
                                          Widget nav_forward)
{
    back_button_ = back_button;
    forward_button_ = forward_button;
    nav_back_button_ = nav_back;
    nav_forward_button_ = nav_forward;
    updateNavigationButtons(current_tab_);
}

void TabManager::updateNavigationButtons(BrowserTab *tab)
{
    bool can_back = tab && tab->can_go_back;
    bool can_forward = tab && tab->can_go_forward;

    auto update_state = [](Widget widget, bool enabled) {
        if (!widget) return;
        XtSetSensitive(widget, enabled ? True : False);
    };

    update_state(back_button_, can_back);
    update_state(forward_button_, can_forward);
    update_state(nav_back_button_, can_back);
    update_state(nav_forward_button_, can_forward);
}

void TabManager::goBack(BrowserTab *tab)
{
    if (!tab || !tab->browser) return;
    browser_set_focus(tab, False);
    if (tab->browser->CanGoBack()) {
        tab->browser->GoBack();
    }
}

void TabManager::goForward(BrowserTab *tab)
{
    if (!tab || !tab->browser) return;
    browser_set_focus(tab, False);
    if (tab->browser->CanGoForward()) {
        tab->browser->GoForward();
    }
}

void TabManager::registerReloadButton(Widget reload_button)
{
    reload_button_ = reload_button;
    updateReloadButton(current_tab_);
}

void TabManager::updateReloadButton(BrowserTab *tab)
{
    if (!reload_button_) return;
    bool loading = tab && tab->loading;
    const char *label = loading ? "Stop" : "Reload";
    XmString xm_label = XmStringCreateLocalized(label);
    XtVaSetValues(reload_button_, XmNlabelString, xm_label, NULL);
    XmStringFree(xm_label);
}

void TabManager::reloadTab(BrowserTab *tab)
{
    if (!tab || !tab->browser) return;
    browser_set_focus(tab, False);
    if (tab->loading) {
        tab->browser->StopLoad();
        tab->loading = false;
    } else {
        tab->browser->Reload();
    }
    if (tab == current_tab_) {
        updateReloadButton(tab);
    }
}

void TabManager::registerZoomControls(Widget zoom_label, Widget zoom_minus, Widget zoom_plus)
{
    zoom_label_ = zoom_label;
    zoom_minus_button_ = zoom_minus;
    zoom_plus_button_ = zoom_plus;
    updateZoomControls(current_tab_);
}

void TabManager::updateZoomControls(BrowserTab *tab)
{
    if (!zoom_label_) return;
    double level = tab ? tab->zoom_level : 0.0;
    int percent = zoom_percent_from_level(level);
    char buf[64];
    snprintf(buf, sizeof(buf), "Zoom: %d%%", percent);
    XmString xm_text = XmStringCreateLocalized((String)buf);
    XtVaSetValues(zoom_label_, XmNlabelString, xm_text, NULL);
    XmStringFree(xm_text);
}

void TabManager::pollZoomLevels()
{
    static int tick = 0;
    tick++;
    if ((tick % 20) != 0) {
        return;
    }
    BrowserTab *current_tab = currentTab();
    for (const auto &entry : tabs_) {
        BrowserTab *tab = entry.get();
        if (!tab || !tab->browser) continue;
        CefRefPtr<CefBrowserHost> host = tab->browser->GetHost();
        if (!host) continue;
        double current = host->GetZoomLevel();
        double diff = current - tab->zoom_level;
        if (diff < 0) diff = -diff;
        if (diff > 1e-6) {
            tab->zoom_level = current;
            if (tab == current_tab) {
                updateZoomControls(tab);
            }
        }
    }
}

void TabManager::setTabZoomLevel(BrowserTab *tab, double level)
{
    if (!tab || !tab->browser) return;
    CefRefPtr<CefBrowserHost> host = tab->browser->GetHost();
    if (!host) return;
    host->SetZoomLevel(level);
    tab->zoom_level = level;
    if (tab == current_tab_) {
        updateZoomControls(tab);
    }
}

void TabManager::zoomReset(BrowserTab *tab)
{
    if (!tab) return;
    setTabZoomLevel(tab, 0.0);
}

void TabManager::zoomIn(BrowserTab *tab)
{
    if (!tab) return;
    setTabZoomLevel(tab, tab->zoom_level + 0.5);
}

void TabManager::zoomOut(BrowserTab *tab)
{
    if (!tab) return;
    setTabZoomLevel(tab, tab->zoom_level - 0.5);
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
