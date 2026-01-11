#ifndef CK_BROWSER_BROWSER_TAB_H
#define CK_BROWSER_BROWSER_TAB_H

#include <Xm/Xm.h>
#include <X11/Xlib.h>

#include <include/cef_browser.h>
#include <include/cef_client.h>

#include <string>
#include <vector>

class DevToolsClient;

struct BrowserTab {
    Widget page = NULL;
    Widget browser_area = NULL;
    std::string base_title;
    std::string title_full;
    std::string pending_url;
    std::string current_url;
    CefRefPtr<CefBrowser> browser;
    CefRefPtr<CefClient> client;
    bool create_scheduled = false;
    bool can_go_back = false;
    bool can_go_forward = false;
    std::string status_message;
    double zoom_level = 0.0;
    std::string security_status;
    std::string favicon_url;
    Pixmap favicon_toolbar_pixmap = None;
    int favicon_toolbar_size = 0;
    Pixmap favicon_window_pixmap = None;
    Pixmap favicon_window_mask = None;
    int favicon_window_size = 0;
    bool tab_default_colors_initialized = false;
    Pixel tab_default_background = 0;
    Pixel tab_default_foreground = 0;
    bool has_theme_color = false;
    unsigned char theme_r = 0;
    unsigned char theme_g = 0;
    unsigned char theme_b = 0;
    int theme_color_retry_count = 0;
    bool theme_color_retry_scheduled = false;
    int theme_color_ready_retry_count = 0;
    Widget devtools_shell = NULL;
    Widget devtools_area = NULL;
    CefRefPtr<CefBrowser> devtools_browser;
    CefRefPtr<DevToolsClient> devtools_client;
    int devtools_inspect_x = 0;
    int devtools_inspect_y = 0;
    bool devtools_show_scheduled = false;
    bool loading = false;
    std::string current_host;
};

#endif // CK_BROWSER_BROWSER_TAB_H
