#ifndef CK_BROWSER_TAB_MANAGER_H
#define CK_BROWSER_TAB_MANAGER_H

#include <functional>
#include <memory>
#include <vector>

#include "browser_tab.h"

class TabManager {
public:
    static TabManager &instance();

    BrowserTab *addTab(std::unique_ptr<BrowserTab> tab);
    void removeTab(BrowserTab *tab);
    void clearTabs();

    BrowserTab *createTab(Widget tab_stack,
                          const char *name,
                          const char *title,
                          const char *base_title,
                          const char *initial_url);

    void scheduleBrowserCreation(BrowserTab *tab);

    using TabSelectionHandler = std::function<void(BrowserTab *tab, BrowserTab *previous)>;
    void set_selection_handler(TabSelectionHandler handler);
    void selectTab(BrowserTab *tab);

    std::vector<std::unique_ptr<BrowserTab>> &tabs();
    const std::vector<std::unique_ptr<BrowserTab>> &tabs() const;

    BrowserTab *currentTab() const;
    void setCurrentTab(BrowserTab *tab);

    int countTabsWithBaseTitle(const char *base_title) const;

private:
    std::vector<std::unique_ptr<BrowserTab>> tabs_;
    BrowserTab *current_tab_ = nullptr;
    TabSelectionHandler selection_handler_;
};

#endif // CK_BROWSER_TAB_MANAGER_H
