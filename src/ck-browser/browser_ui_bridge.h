#ifndef CK_BROWSER_BROWSER_UI_BRIDGE_H
#define CK_BROWSER_BROWSER_UI_BRIDGE_H

#include <include/cef_browser.h>
#include <include/cef_client.h>
#include <include/cef_popup_features.h>
#include <string>

struct BrowserTab;

// Bridge header that documents the UI helpers BrowserClient eventually needs.
void show_devtools_for_tab(BrowserTab *tab, int inspect_x, int inspect_y);
std::string normalize_url(const char *input);
bool is_devtools_url(const std::string &url);
void log_popup_features(const CefPopupFeatures &features);
void spawn_new_browser_window(const std::string &url);
void open_url_in_new_tab(const std::string &url, bool select);
void request_favicon_download(BrowserTab *tab, const char *reason);
void update_favicon_controls(BrowserTab *tab);
void schedule_theme_color_request(BrowserTab *tab, int delay_ms);
void apply_tab_theme_colors(BrowserTab *tab, bool active);
void update_tab_security_status(BrowserTab *tab);
void update_security_controls(BrowserTab *tab);
void update_navigation_buttons(BrowserTab *tab);
void update_reload_button_for_tab(BrowserTab *tab);
void update_url_field_for_tab(BrowserTab *tab);
void update_tab_label(BrowserTab *tab, const char *title);
void update_all_tab_labels(const char *reason);
void focus_browser_area(BrowserTab *tab);
bool is_tab_selected(const BrowserTab *tab);
void set_current_tab(BrowserTab *tab);
void set_status_label_text(const char *text);
BrowserTab *get_selected_tab();
BrowserTab *get_current_tab();
void clear_tab_favicon(BrowserTab *tab);
std::string extract_host_from_url(const std::string &url);
bool route_url_through_ck_browser(CefRefPtr<CefBrowser> browser,
                                  const std::string &url,
                                  bool allow_existing_tab);
void select_tab_page(BrowserTab *tab);
void load_url_for_tab(BrowserTab *tab, const std::string &url);
void resize_devtools_to_area(BrowserTab *tab, const char *reason);
void on_cef_browser_closed(const char *tag);

#endif // CK_BROWSER_BROWSER_UI_BRIDGE_H
