#ifndef CK_BROWSER_BROWSER_APP_H
#define CK_BROWSER_BROWSER_APP_H

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <include/cef_app.h>
#include <include/cef_client.h>
#include <include/internal/cef_types_wrappers.h>

class CkCefApp;
extern CefRefPtr<CkCefApp> g_cef_app;
CefRefPtr<CefApp> ensure_cef_app();
class BrowserClient;
struct BrowserTab;
struct SessionData;
struct BrowserPaths {
    std::string resources_path;
    std::string locales_path;
    std::string subprocess_path;
};
struct BrowserPreflightState {
    std::string homepage_url;
    BrowserPaths cef_paths;
    bool force_disable_gpu = false;
    SessionData *session_data = nullptr;
    bool has_startup_url = false;
    std::string startup_url;
    std::string cache_suffix;
    std::vector<char *> cef_argv;
};
struct SessionData;

class BrowserApp {
public:
    static BrowserApp &instance();

    int run(int argc, char *argv[]);
    int run_main(int argc, char *argv[]);
    int run_ui_loop(int argc, char *argv[]);

    using NewTabRequestCallback = std::function<void(const std::string &url, bool select)>;
    using TabEventCallback = std::function<void(BrowserTab *tab)>;
    void set_new_tab_request_handler(NewTabRequestCallback handler);
    void set_tab_load_finished_handler(TabEventCallback handler);
    void notify_new_tab_request(const std::string &url, bool select);
    void notify_tab_load_finished(BrowserTab *tab);

    BrowserPaths discover_cef_paths() const;
    bool build_path_from_dir(const char *dir, const char *suffix, char *buffer, size_t buffer_len) const;
    void build_cwd_path(char *buffer, size_t buffer_len, const char *suffix) const;
    bool dir_has_files(const char *path) const;
    bool file_exists(const char *path) const;
    bool find_existing_path(char *buffer, size_t buffer_len, const char *suffix) const;
    void get_exe_path(char *buffer, size_t buffer_len) const;
    void report_cef_resource_status(const char *resources_path, const char *locales_path) const;
    void dump_cef_env_and_args(int argc, char *argv[]) const;
    bool parse_cache_suffix_arg(int argc, char *argv[], std::string *out_suffix) const;
    void build_cef_argv(int argc, char *argv[], std::vector<char *> *out_argv) const;
    bool has_opengl_support() const;
    void apply_gpu_switches(bool disable_gpu) const;
    SessionData *prepare_session(int &argc, char **argv) const;
    std::string build_cache_path(const std::string &suffix) const;
    int run_cef_preflight(int argc, char *argv[], CefRefPtr<CefApp> cef_app, BrowserPreflightState *state) const;
    bool initialize_cef(const BrowserPreflightState &preflight,
                        const char *resources_path,
                        const char *locales_path,
                        const char *subprocess_path);
    void shutdown_cef() const;
    void request_theme_color_for_tab(BrowserTab *tab);
    void show_devtools_for_tab(BrowserTab *tab, int inspect_x, int inspect_y);
    void spawn_new_browser_window(const std::string &url);
    void set_subprocess_path(const std::string &path);
    void handle_tab_load_start(BrowserTab *tab, const std::string &url);
    void handle_tab_address_change(BrowserTab *tab, const std::string &url);
    void handle_tab_status_message(BrowserTab *tab, const std::string &message);
    void handle_tab_title_change(BrowserTab *tab, const std::string &title);
    void handle_tab_favicon_change(BrowserTab *tab, const std::string &url);
    void handle_tab_loading_state_change(BrowserTab *tab, bool is_loading, bool can_go_back, bool can_go_forward);
    void handle_tab_focus(BrowserTab *tab);
    void notify_browser_closed(const char *tag);

private:
    friend class BrowserClient;
    BrowserApp() = default;
    void log_popup_features(const CefPopupFeatures &features) const;
    bool route_url_through_ck_browser(CefRefPtr<CefBrowser> browser,
                                      const std::string &url,
                                      bool allow_existing_tab) const;
    bool is_devtools_url(const std::string &url) const;
    std::string subprocess_path_;
    NewTabRequestCallback new_tab_request_callback_;
    TabEventCallback load_finished_callback_;
};

CefRefPtr<CefClient> create_browser_client(BrowserTab *tab);
void detach_browser_client(const CefRefPtr<CefClient> &client);

#endif // CK_BROWSER_BROWSER_APP_H
