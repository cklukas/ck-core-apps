#ifndef CK_BROWSER_TAB_MANAGER_H
#define CK_BROWSER_TAB_MANAGER_H

#include <Xm/Xm.h>
#include <functional>
#include <memory>
#include <vector>

int desired_favicon_size();
Pixmap create_scaled_toolbar_pixmap_from_bgra(Display *display,
                                              int screen,
                                              const unsigned char *bgra,
                                              int src_w,
                                              int src_h,
                                              int target_size,
                                              Pixel bg_pixel);
bool create_scaled_window_icon_from_bgra(Display *display,
                                         int screen,
                                         const unsigned char *bgra,
                                         int src_w,
                                         int src_h,
                                         int target_size,
                                         Pixmap *out_pixmap,
                                         Pixmap *out_mask);

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
    void registerNavigationWidgets(Widget back_button,
                                   Widget forward_button,
                                   Widget nav_back,
                                   Widget nav_forward);
    void updateNavigationButtons(BrowserTab *tab);
    void goBack(BrowserTab *tab);
    void goForward(BrowserTab *tab);
    void registerReloadButton(Widget reload_button);
    void updateReloadButton(BrowserTab *tab);
    void reloadTab(BrowserTab *tab);
    void registerZoomControls(Widget zoom_label, Widget zoom_minus, Widget zoom_plus);
    void updateZoomControls(BrowserTab *tab);
    void pollZoomLevels();
    void zoomReset(BrowserTab *tab);
    void zoomIn(BrowserTab *tab);
    void zoomOut(BrowserTab *tab);
    void setTabZoomLevel(BrowserTab *tab, double level);
    void registerFaviconLabel(Widget favicon_label);
    void updateFaviconControls(BrowserTab *tab);
    void requestFaviconDownload(BrowserTab *tab, const char *reason);
    void clearTabFavicon(BrowserTab *tab);
    bool getCachedFavicon(const std::string &url,
                          std::vector<unsigned char> *raw_data,
                          int *width,
                          int *height,
                          std::vector<unsigned char> *png_data);

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
    Widget back_button_ = NULL;
    Widget forward_button_ = NULL;
    Widget nav_back_button_ = NULL;
    Widget nav_forward_button_ = NULL;
    Widget reload_button_ = NULL;
    Widget zoom_label_ = NULL;
    Widget zoom_minus_button_ = NULL;
    Widget zoom_plus_button_ = NULL;
    Widget favicon_label_ = NULL;
};

#endif // CK_BROWSER_TAB_MANAGER_H
