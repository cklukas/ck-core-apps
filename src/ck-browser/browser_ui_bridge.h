#ifndef CK_BROWSER_BROWSER_UI_BRIDGE_H
#define CK_BROWSER_BROWSER_UI_BRIDGE_H

#include <include/cef_browser.h>
#include <include/cef_client.h>
#include <functional>
#include <string>

struct BrowserTab;

// Bridge header that documents the UI helpers BrowserClient eventually needs.
std::string normalize_url(const char *input);
void open_url_in_new_tab(const std::string &url, bool select);
void request_favicon_download(BrowserTab *tab, const char *reason);
void update_favicon_controls(BrowserTab *tab);
void schedule_theme_color_request(BrowserTab *tab, int delay_ms);
void apply_tab_theme_colors(BrowserTab *tab, bool active);
void update_tab_security_status(BrowserTab *tab);
void update_security_controls(BrowserTab *tab);
void update_url_field_for_tab(BrowserTab *tab);
void update_tab_label(BrowserTab *tab, const char *title);
void update_all_tab_labels(const char *reason);
void focus_browser_area(BrowserTab *tab);
void browser_set_focus(BrowserTab *tab, bool focus);
bool is_tab_selected(const BrowserTab *tab);
void set_current_tab(BrowserTab *tab);
void set_status_label_text(const char *text);
BrowserTab *get_selected_tab();
BrowserTab *get_current_tab();
void clear_tab_favicon(BrowserTab *tab);
std::string extract_host_from_url(const std::string &url);
void select_tab_page(BrowserTab *tab);
void load_url_for_tab(BrowserTab *tab, const std::string &url);
void resize_devtools_to_area(BrowserTab *tab, const char *reason);
void schedule_tab_browser_creation(BrowserTab *tab);
using SurfaceNewTabCallback = std::function<void(const std::string &url, bool select)>;
using SurfaceTabCallback = std::function<void(BrowserTab *tab)>;
void set_surface_new_tab_handler(SurfaceNewTabCallback handler);
void set_surface_tab_load_finished_handler(SurfaceTabCallback handler);
void notify_surface_new_tab_request(const std::string &url, bool select);
void notify_surface_tab_load_finished(BrowserTab *tab);

#endif // CK_BROWSER_BROWSER_UI_BRIDGE_H
