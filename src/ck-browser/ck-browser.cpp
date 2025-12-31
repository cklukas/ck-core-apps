#include <Xm/Xm.h>
#include <Xm/CascadeBG.h>
#include <Xm/DrawingA.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/LabelG.h>
#include <Xm/MessageB.h>
#include <Xm/MenuShell.h>
#include <Xm/Protocols.h>
#include <Xm/PushBG.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/SeparatoG.h>
#include <Xm/ScrolledW.h>
#include <Xm/List.h>
#include <Xm/Outline.h>
#ifndef XmNnodeParent
#define XmNnodeParent "nodeParent"
#endif
#include <Xm/TabBox.h>
#include <Xm/TabStack.h>
#include <Xm/TextF.h>
#include <Xm/ToggleB.h>
#include <X11/Intrinsic.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <Dt/Dt.h>
#include <dlfcn.h>

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <ctime>
#include <algorithm>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>

extern "C" {
#include "../shared/about_dialog.h"
#include "../shared/session_utils.h"
#include "../shared/config_utils.h"
}


#include <include/cef_display_handler.h>
#include <include/cef_focus_handler.h>
#include <include/cef_load_handler.h>
#include <include/cef_request_handler.h>
#include <include/cef_context_menu_handler.h>
#include <include/cef_menu_model.h>
#include <include/cef_process_message.h>
#include <include/cef_render_process_handler.h>
#include <include/cef_v8.h>
#include <include/cef_app.h>
#include <include/cef_browser.h>
#include <include/cef_command_line.h>
#include <include/cef_client.h>
#include <include/cef_frame.h>
#include <include/cef_navigation_entry.h>
#include <include/cef_ssl_status.h>
#include <include/cef_image.h>
#include <include/cef_values.h>
#include <include/cef_parser.h>
#include <include/internal/cef_linux.h>
#include <include/wrapper/cef_helpers.h>

static void log_function_entry(const char *func, const char *fmt, ...)
{
    if (!func || !fmt) return;
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[ck-browser] %s(): ", func);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

#define LOG_ENTER(fmt, ...) log_function_entry(__func__, fmt, ##__VA_ARGS__)

struct BrowserTab;
static XtAppContext g_app = NULL;
static Widget g_toplevel = NULL;
static Widget g_tab_stack = NULL;
static Widget g_tab_box = NULL;
static Widget g_about_shell = NULL;
static Widget g_url_field = NULL;
static Widget g_favicon_label = NULL;
static Widget g_back_button = NULL;
static Widget g_forward_button = NULL;
static Widget g_nav_back = NULL;
static Widget g_nav_forward = NULL;
static Widget g_home_button = NULL;
static Widget g_home_button_menu = NULL;
static Widget g_status_message_label = NULL;
static Widget g_security_label = NULL;
static Widget g_zoom_label = NULL;
static Widget g_zoom_minus_button = NULL;
static Widget g_zoom_plus_button = NULL;
static Widget g_reload_button = NULL;
static bool g_tab_sync_scheduled = false;
static std::string g_homepage_url;
static char g_resources_path[PATH_MAX] = "";
static char g_locales_path[PATH_MAX] = "";
static char g_subprocess_path[PATH_MAX] = "";
static Widget g_tab_header_menu = NULL;
static BrowserTab *g_tab_header_menu_target = NULL;
class BrowserClient;
class DevToolsClient;
class CkCefApp;
static CefRefPtr<CkCefApp> g_cef_app;
static bool g_tab_handlers_attached = false;
static SessionData *g_session_data = NULL;
static bool g_session_loaded = false;
static bool g_force_disable_gpu = false;
static bool g_cef_initialized = false;
static bool g_shutdown_requested = false;
static int g_shutdown_pending_browsers = 0;

struct BrowserTab {
    Widget page = NULL;
    Widget browser_area = NULL;
    std::string base_title;
    std::string title_full;
    std::string pending_url;
    std::string current_url;
    CefRefPtr<CefBrowser> browser;
    CefRefPtr<BrowserClient> client;
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

struct FaviconCacheEntry {
    std::vector<unsigned char> data;
    std::vector<unsigned char> png_data;
    int width = 0;
    int height = 0;
    time_t saved_time = 0;
    std::list<std::string>::iterator order_it;
};

static const size_t kFaviconCacheLimit = 500;
static const time_t kFaviconCacheTTL_SECONDS = 6 * 60 * 60;
static std::unordered_map<std::string, FaviconCacheEntry> g_favicon_cache;
static std::list<std::string> g_favicon_cache_order;

struct BookmarkEntry {
    std::string name;
    std::string url;
    bool show_in_menu = false;
    std::vector<unsigned char> icon_raw;
    std::vector<unsigned char> icon_png;
    int icon_width = 0;
    int icon_height = 0;
};

struct BookmarkGroup {
    std::string name;
    std::vector<std::unique_ptr<BookmarkEntry>> entries;
    std::vector<std::unique_ptr<BookmarkGroup>> children;
};

struct BookmarkDialogContext {
    Widget dialog = NULL;
    Widget name_field = NULL;
    Widget add_to_menu_checkbox = NULL;
    std::vector<BookmarkGroup *> group_entries;
    BookmarkEntry *editing_entry = NULL;
    BookmarkGroup *editing_group = NULL;
    Widget group_list = NULL;
    BookmarkGroup *selected_group = NULL;
};

struct BookmarkManagerContext {
    Widget dialog = NULL;
    Widget group_list = NULL;
    Widget bookmark_list = NULL;
    Widget open_button = NULL;
    Widget edit_button = NULL;
    Widget delete_button = NULL;
    std::vector<BookmarkGroup *> group_entries;
    std::vector<BookmarkEntry *> entry_items;
    std::vector<Widget> bookmark_entry_widgets;
    std::vector<Pixmap> bookmark_entry_pixmaps;
    Widget selected_entry_widget = NULL;
    BookmarkGroup *selected_group = NULL;
    BookmarkEntry *selected_entry = NULL;
};

static std::vector<std::unique_ptr<BrowserTab>> g_browser_tabs;
static BrowserTab *g_current_tab = NULL;
static bool g_cef_message_pump_started = false;
static std::unique_ptr<BookmarkGroup> g_bookmark_root;
static BookmarkGroup *g_selected_bookmark_group = NULL;
static Widget g_bookmarks_menu = NULL;
static std::vector<Widget> g_bookmark_menu_items;
static std::vector<Pixmap> g_bookmark_menu_icon_pixmaps;
static const char *kInitialBrowserUrl = "https://www.wikipedia.org";
static const char *kBookmarksFileName = "bookmarks.html";
static char g_bookmarks_file_path[PATH_MAX] = "";
static bool g_bookmarks_path_ready = false;
static time_t g_bookmarks_file_mtime = 0;
static std::string normalize_url(const char *input);
static const char *display_url_for_tab(const BrowserTab *tab);
static void update_url_field_for_tab(BrowserTab *tab);
static bool is_devtools_url(const std::string &url);
static bool is_url_parseable(const std::string &url);
static void show_devtools_for_tab(BrowserTab *tab, int inspect_x, int inspect_y);
static void start_devtools_browser_cb(XtPointer client_data, XtIntervalId *id);
static char *xm_name(const char *name);
static void on_tab_destroyed(Widget w, XtPointer client_data, XtPointer call_data);
static void on_tab_selection_changed(Widget w, XtPointer client_data, XtPointer call_data);
static void initialize_cef_browser_cb(XtPointer client_data, XtIntervalId *id);
static BrowserTab *get_selected_tab();
static bool is_tab_selected(const BrowserTab *tab);
static void set_current_tab(BrowserTab *tab);
static void schedule_tab_browser_creation(BrowserTab *tab);
static void on_back(Widget w, XtPointer client_data, XtPointer call_data);
static void on_forward(Widget w, XtPointer client_data, XtPointer call_data);
static void on_reload(Widget w, XtPointer client_data, XtPointer call_data);
static void on_home(Widget w, XtPointer client_data, XtPointer call_data);
static void on_enter_url(Widget w, XtPointer client_data, XtPointer call_data);
static void on_go_back_menu(Widget w, XtPointer client_data, XtPointer call_data);
static void on_go_forward_menu(Widget w, XtPointer client_data, XtPointer call_data);
static void on_reload_menu(Widget w, XtPointer client_data, XtPointer call_data);
static void set_status_label_text(const char *text);
static void update_navigation_buttons(BrowserTab *tab);
static void update_tab_label(BrowserTab *tab, const char *text);
static void update_all_tab_labels(const char *reason);
static void log_widget_size(const char *context, Widget widget);
static void on_main_window_resize(Widget w, XtPointer client_data, XtPointer call_data);
static void on_tab_stack_resize(Widget w, XtPointer client_data, XtPointer call_data);
static void resize_cef_browser_to_area(BrowserTab *tab, const char *reason);
static void update_zoom_controls(BrowserTab *tab);
static void poll_zoom_levels();
static void on_zoom_reset(Widget w, XtPointer client_data, XtPointer call_data);
static void on_zoom_in(Widget w, XtPointer client_data, XtPointer call_data);
static void on_zoom_out(Widget w, XtPointer client_data, XtPointer call_data);
static BookmarkGroup *ensure_bookmark_groups();
static BookmarkGroup *add_bookmark_group(BookmarkGroup *parent, const char *name);
static void collect_bookmark_groups(BookmarkGroup *group,
                                    std::vector<BookmarkGroup *> &entries,
                                    std::vector<std::string> &labels,
                                    int depth);
static void collect_bookmark_menu_entries(BookmarkGroup *group,
                                          std::vector<BookmarkEntry *> &entries);
static void rebuild_bookmarks_menu_items();
static void on_bookmark_menu_activate(Widget w, XtPointer client_data, XtPointer call_data);
static void on_bookmark_dialog_save(Widget w, XtPointer client_data, XtPointer call_data);
static void on_bookmark_dialog_cancel(Widget w, XtPointer client_data, XtPointer call_data);
static void show_add_bookmark_dialog(BrowserTab *tab, BookmarkEntry *entry = NULL, BookmarkGroup *group = NULL);
static void bookmark_dialog_group_list_selection_cb(Widget w, XtPointer client_data, XtPointer call_data);
static void bookmark_dialog_populate_group_list(BookmarkDialogContext *ctx, Widget list, BookmarkGroup *group, int depth);
static BookmarkEntry *find_bookmark_by_url(BookmarkGroup *group, const std::string &url, BookmarkGroup **out_group);
static void save_bookmarks_to_file();
static std::unique_ptr<BookmarkGroup> load_bookmarks_from_file();
static void ensure_path_directory(const char *path);
static const char *get_bookmarks_file_path();
static time_t get_file_mtime(const char *path);
static std::string escape_html(const std::string &input);
static std::string base64_encode(const std::vector<unsigned char> &data);
static bool base64_decode(const std::string &input, std::vector<unsigned char> &output);
static bool extract_base64_payload(const std::string &value, std::string &out_payload);
static bool bookmark_entry_set_icon_png(BookmarkEntry *entry, const unsigned char *png_data, size_t png_size);
static void bookmark_entry_copy_icon_from_cache(BookmarkEntry *entry, const char *url);
static Pixmap create_bookmark_icon_pixmap(BookmarkEntry *entry, int target_size, Pixel bg_pixel);
static void clear_bookmark_menu_icon_pixmaps();
static void bookmark_manager_free_entry_pixmaps(BookmarkManagerContext *ctx);
static void bookmark_manager_clear_entry_widgets(BookmarkManagerContext *ctx);
static void bookmark_manager_entry_activate_cb(Widget w, XtPointer client_data, XtPointer call_data);
static void clear_tab_favicon(BrowserTab *tab);
static std::string extract_host_from_url(const std::string &url);
static void show_invalid_url_dialog(const char *text);
static void set_reload_button_label(const char *text);
static void update_reload_button_for_tab(BrowserTab *tab);
static void write_netscape_bookmarks(FILE *f, BookmarkGroup *root);
static void write_group_contents(FILE *f, BookmarkGroup *group, int indent);
static void parse_netscape_bookmarks(const std::string &content, BookmarkGroup *root);
static bool extract_tag_text(const std::string &content, size_t start, const char *closing_tag, size_t &out_end, std::string &out_text);
static bool parse_attribute_value(const std::string &tag, const char *name, std::string &out_value);
static BookmarkGroup *find_or_create_child_group(BookmarkGroup *parent, const std::string &name);
static std::unique_ptr<BookmarkGroup> create_default_bookmark_tree();
static void bookmark_file_monitor_timer_cb(XtPointer client_data, XtIntervalId *id);
static void close_bookmark_manager_dialog(BookmarkManagerContext *ctx);
static void bookmark_manager_update_entry_list(BookmarkManagerContext *ctx);
static void bookmark_manager_group_selection_cb(Widget w, XtPointer client_data, XtPointer call_data);
static void on_bookmark_manager_open(Widget w, XtPointer client_data, XtPointer call_data);
static void on_bookmark_manager_edit(Widget w, XtPointer client_data, XtPointer call_data);
static void on_bookmark_manager_delete(Widget w, XtPointer client_data, XtPointer call_data);
static void on_bookmark_manager_cancel(Widget w, XtPointer client_data, XtPointer call_data);
static void show_bookmark_manager_dialog();
static void set_security_label_text(const char *text);
static void update_security_controls(BrowserTab *tab);
static void update_tab_security_status(BrowserTab *tab);
static void focus_motif_widget(Widget widget);
static void browser_set_focus(BrowserTab *tab, bool focus);
static void on_url_focus(Widget w, XtPointer client_data, XtPointer call_data);
static void on_url_button_press(Widget w, XtPointer client_data, XEvent *event, Boolean *continue_to_dispatch);
static void on_browser_area_button_press(Widget w, XtPointer client_data, XEvent *event, Boolean *continue_to_dispatch);
static Widget pick_adjacent_tab_page(Widget current_page);
static void focus_browser_area(BrowserTab *tab);
static Widget find_tab_stack_tabbox(Widget tab_stack);
static void sync_current_tab_cb(XtPointer client_data, XtIntervalId *id);
static void sync_current_tab_timeout_cb(XtPointer client_data, XtIntervalId *id);
static void schedule_sync_current_tab(const char *reason);
static void on_tabbox_input(Widget w, XtPointer client_data, XEvent *event, Boolean *continue_to_dispatch);
static void on_global_button_press(Widget w, XtPointer client_data, XEvent *event, Boolean *continue_to_dispatch);
static void ensure_tab_header_menu();
static BrowserTab *get_visible_tab_at_index(int index);
static int get_visible_tab_index(const BrowserTab *tab);
static void on_tab_menu_reload(Widget w, XtPointer client_data, XtPointer call_data);
static void on_tab_menu_close(Widget w, XtPointer client_data, XtPointer call_data);
static void on_tab_menu_close_right(Widget w, XtPointer client_data, XtPointer call_data);
static void on_tab_menu_move_new_window(Widget w, XtPointer client_data, XtPointer call_data);
static void ensure_tab_default_colors(BrowserTab *tab);
static bool alloc_rgb_pixel(Display *display, int screen, unsigned char r, unsigned char g, unsigned char b, Pixel *out_pixel);
static void pick_contrast_color(unsigned char bg_r, unsigned char bg_g, unsigned char bg_b,
                                unsigned char *out_r, unsigned char *out_g, unsigned char *out_b);
static void apply_tab_theme_colors(BrowserTab *tab, bool active);
static void attach_tab_handlers_cb(XtPointer client_data, XtIntervalId *id);
static void update_favicon_controls(BrowserTab *tab);
static void request_favicon_download(BrowserTab *tab, const char *reason);
static void spawn_new_browser_window(const std::string &url);
static void open_url_in_new_tab(const std::string &url, bool select);
static int count_tabs_with_base_title(const char *base_title);
static BrowserTab *create_tab_page(Widget tab_stack,
                                   const char *name,
                                   const char *title,
                                   const char *base_title,
                                   const char *initial_url);
static void capture_session_state(const char *reason);
static void save_last_session_file(const char *reason);
static void restore_last_session_from_file(const char *reason);
static std::string load_homepage_file();
static void save_homepage_file(const std::string &url, const char *reason);
static void wm_save_yourself_cb(Widget w, XtPointer client_data, XtPointer call_data);
static void restore_tabs_from_session_data(SessionData *data);
static void on_restore_session(Widget w, XtPointer client_data, XtPointer call_data);
static const char *window_disposition_name(cef_window_open_disposition_t disposition);
static void on_new_window(Widget w, XtPointer client_data, XtPointer call_data);
static void resize_devtools_to_area(BrowserTab *tab, const char *reason);
static void on_devtools_area_resize(Widget w, XtPointer client_data, XtPointer call_data);
static void on_devtools_shell_wm_delete(Widget w, XtPointer client_data, XtPointer call_data);
static void ensure_home_button_menu();
static void on_home_button_press(Widget w, XtPointer client_data, XEvent *event, Boolean *continue_to_dispatch);
static void on_home_menu_set_blank(Widget w, XtPointer client_data, XtPointer call_data);
static void on_home_menu_use_current(Widget w, XtPointer client_data, XtPointer call_data);
static int count_open_browsers();
static void on_cef_browser_closed(const char *tag);
static void begin_shutdown_sequence(const char *reason);
static void focus_url_field_timer(XtPointer client_data, XtIntervalId *id);
static bool has_opengl_support();
static void apply_gpu_switches();
static void on_add_bookmark_menu(Widget w, XtPointer client_data, XtPointer call_data);
static void on_open_bookmark_manager_menu(Widget w, XtPointer client_data, XtPointer call_data);

class BrowserClient : public CefClient,
                      public CefLifeSpanHandler,
                      public CefDisplayHandler,
                      public CefFocusHandler,
                      public CefLoadHandler,
                      public CefRequestHandler,
                      public CefContextMenuHandler {
 public:
  explicit BrowserClient(BrowserTab *tab) : tab_(tab) {}

  CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override {
    return this;
  }

  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
    return this;
  }

  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override {
    return this;
  }

  CefRefPtr<CefFocusHandler> GetFocusHandler() override {
    return this;
  }

  CefRefPtr<CefLoadHandler> GetLoadHandler() override {
    return this;
  }

  CefRefPtr<CefRequestHandler> GetRequestHandler() override {
    return this;
  }

  void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefContextMenuParams> params,
                           CefRefPtr<CefMenuModel> model) override {
	    CEF_REQUIRE_UI_THREAD();
	    (void)browser;
	    (void)frame;
	    if (!params || !model) return;
	    last_context_x_ = params->GetXCoord();
	    last_context_y_ = params->GetYCoord();
	    std::string link_url = params->GetLinkUrl().ToString();
	    last_context_link_url_ = link_url;
	    default_open_link_new_tab_cmd_ = -1;
	    default_open_link_new_window_cmd_ = -1;
	    inspect_element_cmds_.clear();
	    fprintf(stderr,
	            "[ck-browser] OnBeforeContextMenu tab=%s (%p) link_url=%s\n",
	            tab_ ? (tab_->base_title.empty() ? "Tab" : tab_->base_title.c_str()) : "(none)",
	            (void *)tab_,
	            link_url.c_str());

	    auto simplify_label = [](const std::string &s) -> std::string {
	      std::string out;
	      out.reserve(s.size());
	      for (unsigned char uc : s) {
	        char c = (char)uc;
	        if (c == '&') continue;  // Motif/Chromium accelerator markers (e.g. I&nspect)
	        if (c >= 'A' && c <= 'Z') out.push_back((char)(c - 'A' + 'a'));
	        else out.push_back(c);
	      }
	      return out;
	    };

	    std::function<void(CefRefPtr<CefMenuModel>, int)> scan_menu =
	        [&](CefRefPtr<CefMenuModel> m, int depth) {
	          if (!m) return;
	          size_t count = m->GetCount();
	          for (size_t i = 0; i < count; ++i) {
	            int cmd = m->GetCommandIdAt(i);
	            CefString label = m->GetLabelAt(i);
	            std::string text = label.ToString();
	            std::string lower = simplify_label(text);
	            fprintf(stderr,
	                    "[ck-browser] context menu item depth=%d idx=%zu cmd=%d label='%s' lower='%s'\n",
	                    depth,
	                    i,
	                    cmd,
	                    text.c_str(),
	                    lower.c_str());
	            if (cmd >= 0) {
	              if (lower.find("inspect") != std::string::npos ||
	                  lower.find("developer tools") != std::string::npos ||
	                  lower.find("devtools") != std::string::npos) {
	                inspect_element_cmds_.push_back(cmd);
	              }
	              if (lower.find("open link in new tab") != std::string::npos) {
	                default_open_link_new_tab_cmd_ = cmd;
	              } else if (lower.find("open link in new window") != std::string::npos) {
	                default_open_link_new_window_cmd_ = cmd;
	              }
	            }
	            if (m->GetTypeAt(i) == MENUITEMTYPE_SUBMENU) {
	              CefRefPtr<CefMenuModel> sub = m->GetSubMenuAt(i);
	              scan_menu(sub, depth + 1);
	            }
	          }
	        };

	    scan_menu(model, 0);
	    if (!inspect_element_cmds_.empty()) {
	      fprintf(stderr,
	              "[ck-browser] context menu inspect/devtools cmd ids (%zu):",
	              inspect_element_cmds_.size());
	      for (size_t i = 0; i < inspect_element_cmds_.size(); ++i) {
	        fprintf(stderr, " %d", inspect_element_cmds_[i]);
	      }
	      fprintf(stderr, "\n");
	    }

	    if (link_url.empty()) return;

	    fprintf(stderr,
	            "[ck-browser] context menu default cmd ids: new_tab=%d new_window=%d\n",
	            default_open_link_new_tab_cmd_,
	            default_open_link_new_window_cmd_);

	    model->InsertItemAt(0, 26500, "Open Link in New Tab (CK)");
	    model->InsertItemAt(1, 26501, "Open Link in New Window (CK)");
	    model->InsertSeparatorAt(2);
	  }

	  bool OnContextMenuCommand(CefRefPtr<CefBrowser> browser,
	                            CefRefPtr<CefFrame> frame,
	                            CefRefPtr<CefContextMenuParams> params,
	                            int command_id,
	                            EventFlags event_flags) override {
	    CEF_REQUIRE_UI_THREAD();
	    (void)browser;
	    (void)frame;
	    (void)event_flags;
	    if (!params) return false;
	    std::string link_url = params->GetLinkUrl().ToString();
	    fprintf(stderr,
	            "[ck-browser] OnContextMenuCommand tab=%s (%p) cmd=%d url=%s\n",
	            tab_ ? (tab_->base_title.empty() ? "Tab" : tab_->base_title.c_str()) : "(none)",
	            (void *)tab_,
	            command_id,
	            link_url.c_str());
	    bool is_inspect = false;
	    for (int cmd : inspect_element_cmds_) {
	      if (cmd == command_id) {
	        is_inspect = true;
	        break;
	      }
	    }
	    if (is_inspect) {
	      fprintf(stderr,
	              "[ck-browser] intercept Inspect Element cmd=%d at (%d,%d)\n",
	              command_id,
	              last_context_x_,
	              last_context_y_);
	      if (tab_) {
	        show_devtools_for_tab(tab_, last_context_x_, last_context_y_);
	      }
	      return true;
	    }
	    if (link_url.empty()) link_url = last_context_link_url_;
	    if (link_url.empty()) return (command_id == 26500 || command_id == 26501 ||
	                                  command_id == default_open_link_new_tab_cmd_ ||
	                                  command_id == default_open_link_new_window_cmd_);
	    link_url = normalize_url(link_url.c_str());
	    if (link_url.empty()) return (command_id == 26500 || command_id == 26501 ||
	                                  command_id == default_open_link_new_tab_cmd_ ||
	                                  command_id == default_open_link_new_window_cmd_);

	    if (command_id == 26501 || command_id == default_open_link_new_window_cmd_) {
	      spawn_new_browser_window(link_url);
	      return true;
	    }
	    if (command_id == 26500 || command_id == default_open_link_new_tab_cmd_) {
	      open_url_in_new_tab(link_url, true);
	      return true;
	    }
	    return false;
	  }

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    CEF_REQUIRE_UI_THREAD();
    if (tab_) {
      tab_->browser = browser;
    }
  }

  void OnLoadStart(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   TransitionType transition_type) override {
    CEF_REQUIRE_UI_THREAD();
    (void)browser;
    (void)transition_type;
    if (!tab_ || !frame || !frame->IsMain()) return;
    std::string url = frame->GetURL().ToString();
    if (url.empty()) return;
    tab_->pending_url = url;
    std::string host = extract_host_from_url(url);
    if (host != tab_->current_host) {
      tab_->current_host = host;
      clear_tab_favicon(tab_);
    }
    tab_->loading = true;
    if (tab_ == g_current_tab) {
      update_url_field_for_tab(tab_);
      update_reload_button_for_tab(tab_);
    }
  }

  void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                 CefRefPtr<CefFrame> frame,
                 int httpStatusCode) override {
    CEF_REQUIRE_UI_THREAD();
    (void)httpStatusCode;
    if (!tab_ || !browser || !frame || !frame->IsMain()) return;
    CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("ck_request_theme_color");
    frame->SendProcessMessage(PID_RENDERER, msg);
  }

  bool OnOpenURLFromTab(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        const CefString& target_url,
                        cef_window_open_disposition_t target_disposition,
                        bool user_gesture) override {
    CEF_REQUIRE_UI_THREAD();
    (void)browser;
    (void)frame;
    (void)user_gesture;
    std::string url = target_url.ToString();
    fprintf(stderr,
            "[ck-browser] OnOpenURLFromTab tab=%s (%p) disposition=%d(%s) user_gesture=%d url=%s\n",
            tab_ ? (tab_->base_title.empty() ? "Tab" : tab_->base_title.c_str()) : "(none)",
            (void *)tab_,
            (int)target_disposition,
            window_disposition_name(target_disposition),
            user_gesture ? 1 : 0,
            url.c_str());
    if (url.empty()) return false;
    if (is_devtools_url(url)) {
      fprintf(stderr, "[ck-browser] devtools url allowed (OnOpenURLFromTab)\n");
      return false;
    }
    url = normalize_url(url.c_str());
    if (url.empty()) return true;

    if (target_disposition == CEF_WOD_NEW_WINDOW ||
        target_disposition == CEF_WOD_OFF_THE_RECORD) {
      spawn_new_browser_window(url);
      return true;
    }

    if (target_disposition == CEF_WOD_NEW_FOREGROUND_TAB) {
      open_url_in_new_tab(url, true);
      return true;
    }

    if (target_disposition == CEF_WOD_NEW_BACKGROUND_TAB) {
      open_url_in_new_tab(url, false);
      return true;
    }

    return false;
  }

  bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     int popup_id,
                     const CefString& target_url,
                     const CefString& target_frame_name,
                     cef_window_open_disposition_t target_disposition,
                     bool user_gesture,
                     const CefPopupFeatures& popupFeatures,
                     CefWindowInfo& windowInfo,
                     CefRefPtr<CefClient>& client,
                     CefBrowserSettings& settings,
                     CefRefPtr<CefDictionaryValue>& extra_info,
                     bool* no_javascript_access) override {
    CEF_REQUIRE_UI_THREAD();
    (void)browser;
    (void)frame;
    (void)popup_id;
    (void)target_frame_name;
    (void)user_gesture;
    (void)popupFeatures;
    (void)windowInfo;
    (void)client;
    (void)settings;
    (void)extra_info;
    (void)no_javascript_access;

    std::string url = target_url.ToString();
    std::string frame_name = target_frame_name.ToString();
    fprintf(stderr,
            "[ck-browser] OnBeforePopup tab=%s (%p) popup_id=%d disposition=%d(%s) user_gesture=%d frame_name=%s url=%s\n",
            tab_ ? (tab_->base_title.empty() ? "Tab" : tab_->base_title.c_str()) : "(none)",
            (void *)tab_,
            popup_id,
            (int)target_disposition,
            window_disposition_name(target_disposition),
            user_gesture ? 1 : 0,
            frame_name.c_str(),
            url.c_str());
    fprintf(stderr,
            "[ck-browser] OnBeforePopup features x=%d(xSet=%d) y=%d(ySet=%d) w=%d(wSet=%d) h=%d(hSet=%d) isPopup=%d\n",
            popupFeatures.x,
            popupFeatures.xSet,
            popupFeatures.y,
            popupFeatures.ySet,
            popupFeatures.width,
            popupFeatures.widthSet,
            popupFeatures.height,
            popupFeatures.heightSet,
            popupFeatures.isPopup);
    if (url.empty()) {
      return false;
    }
    if (is_devtools_url(url)) {
      fprintf(stderr, "[ck-browser] devtools url allowed (OnBeforePopup)\n");
      return false;
    }
    url = normalize_url(url.c_str());
    if (url.empty()) {
      return true;
    }

    if (target_disposition == CEF_WOD_NEW_WINDOW ||
        target_disposition == CEF_WOD_OFF_THE_RECORD) {
      spawn_new_browser_window(url);
      return true;
    }

    if (target_disposition == CEF_WOD_NEW_POPUP ||
        target_disposition == CEF_WOD_NEW_FOREGROUND_TAB) {
      open_url_in_new_tab(url, true);
      return true;
    }

    if (target_disposition == CEF_WOD_NEW_BACKGROUND_TAB) {
      open_url_in_new_tab(url, false);
      return true;
    }

    return false;
  }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override {
    CEF_REQUIRE_UI_THREAD();
    (void)browser;
    (void)frame;
    (void)source_process;
    if (!tab_ || !message) return false;
    std::string name = message->GetName().ToString();
    if (name != "ck_theme_color") return false;
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    if (!args || args->GetSize() < 3) return true;
    int r = args->GetInt(0);
    int g = args->GetInt(1);
    int b = args->GetInt(2);
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    tab_->theme_r = (unsigned char)r;
    tab_->theme_g = (unsigned char)g;
    tab_->theme_b = (unsigned char)b;
    tab_->has_theme_color = true;
    fprintf(stderr, "[ck-browser] theme color tab=%s (%p) rgb=%d,%d,%d\n",
            tab_->base_title.empty() ? "Tab" : tab_->base_title.c_str(),
            (void *)tab_,
            r, g, b);
    if (tab_ == g_current_tab) {
      apply_tab_theme_colors(tab_, true);
    }
    return true;
  }

  bool DoClose(CefRefPtr<CefBrowser> browser) override {
    CEF_REQUIRE_UI_THREAD();
    (void)browser;
    return false;
  }

  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    CEF_REQUIRE_UI_THREAD();
    if (tab_ && tab_->browser == browser) {
      tab_->browser = nullptr;
    }
    on_cef_browser_closed("browser");
  }

  void detach_tab() {
    tab_ = nullptr;
  }

  void OnAddressChange(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       const CefString &url) override {
    CEF_REQUIRE_UI_THREAD();
    (void)browser;
    if (!tab_ || !frame || !frame->IsMain()) return;
    tab_->current_url = url.ToString();
    update_tab_security_status(tab_);
    if (tab_ == g_current_tab) {
      update_security_controls(tab_);
    }
    if (tab_ == g_current_tab && g_url_field) {
      XmTextFieldSetString(g_url_field, const_cast<char *>(tab_->current_url.c_str()));
    }
  }

  void OnStatusMessage(CefRefPtr<CefBrowser> browser, const CefString &value) override {
    CEF_REQUIRE_UI_THREAD();
    (void)browser;
    if (!tab_) return;
    std::string message = value.ToString();
    if (message == "Ready") {
      message.clear();
    }
    tab_->status_message = message;
    if (is_tab_selected(tab_) && g_current_tab != tab_) {
        set_current_tab(tab_);
    }
    bool is_current = (tab_ == g_current_tab);
    fprintf(stderr, "[ck-browser] OnStatusMessage for tab %s (%p): incoming='%s' stored='%s' (current=%s)\n",
            tab_->base_title.empty() ? "Tab" : tab_->base_title.c_str(),
            (void *)tab_,
            message.c_str(),
            tab_->status_message.c_str(),
            is_current ? "yes" : "no");
    if (is_current) {
      set_status_label_text(tab_->status_message.c_str());
    }
  }

  void OnTitleChange(CefRefPtr<CefBrowser> browser,
                     const CefString &title) override {
    CEF_REQUIRE_UI_THREAD();
    (void)browser;
    if (!tab_) return;
    std::string new_title = title.ToString();
    if (new_title.empty()) {
      new_title = tab_->base_title.empty() ? "New Tab" : tab_->base_title;
    }
    update_tab_label(tab_, new_title.c_str());
    update_all_tab_labels("title change");
  }

  void OnFaviconURLChange(CefRefPtr<CefBrowser> browser,
                          const std::vector<CefString> &icon_urls) override {
    CEF_REQUIRE_UI_THREAD();
    (void)browser;
    if (!tab_) return;
    if (icon_urls.empty()) return;
    tab_->favicon_url = icon_urls[0].ToString();
    if (tab_->favicon_url.empty()) return;
    fprintf(stderr, "[ck-browser] OnFaviconURLChange tab=%s (%p) url=%s\n",
            tab_->base_title.empty() ? "Tab" : tab_->base_title.c_str(),
            (void *)tab_,
            tab_->favicon_url.c_str());
    request_favicon_download(tab_, "OnFaviconURLChange");
    if (tab_ == g_current_tab) {
      update_favicon_controls(tab_);
    }
  }

  void OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                            bool isLoading,
                            bool canGoBack,
                            bool canGoForward) override {
    CEF_REQUIRE_UI_THREAD();
    (void)browser;
    if (!tab_) return;
    tab_->can_go_back = canGoBack;
    tab_->can_go_forward = canGoForward;
    tab_->loading = isLoading;
    if (!isLoading) {
      update_tab_security_status(tab_);
      if (tab_ == g_current_tab) {
        update_security_controls(tab_);
      }
    }
    if (tab_ == g_current_tab) {
      update_navigation_buttons(tab_);
      update_reload_button_for_tab(tab_);
    }
  }

  void OnGotFocus(CefRefPtr<CefBrowser> browser) override {
    CEF_REQUIRE_UI_THREAD();
    (void)browser;
    if (!tab_) return;
    if (is_tab_selected(tab_)) {
      focus_browser_area(tab_);
    }
  }

 private:
	  BrowserTab *tab_ = NULL;
	  int default_open_link_new_tab_cmd_ = -1;
	  int default_open_link_new_window_cmd_ = -1;
	  std::vector<int> inspect_element_cmds_;
	  int last_context_x_ = 0;
	  int last_context_y_ = 0;
	  std::string last_context_link_url_;
	  IMPLEMENT_REFCOUNTING(BrowserClient);
};

class CkCefApp : public CefApp, public CefRenderProcessHandler {
 public:
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override {
    (void)browser;
    (void)source_process;
    if (!frame || !message) return false;
    std::string name = message->GetName().ToString();
    if (name != "ck_request_theme_color") return false;

    CefRefPtr<CefV8Context> ctx = frame->GetV8Context();
    if (!ctx || !ctx->IsValid()) return true;
    if (!ctx->Enter()) return true;

    CefRefPtr<CefV8Value> retval;
    CefRefPtr<CefV8Exception> exception;
    const char *code =
        "(function(){"
        "function norm(c){"
        "  var d=document.createElement('div');"
        "  d.style.color=c;"
        "  (document.body||document.documentElement).appendChild(d);"
        "  var s=getComputedStyle(d).color||'';"
        "  d.remove();"
        "  var m=s.match(/rgba?\\((\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)/);"
        "  if(m) return [parseInt(m[1]),parseInt(m[2]),parseInt(m[3])];"
        "  return null;"
        "}"
        "var c='';"
        "var meta=document.querySelector('meta[name=\"theme-color\"]');"
        "if(meta&&meta.content) c=meta.content;"
        "if(!c){"
        "  var e=document.documentElement;"
        "  var cs=getComputedStyle(e);"
        "  c=(cs&&cs.backgroundColor)||'';"
        "}"
        "if(!c&&document.body){"
        "  var cs2=getComputedStyle(document.body);"
        "  c=(cs2&&cs2.backgroundColor)||'';"
        "}"
        "var rgb=norm(c||'#ffffff')||[255,255,255];"
        "return rgb;"
        "})()";

    bool ok = ctx->Eval(code, "ck_theme_color.js", 1, retval, exception);
    ctx->Exit();
    if (!ok || !retval || !retval->IsArray()) {
      return true;
    }

    int r = 255, g = 255, b = 255;
    CefRefPtr<CefV8Value> v0 = retval->GetValue(0);
    CefRefPtr<CefV8Value> v1 = retval->GetValue(1);
    CefRefPtr<CefV8Value> v2 = retval->GetValue(2);
    if (v0 && v0->IsInt()) r = v0->GetIntValue();
    if (v1 && v1->IsInt()) g = v1->GetIntValue();
    if (v2 && v2->IsInt()) b = v2->GetIntValue();
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;

    CefRefPtr<CefProcessMessage> reply = CefProcessMessage::Create("ck_theme_color");
    CefRefPtr<CefListValue> args = reply->GetArgumentList();
    args->SetInt(0, r);
    args->SetInt(1, g);
    args->SetInt(2, b);
    frame->SendProcessMessage(PID_BROWSER, reply);
    return true;
  }

 private:
  IMPLEMENT_REFCOUNTING(CkCefApp);
};

class DevToolsClient : public CefClient, public CefLifeSpanHandler {
 public:
  explicit DevToolsClient(BrowserTab *tab) : tab_(tab) {}

  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    CEF_REQUIRE_UI_THREAD();
    fprintf(stderr, "[ck-browser] DevToolsClient::OnAfterCreated tab=%p browser=%p\n",
            (void *)tab_, (void *)browser.get());
    if (tab_) {
      tab_->devtools_browser = browser;
      resize_devtools_to_area(tab_, "devtools created");
    }
  }

  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    CEF_REQUIRE_UI_THREAD();
    fprintf(stderr, "[ck-browser] DevToolsClient::OnBeforeClose tab=%p browser=%p\n",
            (void *)tab_, (void *)browser.get());
    (void)browser;
    if (!tab_) return;
    tab_->devtools_browser = nullptr;
    tab_->devtools_client = nullptr;
    if (tab_->devtools_shell) {
      XtDestroyWidget(tab_->devtools_shell);
      tab_->devtools_shell = NULL;
      tab_->devtools_area = NULL;
    }
    on_cef_browser_closed("devtools");
  }

  void detach_tab() {
    tab_ = nullptr;
  }

 private:
  BrowserTab *tab_ = NULL;
  IMPLEMENT_REFCOUNTING(DevToolsClient);
};

static void wm_delete_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    capture_session_state("wm delete");
    if (g_session_data && g_toplevel) {
        session_save(g_toplevel, g_session_data, g_subprocess_path);
    }
    save_last_session_file("wm delete");
    begin_shutdown_sequence("wm delete");
}

static bool alloc_rgb_pixel(Display *display, int screen, unsigned char r, unsigned char g, unsigned char b, Pixel *out_pixel)
{
    if (!out_pixel) return false;
    *out_pixel = 0;
    if (!display) return false;
    Colormap cmap = DefaultColormap(display, screen);
    XColor color;
    color.red = (unsigned short)r << 8;
    color.green = (unsigned short)g << 8;
    color.blue = (unsigned short)b << 8;
    color.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(display, cmap, &color)) return false;
    *out_pixel = color.pixel;
    return true;
}

static void pick_contrast_color(unsigned char bg_r, unsigned char bg_g, unsigned char bg_b,
                                unsigned char *out_r, unsigned char *out_g, unsigned char *out_b)
{
    double lum = 0.299 * (double)bg_r + 0.587 * (double)bg_g + 0.114 * (double)bg_b;
    bool use_dark = lum > 160.0;
    unsigned char v = use_dark ? 0 : 255;
    if (out_r) *out_r = v;
    if (out_g) *out_g = v;
    if (out_b) *out_b = v;
}

static void ensure_tab_default_colors(BrowserTab *tab)
{
    if (!tab || !tab->page || tab->tab_default_colors_initialized) return;
    Pixel bg = 0;
    Pixel fg = 0;
    XtVaGetValues(tab->page,
                  XmNtabBackground, &bg,
                  XmNtabForeground, &fg,
                  NULL);
    tab->tab_default_background = bg;
    tab->tab_default_foreground = fg;
    tab->tab_default_colors_initialized = true;
}

static void apply_tab_theme_colors(BrowserTab *tab, bool active)
{
    if (!tab || !tab->page) return;
    ensure_tab_default_colors(tab);

    if (!active || !tab->has_theme_color || !g_toplevel || !XtIsRealized(g_toplevel)) {
        XtVaSetValues(tab->page,
                      XmNtabBackground, tab->tab_default_background,
                      XmNtabForeground, tab->tab_default_foreground,
                      NULL);
        return;
    }

    Display *display = XtDisplay(g_toplevel);
    if (!display) return;
    int screen = DefaultScreen(display);

    Pixel bg_pixel = 0;
    if (!alloc_rgb_pixel(display, screen, tab->theme_r, tab->theme_g, tab->theme_b, &bg_pixel)) {
        return;
    }
    unsigned char fg_r = 0, fg_g = 0, fg_b = 0;
    pick_contrast_color(tab->theme_r, tab->theme_g, tab->theme_b, &fg_r, &fg_g, &fg_b);
    Pixel fg_pixel = 0;
    if (!alloc_rgb_pixel(display, screen, fg_r, fg_g, fg_b, &fg_pixel)) {
        fg_pixel = tab->tab_default_foreground;
    }
    XtVaSetValues(tab->page,
                  XmNtabBackground, bg_pixel,
                  XmNtabForeground, fg_pixel,
                  NULL);
}

static XmString make_string(const char *text)
{
    return XmStringCreateLocalized((String)(text ? text : ""));
}

static void capture_session_state(const char *reason)
{
    LOG_ENTER("reason=%s session=%p toplevel=%p",
              reason ? reason : "(null)",
              (void *)g_session_data,
              (void *)g_toplevel);
    if (!g_session_data || !g_toplevel) {
        LOG_ENTER("capture skipped (session=%p toplevel=%p)",
                  (void *)g_session_data,
                  (void *)g_toplevel);
        return;
    }
    session_capture_geometry(g_toplevel, g_session_data, "x", "y", "w", "h");

    BrowserTab *selected = get_selected_tab();
    int active_idx = selected ? get_visible_tab_index(selected) : -1;
    if (active_idx < 0) active_idx = 0;
    session_data_set_int(g_session_data, "active_tab", active_idx);

    int count = 0;
    for (const auto &entry : g_browser_tabs) {
        BrowserTab *tab = entry.get();
        if (!tab) continue;
        char key[64];
        snprintf(key, sizeof(key), "tab_url_%d", count);
        const char *url = display_url_for_tab(tab);
        session_data_set(g_session_data, key, url ? url : "");
        count++;
        if (count > 100) break;
    }
    session_data_set_int(g_session_data, "tab_count", count);
    fprintf(stderr, "[ck-browser] capture_session_state reason=%s tabs=%d active=%d\n",
            reason ? reason : "(null)", count, active_idx);
}

static void ensure_parent_dir_exists(const char *path)
{
    if (!path || !path[0]) return;
    char dir[PATH_MAX];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';
    if (!dir[0]) return;
    mkdir(dir, 0700);
}

static std::string load_homepage_file()
{
    char path[PATH_MAX];
    config_build_path(path, sizeof(path), "ck-browser.homepage");
    if (!path[0]) return std::string();
    FILE *fp = fopen(path, "r");
    if (!fp) return std::string();
    char line[4096];
    std::string url;
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;
        if (line[0] == '#') continue;
        url = std::string(line);
        break;
    }
    fclose(fp);
    if (url.empty()) return std::string();
    url = normalize_url(url.c_str());
    return url;
}

static void save_homepage_file(const std::string &url, const char *reason)
{
    char path[PATH_MAX];
    config_build_path(path, sizeof(path), "ck-browser.homepage");
    if (!path[0]) return;
    ensure_parent_dir_exists(path);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "# ck-browser homepage\n");
    fprintf(fp, "%s\n", url.c_str());
    fclose(fp);
    fprintf(stderr, "[ck-browser] saved homepage reason=%s url=%s path=%s\n",
            reason ? reason : "(null)", url.c_str(), path);
}

static bool has_opengl_support()
{
    const char *libs[] = {"libGL.so.1", "libGL.so", NULL};
    for (const char **it = libs; *it; ++it) {
        void *handle = dlopen(*it, RTLD_LAZY | RTLD_LOCAL);
        if (!handle) continue;
        void *sym = dlsym(handle, "glXGetCurrentContext");
        dlclose(handle);
        if (sym) return true;
    }
    return false;
}

static void apply_gpu_switches()
{
    if (!g_force_disable_gpu) return;
    CefRefPtr<CefCommandLine> global = CefCommandLine::GetGlobalCommandLine();
    if (!global) return;
    global->AppendSwitch("disable-gpu");
    global->AppendSwitch("disable-software-rasterizer");
    global->AppendSwitch("disable-gpu-compositing");
}

static void save_last_session_file(const char *reason)
{
    LOG_ENTER("reason=%s", reason ? reason : "(null)");
    char path[PATH_MAX];
    config_build_path(path, sizeof(path), "ck-browser.lastsession");
    if (!path[0]) return;
    ensure_parent_dir_exists(path);

    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "# ck-browser last session\n");
    int count = 0;
    for (const auto &entry : g_browser_tabs) {
        BrowserTab *tab = entry.get();
        if (!tab) continue;
        const char *url = display_url_for_tab(tab);
        if (url && url[0]) {
            fprintf(fp, "%s\n", url);
            count++;
        }
    }
    fclose(fp);
    fprintf(stderr, "[ck-browser] saved last session reason=%s path=%s tabs=%d\n",
            reason ? reason : "(null)", path, count);
}

static void restore_last_session_from_file(const char *reason)
{
    (void)reason;
    if (!g_tab_stack) return;
    char path[PATH_MAX];
    config_build_path(path, sizeof(path), "ck-browser.lastsession");
    if (!path[0]) return;
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "[ck-browser] restore session: no file %s\n", path);
        return;
    }
    std::vector<std::string> urls;
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;
        if (line[0] == '#') continue;
        urls.emplace_back(normalize_url(line));
        if ((int)urls.size() >= 100) break;
    }
    fclose(fp);
    fprintf(stderr, "[ck-browser] restore last session reason=%s path=%s tabs=%d\n",
            reason ? reason : "(null)", path, (int)urls.size());

    std::vector<Widget> existing_pages;
    existing_pages.reserve(g_browser_tabs.size());
    for (const auto &entry : g_browser_tabs) {
        BrowserTab *tab = entry.get();
        if (tab && tab->page) {
            existing_pages.push_back(tab->page);
        }
    }
    for (Widget page : existing_pages) {
        XtDestroyWidget(page);
    }

    if (urls.empty()) {
        const char *fallback = g_homepage_url.empty() ? kInitialBrowserUrl : g_homepage_url.c_str();
        BrowserTab *tab_home = create_tab_page(g_tab_stack, "tabWelcome", "Welcome", "Welcome", fallback);
        schedule_tab_browser_creation(tab_home);
        XmTabStackSelectTab(tab_home->page, False);
        set_current_tab(tab_home);
        return;
    }

    std::vector<BrowserTab *> created;
    created.reserve(urls.size());
    int idx = 0;
    for (const auto &url : urls) {
        if (url.empty()) continue;
        idx++;
        char name[32];
        snprintf(name, sizeof(name), "tabRest%d", idx);
        BrowserTab *tab = create_tab_page(g_tab_stack, name, url.c_str(), "Session", url.c_str());
        schedule_tab_browser_creation(tab);
        created.push_back(tab);
    }
    if (!created.empty()) {
        XmTabStackSelectTab(created[0]->page, False);
        set_current_tab(created[0]);
    }
}

static void wm_save_yourself_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    fprintf(stderr, "[ck-browser] WM_SAVE_YOURSELF\n");
    capture_session_state("wm_save_yourself");
    if (g_session_data && g_toplevel) {
        session_save(g_toplevel, g_session_data, g_subprocess_path);
    }
    save_last_session_file("wm_save_yourself");
}

static void restore_tabs_from_session_data(SessionData *data)
{
    if (!data || !g_tab_stack) return;
    int count = session_data_get_int(data, "tab_count", 0);
    int active = session_data_get_int(data, "active_tab", 0);
    if (count <= 0) return;
    if (count > 100) count = 100;
    if (active < 0) active = 0;
    if (active >= count) active = 0;
    std::vector<BrowserTab *> created;
    created.reserve((size_t)count);
    for (int i = 0; i < count; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "tab_url_%d", i);
        const char *url = session_data_get(data, key);
        std::string normalized = normalize_url(url ? url : "");
        if (normalized.empty()) {
            normalized = kInitialBrowserUrl;
        }
        char name[32];
        snprintf(name, sizeof(name), "tabSess%d", i + 1);
        char title[256];
        snprintf(title, sizeof(title), "%s", normalized.c_str());
        BrowserTab *tab = create_tab_page(g_tab_stack, name, title, "Session", normalized.c_str());
        schedule_tab_browser_creation(tab);
        created.push_back(tab);
    }
    if (!created.empty()) {
        BrowserTab *tab = created[(size_t)active];
        XmTabStackSelectTab(tab->page, False);
        set_current_tab(tab);
    }
}

static void on_restore_session(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    restore_last_session_from_file("menu");
}

static void cef_message_pump(XtPointer client_data, XtIntervalId *id)
{
    (void)client_data;
    (void)id;
    CefDoMessageLoopWork();
    poll_zoom_levels();
    XtAppAddTimeOut(g_app, 10, cef_message_pump, NULL);
}

static std::string normalize_url(const char *input)
{
    if (!input || input[0] == '\0') return std::string();
    std::string normalized(input);
    size_t colon = normalized.find(':');
    bool has_scheme = false;
    if (colon != std::string::npos && colon > 0) {
        std::string prefix = normalized.substr(0, colon);
        std::string lower;
        lower.reserve(prefix.size());
        for (char c : prefix) {
            if (c >= 'A' && c <= 'Z') lower.push_back((char)(c - 'A' + 'a'));
            else lower.push_back(c);
        }
        if (lower == "about" || lower == "chrome" || lower == "chrome-devtools" ||
            lower == "devtools" || lower == "data" || lower == "file" ||
            lower == "view-source" || lower == "javascript" || lower == "mailto") {
            has_scheme = true;
        } else if (normalized.size() >= colon + 3 && normalized[colon + 1] == '/' &&
                   normalized[colon + 2] == '/') {
            has_scheme = true;
        }
    }
    if (!has_scheme) {
        normalized.insert(0, "https://");
    }
    return normalized;
}

static bool is_url_parseable(const std::string &url)
{
    if (url.empty()) return false;
    CefURLParts parts;
    CefString cef_url(url);
    return CefParseURL(cef_url, parts);
}

static const char *window_disposition_name(cef_window_open_disposition_t disposition)
{
    switch (disposition) {
        case CEF_WOD_UNKNOWN: return "UNKNOWN";
        case CEF_WOD_CURRENT_TAB: return "CURRENT_TAB";
        case CEF_WOD_SINGLETON_TAB: return "SINGLETON_TAB";
        case CEF_WOD_NEW_FOREGROUND_TAB: return "NEW_FOREGROUND_TAB";
        case CEF_WOD_NEW_BACKGROUND_TAB: return "NEW_BACKGROUND_TAB";
        case CEF_WOD_NEW_POPUP: return "NEW_POPUP";
        case CEF_WOD_NEW_WINDOW: return "NEW_WINDOW";
        case CEF_WOD_SAVE_TO_DISK: return "SAVE_TO_DISK";
        case CEF_WOD_OFF_THE_RECORD: return "OFF_THE_RECORD";
        case CEF_WOD_IGNORE_ACTION: return "IGNORE_ACTION";
        case CEF_WOD_SWITCH_TO_TAB: return "SWITCH_TO_TAB";
        case CEF_WOD_NEW_PICTURE_IN_PICTURE: return "NEW_PICTURE_IN_PICTURE";
        default: return "OTHER";
    }
}

static bool is_devtools_url(const std::string &url)
{
    if (url.empty()) return false;
    return (url.rfind("chrome-devtools://", 0) == 0) || (url.rfind("devtools://", 0) == 0);
}

static void devtools_resize_timer_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)id;
    BrowserTab *tab = (BrowserTab *)client_data;
    if (!tab) return;
    resize_devtools_to_area(tab, "devtools timer");
}

static void start_devtools_browser_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)id;
    BrowserTab *tab = (BrowserTab *)client_data;
    if (!tab) return;
    tab->devtools_show_scheduled = false;
    if (!tab->browser || !tab->devtools_area || !tab->devtools_shell || !tab->devtools_client) {
        return;
    }

    if (!XtIsRealized(tab->devtools_area)) {
        if (g_app) {
            tab->devtools_show_scheduled = true;
            XtAppAddTimeOut(g_app, 20, start_devtools_browser_cb, (XtPointer)tab);
        }
        return;
    }

    CefRefPtr<CefBrowserHost> host = tab->browser->GetHost();
    if (!host) return;

    Dimension width = 0;
    Dimension height = 0;
    XtVaGetValues(tab->devtools_area, XmNwidth, &width, XmNheight, &height, NULL);
    if (width <= 1 || height <= 1) {
        if (g_app) {
            tab->devtools_show_scheduled = true;
            XtAppAddTimeOut(g_app, 20, start_devtools_browser_cb, (XtPointer)tab);
        }
        return;
    }

    Window xid = XtWindow(tab->devtools_area);
    if (!xid) {
        if (g_app) {
            tab->devtools_show_scheduled = true;
            XtAppAddTimeOut(g_app, 20, start_devtools_browser_cb, (XtPointer)tab);
        }
        return;
    }

    CefWindowInfo wi;
    wi.SetAsChild((CefWindowHandle)xid, CefRect(0, 0, (int)width, (int)height));
    CefBrowserSettings bs;
    CefPoint point(tab->devtools_inspect_x, tab->devtools_inspect_y);
    fprintf(stderr,
            "[ck-browser] ShowDevTools tab=%p parent_xid=%lu area=%dx%d point=%d,%d\n",
            (void *)tab,
            (unsigned long)xid,
            (int)width,
            (int)height,
            tab->devtools_inspect_x,
            tab->devtools_inspect_y);
    host->ShowDevTools(wi, tab->devtools_client, bs, point);
    fprintf(stderr, "[ck-browser] ShowDevTools returned HasDevTools=%d\n", host->HasDevTools() ? 1 : 0);
    resize_devtools_to_area(tab, "devtools show");
    if (g_app) {
        XtAppAddTimeOut(g_app, 50, devtools_resize_timer_cb, (XtPointer)tab);
    } else {
        devtools_resize_timer_cb((XtPointer)tab, NULL);
    }
}

static void show_devtools_for_tab(BrowserTab *tab, int inspect_x, int inspect_y)
{
    if (!tab || !tab->browser || !g_toplevel) return;
    CefRefPtr<CefBrowserHost> host = tab->browser->GetHost();
    if (!host) return;

    if (tab->devtools_shell && XtIsRealized(tab->devtools_shell)) {
        XtPopup(tab->devtools_shell, XtGrabNone);
        return;
    }

    bool had_devtools = host->HasDevTools();
    fprintf(stderr, "[ck-browser] show_devtools_for_tab tab=%p HasDevTools=%d\n",
            (void *)tab, had_devtools ? 1 : 0);
    if (had_devtools) {
        fprintf(stderr, "[ck-browser] show_devtools_for_tab closing existing DevTools first\n");
        host->CloseDevTools();
    }

    Widget shell = XtVaCreatePopupShell(
        "devtoolsShell",
        topLevelShellWidgetClass,
        g_toplevel,
        XmNtitle, "Developer Tools",
        XmNiconName, "Developer Tools",
        XmNwidth, 1100,
        XmNheight, 800,
        NULL);

    Widget form = XmCreateForm(shell, xm_name("devtoolsForm"), NULL, 0);
    XtVaSetValues(form,
                  XmNmarginWidth, 0,
                  XmNmarginHeight, 0,
                  NULL);
    XtManageChild(form);

    Widget area = XmCreateDrawingArea(form, xm_name("devtoolsView"), NULL, 0);
    XtVaSetValues(area,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNresizePolicy, XmRESIZE_ANY,
                  NULL);
    XtAddCallback(area, XmNresizeCallback, on_devtools_area_resize, tab);
    XtManageChild(area);

    XtRealizeWidget(shell);
    XtPopup(shell, XtGrabNone);

    Display *dpy = XtDisplay(area);
    Window xid = XtWindow(area);
    if (!dpy || !xid) {
        fprintf(stderr, "[ck-browser] show_devtools: missing XID\n");
        XtDestroyWidget(shell);
        return;
    }

    tab->devtools_shell = shell;
    tab->devtools_area = area;
    tab->devtools_client = new DevToolsClient(tab);

    Atom wm_delete = XmInternAtom(XtDisplay(shell), xm_name("WM_DELETE_WINDOW"), False);
    XmAddWMProtocolCallback(shell, wm_delete, on_devtools_shell_wm_delete, tab);
    XmActivateWMProtocol(shell, wm_delete);

    tab->devtools_inspect_x = inspect_x;
    tab->devtools_inspect_y = inspect_y;
    tab->devtools_show_scheduled = false;
    if (g_app) {
        tab->devtools_show_scheduled = true;
        XtAppAddTimeOut(g_app, 20, start_devtools_browser_cb, (XtPointer)tab);
    } else {
        start_devtools_browser_cb((XtPointer)tab, NULL);
    }
}

static void resize_devtools_to_area(BrowserTab *tab, const char *reason)
{
    if (!tab || !tab->devtools_browser || !tab->devtools_area) return;
    CefRefPtr<CefBrowserHost> host = tab->devtools_browser->GetHost();
    if (!host) return;

    Dimension width = 0;
    Dimension height = 0;
    XtVaGetValues(tab->devtools_area, XmNwidth, &width, XmNheight, &height, NULL);
    if (width <= 1 || height <= 1) return;

    CefWindowHandle child_handle = host->GetWindowHandle();
    Display *dpy = XtDisplay(tab->devtools_area);
    if (child_handle && dpy) {
        XMoveResizeWindow(dpy,
                          (Window)child_handle,
                          0,
                          0,
                          (unsigned int)width,
                          (unsigned int)height);
        XFlush(dpy);
    }

    host->NotifyMoveOrResizeStarted();
    host->WasResized();

    fprintf(stderr,
            "[ck-browser] resize_devtools_to_area(%s) tab=%p child=%lu area=%dx%d\n",
            reason ? reason : "unknown",
            (void *)tab,
            (unsigned long)child_handle,
            (int)width,
            (int)height);
}

static void on_devtools_area_resize(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    BrowserTab *tab = (BrowserTab *)client_data;
    if (!tab) return;
    resize_devtools_to_area(tab, "devtools area resize");
}

static void on_devtools_shell_wm_delete(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    BrowserTab *tab = (BrowserTab *)client_data;
    if (!tab || !tab->browser) return;
    CefRefPtr<CefBrowserHost> host = tab->browser->GetHost();
    if (host && host->HasDevTools()) {
        fprintf(stderr, "[ck-browser] devtools WM_DELETE -> CloseDevTools tab=%p\n", (void *)tab);
        host->CloseDevTools();
    }
    if (tab->devtools_shell) {
        XtDestroyWidget(tab->devtools_shell);
        tab->devtools_shell = NULL;
        tab->devtools_area = NULL;
    }
    tab->devtools_browser = nullptr;
    tab->devtools_client = nullptr;
}

static const char *display_url_for_tab(const BrowserTab *tab)
{
    if (!tab) return kInitialBrowserUrl;
    if (!tab->current_url.empty()) return tab->current_url.c_str();
    if (!tab->pending_url.empty()) return tab->pending_url.c_str();
    return kInitialBrowserUrl;
}

static void update_url_field_for_tab(BrowserTab *tab)
{
    if (!g_url_field) return;
    const char *value = display_url_for_tab(tab);
    XmTextFieldSetString(g_url_field, const_cast<char *>(value ? value : ""));
}

static void set_status_label_text(const char *text)
{
    if (!g_status_message_label) return;
    const char *display = text ? text : "";
    XmString xm_text = make_string(display);
    XtVaSetValues(g_status_message_label, XmNlabelString, xm_text, NULL);
    XmStringFree(xm_text);
}

static void set_security_label_text(const char *text)
{
    if (!g_security_label) return;
    const char *display = text ? text : "Security: None";
    XmString xm_text = make_string(display);
    XtVaSetValues(g_security_label, XmNlabelString, xm_text, NULL);
    XmStringFree(xm_text);
}

static void update_security_controls(BrowserTab *tab)
{
    if (!tab) {
        set_security_label_text("Security: None");
        return;
    }
    if (tab->security_status.empty()) {
        update_tab_security_status(tab);
    }
    set_security_label_text(tab->security_status.empty() ? "Security: None" : tab->security_status.c_str());
}

static std::string cert_status_to_short_text(cef_cert_status_t status)
{
    if (status == CERT_STATUS_NONE) {
        return "Valid Certificate";
    }
    std::vector<std::string> issues;
    if (status & CERT_STATUS_COMMON_NAME_INVALID) issues.push_back("CN mismatch");
    if (status & CERT_STATUS_DATE_INVALID) issues.push_back("Expired");
    if (status & CERT_STATUS_AUTHORITY_INVALID) issues.push_back("Untrusted");
    if (status & CERT_STATUS_REVOKED) issues.push_back("Revoked");
    if (status & CERT_STATUS_INVALID) issues.push_back("Invalid");
    if (status & CERT_STATUS_WEAK_SIGNATURE_ALGORITHM) issues.push_back("Weak signature");
    if (status & CERT_STATUS_NON_UNIQUE_NAME) issues.push_back("Non-unique name");
    if (status & CERT_STATUS_WEAK_KEY) issues.push_back("Weak key");
    if (status & CERT_STATUS_PINNED_KEY_MISSING) issues.push_back("Pinned key missing");
    if (issues.empty()) {
        return "Certificate Error";
    }
    std::string joined;
    for (size_t i = 0; i < issues.size(); ++i) {
        if (i) joined += ", ";
        joined += issues[i];
    }
    return joined;
}

static std::string content_status_to_short_text(cef_ssl_content_status_t status)
{
    if (status == SSL_CONTENT_NORMAL_CONTENT) {
        return "No Mixed Content";
    }
    std::vector<std::string> issues;
    if (status & SSL_CONTENT_DISPLAYED_INSECURE_CONTENT) issues.push_back("Displayed mixed content");
    if (status & SSL_CONTENT_RAN_INSECURE_CONTENT) issues.push_back("Ran mixed content");
    if (issues.empty()) {
        return "Mixed Content";
    }
    std::string joined;
    for (size_t i = 0; i < issues.size(); ++i) {
        if (i) joined += ", ";
        joined += issues[i];
    }
    return joined;
}

static void update_tab_security_status(BrowserTab *tab)
{
    if (!tab) return;
    std::string url = tab->current_url.empty() ? tab->pending_url : tab->current_url;
    bool is_https = (url.rfind("https://", 0) == 0);
    if (!tab->browser || !is_https) {
        tab->security_status = is_https ? "Security: TLS (unknown)" : "Security: None";
        return;
    }

    CefRefPtr<CefBrowserHost> host = tab->browser->GetHost();
    if (!host) {
        tab->security_status = "Security: TLS (unknown)";
        return;
    }
    CefRefPtr<CefNavigationEntry> entry = host->GetVisibleNavigationEntry();
    if (!entry) {
        tab->security_status = "Security: TLS (unknown)";
        return;
    }
    CefRefPtr<CefSSLStatus> ssl = entry->GetSSLStatus();
    if (!ssl || !ssl->IsSecureConnection()) {
        tab->security_status = "Security: None";
        return;
    }

    cef_cert_status_t cert = ssl->GetCertStatus();
    cef_ssl_content_status_t content = ssl->GetContentStatus();
    if (cert != CERT_STATUS_NONE) {
        tab->security_status = std::string("Security: TLS (") + cert_status_to_short_text(cert) + ")";
        return;
    }
    if (content != SSL_CONTENT_NORMAL_CONTENT) {
        tab->security_status = std::string("Security: TLS (") + content_status_to_short_text(content) + ")";
        return;
    }
    tab->security_status = "Security: TLS (Valid)";
}

static void update_tab_label(BrowserTab *tab, const char *text)
{
    if (!tab || !tab->page) return;
    const char *label = (text && text[0]) ? text : (tab->base_title.empty() ? "New Tab" : tab->base_title.c_str());
    tab->title_full = label ? label : "";

    Dimension tabbox_width = 0;
    Widget tabbox = g_tab_stack ? find_tab_stack_tabbox(g_tab_stack) : NULL;
    if (tabbox) {
        XtVaGetValues(tabbox, XmNwidth, &tabbox_width, NULL);
    } else if (g_tab_stack) {
        XtVaGetValues(g_tab_stack, XmNwidth, &tabbox_width, NULL);
    }
    int available_width = (int)tabbox_width;
    if (available_width <= 0) available_width = 400;

    size_t tab_count = g_browser_tabs.empty() ? 1u : g_browser_tabs.size();
    int per_tab = available_width / (int)tab_count;
    int max_label_width = per_tab - 36;
    if (max_label_width < 80) max_label_width = 80;
    if (max_label_width > available_width - 40) max_label_width = available_width - 40;

    XmFontList fontlist = NULL;
    if (tabbox) {
        XtVaGetValues(tabbox, XmNfontList, &fontlist, NULL);
    }
    if (!fontlist && g_tab_stack) {
        XtVaGetValues(g_tab_stack, XmNfontList, &fontlist, NULL);
    }

    auto string_width_px = [&](const std::string &value) -> int {
        if (!fontlist) return (int)value.size() * 8;
        XmString s = XmStringCreateLocalized((String)value.c_str());
        Dimension w = 0, h = 0;
        XmStringExtent(fontlist, s, &w, &h);
        XmStringFree(s);
        return (int)w;
    };

    auto utf8_codepoints = [](const std::string &s) -> size_t {
        size_t count = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            unsigned char c = (unsigned char)s[i];
            if ((c & 0xC0) != 0x80) count++;
        }
        return count;
    };

    auto utf8_prefix_bytes = [](const std::string &s, size_t codepoints) -> size_t {
        if (codepoints == 0) return 0;
        size_t seen = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            unsigned char c = (unsigned char)s[i];
            if ((c & 0xC0) != 0x80) {
                seen++;
                if (seen > codepoints) return i;
            }
        }
        return s.size();
    };

    std::string display(label ? label : "");
    if (max_label_width > 0 && string_width_px(display) > max_label_width) {
        const std::string ellipsis = "...";
        if (string_width_px(ellipsis) >= max_label_width) {
            display = ellipsis;
        } else {
            size_t total = utf8_codepoints(display);
            size_t lo = 0;
            size_t hi = total;
            size_t best = 0;
            while (lo <= hi) {
                size_t mid = (lo + hi) / 2;
                size_t bytes = utf8_prefix_bytes(display, mid);
                std::string candidate = display.substr(0, bytes) + ellipsis;
                if (string_width_px(candidate) <= max_label_width) {
                    best = mid;
                    lo = mid + 1;
                } else {
                    if (mid == 0) break;
                    hi = mid - 1;
                }
            }
            size_t bytes = utf8_prefix_bytes(display, best);
            display = display.substr(0, bytes) + ellipsis;
        }
    }

    XmString xm_label = make_string(display.c_str());
    XtVaSetValues(tab->page, XmNtabLabelString, xm_label, NULL);
    XmStringFree(xm_label);
}

static void update_all_tab_labels(const char *reason)
{
    (void)reason;
    for (const auto &entry : g_browser_tabs) {
        BrowserTab *tab = entry.get();
        if (!tab) continue;
        const char *label = tab->title_full.empty()
                                ? (tab->base_title.empty() ? "New Tab" : tab->base_title.c_str())
                                : tab->title_full.c_str();
        update_tab_label(tab, label);
    }
}

static void spawn_new_browser_window(const std::string &url)
{
    if (g_subprocess_path[0] == '\0') {
        fprintf(stderr, "[ck-browser] spawn_new_browser_window: missing executable path\n");
        return;
    }
    fprintf(stderr, "[ck-browser] spawn_new_browser_window exe=%s url=%s\n", g_subprocess_path, url.c_str());
    pid_t pid = fork();
    if (pid < 0) {
        perror("[ck-browser] fork");
        return;
    }
    if (pid == 0) {
        std::string arg = std::string("--ck-open-url=") + url;
        std::string cache_arg = std::string("--ck-cache-suffix=") + std::to_string((long)getpid());
        execl(g_subprocess_path, g_subprocess_path, arg.c_str(), cache_arg.c_str(), (char *)NULL);
        perror("[ck-browser] execl");
        _exit(127);
    }
    fprintf(stderr, "[ck-browser] spawn_new_browser_window pid=%ld\n", (long)pid);
}

static void open_url_in_new_tab(const std::string &url, bool select)
{
    if (!g_tab_stack) return;
    const char *base = "New Tab";
    int count = count_tabs_with_base_title(base);
    char name[32];
    snprintf(name, sizeof(name), "tabNew%d", count + 1);
    char tab_title[64];
    snprintf(tab_title, sizeof(tab_title), "%s (%d)", base, count + 1);

    BrowserTab *tab = create_tab_page(g_tab_stack, name, tab_title, base, url.c_str());
    schedule_tab_browser_creation(tab);
    if (select) {
        XmTabStackSelectTab(tab->page, True);
        set_current_tab(tab);
    }
}

static void log_widget_size(const char *context, Widget widget)
{
    if (!context || !widget) return;
    Dimension width = 0;
    Dimension height = 0;
    XtVaGetValues(widget, XmNwidth, &width, XmNheight, &height, NULL);
    fprintf(stderr, "[ck-browser] %s size %dx%d\n", context, (int)width, (int)height);
}

static void update_navigation_buttons(BrowserTab *tab)
{
    if (g_back_button) {
        XtSetSensitive(g_back_button, tab && tab->can_go_back);
    }
    if (g_forward_button) {
        XtSetSensitive(g_forward_button, tab && tab->can_go_forward);
    }
    if (g_nav_back) {
        XtSetSensitive(g_nav_back, tab && tab->can_go_back);
    }
    if (g_nav_forward) {
        XtSetSensitive(g_nav_forward, tab && tab->can_go_forward);
    }
}

static int zoom_percent_from_level(double level)
{
    const double kStepFactor = 1.0954451150103321;  // sqrt(1.2)
    int steps = (int)(level * 2.0 + (level >= 0 ? 0.5 : -0.5));  // round to 0.5 increments
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

static void update_zoom_controls(BrowserTab *tab)
{
    if (!g_zoom_label) return;
    double level = tab ? tab->zoom_level : 0.0;
    int percent = zoom_percent_from_level(level);
    char buf[64];
    snprintf(buf, sizeof(buf), "Zoom: %d%%", percent);
    XmString xm_text = make_string(buf);
    XtVaSetValues(g_zoom_label, XmNlabelString, xm_text, NULL);
    XmStringFree(xm_text);
}

static void poll_zoom_levels()
{
    static int tick = 0;
    tick++;
    if ((tick % 20) != 0) {  // ~200ms
        return;
    }
    for (const auto &entry : g_browser_tabs) {
        BrowserTab *tab = entry.get();
        if (!tab || !tab->browser) continue;
        CefRefPtr<CefBrowserHost> host = tab->browser->GetHost();
        if (!host) continue;
        double current = host->GetZoomLevel();
        double diff = current - tab->zoom_level;
        if (diff < 0) diff = -diff;
        if (diff > 1e-6) {
            tab->zoom_level = current;
            if (tab == g_current_tab) {
                update_zoom_controls(tab);
            }
        }
    }
}

static void set_tab_zoom_level(BrowserTab *tab, double level)
{
    if (!tab || !tab->browser) return;
    CefRefPtr<CefBrowserHost> host = tab->browser->GetHost();
    if (!host) return;
    host->SetZoomLevel(level);
    tab->zoom_level = level;
    if (tab == g_current_tab) {
        update_zoom_controls(tab);
    }
}

static void on_zoom_reset(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    BrowserTab *tab = get_selected_tab();
    if (!tab) return;
    set_current_tab(tab);
    browser_set_focus(tab, false);
    set_tab_zoom_level(tab, 0.0);
}

static void on_zoom_in(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    BrowserTab *tab = get_selected_tab();
    if (!tab) return;
    set_current_tab(tab);
    browser_set_focus(tab, false);
    set_tab_zoom_level(tab, tab->zoom_level + 0.5);
}

static void on_zoom_out(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    BrowserTab *tab = get_selected_tab();
    if (!tab) return;
    set_current_tab(tab);
    browser_set_focus(tab, false);
    set_tab_zoom_level(tab, tab->zoom_level - 0.5);
}

static bool tab_is_alive(const BrowserTab *tab)
{
    if (!tab) return false;
    for (const auto &entry : g_browser_tabs) {
        if (entry.get() == tab) return true;
    }
    return false;
}

static void update_wm_icon_pixmap(Display *display, Window window, Pixmap pixmap, Pixmap mask)
{
    if (!display || !window || pixmap == None) return;
    XWMHints *hints = XGetWMHints(display, window);
    XWMHints local;
    if (hints) {
        local = *hints;
        XFree(hints);
    } else {
        memset(&local, 0, sizeof(local));
    }
    local.flags |= IconPixmapHint;
    local.icon_pixmap = pixmap;
    if (mask != None) {
        local.flags |= IconMaskHint;
        local.icon_mask = mask;
    }
    XSetWMHints(display, window, &local);
}

static void mask_shift_bits(unsigned long mask, int *shift_out, int *bits_out)
{
    int shift = 0;
    int bits = 0;
    if (mask) {
        while ((mask & 1UL) == 0UL) {
            mask >>= 1;
            shift++;
        }
        while (mask & 1UL) {
            bits++;
            mask >>= 1;
        }
    }
    if (shift_out) *shift_out = shift;
    if (bits_out) *bits_out = bits;
}

static unsigned long pack_rgb_pixel(Visual *visual, unsigned char r, unsigned char g, unsigned char b)
{
    if (!visual) return 0;
    int rshift = 0, rbits = 0;
    int gshift = 0, gbits = 0;
    int bshift = 0, bbits = 0;
    mask_shift_bits(visual->red_mask, &rshift, &rbits);
    mask_shift_bits(visual->green_mask, &gshift, &gbits);
    mask_shift_bits(visual->blue_mask, &bshift, &bbits);
    unsigned long rv = rbits ? (unsigned long)(r * ((1U << rbits) - 1U) / 255U) : 0UL;
    unsigned long gv = gbits ? (unsigned long)(g * ((1U << gbits) - 1U) / 255U) : 0UL;
    unsigned long bv = bbits ? (unsigned long)(b * ((1U << bbits) - 1U) / 255U) : 0UL;
    return (rv << rshift) | (gv << gshift) | (bv << bshift);
}

static bool query_pixel_rgb(Display *display,
                            int screen,
                            Pixel pixel,
                            unsigned char *r_out,
                            unsigned char *g_out,
                            unsigned char *b_out)
{
    if (!display) return false;
    Colormap cmap = DefaultColormap(display, screen);
    XColor color;
    color.pixel = pixel;
    if (!XQueryColor(display, cmap, &color)) return false;
    if (r_out) *r_out = (unsigned char)(color.red >> 8);
    if (g_out) *g_out = (unsigned char)(color.green >> 8);
    if (b_out) *b_out = (unsigned char)(color.blue >> 8);
    return true;
}

static Pixmap create_scaled_toolbar_pixmap_from_bgra(Display *display,
                                                     int screen,
                                                     const unsigned char *bgra,
                                                     int src_w,
                                                     int src_h,
                                                     int target_size,
                                                     Pixel bg_pixel)
{
    if (!display || !bgra || src_w <= 0 || src_h <= 0 || target_size <= 0) return None;
    Visual *visual = DefaultVisual(display, screen);
    int depth = DefaultDepth(display, screen);
    Window root = RootWindow(display, screen);
    Pixmap pixmap = XCreatePixmap(display, root, (unsigned int)target_size, (unsigned int)target_size, (unsigned int)depth);
    if (pixmap == None) return None;

    XImage *img = XCreateImage(display, visual, (unsigned int)depth, ZPixmap, 0, NULL,
                               (unsigned int)target_size, (unsigned int)target_size, 32, 0);
    if (!img) {
        XFreePixmap(display, pixmap);
        return None;
    }
    size_t bytes = (size_t)img->bytes_per_line * (size_t)img->height;
    img->data = (char *)calloc(1, bytes);
    if (!img->data) {
        XDestroyImage(img);
        XFreePixmap(display, pixmap);
        return None;
    }

    unsigned long bg = (unsigned long)bg_pixel;
    unsigned char bg_r = 0, bg_g = 0, bg_b = 0;
    (void)query_pixel_rgb(display, screen, bg_pixel, &bg_r, &bg_g, &bg_b);
    for (int y = 0; y < target_size; ++y) {
        for (int x = 0; x < target_size; ++x) {
            XPutPixel(img, x, y, bg);
        }
    }

    for (int y = 0; y < target_size; ++y) {
        int src_y = (int)((long long)y * src_h / target_size);
        if (src_y < 0) src_y = 0;
        if (src_y >= src_h) src_y = src_h - 1;
        for (int x = 0; x < target_size; ++x) {
            int src_x = (int)((long long)x * src_w / target_size);
            if (src_x < 0) src_x = 0;
            if (src_x >= src_w) src_x = src_w - 1;
            const unsigned char *p = bgra + ((src_y * src_w + src_x) * 4);
            unsigned char b_premul = p[0];
            unsigned char g_premul = p[1];
            unsigned char r_premul = p[2];
            unsigned char a = p[3];
            if (a == 0) continue;
            unsigned char out_r = (unsigned char)std::min(255, (int)r_premul + ((255 - (int)a) * (int)bg_r) / 255);
            unsigned char out_g = (unsigned char)std::min(255, (int)g_premul + ((255 - (int)a) * (int)bg_g) / 255);
            unsigned char out_b = (unsigned char)std::min(255, (int)b_premul + ((255 - (int)a) * (int)bg_b) / 255);
            XPutPixel(img, x, y, pack_rgb_pixel(visual, out_r, out_g, out_b));
        }
    }

    GC gc = XCreateGC(display, pixmap, 0, NULL);
    if (gc) {
        XPutImage(display, pixmap, gc, img, 0, 0, 0, 0, (unsigned int)target_size, (unsigned int)target_size);
        XFreeGC(display, gc);
        XFlush(display);
    }
    XDestroyImage(img);
    return pixmap;
}

static bool create_scaled_window_icon_from_bgra(Display *display,
                                                int screen,
                                                const unsigned char *bgra,
                                                int src_w,
                                                int src_h,
                                                int target_size,
                                                Pixmap *out_pixmap,
                                                Pixmap *out_mask)
{
    if (!out_pixmap || !out_mask) return false;
    *out_pixmap = None;
    *out_mask = None;
    if (!display || !bgra || src_w <= 0 || src_h <= 0 || target_size <= 0) return false;

    Visual *visual = DefaultVisual(display, screen);
    int depth = DefaultDepth(display, screen);
    Window root = RootWindow(display, screen);
    Pixmap pixmap = XCreatePixmap(display, root, (unsigned int)target_size, (unsigned int)target_size, (unsigned int)depth);
    if (pixmap == None) return false;
    Pixmap mask = XCreatePixmap(display, root, (unsigned int)target_size, (unsigned int)target_size, 1);
    if (mask == None) {
        XFreePixmap(display, pixmap);
        return false;
    }

    XImage *img = XCreateImage(display, visual, (unsigned int)depth, ZPixmap, 0, NULL,
                               (unsigned int)target_size, (unsigned int)target_size, 32, 0);
    if (!img) {
        XFreePixmap(display, pixmap);
        XFreePixmap(display, mask);
        return false;
    }
    size_t bytes = (size_t)img->bytes_per_line * (size_t)img->height;
    img->data = (char *)calloc(1, bytes);
    if (!img->data) {
        XDestroyImage(img);
        XFreePixmap(display, pixmap);
        XFreePixmap(display, mask);
        return false;
    }

    GC mask_gc = XCreateGC(display, mask, 0, NULL);
    if (!mask_gc) {
        XDestroyImage(img);
        XFreePixmap(display, pixmap);
        XFreePixmap(display, mask);
        return false;
    }
    XSetForeground(display, mask_gc, 0);
    XFillRectangle(display, mask, mask_gc, 0, 0, (unsigned int)target_size, (unsigned int)target_size);
    XSetForeground(display, mask_gc, 1);

    unsigned long bg = WhitePixel(display, screen);
    for (int y = 0; y < target_size; ++y) {
        for (int x = 0; x < target_size; ++x) {
            XPutPixel(img, x, y, bg);
        }
    }

    for (int y = 0; y < target_size; ++y) {
        int src_y = (int)((long long)y * src_h / target_size);
        if (src_y < 0) src_y = 0;
        if (src_y >= src_h) src_y = src_h - 1;
        for (int x = 0; x < target_size; ++x) {
            int src_x = (int)((long long)x * src_w / target_size);
            if (src_x < 0) src_x = 0;
            if (src_x >= src_w) src_x = src_w - 1;
            const unsigned char *p = bgra + ((src_y * src_w + src_x) * 4);
            unsigned char b_premul = p[0];
            unsigned char g_premul = p[1];
            unsigned char r_premul = p[2];
            unsigned char a = p[3];
            if (a == 0) continue;
            unsigned char r = (unsigned char)std::min(255, (int)r_premul * 255 / (int)a);
            unsigned char g = (unsigned char)std::min(255, (int)g_premul * 255 / (int)a);
            unsigned char b = (unsigned char)std::min(255, (int)b_premul * 255 / (int)a);
            XPutPixel(img, x, y, pack_rgb_pixel(visual, r, g, b));
            XDrawPoint(display, mask, mask_gc, x, y);
        }
    }

    GC gc = XCreateGC(display, pixmap, 0, NULL);
    if (gc) {
        XPutImage(display, pixmap, gc, img, 0, 0, 0, 0, (unsigned int)target_size, (unsigned int)target_size);
        XFreeGC(display, gc);
    }
    XFreeGC(display, mask_gc);
    XDestroyImage(img);
    XFlush(display);

    *out_pixmap = pixmap;
    *out_mask = mask;
    return true;
}

static int desired_favicon_size()
{
    Dimension height = 0;
    if (g_url_field) {
        XtVaGetValues(g_url_field, XmNheight, &height, NULL);
    }
    int size = (int)height;
    if (size <= 0) size = 20;
    if (size < 16) size = 16;
    if (size > 32) size = 32;
    return size;
}

static int desired_window_icon_size()
{
    return 96;
}

static void remove_favicon_cache_entry(const std::string &key)
{
    auto it = g_favicon_cache.find(key);
    if (it == g_favicon_cache.end()) return;
    if (it->second.order_it != g_favicon_cache_order.end()) {
        g_favicon_cache_order.erase(it->second.order_it);
    }
    g_favicon_cache.erase(it);
}

static void cleanup_favicon_cache()
{
    time_t now = time(NULL);
    std::vector<std::string> expired;
    for (const auto &pair : g_favicon_cache) {
        if (now - pair.second.saved_time > kFaviconCacheTTL_SECONDS) {
            expired.push_back(pair.first);
        }
    }
    for (const auto &key : expired) {
        remove_favicon_cache_entry(key);
    }
}

static bool apply_favicon_to_tab_from_raw(BrowserTab *tab,
                                          const unsigned char *raw,
                                          int raw_w,
                                          int raw_h,
                                          int toolbar_size,
                                          int window_size,
                                          Pixel toolbar_bg_pixel)
{
    if (!tab || !g_toplevel || !raw || raw_w <= 0 || raw_h <= 0) return false;
    Display *display = XtDisplay(g_toplevel);
    if (!display) return false;
    int screen = DefaultScreen(display);
    Pixmap toolbar_pix = create_scaled_toolbar_pixmap_from_bgra(display,
                                                                screen,
                                                                raw,
                                                                raw_w,
                                                                raw_h,
                                                                toolbar_size,
                                                                toolbar_bg_pixel);
    Pixmap window_pix = None;
    Pixmap window_mask = None;
    (void)create_scaled_window_icon_from_bgra(display,
                                              screen,
                                              raw,
                                              raw_w,
                                              raw_h,
                                              window_size,
                                              &window_pix,
                                              &window_mask);
    if (toolbar_pix != None) {
        if (tab->favicon_toolbar_pixmap != None) {
            XFreePixmap(display, tab->favicon_toolbar_pixmap);
        }
        tab->favicon_toolbar_pixmap = toolbar_pix;
        tab->favicon_toolbar_size = toolbar_size;
    }
    if (window_pix != None) {
        if (tab->favicon_window_pixmap != None) {
            XFreePixmap(display, tab->favicon_window_pixmap);
        }
        if (tab->favicon_window_mask != None) {
            XFreePixmap(display, tab->favicon_window_mask);
        }
        tab->favicon_window_pixmap = window_pix;
        tab->favicon_window_mask = window_mask;
        tab->favicon_window_size = window_size;
    }
    return toolbar_pix != None || window_pix != None;
}

static void store_favicon_in_cache(const char *url,
                                   const unsigned char *raw,
                                   size_t raw_size,
                                   int width,
                                   int height,
                                   const unsigned char *png,
                                   size_t png_size)
{
    if (!url || !raw || raw_size == 0 || width <= 0 || height <= 0) return;
    std::string key(url);
    std::vector<unsigned char> copy(raw, raw + raw_size);
    std::vector<unsigned char> png_copy;
    if (png && png_size > 0) {
        png_copy.assign(png, png + png_size);
    }
    time_t now = time(NULL);
    auto it = g_favicon_cache.find(key);
    if (it != g_favicon_cache.end()) {
        it->second.data = std::move(copy);
        it->second.width = width;
        it->second.height = height;
        it->second.saved_time = now;
        it->second.png_data = std::move(png_copy);
        if (it->second.order_it != g_favicon_cache_order.end()) {
            g_favicon_cache_order.erase(it->second.order_it);
        }
        g_favicon_cache_order.push_back(key);
        auto order_it = g_favicon_cache_order.end();
        --order_it;
        it->second.order_it = order_it;
    } else {
        FaviconCacheEntry entry;
        entry.data = std::move(copy);
        entry.width = width;
        entry.height = height;
        entry.saved_time = now;
        entry.png_data = std::move(png_copy);
        g_favicon_cache_order.push_back(key);
        auto order_it = g_favicon_cache_order.end();
        --order_it;
        entry.order_it = order_it;
        g_favicon_cache.emplace(key, std::move(entry));
    }
    while (g_favicon_cache.size() > kFaviconCacheLimit && !g_favicon_cache_order.empty()) {
        remove_favicon_cache_entry(g_favicon_cache_order.front());
    }
}

class FaviconDownloadCallback : public CefDownloadImageCallback {
 public:
  FaviconDownloadCallback(BrowserTab *tab,
                          int toolbar_size,
                          int window_size,
                          Pixel toolbar_bg_pixel)
      : tab_(tab),
        toolbar_size_(toolbar_size),
        window_size_(window_size),
        toolbar_bg_pixel_(toolbar_bg_pixel) {}

  void OnDownloadImageFinished(const CefString &image_url,
                               int http_status_code,
                               CefRefPtr<CefImage> image) override {
    CEF_REQUIRE_UI_THREAD();
    (void)image_url;
    if (!tab_ || !tab_is_alive(tab_)) return;
    if (!image || image->IsEmpty() || http_status_code <= 0) {
      fprintf(stderr, "[ck-browser] favicon download failed tab=%p http=%d\n", (void *)tab_, http_status_code);
      return;
    }
    int pixel_w = 0;
    int pixel_h = 0;
    CefRefPtr<CefBinaryValue> data = image->GetAsBitmap(1.0f,
                                                        CEF_COLOR_TYPE_BGRA_8888,
                                                        CEF_ALPHA_TYPE_PREMULTIPLIED,
                                                        pixel_w,
                                                        pixel_h);
    if (!data || pixel_w <= 0 || pixel_h <= 0) {
      fprintf(stderr, "[ck-browser] favicon bitmap missing tab=%p\n", (void *)tab_);
      return;
    }
    const unsigned char *raw = (const unsigned char *)data->GetRawData();
    size_t raw_size = data->GetSize();
    if (!raw || raw_size < (size_t)pixel_w * (size_t)pixel_h * 4u) {
      fprintf(stderr, "[ck-browser] favicon bitmap invalid tab=%p size=%zu\n", (void *)tab_, raw_size);
      return;
    }
    int png_w = 0;
    int png_h = 0;
    CefRefPtr<CefBinaryValue> png_value = image->GetAsPNG(1.0f, true, png_w, png_h);
    std::vector<unsigned char> png_bytes;
    if (png_value && png_value->GetSize() > 0) {
      const unsigned char *png_data = (const unsigned char *)png_value->GetRawData();
      size_t png_size = png_value->GetSize();
      if (png_data && png_size > 0) {
        png_bytes.assign(png_data, png_data + png_size);
      }
    }
    store_favicon_in_cache(tab_->favicon_url.c_str(),
                           raw,
                           raw_size,
                           pixel_w,
                           pixel_h,
                           png_bytes.empty() ? NULL : png_bytes.data(),
                           png_bytes.size());
    if (apply_favicon_to_tab_from_raw(tab_, raw, pixel_w, pixel_h, toolbar_size_, window_size_, toolbar_bg_pixel_)) {
      if (tab_ == g_current_tab) {
        update_favicon_controls(tab_);
      }
    }
  }

 private:
  BrowserTab *tab_ = NULL;
  int toolbar_size_ = 0;
  int window_size_ = 0;
  Pixel toolbar_bg_pixel_ = 0;
  IMPLEMENT_REFCOUNTING(FaviconDownloadCallback);
};

static void request_favicon_download(BrowserTab *tab, const char *reason)
{
    if (!tab || !tab->browser) return;
    if (tab->favicon_url.empty()) return;
    CefRefPtr<CefBrowserHost> host = tab->browser->GetHost();
    if (!host) return;

    int toolbar_size = desired_favicon_size();
    int window_size = desired_window_icon_size();
    int max_size = toolbar_size > window_size ? toolbar_size : window_size;

    Pixel bg_pixel = 0;
    if (g_favicon_label) {
        XtVaGetValues(g_favicon_label, XmNbackground, &bg_pixel, NULL);
    } else if (g_toplevel) {
        XtVaGetValues(g_toplevel, XmNbackground, &bg_pixel, NULL);
    }

    cleanup_favicon_cache();
    time_t now = time(NULL);
    auto cache_it = g_favicon_cache.find(tab->favicon_url);
    if (cache_it != g_favicon_cache.end()) {
        time_t age = now - cache_it->second.saved_time;
        size_t expected_size = (size_t)cache_it->second.width * (size_t)cache_it->second.height * 4u;
        if (age <= kFaviconCacheTTL_SECONDS &&
            cache_it->second.data.size() >= expected_size &&
            apply_favicon_to_tab_from_raw(tab,
                                          cache_it->second.data.data(),
                                          cache_it->second.width,
                                          cache_it->second.height,
                                          toolbar_size,
                                          window_size,
                                          bg_pixel)) {
            if (tab == g_current_tab) {
                update_favicon_controls(tab);
            }
            return;
        }
        remove_favicon_cache_entry(cache_it->first);
    }

    fprintf(stderr,
            "[ck-browser] request_favicon_download reason=%s tab=%p max=%d toolbar=%d window=%d url=%s\n",
            reason ? reason : "(null)",
            (void *)tab,
            max_size,
            toolbar_size,
            window_size,
            tab->favicon_url.c_str());
    host->DownloadImage(tab->favicon_url,
                        true,
                        max_size,
                        false,
                        new FaviconDownloadCallback(tab, toolbar_size, window_size, bg_pixel));
}

static void update_favicon_controls(BrowserTab *tab)
{
    const char *tab_label = tab ? (tab->base_title.empty() ? "Tab" : tab->base_title.c_str()) : "(none)";
    const char *host = tab ? tab->current_host.c_str() : "(none)";
    const char *url = tab ? (tab->favicon_url.empty() ? "(none)" : tab->favicon_url.c_str()) : "(none)";
    fprintf(stderr,
            "[ck-browser] update_favicon_controls tab=%s (%p) host=%s url=%s pixmap=%s window=%s\n",
            tab_label,
            (void *)tab,
            host,
            url,
            tab && tab->favicon_toolbar_pixmap != None ? "toolbar" : "no-toolbar",
            tab && tab->favicon_window_pixmap != None ? "window" : "no-window");
    if (g_favicon_label) {
        Pixmap pix = (tab && tab->favicon_toolbar_pixmap != None) ? tab->favicon_toolbar_pixmap : XmUNSPECIFIED_PIXMAP;
        int size = desired_favicon_size();
        XtVaSetValues(g_favicon_label,
                      XmNlabelType, XmPIXMAP,
                      XmNlabelPixmap, pix,
                      XmNwidth, size,
                      XmNheight, size,
                      NULL);
    }
    if (g_toplevel && tab && tab->favicon_window_pixmap != None) {
        XtVaSetValues(g_toplevel,
                      XmNiconPixmap, tab->favicon_window_pixmap,
                      XmNiconMask, tab->favicon_window_mask,
                      NULL);
        if (XtIsRealized(g_toplevel)) {
            update_wm_icon_pixmap(XtDisplay(g_toplevel),
                                  XtWindow(g_toplevel),
                                  tab->favicon_window_pixmap,
                                  tab->favicon_window_mask);
        }
    }
    if (tab && tab->favicon_toolbar_pixmap == None && tab->favicon_window_pixmap == None && !tab->favicon_url.empty()) {
        request_favicon_download(tab, "update_favicon_controls");
    }
}

static void
set_reload_button_label(const char *text)
{
    if (!g_reload_button) return;
    XmString xm_label = make_string(text ? text : "");
    XtVaSetValues(g_reload_button, XmNlabelString, xm_label, NULL);
    XmStringFree(xm_label);
}

static void
update_reload_button_for_tab(BrowserTab *tab)
{
    bool loading = tab && tab->loading;
    set_reload_button_label(loading ? "Stop" : "Reload");
}

static void
clear_tab_favicon(BrowserTab *tab)
{
    fprintf(stderr,
            "[ck-browser] clear_tab_favicon tab=%p host=%s url=%s\n",
            (void *)tab,
            tab ? (tab->current_host.empty() ? "(none)" : tab->current_host.c_str()) : "(none)",
            tab ? (tab->favicon_url.empty() ? "(none)" : tab->favicon_url.c_str()) : "(none)");
    if (!tab) return;
    Display *display = g_toplevel ? XtDisplay(g_toplevel) : NULL;
    if (tab->favicon_toolbar_pixmap != None && display) {
        XFreePixmap(display, tab->favicon_toolbar_pixmap);
    }
    if (tab->favicon_window_pixmap != None && display) {
        XFreePixmap(display, tab->favicon_window_pixmap);
    }
    if (tab->favicon_window_mask != None && display) {
        XFreePixmap(display, tab->favicon_window_mask);
    }
    tab->favicon_toolbar_pixmap = None;
    tab->favicon_toolbar_size = 0;
    tab->favicon_window_pixmap = None;
    tab->favicon_window_mask = None;
    tab->favicon_window_size = 0;
    tab->favicon_url.clear();
    if (tab == g_current_tab) {
        update_favicon_controls(tab);
    }
}

static std::string
extract_host_from_url(const std::string &url)
{
    size_t scheme = url.find("://");
    if (scheme == std::string::npos) return std::string();
    size_t start = scheme + 3;
    if (start >= url.size()) return std::string();
    size_t end = start;
    if (url[start] == '[') {
        end = start + 1;
        while (end < url.size() && url[end] != ']') {
            end++;
        }
        if (end < url.size()) {
            end++;
        }
    } else {
        while (end < url.size()) {
            char ch = url[end];
            if (ch == '/' || ch == ':' || ch == '?' || ch == '#') break;
            end++;
        }
    }
    if (start >= end) return std::string();
    std::string host = url.substr(start, end - start);
    size_t at_pos = host.rfind('@');
    if (at_pos != std::string::npos) {
        host.erase(0, at_pos + 1);
    }
    while (!host.empty() && host.back() == '.') {
        host.pop_back();
    }
    for (char &c : host) {
        c = (char)std::tolower((unsigned char)c);
    }
    return host;
}

static void load_url_for_tab(BrowserTab *tab, const std::string &url)
{
    if (!tab || url.empty()) return;
    const std::string normalized = normalize_url(url.c_str());
    if (normalized.empty()) return;
    std::string host = extract_host_from_url(normalized);
    if (host != tab->current_host) {
        tab->current_host = host;
        clear_tab_favicon(tab);
    }
    tab->pending_url = normalized;
    if (!tab->browser) {
        schedule_tab_browser_creation(tab);
    }
    if (tab->browser) {
        CefRefPtr<CefFrame> frame = tab->browser->GetMainFrame();
        if (frame) {
            frame->LoadURL(normalized);
        }
    }
    if (tab == g_current_tab && g_url_field) {
        XmTextFieldSetString(g_url_field, const_cast<char *>(tab->pending_url.c_str()));
    }
}

static void close_tab_browser(BrowserTab *tab)
{
    LOG_ENTER("tab=%p title='%s' browser=%p",
              (void *)tab,
              tab ? (tab->base_title.empty() ? "Tab" : tab->base_title.c_str()) : "(null)",
              tab ? (void *)tab->browser.get() : (void *)NULL);
    if (!tab || !tab->browser) {
        LOG_ENTER("close_tab_browser aborted (tab=%p browser=%p)",
                  (void *)tab,
                  tab ? (void *)tab->browser.get() : (void *)NULL);
        return;
    }
    CefRefPtr<CefBrowserHost> host = tab->browser->GetHost();
    if (host) {
        if (host->HasDevTools()) {
            host->CloseDevTools();
        }
        host->CloseBrowser(true);
    }
}

static int count_open_browsers()
{
    int pending = 0;
    for (const auto &tab : g_browser_tabs) {
        if (tab->browser) pending++;
        if (tab->devtools_browser) pending++;
    }
    return pending;
}

static void on_cef_browser_closed(const char *tag)
{
    if (!g_shutdown_requested) return;
    if (g_shutdown_pending_browsers > 0) {
        g_shutdown_pending_browsers--;
    }
    fprintf(stderr,
            "[ck-browser] %s closed, pending=%d\n",
            tag ? tag : "browser",
            g_shutdown_pending_browsers);
    if (g_shutdown_pending_browsers <= 0 && g_app) {
        fprintf(stderr, "[ck-browser] shutdown complete, exiting main loop\n");
        XtAppSetExitFlag(g_app);
    }
}

static void begin_shutdown_sequence(const char *reason)
{
    if (g_shutdown_requested) return;
    g_shutdown_requested = true;
    g_shutdown_pending_browsers = count_open_browsers();
    fprintf(stderr,
            "[ck-browser] begin shutdown reason=%s pending=%d\n",
            reason ? reason : "(null)",
            g_shutdown_pending_browsers);
    if (g_shutdown_pending_browsers == 0) {
        if (g_app) {
            XtAppSetExitFlag(g_app);
        }
        return;
    }
    for (const auto &tab : g_browser_tabs) {
        close_tab_browser(tab.get());
    }
}

static void resize_cef_browser_to_area(BrowserTab *tab, const char *reason)
{
    if (!tab || !tab->browser || !tab->browser_area) return;
    CefRefPtr<CefBrowserHost> host = tab->browser->GetHost();
    if (!host) return;

    Dimension width = 0;
    Dimension height = 0;
    XtVaGetValues(tab->browser_area, XmNwidth, &width, XmNheight, &height, NULL);
    if (width <= 1 || height <= 1) return;

    CefWindowHandle child_handle = host->GetWindowHandle();
    Display *dpy = XtDisplay(tab->browser_area);
    if (child_handle && dpy) {
        XMoveResizeWindow(dpy,
                          (Window)child_handle,
                          0,
                          0,
                          (unsigned int)width,
                          (unsigned int)height);
        XFlush(dpy);
    }

    host->NotifyMoveOrResizeStarted();
    host->WasResized();

    fprintf(stderr,
            "[ck-browser] resize_cef_browser_to_area(%s) tab=%s (%p) child=%lu area=%dx%d\n",
            reason ? reason : "unknown",
            tab->base_title.empty() ? "Tab" : tab->base_title.c_str(),
            (void *)tab,
            (unsigned long)child_handle,
            (int)width,
            (int)height);
}

static void on_main_window_resize(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    (void)call_data;
    log_widget_size("main window (resize)", w);
    if (g_tab_stack) {
        log_widget_size("tab stack (window resize)", g_tab_stack);
    }
    BrowserTab *tab = get_selected_tab();
    if (tab && tab->browser_area) {
        log_widget_size("current browser area (window resize)", tab->browser_area);
        resize_cef_browser_to_area(tab, "window resize");
    }
    update_all_tab_labels("main window resize");
}

static void on_tab_stack_resize(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    (void)call_data;
    log_widget_size("tab stack (resize)", w);
    BrowserTab *tab = get_selected_tab();
    if (tab && tab->browser_area) {
        log_widget_size("current browser area (tab stack resize)", tab->browser_area);
        resize_cef_browser_to_area(tab, "tab stack resize");
    }
    update_all_tab_labels("tab stack resize");
}

static void on_browser_resize(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    BrowserTab *tab = (BrowserTab *)client_data;
    if (!tab || !tab->browser) return;
    Dimension width = 0, height = 0;
    XtVaGetValues(tab->browser_area, XmNwidth, &width, XmNheight, &height, NULL);
    fprintf(stderr, "[ck-browser] resize callback for tab %s (%p) browser area: %dx%d\n",
            tab->base_title.empty() ? "Tab" : tab->base_title.c_str(), (void *)tab, (int)width, (int)height);
    log_widget_size("browser area (browser resize)", tab->browser_area);
    if (tab->page) {
        log_widget_size("tab page (browser resize)", tab->page);
    }
    if (g_tab_stack) {
        log_widget_size("tab stack (browser resize)", g_tab_stack);
    }
    resize_cef_browser_to_area(tab, "browser area resize");
}

static void on_url_activate(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    (void)call_data;
    char *value = XmTextFieldGetString(w);
    if (!value) return;
    std::string typed = value ? value : "";
    std::string normalized = normalize_url(value);
    XtFree(value);
    if (normalized.empty()) {
        show_invalid_url_dialog(typed.c_str());
        set_status_label_text("Invalid URL");
        return;
    }
    if (normalized.find("://") != std::string::npos) {
        std::string host = extract_host_from_url(normalized);
        if (host.empty() || !is_url_parseable(normalized)) {
            show_invalid_url_dialog(typed.c_str());
            set_status_label_text("Invalid URL");
            return;
        }
    }
    BrowserTab *active = get_selected_tab();
    if (!active) return;
    set_current_tab(active);
    load_url_for_tab(active, normalized);
}

static void on_back(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    BrowserTab *tab = get_selected_tab();
    if (!tab || !tab->browser) return;
    browser_set_focus(tab, false);
    if (tab->browser->CanGoBack()) {
        tab->browser->GoBack();
    }
}

static void on_forward(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    BrowserTab *tab = get_selected_tab();
    if (!tab || !tab->browser) return;
    browser_set_focus(tab, false);
    if (tab->browser->CanGoForward()) {
        tab->browser->GoForward();
    }
}

static void on_reload(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    BrowserTab *tab = get_selected_tab();
    if (!tab || !tab->browser) return;
    browser_set_focus(tab, false);
    Dimension width = 0, height = 0;
    if (tab->browser_area) {
        XtVaGetValues(tab->browser_area, XmNwidth, &width, XmNheight, &height, NULL);
    }
    fprintf(stderr, "[ck-browser] Reload requested for tab %s (%p), browser area %dx%d\n",
            tab->base_title.empty() ? "Tab" : tab->base_title.c_str(), (void *)tab,
            (int)width, (int)height);
    if (tab->browser_area) {
        log_widget_size("browser area (reload)", tab->browser_area);
    }
    if (tab->page) {
        log_widget_size("tab page (reload)", tab->page);
    }
    if (g_tab_stack) {
        log_widget_size("tab stack (reload)", g_tab_stack);
    }
    if (g_toplevel) {
        log_widget_size("main window (reload)", g_toplevel);
    }
    resize_cef_browser_to_area(tab, "reload");
    if (tab->loading) {
        tab->browser->StopLoad();
        tab->loading = false;
        if (tab == g_current_tab) {
            update_reload_button_for_tab(tab);
        }
    } else {
        tab->browser->Reload();
    }
}

static void on_home(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    BrowserTab *tab = get_selected_tab();
    if (!tab) return;
    browser_set_focus(tab, false);
    const char *url = g_homepage_url.empty() ? kInitialBrowserUrl : g_homepage_url.c_str();
    load_url_for_tab(tab, url);
}

static void focus_url_field_timer(XtPointer client_data, XtIntervalId *id)
{
    (void)client_data;
    (void)id;
    if (!g_url_field) return;
    focus_motif_widget(g_url_field);
    char *value = XmTextFieldGetString(g_url_field);
    int len = value ? (int)strlen(value) : 0;
    XmTextFieldSetSelection(g_url_field, 0, len, CurrentTime);
    XmTextFieldSetInsertionPosition(g_url_field, len);
    XmTextFieldShowPosition(g_url_field, len);
    if (value) XtFree(value);
}

static void on_enter_url(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    if (!g_url_field) return;
    BrowserTab *tab = get_selected_tab();
    if (tab) {
        browser_set_focus(tab, false);
    }
    if (g_app) {
        XtAppAddTimeOut(g_app, 10, focus_url_field_timer, NULL);
    } else {
        focus_url_field_timer(NULL, NULL);
    }
}

static void on_go_back_menu(Widget w, XtPointer client_data, XtPointer call_data)
{
    on_back(w, client_data, call_data);
}

static void on_go_forward_menu(Widget w, XtPointer client_data, XtPointer call_data)
{
    on_forward(w, client_data, call_data);
}

static void on_reload_menu(Widget w, XtPointer client_data, XtPointer call_data)
{
    on_reload(w, client_data, call_data);
}

static void schedule_tab_browser_creation(BrowserTab *tab)
{
    if (!tab || tab->browser || tab->create_scheduled) return;
    tab->create_scheduled = true;
    XtAppAddTimeOut(g_app, 20, initialize_cef_browser_cb, tab);
}

static bool create_cef_browser_for_tab(BrowserTab *tab)
{
    if (!tab || !tab->browser_area) return false;
    if (!XtIsRealized(tab->browser_area)) return false;
    Window xid = XtWindow(tab->browser_area);
    if (!xid) return false;

    Dimension width = 0;
    Dimension height = 0;
    XtVaGetValues(tab->browser_area, XmNwidth, &width, XmNheight, &height, NULL);
    if (width <= 1 || height <= 1) return false;

    CefWindowInfo window_info;
    window_info.SetAsChild(reinterpret_cast<CefWindowHandle>(xid),
                           CefRect(0, 0, (int)width, (int)height));

    CefBrowserSettings browser_settings;
    browser_settings.chrome_status_bubble = STATE_DISABLED;
    const std::string initial = tab->pending_url.empty() ? kInitialBrowserUrl : tab->pending_url;
    CefString cef_url(initial);
    tab->client = new BrowserClient(tab);
    tab->browser = CefBrowserHost::CreateBrowserSync(window_info,
                                                     tab->client,
                                                     cef_url,
                                                     browser_settings,
                                                     nullptr,
                                                     nullptr);
    if (!tab->browser) {
        tab->client = nullptr;
        return false;
    }
    tab->current_url = initial;
    if (!g_cef_message_pump_started && g_app) {
        g_cef_message_pump_started = true;
        XtAppAddTimeOut(g_app, 10, cef_message_pump, NULL);
    }
    CefRefPtr<CefBrowserHost> host = tab->browser->GetHost();
    if (host) {
        resize_cef_browser_to_area(tab, "initial create");
    }
    return true;
}

static void initialize_cef_browser_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)id;
    BrowserTab *tab = (BrowserTab *)client_data;
    if (!tab) return;
    if (tab->browser) {
        tab->create_scheduled = false;
        return;
    }
    if (!g_app || !tab->browser_area) return;
    if (!XtIsRealized(tab->browser_area)) {
        XtAppAddTimeOut(g_app, 20, initialize_cef_browser_cb, tab);
        return;
    }
    Dimension width = 0;
    Dimension height = 0;
    XmUpdateDisplay(tab->browser_area);
    XtVaGetValues(tab->browser_area, XmNwidth, &width, XmNheight, &height, NULL);
    if (width <= 1 || height <= 1) {
        XtAppAddTimeOut(g_app, 20, initialize_cef_browser_cb, tab);
        return;
    }
    if (!create_cef_browser_for_tab(tab)) {
        XtAppAddTimeOut(g_app, 20, initialize_cef_browser_cb, tab);
        return;
    }
    tab->create_scheduled = false;
}

static Widget create_menu_item(Widget parent, const char *name, const char *label, Pixmap icon = XmUNSPECIFIED_PIXMAP)
{
    XmString xm_label = make_string(label);
    Widget item = XtVaCreateManagedWidget(
        name,
        xmPushButtonGadgetClass, parent,
        XmNlabelString, xm_label,
        NULL);
    if (icon != XmUNSPECIFIED_PIXMAP && icon != None) {
        XtVaSetValues(item,
                      XmNlabelType, XmPIXMAP_AND_STRING,
                      XmNlabelPixmap, icon,
                      NULL);
    }
    XmStringFree(xm_label);
    return item;
}

static Widget create_cascade_menu(Widget menu_bar, const char *label, const char *name, char mnemonic)
{
    Widget menu = XmCreatePulldownMenu(menu_bar, (String)name, NULL, 0);
    XmString xm_label = make_string(label);
    Widget cascade = XtVaCreateManagedWidget(
        name,
        xmCascadeButtonGadgetClass, menu_bar,
        XmNlabelString, xm_label,
        XmNmnemonic, mnemonic,
        XmNsubMenuId, menu,
        NULL);
    XmStringFree(xm_label);
    (void)cascade;
    return menu;
}

static void set_menu_accelerator(Widget item, const char *accel, const char *accel_text)
{
    if (!item) return;
    XmString xm = make_string(accel_text ? accel_text : "");
    XtVaSetValues(item,
                  XmNaccelerator, accel ? accel : "",
                  XmNacceleratorText, xm,
                  NULL);
    XmStringFree(xm);
}

static BrowserTab *get_tab_for_widget(Widget page)
{
    if (!page) return NULL;
    BrowserTab *tab = NULL;
    XtVaGetValues(page, XmNuserData, &tab, NULL);
    return tab;
}

static BrowserTab *get_selected_tab()
{
    if (!g_tab_stack) return NULL;
    Widget selected = XmTabStackGetSelectedTab(g_tab_stack);
    return get_tab_for_widget(selected);
}

static bool is_tab_selected(const BrowserTab *tab)
{
    if (!tab || !g_tab_stack) return false;
    Widget selected = XmTabStackGetSelectedTab(g_tab_stack);
    return selected == tab->page;
}

static void focus_motif_widget(Widget widget)
{
    if (!widget) return;
    XmProcessTraversal(widget, XmTRAVERSE_CURRENT);
    if (XtIsRealized(widget)) {
        Display *dpy = XtDisplay(widget);
        Window xid = XtWindow(widget);
        if (dpy && xid) {
            XSetInputFocus(dpy, xid, RevertToParent, CurrentTime);
            XFlush(dpy);
        }
    }
}

static void browser_set_focus(BrowserTab *tab, bool focus)
{
    if (!tab || !tab->browser) return;
    CefRefPtr<CefBrowserHost> host = tab->browser->GetHost();
    if (!host) return;
    host->SetFocus(focus ? 1 : 0);
}

static void focus_browser_area(BrowserTab *tab)
{
    if (!tab || !tab->browser_area) return;
    if (g_url_field) {
        XtVaSetValues(g_url_field, XmNcursorPositionVisible, False, NULL);
    }
    if (g_toplevel) {
        XtSetKeyboardFocus(tab->page ? tab->page : g_toplevel, tab->browser_area);
    }
    XmProcessTraversal(tab->browser_area, XmTRAVERSE_CURRENT);
    if (tab->browser) {
        CefRefPtr<CefBrowserHost> host = tab->browser->GetHost();
        if (host) {
            CefWindowHandle child_handle = host->GetWindowHandle();
            Display *dpy = XtDisplay(tab->browser_area);
            if (child_handle && dpy) {
                XSetInputFocus(dpy, (Window)child_handle, RevertToParent, CurrentTime);
                XFlush(dpy);
            }
            host->SetFocus(1);
        }
    }
}

static void on_url_focus(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    (void)call_data;
    BrowserTab *tab = get_selected_tab();
    if (tab) {
        set_current_tab(tab);
        browser_set_focus(tab, false);
    }
    XtVaSetValues(w, XmNcursorPositionVisible, True, NULL);
    focus_motif_widget(w);
}

static void on_url_button_press(Widget w, XtPointer client_data, XEvent *event, Boolean *continue_to_dispatch)
{
    (void)client_data;
    (void)event;
    if (continue_to_dispatch) {
        *continue_to_dispatch = True;
    }
    BrowserTab *tab = get_selected_tab();
    if (tab) {
        set_current_tab(tab);
        browser_set_focus(tab, false);
    }
    XtVaSetValues(w, XmNcursorPositionVisible, True, NULL);
    focus_motif_widget(w);
}

static void
show_invalid_url_dialog(const char *text)
{
    if (!g_toplevel) return;
    Widget dialog = XmCreateErrorDialog(g_toplevel, xm_name("invalidUrlDialog"), NULL, 0);
    std::string combined = "Invalid URL";
    combined += "\n";
    combined += text && text[0] ? text : "Please check the address.";
    XmString message = make_string(combined.c_str());
    XtVaSetValues(dialog,
                  XmNdialogType, XmDIALOG_ERROR,
                  XmNmessageString, message,
                  NULL);
    XmStringFree(message);
    Widget help = XmMessageBoxGetChild(dialog, XmDIALOG_HELP_BUTTON);
    if (help) XtUnmanageChild(help);
    Widget cancel = XmMessageBoxGetChild(dialog, XmDIALOG_CANCEL_BUTTON);
    if (cancel) XtUnmanageChild(cancel);
    XtAddCallback(dialog, XmNokCallback, (XtCallbackProc)XtDestroyWidget, dialog);
    XtManageChild(dialog);
}

static void on_browser_area_button_press(Widget w, XtPointer client_data, XEvent *event, Boolean *continue_to_dispatch)
{
    (void)w;
    (void)event;
    if (continue_to_dispatch) {
        *continue_to_dispatch = True;
    }
    BrowserTab *tab = (BrowserTab *)client_data;
    if (!tab) return;
    focus_browser_area(tab);
}

static Widget pick_adjacent_tab_page(Widget current_page)
{
    if (!g_tab_stack || !current_page) return NULL;
    WidgetList children = NULL;
    Cardinal num_children = 0;
    XtVaGetValues(g_tab_stack, XmNchildren, &children, XmNnumChildren, &num_children, NULL);
    if (!children || num_children == 0) return NULL;

    int current_index = -1;
    for (Cardinal i = 0; i < num_children; ++i) {
        if (children[i] == current_page) {
            current_index = (int)i;
            break;
        }
    }
    if (current_index < 0) return NULL;

    for (int offset = 1; offset < (int)num_children; ++offset) {
        int idx = current_index + offset;
        if (idx >= 0 && idx < (int)num_children) {
            if (get_tab_for_widget(children[idx])) return children[idx];
        }
    }
    for (int offset = 1; offset < (int)num_children; ++offset) {
        int idx = current_index - offset;
        if (idx >= 0 && idx < (int)num_children) {
            if (get_tab_for_widget(children[idx])) return children[idx];
        }
    }
    return NULL;
}

static Widget find_tab_stack_tabbox(Widget tab_stack)
{
    if (!tab_stack) return NULL;
    WidgetList children = NULL;
    Cardinal num_children = 0;
    XtVaGetValues(tab_stack, XmNchildren, &children, XmNnumChildren, &num_children, NULL);
    for (Cardinal i = 0; i < num_children; ++i) {
        if (XmIsTabBox(children[i])) return children[i];
    }
    for (Cardinal i = 0; i < num_children; ++i) {
        WidgetList nested = NULL;
        Cardinal nested_count = 0;
        XtVaGetValues(children[i], XmNchildren, &nested, XmNnumChildren, &nested_count, NULL);
        for (Cardinal j = 0; j < nested_count; ++j) {
            if (XmIsTabBox(nested[j])) return nested[j];
        }
    }
    return NULL;
}

static void sync_current_tab_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)client_data;
    (void)id;
    BrowserTab *tab = get_selected_tab();
    fprintf(stderr, "[ck-browser] sync_current_tab_cb selected_page=%p tab=%p\n",
            (void *)(g_tab_stack ? XmTabStackGetSelectedTab(g_tab_stack) : NULL),
            (void *)tab);
    if (tab) {
        schedule_tab_browser_creation(tab);
        resize_cef_browser_to_area(tab, "sync tab selection");
    }
    set_current_tab(tab);
}

static void sync_current_tab_timeout_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)client_data;
    (void)id;
    g_tab_sync_scheduled = false;
    sync_current_tab_cb(NULL, NULL);
}

static void schedule_sync_current_tab(const char *reason)
{
    (void)reason;
    if (!g_app) return;
    if (g_tab_sync_scheduled) return;
    g_tab_sync_scheduled = true;
    fprintf(stderr, "[ck-browser] schedule_sync_current_tab reason=%s\n", reason ? reason : "(null)");
    XtAppAddTimeOut(g_app, 0, sync_current_tab_timeout_cb, NULL);
}

static void ensure_tab_header_menu()
{
    if (g_tab_header_menu || !g_tab_stack) return;
    Widget tabbox = find_tab_stack_tabbox(g_tab_stack);
    Widget parent = tabbox ? tabbox : g_tab_stack;
    Widget menu = XmCreatePopupMenu(parent, xm_name("tabHeaderMenu"), NULL, 0);
    XtVaSetValues(menu, XmNwhichButton, Button3, NULL);
    XmString reload_label = make_string("Reload");
    Widget reload = XtVaCreateManagedWidget("tabMenuReload", xmPushButtonGadgetClass, menu,
                                            XmNlabelString, reload_label,
                                            NULL);
    XmStringFree(reload_label);

    XmString close_label = make_string("Close Tab");
    Widget close = XtVaCreateManagedWidget("tabMenuClose", xmPushButtonGadgetClass, menu,
                                           XmNlabelString, close_label,
                                           NULL);
    XmStringFree(close_label);

    XmString close_right_label = make_string("Close Tabs to The Right");
    Widget close_right = XtVaCreateManagedWidget("tabMenuCloseRight", xmPushButtonGadgetClass, menu,
                                                 XmNlabelString, close_right_label,
                                                 NULL);
    XmStringFree(close_right_label);

    XmString move_label = make_string("Move Tab to New Window");
    Widget move_new_window = XtVaCreateManagedWidget("tabMenuMoveNewWindow", xmPushButtonGadgetClass, menu,
                                                     XmNlabelString, move_label,
                                                     NULL);
    XmStringFree(move_label);
    XtAddCallback(reload, XmNactivateCallback, on_tab_menu_reload, NULL);
    XtAddCallback(close, XmNactivateCallback, on_tab_menu_close, NULL);
    XtAddCallback(close_right, XmNactivateCallback, on_tab_menu_close_right, NULL);
    XtAddCallback(move_new_window, XmNactivateCallback, on_tab_menu_move_new_window, NULL);
    g_tab_header_menu = menu;
}

static BrowserTab *get_visible_tab_at_index(int index)
{
    if (!g_tab_stack || index < 0) return NULL;
    WidgetList children = NULL;
    Cardinal num_children = 0;
    XtVaGetValues(g_tab_stack, XmNchildren, &children, XmNnumChildren, &num_children, NULL);
    int seen = 0;
    for (Cardinal i = 0; i < num_children; ++i) {
        BrowserTab *tab = get_tab_for_widget(children[i]);
        if (!tab) continue;
        if (seen == index) return tab;
        seen++;
    }
    return NULL;
}

static int get_visible_tab_index(const BrowserTab *tab)
{
    if (!g_tab_stack || !tab) return -1;
    WidgetList children = NULL;
    Cardinal num_children = 0;
    XtVaGetValues(g_tab_stack, XmNchildren, &children, XmNnumChildren, &num_children, NULL);
    int seen = 0;
    for (Cardinal i = 0; i < num_children; ++i) {
        BrowserTab *candidate = get_tab_for_widget(children[i]);
        if (!candidate) continue;
        if (candidate == tab) return seen;
        seen++;
    }
    return -1;
}

static void on_tab_menu_reload(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    BrowserTab *tab = g_tab_header_menu_target;
    if (!tab || !tab->browser) return;
    fprintf(stderr, "[ck-browser] tab menu: reload tab=%s (%p)\n",
            tab->base_title.empty() ? "Tab" : tab->base_title.c_str(),
            (void *)tab);
    tab->browser->Reload();
    resize_cef_browser_to_area(tab, "tab menu reload");
}

static void on_tab_menu_close(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    BrowserTab *tab = g_tab_header_menu_target;
    if (!tab || !tab->page) return;
    fprintf(stderr, "[ck-browser] tab menu: close tab=%s (%p)\n",
            tab->base_title.empty() ? "Tab" : tab->base_title.c_str(),
            (void *)tab);
    if (g_tab_stack) {
        XmTabStackSelectTab(tab->page, True);
    }
    set_current_tab(tab);
    browser_set_focus(tab, false);
    close_tab_browser(tab);
    XtDestroyWidget(tab->page);
}

static void on_tab_menu_close_right(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    BrowserTab *tab = g_tab_header_menu_target;
    if (!tab) return;
    int index = get_visible_tab_index(tab);
    if (index < 0) return;
    fprintf(stderr, "[ck-browser] tab menu: close right from index=%d tab=%s (%p)\n",
            index,
            tab->base_title.empty() ? "Tab" : tab->base_title.c_str(),
            (void *)tab);
    std::vector<BrowserTab *> to_close;
    for (int i = index + 1;; ++i) {
        BrowserTab *candidate = get_visible_tab_at_index(i);
        if (!candidate) break;
        to_close.push_back(candidate);
    }
    for (auto it = to_close.rbegin(); it != to_close.rend(); ++it) {
        BrowserTab *candidate = *it;
        if (!candidate || !candidate->page) continue;
        browser_set_focus(candidate, false);
        close_tab_browser(candidate);
        XtDestroyWidget(candidate->page);
    }
}

static void on_tab_menu_move_new_window(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    BrowserTab *tab = g_tab_header_menu_target;
    if (!tab) return;
    std::string url = normalize_url(display_url_for_tab(tab));
    fprintf(stderr,
            "[ck-browser] tab menu: move to new window (url-only) tab=%s (%p) url=%s\n",
            tab->base_title.empty() ? "Tab" : tab->base_title.c_str(),
            (void *)tab,
            url.c_str());
    if (!url.empty()) {
        spawn_new_browser_window(url);
    }
    if (tab->page) {
        if (g_tab_stack) {
            XmTabStackSelectTab(tab->page, True);
        }
        set_current_tab(tab);
        browser_set_focus(tab, false);
        close_tab_browser(tab);
        XtDestroyWidget(tab->page);
    }
}

static void on_tabbox_input(Widget w, XtPointer client_data, XEvent *event, Boolean *continue_to_dispatch)
{
    (void)w;
    (void)client_data;
    if (continue_to_dispatch) {
        *continue_to_dispatch = True;
    }
    if (!event) return;
    int type = event->type;
    const char *type_name = "other";
    if (type == ButtonPress) type_name = "ButtonPress";
    else if (type == ButtonRelease) type_name = "ButtonRelease";
    else if (type == KeyPress) type_name = "KeyPress";
    else if (type == KeyRelease) type_name = "KeyRelease";
    fprintf(stderr, "[ck-browser] on_tabbox_input type=%s\n", type_name);
    if (type == ButtonPress) {
        XButtonEvent *bev = (XButtonEvent *)event;
        fprintf(stderr, "[ck-browser] on_tabbox_input ButtonPress button=%u x=%d y=%d x_root=%d y_root=%d\n",
                (unsigned int)bev->button, bev->x, bev->y, bev->x_root, bev->y_root);
        if (bev->button == Button3) {
            ensure_tab_header_menu();
            if (!g_tab_header_menu) return;
            BrowserTab *tab = NULL;
            Widget tabbox = g_tab_stack ? find_tab_stack_tabbox(g_tab_stack) : NULL;
            if (tabbox && XtIsRealized(tabbox)) {
                Dimension tb_w = 0;
                Dimension tb_h = 0;
                XtVaGetValues(tabbox, XmNwidth, &tb_w, XmNheight, &tb_h, NULL);
                fprintf(stderr, "[ck-browser] tab header context coords x=%d y=%d tabbox=%dx%d\n",
                        bev->x, bev->y, (int)tb_w, (int)tb_h);
                int idx = XmTabBoxXYToIndex(tabbox, bev->x, bev->y);
                if (idx >= 0) {
                    tab = get_visible_tab_at_index(idx);
                    fprintf(stderr, "[ck-browser] tab header context idx=%d tab=%p\n", idx, (void *)tab);
                }
            }
            if (!tab) {
                fprintf(stderr, "[ck-browser] tab header context fallback root x=%d y=%d\n",
                        bev->x_root, bev->y_root);
                if (tabbox && XtIsRealized(tabbox)) {
                    int tx = 0, ty = 0;
                    Window child = 0;
                    Display *dpy = XtDisplay(tabbox);
                    if (XTranslateCoordinates(dpy,
                                              RootWindow(dpy, DefaultScreen(dpy)),
                                              XtWindow(tabbox),
                                              bev->x_root,
                                              bev->y_root,
                                              &tx,
                                              &ty,
                                              &child)) {
                        int idx = XmTabBoxXYToIndex(tabbox, tx, ty);
                        tab = get_visible_tab_at_index(idx);
                        fprintf(stderr, "[ck-browser] tab header context fallback idx=%d tab=%p\n", idx, (void *)tab);
                    }
                }
            }
            g_tab_header_menu_target = tab ? tab : get_selected_tab();
            fprintf(stderr, "[ck-browser] tab header context target=%p selected=%p\n",
                    (void *)g_tab_header_menu_target,
                    (void *)get_selected_tab());
            if (continue_to_dispatch) {
                *continue_to_dispatch = False;
            }
            XmMenuPosition(g_tab_header_menu, bev);
            XtManageChild(g_tab_header_menu);
            return;
        }
    }
    if (type == ButtonRelease) {
        XButtonEvent *bev = (XButtonEvent *)event;
        if (bev->button == Button3) {
            return;
        }
    }
    if (type == ButtonRelease || type == KeyRelease) {
        schedule_sync_current_tab("tabbox input release");
    }
}

static void on_global_button_press(Widget w, XtPointer client_data, XEvent *event, Boolean *continue_to_dispatch)
{
    (void)w;
    (void)client_data;
    if (continue_to_dispatch) {
        *continue_to_dispatch = True;
    }
    if (!event || event->type != ButtonPress) return;

    XButtonEvent *bev = (XButtonEvent *)event;
    if (bev->button != Button3) return;

    if (!g_tab_box || !XtIsRealized(g_tab_box)) {
        fprintf(stderr, "[ck-browser] tab header context: no tab box (global handler)\n");
        return;
    }

    Display *dpy = XtDisplay(g_tab_box);
    Window tab_win = XtWindow(g_tab_box);
    if (!dpy || !tab_win) return;

    Window child = 0;
    int x = 0;
    int y = 0;
    if (!XTranslateCoordinates(dpy,
                               RootWindow(dpy, DefaultScreen(dpy)),
                               tab_win,
                               bev->x_root,
                               bev->y_root,
                               &x,
                               &y,
                               &child)) {
        return;
    }

    Dimension w_px = 0;
    Dimension h_px = 0;
    XtVaGetValues(g_tab_box, XmNwidth, &w_px, XmNheight, &h_px, NULL);
    if (x < 0 || y < 0 || x >= (int)w_px || y >= (int)h_px) {
        return;
    }

    fprintf(stderr,
            "[ck-browser] global tab header right click x=%d y=%d root=%d,%d\n",
            x, y, bev->x_root, bev->y_root);

    ensure_tab_header_menu();
    if (!g_tab_header_menu) return;

    int idx = XmTabBoxXYToIndex(g_tab_box, x, y);
    BrowserTab *tab = get_visible_tab_at_index(idx);
    fprintf(stderr, "[ck-browser] global tab header context idx=%d tab=%p\n", idx, (void *)tab);
    g_tab_header_menu_target = tab ? tab : get_selected_tab();

    XButtonEvent pos = *bev;
    pos.window = tab_win;
    pos.x = x;
    pos.y = y;
    XmMenuPosition(g_tab_header_menu, &pos);
    XtManageChild(g_tab_header_menu);
    if (continue_to_dispatch) {
        *continue_to_dispatch = False;
    }
}

static void attach_tab_handlers_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)client_data;
    (void)id;
    if (g_tab_handlers_attached) return;
    if (!g_tab_stack) return;
    if (!XtIsRealized(g_tab_stack)) {
        if (g_app) {
            XtAppAddTimeOut(g_app, 50, attach_tab_handlers_cb, NULL);
        }
        return;
    }

    Widget tab_box = find_tab_stack_tabbox(g_tab_stack);
    g_tab_box = tab_box;
    fprintf(stderr,
            "[ck-browser] attach_tab_handlers tab_stack=%p realized=%d tab_box=%p\n",
            (void *)g_tab_stack,
            XtIsRealized(g_tab_stack) ? 1 : 0,
            (void *)tab_box);

    XtCallbackStatus stack_has = XtHasCallbacks(g_tab_stack, XmNtabSelectedCallback);
    fprintf(stderr, "[ck-browser] XtHasCallbacks(tab_stack, XmNtabSelectedCallback)=%d\n", (int)stack_has);
    if (stack_has != XtCallbackNoList) {
        XtAddCallback(g_tab_stack, XmNtabSelectedCallback, on_tab_selection_changed, NULL);
    }

    long tab_mask = ButtonPressMask | ButtonReleaseMask | KeyPressMask | KeyReleaseMask;
    XtInsertEventHandler(g_tab_stack, tab_mask, False, on_tabbox_input, NULL, XtListHead);

    if (tab_box) {
        XtVaSetValues(tab_box, XmNuniformTabSize, False, NULL);
        XtCallbackStatus box_has = XtHasCallbacks(tab_box, XmNtabSelectedCallback);
        fprintf(stderr, "[ck-browser] XtHasCallbacks(tab_box, XmNtabSelectedCallback)=%d\n", (int)box_has);
        if (box_has != XtCallbackNoList) {
            XtAddCallback(tab_box, XmNtabSelectedCallback, on_tab_selection_changed, NULL);
        }
        XtInsertEventHandler(tab_box, tab_mask, False, on_tabbox_input, NULL, XtListHead);
    }

    if (g_toplevel) {
        XtInsertEventHandler(g_toplevel, ButtonPressMask, True, on_global_button_press, NULL, XtListHead);
    }

    g_tab_handlers_attached = true;
    fprintf(stderr, "[ck-browser] attach_tab_handlers done\n");
}

static void ensure_home_button_menu()
{
    if (g_home_button_menu || !g_home_button) return;
    Widget menu = XmCreatePopupMenu(g_home_button, xm_name("homeButtonMenu"), NULL, 0);
    XtVaSetValues(menu, XmNwhichButton, Button3, NULL);
    XmString blank_label = make_string("Set To Blank Page");
    Widget item_blank = XtVaCreateManagedWidget("homeSetBlank", xmPushButtonGadgetClass, menu,
                                                XmNlabelString, blank_label, NULL);
    XmStringFree(blank_label);
    XmString current_label = make_string("Use Current Page");
    Widget item_current = XtVaCreateManagedWidget("homeUseCurrent", xmPushButtonGadgetClass, menu,
                                                  XmNlabelString, current_label, NULL);
    XmStringFree(current_label);

    XtAddCallback(item_blank, XmNactivateCallback, on_home_menu_set_blank, NULL);
    XtAddCallback(item_current, XmNactivateCallback, on_home_menu_use_current, NULL);
    g_home_button_menu = menu;
}

static void on_home_button_press(Widget w, XtPointer client_data, XEvent *event, Boolean *continue_to_dispatch)
{
    (void)w;
    (void)client_data;
    if (continue_to_dispatch) {
        *continue_to_dispatch = True;
    }
    if (!event || event->type != ButtonPress) return;
    XButtonEvent *bev = (XButtonEvent *)event;
    if (bev->button != Button3) return;
    ensure_home_button_menu();
    if (!g_home_button_menu) return;
    fprintf(stderr, "[ck-browser] home button context menu\n");
    XmMenuPosition(g_home_button_menu, bev);
    XtManageChild(g_home_button_menu);
    if (continue_to_dispatch) {
        *continue_to_dispatch = False;
    }
}

static void on_home_menu_set_blank(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    g_homepage_url = "about:blank";
    save_homepage_file(g_homepage_url, "home menu blank");
}

static void on_home_menu_use_current(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    BrowserTab *tab = get_selected_tab();
    std::string url = normalize_url(display_url_for_tab(tab));
    if (url.empty()) url = kInitialBrowserUrl;
    g_homepage_url = url;
    save_homepage_file(g_homepage_url, "home menu current");
}

static void detach_tab_clients(BrowserTab *tab)
{
    if (!tab) return;
    if (tab->client) {
        tab->client->detach_tab();
        tab->client = nullptr;
    }
    if (tab->devtools_client) {
        tab->devtools_client->detach_tab();
        tab->devtools_client = nullptr;
    }
}

static void remove_tab_from_collection(BrowserTab *tab)
{
    if (!tab) return;
    auto it = std::find_if(g_browser_tabs.begin(), g_browser_tabs.end(),
                           [tab](const std::unique_ptr<BrowserTab> &entry) {
                               return entry.get() == tab;
                           });
    if (it != g_browser_tabs.end()) {
        BrowserTab *match = it->get();
        detach_tab_clients(match);
        g_browser_tabs.erase(it);
    }
}

static int count_tabs_with_base_title(const char *base_title)
{
    if (!base_title) return 0;
    int matches = 0;
    for (const auto &tab : g_browser_tabs) {
        if (tab->base_title == base_title) {
            matches++;
        }
    }
    return matches;
}

static BrowserTab *create_tab_page(Widget tab_stack,
                                   const char *name,
                                   const char *title,
                                   const char *base_title,
                                   const char *initial_url)
{
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
    g_browser_tabs.push_back(std::move(tab));
    update_all_tab_labels("create_tab_page");
    return tab_ptr;
}

static void on_new_tab(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    if (!g_tab_stack) return;

    const char *base = "New Tab";
    int count = count_tabs_with_base_title(base);
    char name[32];
    snprintf(name, sizeof(name), "tabNew%d", count + 1);
    char tab_title[64];
    snprintf(tab_title, sizeof(tab_title), "%s (%d)", base, count + 1);

    BrowserTab *tab = create_tab_page(g_tab_stack, name, tab_title, base, kInitialBrowserUrl);
    schedule_tab_browser_creation(tab);
    XmTabStackSelectTab(tab->page, True);
    set_current_tab(tab);
    update_all_tab_labels("new tab");
}

static void on_new_window(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    BrowserTab *tab = get_selected_tab();
    std::string url = normalize_url(display_url_for_tab(tab));
    if (url.empty()) url = kInitialBrowserUrl;
    fprintf(stderr, "[ck-browser] new window requested url=%s\n", url.c_str());
    spawn_new_browser_window(url);
}

static void on_close_tab(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    if (!g_tab_stack) return;
    Widget selected = XmTabStackGetSelectedTab(g_tab_stack);
    if (!selected) return;

    Widget next_page = pick_adjacent_tab_page(selected);
    if (next_page && next_page != selected) {
        XmTabStackSelectTab(next_page, True);
        BrowserTab *next_tab = get_tab_for_widget(next_page);
        set_current_tab(next_tab);
    }

    BrowserTab *tab = get_tab_for_widget(selected);
    if (tab) {
        browser_set_focus(tab, false);
        close_tab_browser(tab);
    }
    XtDestroyWidget(selected);
}

static void set_current_tab(BrowserTab *tab)
{
    BrowserTab *previous = g_current_tab;
    g_current_tab = tab;
    if (previous && previous != tab) {
        apply_tab_theme_colors(previous, false);
    }
    if (tab) {
        apply_tab_theme_colors(tab, true);
    }
    if (tab) {
        fprintf(stderr, "[ck-browser] set_current_tab -> %s (%p) status='%s'\n",
                tab->base_title.empty() ? "Tab" : tab->base_title.c_str(),
                (void *)tab,
                tab->status_message.c_str());
    } else {
        fprintf(stderr, "[ck-browser] set_current_tab -> (none)\n");
    }
    update_url_field_for_tab(tab);
    if (tab) {
        set_status_label_text(tab->status_message.c_str());
    } else {
        set_status_label_text(NULL);
    }
    update_navigation_buttons(tab);
    update_reload_button_for_tab(tab);
    update_security_controls(tab);
    update_favicon_controls(tab);
    if (tab && tab->browser) {
        CefRefPtr<CefBrowserHost> host = tab->browser->GetHost();
        if (host) {
            tab->zoom_level = host->GetZoomLevel();
        }
    }
    update_zoom_controls(tab);
}

static void on_tab_selection_changed(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    XmTabStackCallbackStruct *info = (XmTabStackCallbackStruct *)call_data;
    if (!g_tab_stack) return;
    Widget selected = XmTabStackGetSelectedTab(g_tab_stack);
    fprintf(stderr, "[ck-browser] on_tab_selection_changed selected_page=%p name=%s\n",
            (void *)selected,
            selected ? XtName(selected) : "(null)");
    if (info) {
        fprintf(stderr,
                "[ck-browser] on_tab_selection_changed reason=%d event=%p selected_child=%p\n",
                info->reason,
                (void *)info->event,
                (void *)info->selected_child);
    }
    BrowserTab *tab = get_tab_for_widget(selected);
    fprintf(stderr, "[ck-browser] active tab changed -> tab=%p title=%s url=%s\n",
            (void *)tab,
            tab ? (tab->base_title.empty() ? "Tab" : tab->base_title.c_str()) : "(none)",
            tab ? display_url_for_tab(tab) : "(none)");
    if (tab) {
        schedule_tab_browser_creation(tab);
        resize_cef_browser_to_area(tab, "tab selection changed");
    }
    set_current_tab(tab);
    if (tab) {
        fprintf(stderr, "[ck-browser] tab selection changed to %s (%p)\n",
                tab->base_title.empty() ? "Tab" : tab->base_title.c_str(), (void *)tab);
    }
}

static void on_tab_destroyed(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    BrowserTab *tab = (BrowserTab *)client_data;
    if (!tab) return;
    bool was_current = (g_current_tab == tab);
    if (was_current) {
        Widget next = g_tab_stack ? XmTabStackGetSelectedTab(g_tab_stack) : NULL;
        BrowserTab *next_tab = get_tab_for_widget(next);
        if (next_tab == tab) {
            next_tab = NULL;
        }
        set_current_tab(next_tab);
    }
    if (g_toplevel && XtIsRealized(g_toplevel)) {
        Display *display = XtDisplay(g_toplevel);
        if (display) {
            if (tab->favicon_toolbar_pixmap != None) {
                XFreePixmap(display, tab->favicon_toolbar_pixmap);
                tab->favicon_toolbar_pixmap = None;
            }
            if (tab->favicon_window_pixmap != None) {
                XFreePixmap(display, tab->favicon_window_pixmap);
                tab->favicon_window_pixmap = None;
            }
            if (tab->favicon_window_mask != None) {
                XFreePixmap(display, tab->favicon_window_mask);
                tab->favicon_window_mask = None;
            }
        }
    }
    close_tab_browser(tab);
    remove_tab_from_collection(tab);
    update_all_tab_labels("tab destroyed");
}

static void on_about_destroy(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    g_about_shell = NULL;
}

static void on_help_about(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    if (!g_toplevel) return;
    if (g_about_shell && XtIsWidget(g_about_shell)) {
        XtPopup(g_about_shell, XtGrabNone);
        return;
    }

    Widget shell = NULL;
    Widget notebook = about_dialog_build(g_toplevel, "about_browser", "About Internet Browser", &shell);
    if (!notebook || !shell) return;

    about_add_standard_pages(notebook, 1,
                             "Internet Browser",
                             "Internet Browser",
                             "A placeholder browser shell for CK-Core.\n"
                             "Tabs, navigation controls, and status display are ready.",
                             True);
    XtAddCallback(shell, XmNdestroyCallback, on_about_destroy, NULL);
    g_about_shell = shell;
    XtPopup(shell, XtGrabNone);
}

static void on_help_view(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    if (!g_toplevel) return;
    Widget dialog = XmCreateInformationDialog(g_toplevel, xm_name("browserHelpDialog"), NULL, 0);
    XmString msg = make_string("Help will be added once browsing features are implemented.");
    XtVaSetValues(dialog, XmNmessageString, msg, NULL);
    XmStringFree(msg);
    XtAddCallback(dialog, XmNokCallback, (XtCallbackProc)XtDestroyWidget, dialog);
    XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));
    XtManageChild(dialog);
}

static void on_add_bookmark_menu(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    fprintf(stderr, "[ck-browser] Add Page menu selected\n");
    show_add_bookmark_dialog(get_selected_tab());
}

static void on_open_bookmark_manager_menu(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    fprintf(stderr, "[ck-browser] Open Bookmark Manager menu selected (bookmarks TODO)\n");
    show_bookmark_manager_dialog();
}

static void
close_bookmark_manager_dialog(BookmarkManagerContext *ctx)
{
    if (!ctx) return;
    bookmark_manager_clear_entry_widgets(ctx);
    if (ctx->dialog) {
        XtDestroyWidget(ctx->dialog);
    }
    delete ctx;
}

static void
bookmark_manager_update_entry_list(BookmarkManagerContext *ctx)
{
    if (!ctx || !ctx->bookmark_list) return;
    bookmark_manager_clear_entry_widgets(ctx);
    if (!ctx->selected_group) return;
    Pixel bg = 0;
    XtVaGetValues(ctx->bookmark_list, XmNbackground, &bg, NULL);
    for (const auto &entry : ctx->selected_group->entries) {
        if (!entry) continue;
        std::string label;
        if (!entry->name.empty()) {
            label = entry->name;
        } else if (!entry->url.empty()) {
            label = entry->url;
        }
        if (label.empty()) {
            label = "Bookmark";
        }
        Pixmap icon_pix = create_bookmark_icon_pixmap(entry.get(), desired_favicon_size(), bg);
        if (icon_pix != None) {
            ctx->bookmark_entry_pixmaps.push_back(icon_pix);
        }
        char item_name[64];
        snprintf(item_name, sizeof(item_name), "bookmarkManagerEntry%p", (void *)entry.get());
        Widget item = create_menu_item(ctx->bookmark_list, item_name, label.c_str(), icon_pix);
        XtVaSetValues(item,
                      XmNalignment, XmALIGNMENT_BEGINNING,
                      XmNuserData, entry.get(),
                      NULL);
        XtAddCallback(item, XmNactivateCallback, bookmark_manager_entry_activate_cb, ctx);
        ctx->bookmark_entry_widgets.push_back(item);
        ctx->entry_items.push_back(entry.get());
    }
    if (!ctx->bookmark_entry_widgets.empty()) {
        Widget first = ctx->bookmark_entry_widgets[0];
        if (first) {
            bookmark_manager_entry_activate_cb(first, ctx, NULL);
        }
    }
}

static void
bookmark_manager_group_selection_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    BookmarkManagerContext *ctx = (BookmarkManagerContext *)client_data;
    if (!ctx || !call_data) return;
    XmListCallbackStruct *cbs = (XmListCallbackStruct *)call_data;
    if (cbs->item_position <= 0 ||
        cbs->item_position > (int)ctx->group_entries.size()) {
        return;
    }
    ctx->selected_group = ctx->group_entries[cbs->item_position - 1];
    bookmark_manager_update_entry_list(ctx);
}

static void
on_bookmark_manager_open(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    BookmarkManagerContext *ctx = (BookmarkManagerContext *)client_data;
    if (!ctx || !ctx->selected_entry || ctx->selected_entry->url.empty()) return;
    BrowserTab *tab = get_selected_tab();
    if (!tab) {
        spawn_new_browser_window(ctx->selected_entry->url.c_str());
        close_bookmark_manager_dialog(ctx);
        return;
    }
    set_current_tab(tab);
    load_url_for_tab(tab, ctx->selected_entry->url);
    close_bookmark_manager_dialog(ctx);
}

static void
on_bookmark_manager_edit(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    BookmarkManagerContext *ctx = (BookmarkManagerContext *)client_data;
    if (!ctx || !ctx->selected_entry) return;
    BookmarkEntry *entry = ctx->selected_entry;
    BookmarkGroup *group = ctx->selected_group ? ctx->selected_group : ensure_bookmark_groups();
    close_bookmark_manager_dialog(ctx);
    show_add_bookmark_dialog(get_selected_tab(), entry, group);
}

static void
on_bookmark_manager_delete(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    BookmarkManagerContext *ctx = (BookmarkManagerContext *)client_data;
    if (!ctx || !ctx->selected_entry || !ctx->selected_group) return;
    BookmarkEntry *entry = ctx->selected_entry;
    BookmarkGroup *group = ctx->selected_group;
    std::string entry_name = entry->name.empty() ? "(none)" : entry->name;
    std::string entry_url = entry->url.empty() ? "(none)" : entry->url;
    std::string group_name = group->name.empty() ? "(group)" : group->name;
    bool removed = false;
    auto &entries = group->entries;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->get() == entry) {
            entries.erase(it);
            removed = true;
            break;
        }
    }
    if (!removed) return;
    fprintf(stderr,
            "[ck-browser] bookmark deleted name='%s' url='%s' group='%s'\n",
            entry_name.c_str(),
            entry_url.c_str(),
            group_name.c_str());
    ctx->selected_entry = NULL;
    ctx->selected_entry_widget = NULL;
    bookmark_manager_update_entry_list(ctx);
    rebuild_bookmarks_menu_items();
    save_bookmarks_to_file();
}

static void
on_bookmark_manager_cancel(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    BookmarkManagerContext *ctx = (BookmarkManagerContext *)client_data;
    close_bookmark_manager_dialog(ctx);
}

static void
on_bookmark_manager_show_bookmarks(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    BookmarkGroup *root = ensure_bookmark_groups();
    if (!root) return;
    save_bookmarks_to_file();
    const char *path = get_bookmarks_file_path();
    if (!path || !path[0]) return;
    std::string url = std::string("file://") + path;
    open_url_in_new_tab(url, True);
}

static void
show_bookmark_manager_dialog()
{
    if (!g_toplevel) return;
    BookmarkGroup *root = ensure_bookmark_groups();
    if (!root) return;
    BookmarkManagerContext *ctx = new BookmarkManagerContext();
    Widget dialog = XmCreateFormDialog(g_toplevel, xm_name("bookmarkManagerDialog"), NULL, 0);
    XmString manager_title = make_string("Bookmark Manager");
    XtVaSetValues(dialog,
                  XmNautoUnmanage, False,
                  XmNmarginWidth, 8,
                  XmNmarginHeight, 8,
                  XmNtitle, "Bookmark Manager",
                  XmNdialogTitle, manager_title,
                  NULL);
    XmStringFree(manager_title);
    ctx->dialog = dialog;

    Widget button_row = XmCreateRowColumn(dialog, xm_name("bookmarkManagerButtons"), NULL, 0);
    XtVaSetValues(button_row,
                  XmNorientation, XmHORIZONTAL,
                  XmNpacking, XmPACK_TIGHT,
                  XmNspacing, 10,
                  XmNtopAttachment, XmATTACH_NONE,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNbottomOffset, 10,
                  XmNleftOffset, 0,
                  XmNrightOffset, 0,
                  NULL);
    XtManageChild(button_row);

    XmString open_label = make_string("Open");
    Widget open_button = XmCreatePushButtonGadget(button_row, xm_name("bookmarkManagerOpen"), NULL, 0);
    XtVaSetValues(open_button, XmNlabelString, open_label, NULL);
    XmStringFree(open_label);
    XtSetSensitive(open_button, False);
    XtAddCallback(open_button, XmNactivateCallback, on_bookmark_manager_open, ctx);
    ctx->open_button = open_button;
    XtManageChild(open_button);

    XmString edit_label = make_string("Edit");
    Widget edit_button = XmCreatePushButtonGadget(button_row, xm_name("bookmarkManagerEdit"), NULL, 0);
    XtVaSetValues(edit_button, XmNlabelString, edit_label, NULL);
    XmStringFree(edit_label);
    XtSetSensitive(edit_button, False);
    XtAddCallback(edit_button, XmNactivateCallback, on_bookmark_manager_edit, ctx);
    ctx->edit_button = edit_button;
    XtManageChild(edit_button);

    XmString delete_label = make_string("Delete");
    Widget delete_button = XmCreatePushButtonGadget(button_row, xm_name("bookmarkManagerDelete"), NULL, 0);
    XtVaSetValues(delete_button, XmNlabelString, delete_label, NULL);
    XmStringFree(delete_label);
    XtSetSensitive(delete_button, False);
    XtAddCallback(delete_button, XmNactivateCallback, on_bookmark_manager_delete, ctx);
    ctx->delete_button = delete_button;
    XtManageChild(delete_button);

    XmString cancel_label = make_string("Cancel");
    Widget cancel_button = XmCreatePushButtonGadget(button_row, xm_name("bookmarkManagerCancel"), NULL, 0);
    XtVaSetValues(cancel_button, XmNlabelString, cancel_label, NULL);
    XmStringFree(cancel_label);
    XtAddCallback(cancel_button, XmNactivateCallback, on_bookmark_manager_cancel, ctx);
    XtManageChild(cancel_button);

    XmString show_label = make_string("Show Bookmarks");
    Widget show_button = XmCreatePushButtonGadget(button_row, xm_name("bookmarkManagerShowBookmarks"), NULL, 0);
    XtVaSetValues(show_button, XmNlabelString, show_label, NULL);
    XmStringFree(show_label);
    XtAddCallback(show_button, XmNactivateCallback, on_bookmark_manager_show_bookmarks, ctx);
    XtManageChild(show_button);

    Widget lists_area = XmCreateRowColumn(dialog, xm_name("bookmarkManagerLists"), NULL, 0);
    XtVaSetValues(lists_area,
                  XmNorientation, XmHORIZONTAL,
                  XmNpacking, XmPACK_TIGHT,
                  XmNspacing, 12,
                  XmNmarginWidth, 4,
                  XmNmarginHeight, 4,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_WIDGET,
                  XmNbottomWidget, button_row,
                  NULL);
    XtManageChild(lists_area);

    Widget group_frame = XmCreateFrame(lists_area, xm_name("bookmarkManagerGroupFrame"), NULL, 0);
    XtManageChild(group_frame);
    Widget group_list = XmCreateScrolledList(group_frame, xm_name("bookmarkManagerGroupList"), NULL, 0);
    XtVaSetValues(group_list,
                  XmNscrollingPolicy, XmAUTOMATIC,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNwidth, 260,
                  XmNheight, 240,
                  NULL);
    XtAddCallback(group_list, XmNbrowseSelectionCallback, bookmark_manager_group_selection_cb, ctx);
    XtManageChild(group_list);
    ctx->group_list = group_list;

    Widget bookmark_frame = XmCreateFrame(lists_area, xm_name("bookmarkManagerBookmarkFrame"), NULL, 0);
    XtManageChild(bookmark_frame);
    Widget bookmark_scroll = XmCreateScrolledWindow(bookmark_frame, xm_name("bookmarkManagerBookmarkScroll"), NULL, 0);
    XtVaSetValues(bookmark_scroll,
                  XmNscrollingPolicy, XmAUTOMATIC,
                  XmNwidth, 360,
                  XmNheight, 240,
                  NULL);
    XtManageChild(bookmark_scroll);
    Widget bookmark_list = XmCreateRowColumn(bookmark_scroll, xm_name("bookmarkManagerBookmarkList"), NULL, 0);
    XtVaSetValues(bookmark_list,
                  XmNorientation, XmVERTICAL,
                  XmNpacking, XmPACK_TIGHT,
                  XmNrowColumnType, XmWORK_AREA,
                  XmNspacing, 4,
                  NULL);
    XtManageChild(bookmark_list);
    ctx->bookmark_list = bookmark_list;

    std::vector<std::string> labels;
    collect_bookmark_groups(root, ctx->group_entries, labels, 0);
    for (size_t i = 0; i < ctx->group_entries.size(); ++i) {
        XmString xm_label = make_string(labels[i].c_str());
        XmListAddItemUnselected(group_list, xm_label, (int)i + 1);
        XmStringFree(xm_label);
    }
    if (!ctx->group_entries.empty()) {
        ctx->selected_group = ctx->group_entries[0];
        XmListSelectPos(group_list, 1, False);
    }
    bookmark_manager_update_entry_list(ctx);

    XtManageChild(dialog);
}

static BookmarkGroup *
add_bookmark_group(BookmarkGroup *parent, const char *name)
{
    if (!parent) return NULL;
    parent->children.emplace_back(std::make_unique<BookmarkGroup>());
    BookmarkGroup *child = parent->children.back().get();
    child->name = name ? name : "";
    return child;
}

static std::unique_ptr<BookmarkGroup>
create_default_bookmark_tree()
{
    auto root = std::make_unique<BookmarkGroup>();
    root->name = "Bookmarks Menu";
    BookmarkGroup *favorites = add_bookmark_group(root.get(), "Favorites");
    add_bookmark_group(favorites, "Work");
    add_bookmark_group(favorites, "Personal");
    BookmarkGroup *other = add_bookmark_group(root.get(), "Other Bookmarks");
    add_bookmark_group(other, "Research");
    add_bookmark_group(other, "Snippets");
    return root;
}

static BookmarkGroup *
ensure_bookmark_groups()
{
    if (g_bookmark_root) return g_bookmark_root.get();
    std::unique_ptr<BookmarkGroup> loaded = load_bookmarks_from_file();
    if (loaded) {
        g_bookmark_root = std::move(loaded);
    } else {
        g_bookmark_root = create_default_bookmark_tree();
    }
    BookmarkGroup *selected = g_bookmark_root.get();
    if (g_bookmark_root->children.empty()) {
        selected = g_bookmark_root.get();
    } else {
        selected = g_bookmark_root->children[0].get();
    }
    g_selected_bookmark_group = selected ? selected : g_bookmark_root.get();
    return g_bookmark_root.get();
}

static void
collect_bookmark_groups(BookmarkGroup *group,
                        std::vector<BookmarkGroup *> &entries,
                        std::vector<std::string> &labels,
                        int depth)
{
    if (!group) return;
    std::string indent(depth * 2, ' ');
    entries.push_back(group);
    labels.emplace_back(indent + group->name);
    for (const auto &child : group->children) {
        collect_bookmark_groups(child.get(), entries, labels, depth + 1);
    }
}

static void
collect_bookmark_menu_entries(BookmarkGroup *group,
                              std::vector<BookmarkEntry *> &entries)
{
    if (!group) return;
    for (const auto &entry : group->entries) {
        if (entry && entry->show_in_menu) {
            entries.push_back(entry.get());
        }
    }
    for (const auto &child : group->children) {
        collect_bookmark_menu_entries(child.get(), entries);
    }
}

static void
ensure_path_directory(const char *path)
{
    if (!path) return;
    char dir[PATH_MAX];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (dir[0]) {
            mkdir(dir, 0700);
        }
    }
}

static const char *
get_bookmarks_file_path()
{
    if (!g_bookmarks_path_ready) {
        config_build_path(g_bookmarks_file_path, sizeof(g_bookmarks_file_path), kBookmarksFileName);
        g_bookmarks_path_ready = true;
    }
    return g_bookmarks_file_path;
}

static time_t
get_file_mtime(const char *path)
{
    if (!path || path[0] == '\0') return 0;
    struct stat st;
    if (stat(path, &st) == 0) {
        return st.st_mtime;
    }
    return 0;
}

static std::string
escape_html(const std::string &input)
{
    std::string output;
    output.reserve(input.size());
    for (char c : input) {
        switch (c) {
            case '&': output += "&amp;"; break;
            case '<': output += "&lt;"; break;
            case '>': output += "&gt;"; break;
            case '\"': output += "&quot;"; break;
            default: output += c; break;
        }
    }
    return output;
}

static const char kBase64Chars[] =
"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
"abcdefghijklmnopqrstuvwxyz"
"0123456789+/";

static std::string
base64_encode(const std::vector<unsigned char> &data)
{
    std::string output;
    size_t len = data.size();
    size_t i = 0;
    output.reserve(((len + 2) / 3) * 4);
    while (i + 2 < len) {
        unsigned char a = data[i++];
        unsigned char b = data[i++];
        unsigned char c = data[i++];
        output.push_back(kBase64Chars[a >> 2]);
        output.push_back(kBase64Chars[((a & 0x3) << 4) | (b >> 4)]);
        output.push_back(kBase64Chars[((b & 0xf) << 2) | (c >> 6)]);
        output.push_back(kBase64Chars[c & 0x3f]);
    }
    size_t remaining = len - i;
    if (remaining == 1) {
        unsigned char a = data[i];
        output.push_back(kBase64Chars[a >> 2]);
        output.push_back(kBase64Chars[(a & 0x3) << 4]);
        output.push_back('=');
        output.push_back('=');
    } else if (remaining == 2) {
        unsigned char a = data[i++];
        unsigned char b = data[i];
        output.push_back(kBase64Chars[a >> 2]);
        output.push_back(kBase64Chars[((a & 0x3) << 4) | (b >> 4)]);
        output.push_back(kBase64Chars[(b & 0xf) << 2]);
        output.push_back('=');
    }
    return output;
}

static inline bool
is_base64_char(char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '+' || c == '/';
}

static bool
base64_decode(const std::string &input, std::vector<unsigned char> &output)
{
    output.clear();
    int val = 0;
    int valb = -8;
    for (char ch : input) {
        if (isspace((unsigned char)ch)) continue;
        if (ch == '=') {
            valb = -8;
            break;
        }
        if (!is_base64_char(ch)) continue;
        val = (val << 6) + (int)(strchr(kBase64Chars, ch) - kBase64Chars);
        valb += 6;
        if (valb >= 0) {
            output.push_back((unsigned char)((val >> valb) & 0xff));
            valb -= 8;
        }
    }
    return !output.empty();
}

static bool
extract_base64_payload(const std::string &value, std::string &out_payload)
{
    if (value.empty()) return false;
    size_t comma = value.find(',');
    if (comma == std::string::npos) {
        out_payload = value;
    } else {
        out_payload = value.substr(comma + 1);
    }
    while (!out_payload.empty() && isspace((unsigned char)out_payload.front())) {
        out_payload.erase(out_payload.begin());
    }
    while (!out_payload.empty() && isspace((unsigned char)out_payload.back())) {
        out_payload.pop_back();
    }
    return !out_payload.empty();
}

static bool
bookmark_entry_set_icon_png(BookmarkEntry *entry, const unsigned char *png_data, size_t png_size)
{
    if (!entry || !png_data || png_size == 0) return false;
    CefRefPtr<CefImage> image = CefImage::CreateImage();
    if (!image) return false;
    if (!image->AddPNG(1.0f, png_data, png_size)) return false;
    int pixel_w = 0;
    int pixel_h = 0;
    CefRefPtr<CefBinaryValue> raw = image->GetAsBitmap(1.0f,
                                                      CEF_COLOR_TYPE_BGRA_8888,
                                                      CEF_ALPHA_TYPE_PREMULTIPLIED,
                                                      pixel_w,
                                                      pixel_h);
    if (!raw || pixel_w <= 0 || pixel_h <= 0) return false;
    const unsigned char *raw_bytes = (const unsigned char *)raw->GetRawData();
    if (!raw_bytes) return false;
    size_t raw_size = raw->GetSize();
    entry->icon_png.assign(png_data, png_data + png_size);
    entry->icon_raw.assign(raw_bytes, raw_bytes + raw_size);
    entry->icon_width = pixel_w;
    entry->icon_height = pixel_h;
    return true;
}

static void
bookmark_entry_copy_icon_from_cache(BookmarkEntry *entry, const char *url)
{
    if (!entry || !url) return;
    auto it = g_favicon_cache.find(url);
    if (it == g_favicon_cache.end()) return;
    if (it->second.width > 0 && it->second.height > 0) {
        size_t expected_size = (size_t)it->second.width * (size_t)it->second.height * 4u;
        if (it->second.data.size() >= expected_size) {
            entry->icon_raw = it->second.data;
            entry->icon_width = it->second.width;
            entry->icon_height = it->second.height;
        }
    }
    if (!it->second.png_data.empty()) {
        entry->icon_png = it->second.png_data;
    }
}

static Pixmap
create_bookmark_icon_pixmap(BookmarkEntry *entry, int target_size, Pixel bg_pixel)
{
    if (!entry || target_size <= 0) {
        fprintf(stderr, "[ck-browser] create_bookmark_icon_pixmap skipped (entry=%p target=%d)\n",
                (void *)entry, target_size);
        return None;
    }
    if (entry->icon_raw.empty() && g_cef_initialized && !entry->icon_png.empty()) {
        if (bookmark_entry_set_icon_png(entry, entry->icon_png.data(), entry->icon_png.size())) {
            fprintf(stderr,
                    "[ck-browser] bookmark icon converted post-CEF for entry='%s'\n",
                    entry->name.empty() ? "(none)" : entry->name.c_str());
        }
    }
    if (entry->icon_raw.empty() || entry->icon_width <= 0 || entry->icon_height <= 0) {
        fprintf(stderr, "[ck-browser] create_bookmark_icon_pixmap aborted (raw=%zu w=%d h=%d)\n",
                entry->icon_raw.size(), entry->icon_width, entry->icon_height);
        return None;
    }
    if (!g_toplevel) return None;
    Display *display = XtDisplay(g_toplevel);
    if (!display) return None;
    int screen = DefaultScreen(display);
    return create_scaled_toolbar_pixmap_from_bgra(display,
                                                  screen,
                                                  entry->icon_raw.data(),
                                                  entry->icon_width,
                                                  entry->icon_height,
                                                  target_size,
                                                  bg_pixel);
}

static void
clear_bookmark_menu_icon_pixmaps()
{
    Display *display = g_toplevel ? XtDisplay(g_toplevel) : NULL;
    for (Pixmap pix : g_bookmark_menu_icon_pixmaps) {
        if (pix != None && display) {
            XFreePixmap(display, pix);
        }
    }
    g_bookmark_menu_icon_pixmaps.clear();
}

static void
write_indent(FILE *f, int indent)
{
    for (int i = 0; i < indent; ++i) {
        fputc(' ', f);
    }
}

static void
write_bookmark_entry(FILE *f, BookmarkEntry *entry, int indent)
{
    if (!f || !entry) return;
    time_t now = time(NULL);
    write_indent(f, indent);
    std::string icon_attr;
    if (!entry->icon_png.empty()) {
        std::string encoded = base64_encode(entry->icon_png);
        icon_attr = " ICON=\"data:image/png;base64,";
        icon_attr += encoded;
        icon_attr += "\"";
    }
    fprintf(f,
            "<DT><A HREF=\"%s\" ADD_DATE=\"%lld\" LAST_MODIFIED=\"%lld\" LAST_VISIT=\"%lld\" SHOW_IN_MENU=\"%d\"%s>%s</A>\n",
            escape_html(entry->url).c_str(),
            (long long)now,
            (long long)now,
            (long long)now,
            entry->show_in_menu ? 1 : 0,
            icon_attr.c_str(),
            escape_html(entry->name).c_str());
}

static void
write_group_contents(FILE *f, BookmarkGroup *group, int indent)
{
    if (!f || !group) return;
    time_t now = time(NULL);
    for (const auto &child : group->children) {
        if (!child) continue;
        write_indent(f, indent);
        fprintf(f,
                "<DT><H3 ADD_DATE=\"%lld\" LAST_MODIFIED=\"%lld\">%s</H3>\n",
                (long long)now,
                (long long)now,
                escape_html(child->name).c_str());
        write_indent(f, indent);
        fprintf(f, "<DL><p>\n");
        write_group_contents(f, child.get(), indent + 4);
        write_indent(f, indent);
        fprintf(f, "</DL><p>\n");
    }
    for (const auto &entry : group->entries) {
        if (!entry) continue;
        write_bookmark_entry(f, entry.get(), indent);
    }
}

static void
write_netscape_bookmarks(FILE *f, BookmarkGroup *root)
{
    if (!f || !root) return;
    fprintf(f,
            "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
            "<!-- This is an automatically generated file.\n"
            "     It will be read and overwritten.\n"
            "     DO NOT EDIT! -->\n"
            "<HTML>\n"
            "<HEAD>\n"
            "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
            "<TITLE>Bookmarks</TITLE>\n"
            "</HEAD>\n"
            "<BODY>\n"
            "<H1>Bookmarks</H1>\n\n"
            "<DL><p>\n");
    write_group_contents(f, root, 4);
    fprintf(f, "</DL><p>\n</BODY>\n</HTML>\n");
}

static bool
extract_tag_text(const std::string &content, size_t start, const char *closing_tag, size_t &out_end, std::string &out_text)
{
    size_t closing = content.find(closing_tag, start);
    if (closing == std::string::npos) return false;
    out_text = content.substr(start, closing - start);
    out_end = closing + strlen(closing_tag);
    while (!out_text.empty() && isspace((unsigned char)out_text.front())) {
        out_text.erase(out_text.begin());
    }
    while (!out_text.empty() && isspace((unsigned char)out_text.back())) {
        out_text.pop_back();
    }
    return true;
}

static bool
parse_attribute_value(const std::string &tag, const char *name, std::string &out_value)
{
    std::string lower_name;
    for (const char *c = name; *c; ++c) {
        lower_name.push_back(std::tolower((unsigned char)*c));
    }
    size_t pos = 0;
    while (pos < tag.size()) {
        while (pos < tag.size() && isspace((unsigned char)tag[pos])) ++pos;
        size_t name_start = pos;
        while (pos < tag.size() && tag[pos] != '=' && !isspace((unsigned char)tag[pos])) ++pos;
        if (pos >= tag.size()) break;
        std::string attr_name = tag.substr(name_start, pos - name_start);
        std::string attr_name_lower;
        for (char ch : attr_name) {
            attr_name_lower.push_back(std::tolower((unsigned char)ch));
        }
        while (pos < tag.size() && isspace((unsigned char)tag[pos])) ++pos;
        if (pos >= tag.size() || tag[pos] != '=') continue;
        ++pos;
        while (pos < tag.size() && isspace((unsigned char)tag[pos])) ++pos;
        if (pos >= tag.size()) break;
        char quote = tag[pos];
        if (quote == '\"' || quote == '\'') {
            ++pos;
            size_t value_start = pos;
            while (pos < tag.size() && tag[pos] != quote) ++pos;
            std::string attr_value = tag.substr(value_start, pos - value_start);
            if (attr_name_lower == lower_name) {
                out_value = attr_value;
                return true;
            }
            if (pos < tag.size()) ++pos;
        } else {
            size_t value_start = pos;
            while (pos < tag.size() && !isspace((unsigned char)tag[pos])) ++pos;
            std::string attr_value = tag.substr(value_start, pos - value_start);
            if (attr_name_lower == lower_name) {
                out_value = attr_value;
                return true;
            }
        }
    }
    return false;
}

static std::string
trim_whitespace(std::string value)
{
    size_t start = 0;
    while (start < value.size() && isspace((unsigned char)value[start])) start++;
    size_t end = value.size();
    while (end > start && isspace((unsigned char)value[end - 1])) end--;
    return value.substr(start, end - start);
}

static BookmarkGroup *
find_or_create_child_group(BookmarkGroup *parent, const std::string &name)
{
    if (!parent) return NULL;
    for (const auto &child : parent->children) {
        if (child && child->name == name) {
            return child.get();
        }
    }
    return add_bookmark_group(parent, name.c_str());
}

static void
parse_netscape_bookmarks(const std::string &content, BookmarkGroup *root)
{
    if (!root) {
        fprintf(stderr, "[ck-browser] parse_netscape_bookmarks called with null root\n");
        return;
    }
    fprintf(stderr,
            "[ck-browser] parse_netscape_bookmarks start len=%zu root=%p\n",
            content.size(),
            (void *)root);
    std::vector<BookmarkGroup *> stack;
    stack.push_back(root);
    size_t pos = 0;
    size_t iterations = 0;
    static const size_t kBookmarkParseMaxStack = 128;
    static const size_t kBookmarkParseMaxIterations = 200000;
    while (pos < content.size()) {
        if (++iterations > kBookmarkParseMaxIterations) {
            fprintf(stderr, "[ck-browser] parse aborted: too many iterations (%zu)\n", iterations);
            break;
        }
        size_t lt = content.find('<', pos);
        if (lt == std::string::npos) break;
        size_t gt = content.find('>', lt);
        if (gt == std::string::npos) {
            fprintf(stderr, "[ck-browser] parse_netscape_bookmarks: missing closing '>' after pos=%zu\n", lt);
            break;
        }
        std::string tag = content.substr(lt + 1, gt - lt - 1);
        std::string tag_preview = tag.substr(0, std::min(tag.size(), (size_t)128));
        fprintf(stderr,
                "[ck-browser] parse loop pos=%zu lt=%zu gt=%zu stack=%zu tag_preview='%s'\n",
                pos,
                lt,
                gt,
                stack.size(),
                tag_preview.c_str());
        size_t name_start = 0;
        while (name_start < tag.size() && isspace((unsigned char)tag[name_start])) {
            name_start++;
        }
        if (name_start >= tag.size()) {
            pos = gt + 1;
            continue;
        }
        bool closing = false;
        if (tag[name_start] == '/') {
            closing = true;
            name_start++;
        }
        size_t name_end = name_start;
        while (name_end < tag.size() && !isspace((unsigned char)tag[name_end])) {
            name_end++;
        }
        std::string name = tag.substr(name_start, name_end - name_start);
        for (char &c : name) {
            c = std::toupper((unsigned char)c);
        }
        if (closing) {
            if (name == "DL") {
                if (stack.size() > 1) {
                    stack.pop_back();
                } else {
                    fprintf(stderr, "[ck-browser] parse_netscape_bookmarks: extra </DL> at pos=%zu\n", pos);
                }
            }
            pos = gt + 1;
            continue;
        }
        if (name == "DL" || name == "P" || name == "DT") {
            pos = gt + 1;
            continue;
        }
        if (name == "H3") {
            std::string folder_name;
            size_t end_pos;
            if (extract_tag_text(content, gt + 1, "</H3>", end_pos, folder_name)) {
                BookmarkGroup *parent = stack.back();
                std::string trimmed = trim_whitespace(folder_name);
                BookmarkGroup *child = find_or_create_child_group(parent, trimmed);
                if (child) {
                    fprintf(stderr,
                            "[ck-browser] parse bookmark folder='%s' parent='%s' child='%s'\n",
                            trimmed.c_str(),
                            parent ? parent->name.c_str() : "(null)",
                            child->name.c_str());
                    if (stack.size() < kBookmarkParseMaxStack) {
                        stack.push_back(child);
                    } else {
                        fprintf(stderr, "[ck-browser] bookmark stack overflow, resetting\n");
                        stack.clear();
                        stack.push_back(root);
                    }
                }
                pos = end_pos;
                continue;
            }
            fprintf(stderr, "[ck-browser] parse_netscape_bookmarks could not find </H3> after pos=%zu\n", gt + 1);
        } else if (name == "A") {
            std::string href;
            parse_attribute_value(tag, "HREF", href);
            std::string show_in_menu;
            parse_attribute_value(tag, "SHOW_IN_MENU", show_in_menu);
            std::string icon_value;
            parse_attribute_value(tag, "ICON", icon_value);
            std::string link_text;
            size_t end_pos;
            if (extract_tag_text(content, gt + 1, "</A>", end_pos, link_text) && !href.empty()) {
                BookmarkGroup *current = stack.back();
                if (!current) current = root;
                auto entry = std::make_unique<BookmarkEntry>();
                entry->name = trim_whitespace(link_text);
                entry->url = href;
                entry->show_in_menu = (show_in_menu == "1" || show_in_menu == "true" || show_in_menu == "yes");
                if (!icon_value.empty()) {
                    std::string payload;
                    if (extract_base64_payload(icon_value, payload)) {
                        std::vector<unsigned char> icon_bytes;
                        if (base64_decode(payload, icon_bytes)) {
                            entry->icon_png = icon_bytes;
                            entry->icon_width = 0;
                            entry->icon_height = 0;
                        }
                    }
                }
                fprintf(stderr,
                        "[ck-browser] parse bookmark entry name='%s' url='%s' show=%s\n",
                        entry->name.c_str(),
                        entry->url.c_str(),
                        show_in_menu.c_str());
                current->entries.emplace_back(std::move(entry));
                pos = end_pos;
                continue;
            }
            fprintf(stderr, "[ck-browser] parse_netscape_bookmarks could not parse <A> entry near pos=%zu\n", gt + 1);
        }
        pos = gt + 1;
    }
}

static void
save_bookmarks_to_file()
{
    LOG_ENTER("root=%p", (void *)g_bookmark_root.get());
    if (!g_bookmark_root) {
        LOG_ENTER("no bookmark root, skipping save");
        return;
    }
    const char *path = get_bookmarks_file_path();
    if (!path || path[0] == '\0') {
        LOG_ENTER("no bookmark path available, skipping save");
        return;
    }
    fprintf(stderr, "[ck-browser] saving bookmarks to %s\n", path);
    ensure_path_directory(path);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    write_netscape_bookmarks(f, g_bookmark_root.get());
    fclose(f);
    g_bookmarks_file_mtime = get_file_mtime(path);
}

	static std::unique_ptr<BookmarkGroup>
	load_bookmarks_from_file()
{
    const char *path = get_bookmarks_file_path();
    if (!path || path[0] == '\0') return nullptr;
    fprintf(stderr, "[ck-browser] loading bookmarks from %s\n", path);
    time_t new_mtime = get_file_mtime(path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[ck-browser] load_bookmarks_from_file failed to open %s\n", path);
        g_bookmarks_file_mtime = new_mtime;
        return nullptr;
    }
    std::string content;
    char buffer[4096];
    size_t read = 0;
    while ((read = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        content.append(buffer, read);
    }
    fclose(f);
    auto root = std::make_unique<BookmarkGroup>();
    root->name = "Bookmarks Menu";
    parse_netscape_bookmarks(content, root.get());
    g_bookmarks_file_mtime = new_mtime;
    fprintf(stderr,
            "[ck-browser] bookmarks loaded, groups=%zu entries=%zu\n",
            root->children.size(),
            root->entries.size());
    if (root->children.empty() && root->entries.empty()) {
        fprintf(stderr, "[ck-browser] bookmarks file parsed but produced no children; falling back to default tree\n");
        return create_default_bookmark_tree();
    }
    return root;
}

static void
bookmark_file_monitor_timer_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)client_data;
    (void)id;
    const char *path = get_bookmarks_file_path();
    if (path && path[0]) {
        struct stat st;
        if (stat(path, &st) == 0) {
            time_t current = st.st_mtime;
            if (current == 0) {
                current = get_file_mtime(path);
            }
            if (current != g_bookmarks_file_mtime) {
                fprintf(stderr, "[ck-browser] bookmark monitor detected change (current=%ld prior=%ld)\n",
                        (long)current, (long)g_bookmarks_file_mtime);
                std::unique_ptr<BookmarkGroup> loaded = load_bookmarks_from_file();
                if (loaded) {
                    g_bookmark_root = std::move(loaded);
                    BookmarkGroup *selected = g_bookmark_root.get();
                    if (!g_bookmark_root->children.empty()) {
                        selected = g_bookmark_root->children[0].get();
                    }
                    g_selected_bookmark_group = selected ? selected : g_bookmark_root.get();
                    rebuild_bookmarks_menu_items();
                }
            }
        } else if (g_bookmarks_file_mtime != 0) {
            g_bookmarks_file_mtime = 0;
            std::unique_ptr<BookmarkGroup> fallback = create_default_bookmark_tree();
            if (fallback) {
                g_bookmark_root = std::move(fallback);
                BookmarkGroup *selected = g_bookmark_root.get();
                if (!g_bookmark_root->children.empty()) {
                    selected = g_bookmark_root->children[0].get();
                }
                g_selected_bookmark_group = selected ? selected : g_bookmark_root.get();
                rebuild_bookmarks_menu_items();
            }
        }
    }
    if (g_app) {
        XtAppAddTimeOut(g_app, 1000, bookmark_file_monitor_timer_cb, NULL);
    }
}

static BookmarkEntry *
find_bookmark_by_url(BookmarkGroup *group, const std::string &url, BookmarkGroup **out_group)
{
    if (!group || url.empty()) return NULL;
    for (const auto &entry : group->entries) {
        if (entry && entry->url == url) {
            if (out_group) *out_group = group;
            return entry.get();
        }
    }
    for (const auto &child : group->children) {
        BookmarkEntry *found = find_bookmark_by_url(child.get(), url, out_group);
        if (found) return found;
    }
    return NULL;
}

static void
bookmark_dialog_populate_group_list(BookmarkDialogContext *ctx, Widget list, BookmarkGroup *group, int depth)
{
    if (!ctx || !list || !group) return;
    std::string indent(depth * 2, ' ');
    std::string label = indent + (group->name.empty() ? "Bookmarks" : group->name);
    XmString xm_label = make_string(label.c_str());
    XmListAddItemUnselected(list, xm_label, (int)ctx->group_entries.size() + 1);
    XmStringFree(xm_label);
    ctx->group_entries.push_back(group);
    for (const auto &child : group->children) {
        bookmark_dialog_populate_group_list(ctx, list, child.get(), depth + 1);
    }
}

static void
bookmark_dialog_group_list_selection_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    BookmarkDialogContext *ctx = (BookmarkDialogContext *)client_data;
    if (!ctx || !call_data) return;
    XmListCallbackStruct *cbs = (XmListCallbackStruct *)call_data;
    if (!cbs) return;
    int pos = cbs->item_position - 1;
    if (pos < 0 || pos >= (int)ctx->group_entries.size()) return;
    ctx->selected_group = ctx->group_entries[pos];
    g_selected_bookmark_group = ctx->selected_group;
}

static void
rebuild_bookmarks_menu_items()
{
    if (!g_bookmarks_menu) return;
    for (Widget item : g_bookmark_menu_items) {
        if (item) {
            XtDestroyWidget(item);
        }
    }
    g_bookmark_menu_items.clear();
    clear_bookmark_menu_icon_pixmaps();
    BookmarkGroup *root = ensure_bookmark_groups();
    if (!root) return;
    std::vector<BookmarkEntry *> favorites;
    collect_bookmark_menu_entries(root, favorites);
    fprintf(stderr,
            "[ck-browser] rebuild_bookmarks_menu_items found %zu favorites\n",
            favorites.size());
    static const char *kFavoriteAccelerators[] = {
        "Ctrl<Key>1", "Ctrl<Key>2", "Ctrl<Key>3", "Ctrl<Key>4", "Ctrl<Key>5",
        "Ctrl<Key>6", "Ctrl<Key>7", "Ctrl<Key>8", "Ctrl<Key>9"};
    static const char *kFavoriteAccelTexts[] = {
        "Ctrl+1", "Ctrl+2", "Ctrl+3", "Ctrl+4", "Ctrl+5",
        "Ctrl+6", "Ctrl+7", "Ctrl+8", "Ctrl+9"};
    Pixel menu_bg = 0;
    if (g_bookmarks_menu) {
        XtVaGetValues(g_bookmarks_menu, XmNbackground, &menu_bg, NULL);
    } else if (g_toplevel) {
        XtVaGetValues(g_toplevel, XmNbackground, &menu_bg, NULL);
    }
    for (size_t i = 0; i < favorites.size(); ++i) {
        BookmarkEntry *entry = favorites[i];
        if (!entry) continue;
        fprintf(stderr,
                "[ck-browser] bookmark menu entry %zu: name='%s' url='%s'\n",
                i,
                entry->name.empty() ? "(no name)" : entry->name.c_str(),
                entry->url.empty() ? "(no url)" : entry->url.c_str());
        char name_buf[64];
        snprintf(name_buf, sizeof(name_buf), "bookmarkFavorite%p", (void *)entry);
        std::string label = entry->name.empty() ? entry->url : entry->name;
        if (label.empty()) {
            label = "Bookmark";
        }
        Pixmap icon_pix = create_bookmark_icon_pixmap(entry, desired_favicon_size(), menu_bg);
        if (icon_pix != None) {
            g_bookmark_menu_icon_pixmaps.push_back(icon_pix);
        }
        Widget item = create_menu_item(g_bookmarks_menu, name_buf, label.c_str(), icon_pix);
        XtAddCallback(item, XmNactivateCallback, on_bookmark_menu_activate, entry);
        if (i < sizeof(kFavoriteAccelerators) / sizeof(kFavoriteAccelerators[0])) {
            set_menu_accelerator(item, kFavoriteAccelerators[i], kFavoriteAccelTexts[i]);
        }
        g_bookmark_menu_items.push_back(item);
    }
}

static void
bookmark_manager_free_entry_pixmaps(BookmarkManagerContext *ctx)
{
    if (!ctx) return;
    Display *display = NULL;
    if (ctx->bookmark_list) {
        display = XtDisplay(ctx->bookmark_list);
    } else if (g_toplevel) {
        display = XtDisplay(g_toplevel);
    }
    for (Pixmap pix : ctx->bookmark_entry_pixmaps) {
        if (pix != None && display) {
            XFreePixmap(display, pix);
        }
    }
    ctx->bookmark_entry_pixmaps.clear();
}

static void
bookmark_manager_clear_entry_widgets(BookmarkManagerContext *ctx)
{
    if (!ctx) return;
    for (Widget w : ctx->bookmark_entry_widgets) {
        if (w) {
            XtDestroyWidget(w);
        }
    }
    ctx->bookmark_entry_widgets.clear();
    bookmark_manager_free_entry_pixmaps(ctx);
    ctx->entry_items.clear();
    ctx->selected_entry = NULL;
    ctx->selected_entry_widget = NULL;
    if (ctx->open_button) {
        XtSetSensitive(ctx->open_button, False);
    }
    if (ctx->edit_button) {
        XtSetSensitive(ctx->edit_button, False);
    }
    if (ctx->delete_button) {
        XtSetSensitive(ctx->delete_button, False);
    }
}

static void
bookmark_manager_entry_activate_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)call_data;
    BookmarkManagerContext *ctx = (BookmarkManagerContext *)client_data;
    if (!ctx || !w) return;
    BookmarkEntry *entry = NULL;
    XtVaGetValues(w, XmNuserData, &entry, NULL);
    if (!entry) return;
    ctx->selected_entry = entry;
    ctx->selected_entry_widget = w;
    Boolean enabled = True;
    if (ctx->open_button) {
        XtSetSensitive(ctx->open_button, enabled);
    }
    if (ctx->edit_button) {
        XtSetSensitive(ctx->edit_button, enabled);
    }
    if (ctx->delete_button) {
        XtSetSensitive(ctx->delete_button, enabled);
    }
}

static void
on_bookmark_menu_activate(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    BookmarkEntry *entry = (BookmarkEntry *)client_data;
    if (!entry || entry->url.empty()) {
        return;
    }
    BrowserTab *tab = get_selected_tab();
    if (!tab) {
        spawn_new_browser_window(entry->url.c_str());
        return;
    }
    set_current_tab(tab);
    load_url_for_tab(tab, entry->url);
}

static void
on_bookmark_dialog_cancel(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    BookmarkDialogContext *ctx = (BookmarkDialogContext *)client_data;
    if (!ctx) return;
    if (ctx->dialog) {
        XtDestroyWidget(ctx->dialog);
    }
    delete ctx;
}

static void
on_bookmark_dialog_save(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    BookmarkDialogContext *ctx = (BookmarkDialogContext *)client_data;
    if (!ctx) return;
    char *name_value = NULL;
    if (ctx->name_field) {
        name_value = XmTextFieldGetString(ctx->name_field);
    }
    std::string bookmark_name = name_value ? name_value : "";
    BrowserTab *tab = get_selected_tab();
    const char *tab_url = display_url_for_tab(tab);
    std::string bookmark_url;
    if (tab_url && tab_url[0]) {
        bookmark_url = normalize_url(tab_url);
        if (bookmark_url.empty()) {
            bookmark_url = tab_url;
        }
    }
    bool add_to_menu = false;
    if (ctx->add_to_menu_checkbox) {
        Boolean set = False;
        XtVaGetValues(ctx->add_to_menu_checkbox, XmNset, &set, NULL);
        add_to_menu = set;
    }
    BookmarkGroup *target_group = ctx->selected_group ? ctx->selected_group :
                                   (g_selected_bookmark_group ? g_selected_bookmark_group : ensure_bookmark_groups());
    if (!target_group && g_bookmark_root) {
        target_group = g_bookmark_root.get();
    }
    BookmarkEntry *entry_ptr = ctx->editing_entry;
    if (entry_ptr) {
        BookmarkGroup *source_group = ctx->editing_group ? ctx->editing_group : target_group;
        if (source_group && target_group && source_group != target_group) {
            auto &entries = source_group->entries;
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (it->get() == entry_ptr) {
                    std::unique_ptr<BookmarkEntry> moved_entry = std::move(*it);
                    entries.erase(it);
                    if (target_group) {
                        target_group->entries.emplace_back(std::move(moved_entry));
                        entry_ptr = target_group->entries.back().get();
                    } else if (g_bookmark_root) {
                        g_bookmark_root->entries.emplace_back(std::move(moved_entry));
                        entry_ptr = g_bookmark_root->entries.back().get();
                    }
                    break;
                }
            }
        }
        if (entry_ptr) {
            entry_ptr->name = bookmark_name;
            entry_ptr->url = bookmark_url;
            entry_ptr->show_in_menu = add_to_menu;
        }
    } else {
        auto entry = std::make_unique<BookmarkEntry>();
        entry->name = bookmark_name;
        entry->url = bookmark_url;
        entry->show_in_menu = add_to_menu;
        entry_ptr = entry.get();
        if (target_group) {
            target_group->entries.emplace_back(std::move(entry));
        } else if (g_bookmark_root) {
            g_bookmark_root->entries.emplace_back(std::move(entry));
        }
    }
    if (!ctx->editing_entry && tab && tab->favicon_url.c_str()) {
        bookmark_entry_copy_icon_from_cache(entry_ptr, tab->favicon_url.c_str());
    }
    const char *log_url = entry_ptr && !entry_ptr->url.empty() ? entry_ptr->url.c_str() : "(none)";
    fprintf(stderr,
            "[ck-browser] Add Bookmark dialog Save name='%s' target='%s' show_in_menu=%d url='%s'\n",
            entry_ptr ? entry_ptr->name.c_str() : "(none)",
            target_group ? target_group->name.c_str() : "(none)",
            entry_ptr && entry_ptr->show_in_menu ? 1 : 0,
            log_url);
    if (name_value) XtFree(name_value);
    if (ctx->dialog) {
        XtDestroyWidget(ctx->dialog);
    }
    delete ctx;
    rebuild_bookmarks_menu_items();
    save_bookmarks_to_file();
}

static void
show_add_bookmark_dialog(BrowserTab *tab, BookmarkEntry *entry, BookmarkGroup *entry_group)
{
    if (!g_toplevel) return;
    BookmarkGroup *root = ensure_bookmark_groups();
    if (!root) return;
    if (!entry && tab) {
        const char *tab_url = display_url_for_tab(tab);
        if (tab_url && tab_url[0]) {
            std::string normalized_url = normalize_url(tab_url);
            if (!normalized_url.empty()) {
                BookmarkGroup *found_group = NULL;
                BookmarkEntry *found_entry = find_bookmark_by_url(root, normalized_url, &found_group);
                if (found_entry) {
                    entry = found_entry;
                    entry_group = found_group;
                }
            }
        }
    }
    BookmarkGroup *initial_group = entry_group ? entry_group :
                                    (g_selected_bookmark_group ? g_selected_bookmark_group : root);
    g_selected_bookmark_group = initial_group;
    BookmarkDialogContext *ctx = new BookmarkDialogContext();
    ctx->editing_entry = entry;
    ctx->editing_group = entry_group;
    const char *dialog_title = entry ? "Edit Bookmark" : "Add Bookmark";
    Widget dialog = XmCreateFormDialog(g_toplevel, xm_name("bookmarkAddDialog"), NULL, 0);
    XmString dialog_title_str = make_string(dialog_title);
    XtVaSetValues(dialog,
                  XmNautoUnmanage, False,
                  XmNmarginWidth, 10,
                  XmNmarginHeight, 10,
                  XmNwidth, 440,
                  XmNheight, 420,
                  XmNtitle, dialog_title,
                  XmNdialogTitle, dialog_title_str,
                  NULL);
    XmStringFree(dialog_title_str);
    ctx->dialog = dialog;

    Widget column = XmCreateRowColumn(dialog, xm_name("bookmarkDialogColumn"), NULL, 0);
    XtVaSetValues(column,
                  XmNorientation, XmVERTICAL,
                  XmNpacking, XmPACK_COLUMN,
                  XmNspacing, 8,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNtopOffset, 4,
                  XmNbottomOffset, 4,
                  XmNleftOffset, 4,
                  XmNrightOffset, 4,
                  NULL);
    XtManageChild(column);

    Widget name_label = XmCreateLabelGadget(column, xm_name("bookmarkNameLabel"), NULL, 0);
    XmString name_label_text = make_string("Name:");
    XtVaSetValues(name_label,
                  XmNlabelString, name_label_text,
                  XmNalignment, XmALIGNMENT_BEGINNING,
                  NULL);
    XmStringFree(name_label_text);
    XtManageChild(name_label);

    Widget name_field = XmCreateTextField(column, xm_name("bookmarkNameField"), NULL, 0);
    XtVaSetValues(name_field,
                  XmNcolumns, 60,
                  XmNeditable, True,
                  XmNresizable, True,
                  XmNvalue, "",
                  NULL);
    XtManageChild(name_field);
    const char *prefill = NULL;
    if (entry) {
        if (!entry->name.empty()) {
            prefill = entry->name.c_str();
        } else if (!entry->url.empty()) {
            prefill = entry->url.c_str();
        }
    } else if (tab) {
        prefill = tab->title_full.empty() ? display_url_for_tab(tab) : tab->title_full.c_str();
    }
    if (prefill) {
        XmTextFieldSetString(name_field, const_cast<char *>(prefill));
    }
    ctx->name_field = name_field;

    XmString target_label = make_string("Target Folder:");
    Widget target_label_widget = XmCreateLabelGadget(column, xm_name("bookmarkTargetLabel"), NULL, 0);
    XtVaSetValues(target_label_widget,
                  XmNlabelString, target_label,
                  XmNalignment, XmALIGNMENT_BEGINNING,
                  NULL);
    XmStringFree(target_label);
    XtManageChild(target_label_widget);

    Widget target_frame = XmCreateFrame(column, xm_name("bookmarkTargetFrame"), NULL, 0);
    XtVaSetValues(target_frame,
                  XmNshadowType, XmSHADOW_ETCHED_IN,
                  XmNshadowThickness, 2,
                  XmNmarginWidth, 2,
                  XmNmarginHeight, 2,
                  XmNresizePolicy, XmRESIZE_ANY,
                  XmNwidth, 420,
                  XmNheight, 220,
                  NULL);
    XtManageChild(target_frame);

    Widget group_list = XmCreateScrolledList(target_frame, xm_name("bookmarkTargetGroupList"), NULL, 0);
    XtVaSetValues(group_list,
                  XmNscrollingPolicy, XmAUTOMATIC,
                  XmNselectionPolicy, XmBROWSE_SELECT,
                  XmNlistSizePolicy, XmCONSTANT,
                  XmNwidth, 420,
                  XmNheight, 220,
                  NULL);
    XtAddCallback(group_list, XmNbrowseSelectionCallback, bookmark_dialog_group_list_selection_cb, ctx);
    XtManageChild(group_list);
    ctx->group_list = group_list;

    Widget bottom_column = XmCreateRowColumn(column, xm_name("bookmarkBottomColumn"), NULL, 0);
    XtVaSetValues(bottom_column,
                  XmNorientation, XmVERTICAL,
                  XmNpacking, XmPACK_TIGHT,
                  XmNspacing, 8,
                  XmNwidth, 420,
                  XmNalignment, XmALIGNMENT_BEGINNING,
                  NULL);
    XtManageChild(bottom_column);

    XmString toggle_label = make_string("Add to Bookmarks Menu");
    Widget add_to_menu_toggle = XtVaCreateManagedWidget(
        "bookmarkDialogAddToMenu",
        xmToggleButtonWidgetClass,
        bottom_column,
        XmNlabelString, toggle_label,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL);
    XmStringFree(toggle_label);
    Boolean toggle_state = False;
    if (entry && entry->show_in_menu) {
        toggle_state = True;
    }
    XmToggleButtonSetState(add_to_menu_toggle, toggle_state, False);
    ctx->add_to_menu_checkbox = add_to_menu_toggle;
    Widget button_row = XmCreateRowColumn(bottom_column, xm_name("bookmarkButtonRow"), NULL, 0);
    XtVaSetValues(button_row,
                  XmNorientation, XmHORIZONTAL,
                  XmNpacking, XmPACK_TIGHT,
                  XmNspacing, 10,
                  XmNalignment, XmALIGNMENT_CENTER,
                  NULL);
    XtManageChild(button_row);

    ctx->group_entries.clear();
    bookmark_dialog_populate_group_list(ctx, group_list, root, 0);
    if (!ctx->group_entries.empty()) {
        int selected_index = 0;
        BookmarkGroup *preferred_group = entry_group ? entry_group : initial_group;
        for (size_t i = 0; i < ctx->group_entries.size(); ++i) {
            if (ctx->group_entries[i] == preferred_group) {
                selected_index = (int)i;
                break;
            }
        }
        ctx->selected_group = ctx->group_entries[selected_index];
        g_selected_bookmark_group = ctx->selected_group;
        XmListSelectPos(group_list, selected_index + 1, False);
    }

    XmString save_label = make_string("Save");
    Widget save_button = XmCreatePushButtonGadget(button_row, xm_name("bookmarkDialogSave"), NULL, 0);
    XmString cancel_label = make_string("Cancel");
    Widget cancel_button = XmCreatePushButtonGadget(button_row, xm_name("bookmarkDialogCancel"), NULL, 0);
    XtVaSetValues(save_button, XmNlabelString, save_label, NULL);
    XtVaSetValues(cancel_button, XmNlabelString, cancel_label, NULL);
    XmStringFree(save_label);
    XmStringFree(cancel_label);
    XtAddCallback(save_button, XmNactivateCallback, on_bookmark_dialog_save, ctx);
    XtAddCallback(cancel_button, XmNactivateCallback, on_bookmark_dialog_cancel, ctx);
    XtManageChild(save_button);
    XtManageChild(cancel_button);

    XtManageChild(dialog);
}

static void on_menu_exit(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    (void)client_data;
    capture_session_state("menu exit");
    if (g_session_data && g_toplevel) {
        session_save(g_toplevel, g_session_data, g_subprocess_path);
    }
    save_last_session_file("menu exit");
    begin_shutdown_sequence("menu exit");
}

static Widget create_menu_bar(Widget parent)
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
    XtVaCreateManagedWidget("fileSep", xmSeparatorGadgetClass, file_menu, NULL);
    Widget file_exit = create_menu_item(file_menu, "fileExit", "Exit");

    Widget nav_menu = create_cascade_menu(menu_bar, "Navigate", "navigateMenu", 'N');
    Widget nav_back = create_menu_item(nav_menu, "navBack", "Go Back");
    Widget nav_forward = create_menu_item(nav_menu, "navForward", "Go Forward");
    Widget nav_reload = create_menu_item(nav_menu, "navReload", "Reload Page");
    Widget nav_open_url = create_menu_item(nav_menu, "navOpenUrl", "Open URL");
    g_nav_back = nav_back;
    g_nav_forward = nav_forward;

    Widget bookmarks_menu =
        create_cascade_menu(menu_bar, "Bookmarks", "bookmarksMenu", 'B');
    g_bookmarks_menu = bookmarks_menu;
    Widget bookmark_add = create_menu_item(bookmarks_menu, "bookmarkAdd", "Add Page...");
    Widget bookmark_open_manager =
        create_menu_item(bookmarks_menu, "bookmarkOpenManager", "Open Bookmark Manager...");
    XtVaCreateManagedWidget("bookmarkFavoritesSep",
                            xmSeparatorGadgetClass, bookmarks_menu, NULL);
    XtVaSetValues(bookmark_add, XmNmnemonic, 'A', NULL);
    XtVaSetValues(bookmark_open_manager, XmNmnemonic, 'O', NULL);
    XtAddCallback(bookmark_add, XmNactivateCallback, on_add_bookmark_menu, NULL);
    XtAddCallback(bookmark_open_manager, XmNactivateCallback,
                  on_open_bookmark_manager_menu, NULL);

    Widget view_menu = create_cascade_menu(menu_bar, "View", "viewMenu", 'V');
    Widget view_zoom_in = create_menu_item(view_menu, "viewZoomIn", "Zoom In");
    Widget view_zoom_out = create_menu_item(view_menu, "viewZoomOut", "Zoom Out");
    Widget view_zoom_reset = create_menu_item(view_menu, "viewZoomReset", "Reset Zoom");
    set_menu_accelerator(view_zoom_in, "Ctrl<Key>plus", "Ctrl+Plus");
    set_menu_accelerator(view_zoom_out, "Ctrl<Key>minus", "Ctrl+Minus");
    set_menu_accelerator(view_zoom_reset, "Ctrl<Key>0", "Ctrl+0");
    XtVaCreateManagedWidget("viewSep1", xmSeparatorGadgetClass, view_menu, NULL);
    Widget view_restore = create_menu_item(view_menu, "viewRestoreSession", "Restore Session");

    Widget help_menu = XmCreatePulldownMenu(menu_bar, xm_name("helpMenu"), NULL, 0);
    XmString help_label = make_string("Help");
    Widget help_cascade = XtVaCreateManagedWidget(
        "helpCascade",
        xmCascadeButtonGadgetClass, menu_bar,
        XmNlabelString, help_label,
        XmNmnemonic, 'H',
        XmNsubMenuId, help_menu,
        NULL);
    XmStringFree(help_label);
    XtVaSetValues(menu_bar, XmNmenuHelpWidget, help_cascade, NULL);

    Widget help_view = create_menu_item(help_menu, "helpView", "View Help");
    Widget help_about = create_menu_item(help_menu, "helpAbout", "About");

    XtVaSetValues(file_new_window, XmNmnemonic, 'N', NULL);
    XtVaSetValues(file_new_tab, XmNmnemonic, 'T', NULL);
    XtVaSetValues(file_close_tab, XmNmnemonic, 'C', NULL);
    XtVaSetValues(file_exit, XmNmnemonic, 'X', NULL);

    set_menu_accelerator(file_new_window, "Ctrl<Key>N", "Ctrl+N");
    set_menu_accelerator(file_new_tab, "Ctrl<Key>T", "Ctrl+T");
    set_menu_accelerator(file_close_tab, "Ctrl<Key>W", "Ctrl+W");
    set_menu_accelerator(file_exit, "Alt<Key>F4", "Alt+F4");

    XtVaSetValues(nav_back, XmNmnemonic, 'B', NULL);
    XtVaSetValues(nav_forward, XmNmnemonic, 'F', NULL);
    XtVaSetValues(nav_reload, XmNmnemonic, 'R', NULL);
    XtVaSetValues(nav_open_url, XmNmnemonic, 'O', NULL);
    set_menu_accelerator(nav_back, "Alt<Key>osfLeft", "Alt+Left");
    set_menu_accelerator(nav_forward, "Alt<Key>osfRight", "Alt+Right");
    set_menu_accelerator(nav_reload, "Ctrl<Key>R", "Ctrl+R");
    set_menu_accelerator(nav_open_url, "Ctrl<Key>L", "Ctrl+L");

    XtAddCallback(file_new_window, XmNactivateCallback, on_new_window, NULL);
    XtAddCallback(file_new_tab, XmNactivateCallback, on_new_tab, NULL);
    XtAddCallback(file_close_tab, XmNactivateCallback, on_close_tab, NULL);
    XtAddCallback(nav_open_url, XmNactivateCallback, on_enter_url, NULL);
    XtAddCallback(file_exit, XmNactivateCallback, on_menu_exit, g_app);
    XtAddCallback(nav_back, XmNactivateCallback, on_go_back_menu, NULL);
    XtAddCallback(nav_forward, XmNactivateCallback, on_go_forward_menu, NULL);
    XtAddCallback(nav_reload, XmNactivateCallback, on_reload_menu, NULL);
    XtAddCallback(view_zoom_in, XmNactivateCallback, on_zoom_in, NULL);
    XtAddCallback(view_zoom_out, XmNactivateCallback, on_zoom_out, NULL);
    XtAddCallback(view_zoom_reset, XmNactivateCallback, on_zoom_reset, NULL);
    XtAddCallback(view_restore, XmNactivateCallback, on_restore_session, NULL);
    XtAddCallback(help_view, XmNactivateCallback, on_help_view, NULL);
    XtAddCallback(help_about, XmNactivateCallback, on_help_about, NULL);

    rebuild_bookmarks_menu_items();

    return menu_bar;
}

static Widget create_toolbar(Widget parent, Widget attach_top)
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
    g_back_button = back_button;

    XmString forward_label = make_string("Forward");
    Widget forward_button = XtVaCreateManagedWidget("forwardButton", xmPushButtonWidgetClass, button_row,
                            XmNlabelString, forward_label, NULL);
    XmStringFree(forward_label);
    XtAddCallback(forward_button, XmNactivateCallback, on_forward, NULL);
    g_forward_button = forward_button;

    XmString reload_label = make_string("Reload");
    Widget reload_button = XtVaCreateManagedWidget("reloadButton", xmPushButtonWidgetClass, button_row,
                            XmNlabelString, reload_label, NULL);
    XmStringFree(reload_label);
    XtAddCallback(reload_button, XmNactivateCallback, on_reload, NULL);

    XmString home_label = make_string("Home");
    Widget home_button = XtVaCreateManagedWidget("homeButton", xmPushButtonWidgetClass, button_row,
                            XmNlabelString, home_label, NULL);
    XmStringFree(home_label);
    XtAddCallback(home_button, XmNactivateCallback, on_home, NULL);
    XtAddEventHandler(home_button, ButtonPressMask, False, on_home_button_press, NULL);
    g_home_button = home_button;

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
    g_favicon_label = favicon;

    Widget url_field = XtVaCreateManagedWidget(
        "urlField",
        xmTextFieldWidgetClass, toolbar,
        XmNvalue, (g_homepage_url.empty() ? kInitialBrowserUrl : g_homepage_url.c_str()),
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
    g_url_field = url_field;
    XmProcessTraversal(url_field, XmTRAVERSE_CURRENT);

    return toolbar;
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

    XmString xm_text = make_string(text);
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

    XmString minus_label = make_string("-");
    Widget minus_btn = XtVaCreateManagedWidget("zoomMinus", xmPushButtonGadgetClass, row,
                                               XmNlabelString, minus_label,
                                               XmNmarginWidth, 4,
                                               XmNmarginHeight, 0,
                                               NULL);
    XmStringFree(minus_label);
    XtAddCallback(minus_btn, XmNactivateCallback, on_zoom_out, NULL);
    g_zoom_minus_button = minus_btn;

    XmString zoom_label = make_string("Zoom: 100%");
    Widget zoom_btn = XtVaCreateManagedWidget("zoomReset", xmPushButtonGadgetClass, row,
                                              XmNlabelString, zoom_label,
                                              XmNshadowThickness, 0,
                                              XmNmarginWidth, 4,
                                              XmNmarginHeight, 0,
                                              NULL);
    XmStringFree(zoom_label);
    XtAddCallback(zoom_btn, XmNactivateCallback, on_zoom_reset, NULL);
    g_zoom_label = zoom_btn;

    XmString plus_label = make_string("+");
    Widget plus_btn = XtVaCreateManagedWidget("zoomPlus", xmPushButtonGadgetClass, row,
                                              XmNlabelString, plus_label,
                                              XmNmarginWidth, 4,
                                              XmNmarginHeight, 0,
                                              NULL);
    XmStringFree(plus_label);
    XtAddCallback(plus_btn, XmNactivateCallback, on_zoom_in, NULL);
    g_zoom_plus_button = plus_btn;

    return frame;
}

static Widget create_status_bar(Widget parent)
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

    Widget status_left = create_status_segment(status_form, "statusMain", "", &g_status_message_label);
    Widget status_center = create_status_segment(status_form, "statusSecurity", "Security: None", &g_security_label);
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

static char *xm_name(const char *name)
{
    return const_cast<char *>(name ? name : "");
}

static bool build_path_from_dir(const char *dir, const char *suffix, char *buffer, size_t buffer_len)
{
    if (!buffer || buffer_len == 0) return false;
    buffer[0] = '\0';
    if (!dir || dir[0] == '\0') return false;
    size_t dir_len = strnlen(dir, PATH_MAX);
    if (suffix && suffix[0]) {
        size_t suffix_len = strlen(suffix);
        if (dir_len + 1 + suffix_len + 1 > buffer_len) {
            return false;
        }
        memcpy(buffer, dir, dir_len);
        buffer[dir_len] = '/';
        memcpy(buffer + dir_len + 1, suffix, suffix_len);
        buffer[dir_len + 1 + suffix_len] = '\0';
    } else {
        if (dir_len + 1 > buffer_len) {
            return false;
        }
        memcpy(buffer, dir, dir_len);
        buffer[dir_len] = '\0';
    }
    return true;
}

static void build_cwd_path(char *buffer, size_t buffer_len, const char *suffix)
{
    if (!buffer || buffer_len == 0) return;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        buffer[0] = '\0';
        return;
    }
    if (!build_path_from_dir(cwd, suffix, buffer, buffer_len)) {
        buffer[0] = '\0';
    }
}

static int dir_has_files(const char *path)
{
    if (!path || path[0] == '\0') return 0;
    DIR *dir = opendir(path);
    if (!dir) return 0;
    struct dirent *entry = NULL;
    int has_files = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        has_files = 1;
        break;
    }
    closedir(dir);
    return has_files;
}

static int file_exists(const char *path)
{
    if (!path || path[0] == '\0') return 0;
    return access(path, R_OK) == 0;
}

static void report_cef_resource_status(const char *resources_path, const char *locales_path)
{
    const char *resource_dir = resources_path ? resources_path : "(null)";
    const char *locales_dir = locales_path ? locales_path : "(null)";
    fprintf(stderr, "[ck-browser] CEF resources_dir_path=%s\n", resource_dir);
    fprintf(stderr, "[ck-browser] CEF locales_dir_path=%s\n", locales_dir);
    if (resources_path && resources_path[0]) {
        char icu_path[PATH_MAX];
        size_t res_len = strnlen(resources_path, sizeof(icu_path));
        const char *suffix = "/icudtl.dat";
        size_t suffix_len = strlen(suffix);
        if (res_len + suffix_len + 1 <= sizeof(icu_path)) {
            memcpy(icu_path, resources_path, res_len);
            memcpy(icu_path + res_len, suffix, suffix_len);
            icu_path[res_len + suffix_len] = '\0';
        } else {
            icu_path[0] = '\0';
        }
        if (!icu_path[0] || !file_exists(icu_path)) {
            fprintf(stderr, "[ck-browser] Missing ICU data file: %s\n", icu_path);
        } else {
            int fd = open(icu_path, O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "[ck-browser] Unable to open ICU data file: %s\n", icu_path);
            } else {
                close(fd);
            }
        }
    }
}

static void get_exe_path(char *buffer, size_t buffer_len)
{
    if (!buffer || buffer_len == 0) return;
    ssize_t len = readlink("/proc/self/exe", buffer, buffer_len - 1);
    if (len < 0) {
        buffer[0] = '\0';
        return;
    }
    buffer[len] = '\0';
}

static void dump_cef_env_and_args(int argc, char *argv[])
{
    fprintf(stderr, "[ck-browser] argv:\n");
    for (int i = 0; i < argc; ++i) {
        fprintf(stderr, "  argv[%d]=%s\n", i, argv[i] ? argv[i] : "(null)");
    }
    const char *envs[] = {
        "ICU_DATA",
        "ICU_DATA_FILE",
        "CHROME_VERSION_EXTRA",
        "LD_LIBRARY_PATH",
        "CEF_RESOURCE_PATH",
        "CEF_LOCALES_PATH",
        NULL
    };
    fprintf(stderr, "[ck-browser] env:\n");
    for (const char **env = envs; *env; ++env) {
        const char *val = getenv(*env);
        fprintf(stderr, "  %s=%s\n", *env, val ? val : "(unset)");
    }
}

static bool find_existing_path(char *buffer, size_t buffer_len, const char *suffix)
{
    if (!buffer || buffer_len == 0 || !suffix || suffix[0] == '\0') return false;
    buffer[0] = '\0';
    char candidate[PATH_MAX];
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        if (build_path_from_dir(cwd, suffix, candidate, sizeof(candidate)) &&
            dir_has_files(candidate)) {
            strncpy(buffer, candidate, buffer_len);
            buffer[buffer_len - 1] = '\0';
            return true;
        }
    }
    char exe_path[PATH_MAX];
    get_exe_path(exe_path, sizeof(exe_path));
    if (exe_path[0] != '\0') {
        std::filesystem::path parent = std::filesystem::path(exe_path).parent_path();
        std::filesystem::path previous;
        while (!parent.empty() && parent != previous) {
            std::string base = parent.string();
            if (build_path_from_dir(base.c_str(), suffix, candidate, sizeof(candidate)) &&
                dir_has_files(candidate)) {
                strncpy(buffer, candidate, buffer_len);
                buffer[buffer_len - 1] = '\0';
                return true;
            }
            previous = parent;
            parent = parent.parent_path();
        }
    }
    return false;
}

static bool parse_startup_url_arg(int argc, char *argv[], std::string *out_url)
{
    if (!out_url) return false;
    out_url->clear();
    const char *prefix = "--ck-open-url=";
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!arg) continue;
        if (strncmp(arg, prefix, strlen(prefix)) == 0) {
            *out_url = std::string(arg + strlen(prefix));
            break;
        }
        if (strcmp(arg, "--ck-open-url") == 0 && i + 1 < argc && argv[i + 1]) {
            *out_url = std::string(argv[i + 1]);
            break;
        }
    }
    if (out_url->empty()) return false;
    *out_url = normalize_url(out_url->c_str());
    return !out_url->empty();
}

static bool parse_cache_suffix_arg(int argc, char *argv[], std::string *out_suffix)
{
    if (!out_suffix) return false;
    out_suffix->clear();
    const char *prefix = "--ck-cache-suffix=";
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!arg) continue;
        if (strncmp(arg, prefix, strlen(prefix)) == 0) {
            *out_suffix = std::string(arg + strlen(prefix));
            break;
        }
        if (strcmp(arg, "--ck-cache-suffix") == 0 && i + 1 < argc && argv[i + 1]) {
            *out_suffix = std::string(argv[i + 1]);
            break;
        }
    }
    if (out_suffix->empty()) return false;
    for (char &c : *out_suffix) {
        bool ok = ((c >= '0' && c <= '9') ||
                   (c >= 'A' && c <= 'Z') ||
                   (c >= 'a' && c <= 'z') ||
                   c == '-' ||
                   c == '_');
        if (!ok) c = '_';
    }
    return true;
}

static void build_cef_argv(int argc, char *argv[], std::vector<char *> *out_argv)
{
    if (!out_argv) return;
    out_argv->clear();
    if (argc <= 0 || !argv) return;
    out_argv->reserve((size_t)argc);
    out_argv->push_back(argv[0]);

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!arg) continue;

        if (strcmp(arg, "-session") == 0) {
            if (i + 1 < argc) i++;
            continue;
        }

        const char *open_url_prefix = "--ck-open-url=";
        if (strncmp(arg, open_url_prefix, strlen(open_url_prefix)) == 0) {
            continue;
        }
        if (strcmp(arg, "--ck-open-url") == 0) {
            if (i + 1 < argc) i++;
            continue;
        }

        const char *cache_prefix = "--ck-cache-suffix=";
        if (strncmp(arg, cache_prefix, strlen(cache_prefix)) == 0) {
            continue;
        }
        if (strcmp(arg, "--ck-cache-suffix") == 0) {
            if (i + 1 < argc) i++;
            continue;
        }

        out_argv->push_back(argv[i]);
    }
}

int main(int argc, char *argv[])
{
    g_homepage_url = load_homepage_file();
    if (g_homepage_url.empty()) {
        g_homepage_url = kInitialBrowserUrl;
    }
    fprintf(stderr, "[ck-browser] homepage URL=%s\n",
            g_homepage_url.empty() ? "(empty)" : g_homepage_url.c_str());

    if (!find_existing_path(g_resources_path, sizeof(g_resources_path),
                            "third_party/cef/resources")) {
        build_cwd_path(g_resources_path, sizeof(g_resources_path), "third_party/cef/resources");
    }
    if (!find_existing_path(g_locales_path, sizeof(g_locales_path),
                            "third_party/cef/locales")) {
        build_cwd_path(g_locales_path, sizeof(g_locales_path), "third_party/cef/locales");
    }
    if (!dir_has_files(g_locales_path)) {
        if (!find_existing_path(g_locales_path, sizeof(g_locales_path),
                                "third_party/cef/resources/locales")) {
            build_cwd_path(g_locales_path, sizeof(g_locales_path),
                           "third_party/cef/resources/locales");
        }
    }
    get_exe_path(g_subprocess_path, sizeof(g_subprocess_path));
    fprintf(stderr, "[ck-browser] resource_path=%s locales_path=%s subprocess=%s\n",
            g_resources_path[0] ? g_resources_path : "(none)",
            g_locales_path[0] ? g_locales_path : "(none)",
            g_subprocess_path[0] ? g_subprocess_path : "(none)");

    g_force_disable_gpu = !has_opengl_support();
    if (g_force_disable_gpu) {
        fprintf(stderr,
                "[ck-browser] OpenGL stack missing (libGL), forcing --disable-gpu + software fallback\n");
    } else {
        fprintf(stderr, "[ck-browser] OpenGL stack present, GPU acceleration enabled\n");
    }
    apply_gpu_switches();

    char *session_id = session_parse_argument(&argc, argv);
    g_session_data = session_data_create(session_id);
    free(session_id);

    if (!g_cef_app) {
        g_cef_app = new CkCefApp();
    }

    std::vector<char *> cef_argv;
    build_cef_argv(argc, argv, &cef_argv);
    fprintf(stderr, "[ck-browser] cef args:");
    for (int i = 0; i < (int)cef_argv.size(); ++i) {
        fprintf(stderr, " %s", cef_argv[i] ? cef_argv[i] : "(null)");
    }
    fprintf(stderr, "\n");
    CefMainArgs main_args((int)cef_argv.size(), cef_argv.data());
    if (g_force_disable_gpu) {
        apply_gpu_switches();
    }
    fprintf(stderr, "[ck-browser] calling cef_execute_process\n");
    int exit_code = CefExecuteProcess(main_args, g_cef_app, nullptr);
    fprintf(stderr, "[ck-browser] cef_execute_process result=%d\n", exit_code);
    if (exit_code >= 0) {
        return exit_code;
    }

    std::string startup_url;
    bool has_startup_url = parse_startup_url_arg(argc, argv, &startup_url);
    std::string cache_suffix;
    (void)parse_cache_suffix_arg(argc, argv, &cache_suffix);
    if (cache_suffix.empty() && has_startup_url) {
        cache_suffix = std::to_string((long)getpid());
    }

    XtAppContext app;
    Widget toplevel = XtVaAppInitialize(&app, "CkBrowser", NULL, 0,
                                        &argc, argv, NULL, NULL);
    fprintf(stderr, "[ck-browser] XtAppInitialize returned with toplevel=%p\n", (void *)toplevel);
    g_app = app;
    g_toplevel = toplevel;
    DtInitialize(XtDisplay(toplevel), toplevel, xm_name("CkBrowser"), xm_name("CkBrowser"));
    XtVaSetValues(toplevel,
                  XmNtitle, "Internet Browser",
                  XmNiconName, "Internet Browser",
                  XmNwidth, 1280,
                  XmNheight, 900,
                  NULL);

    Widget main_form = XmCreateForm(toplevel, xm_name("browserMainForm"), NULL, 0);
    XtVaSetValues(main_form,
                  XmNmarginWidth, 0,
                  XmNmarginHeight, 0,
                  XmNfractionBase, 100,
                  NULL);
    XtManageChild(main_form);

    Widget menu_bar = create_menu_bar(main_form);
    Widget toolbar = create_toolbar(main_form, menu_bar);
    Widget status_bar = create_status_bar(main_form);

    Widget tab_stack = XmCreateTabStack(main_form, xm_name("browserTabStack"), NULL, 0);
    XtVaSetValues(tab_stack,
                  XmNtopAttachment, XmATTACH_WIDGET,
                  XmNtopWidget, toolbar,
                  XmNbottomAttachment, XmATTACH_WIDGET,
                  XmNbottomWidget, status_bar,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNleftOffset, 16,
                  XmNrightOffset, 16,
                  XmNtopOffset, 16,
                  XmNbottomOffset, 16,
                  XmNmarginWidth, 0,
                  XmNmarginHeight, 0,
                  NULL);
    XtManageChild(tab_stack);
    g_tab_stack = tab_stack;
    // Register tab selection handling after realize; some Motif builds don't
    // create callback lists/widgets until then.
    XtAddCallback(tab_stack, XmNresizeCallback, on_tab_stack_resize, NULL);
    // We still add a handler on the TabStack (best-effort) but finalize wiring in attach_tab_handlers_cb().
    long tab_mask = ButtonPressMask | ButtonReleaseMask | KeyPressMask | KeyReleaseMask;
    XtInsertEventHandler(tab_stack, tab_mask, False, on_tabbox_input, NULL, XtListHead);

    if (g_session_data) {
        g_session_loaded = session_load(toplevel, g_session_data) ? true : false;
        if (g_session_loaded) {
            fprintf(stderr, "[ck-browser] session restored (Dt) id=%s\n",
                    g_session_data->session_id ? g_session_data->session_id : "(null)");
        }
    }

    if (g_session_loaded && g_session_data && session_data_get_int(g_session_data, "tab_count", 0) > 0) {
        restore_tabs_from_session_data(g_session_data);
    } else {
        const char *first_url = has_startup_url ? startup_url.c_str()
                                                : (g_homepage_url.empty() ? kInitialBrowserUrl : g_homepage_url.c_str());
        BrowserTab *tab_home = create_tab_page(tab_stack, "tabWelcome", "Welcome", "Welcome", first_url);
        schedule_tab_browser_creation(tab_home);
        XmTabStackSelectTab(tab_home->page, False);
        set_current_tab(tab_home);
    }

    Atom wm_delete = XmInternAtom(XtDisplay(toplevel), xm_name("WM_DELETE_WINDOW"), False);
    XmAddWMProtocolCallback(toplevel, wm_delete, wm_delete_cb, (XtPointer)app);
    XmActivateWMProtocol(toplevel, wm_delete);
    Atom wm_save = XmInternAtom(XtDisplay(toplevel), xm_name("WM_SAVE_YOURSELF"), False);
    XmAddWMProtocolCallback(toplevel, wm_save, wm_save_yourself_cb, NULL);
    XmActivateWMProtocol(toplevel, wm_save);
    XtAddCallback(toplevel, XmNresizeCallback, on_main_window_resize, NULL);

    XtRealizeWidget(toplevel);
    fprintf(stderr, "[ck-browser] XtRealizeWidget completed\n");
    XtAppAddTimeOut(app, 0, attach_tab_handlers_cb, NULL);
    XtAppAddTimeOut(app, 1000, bookmark_file_monitor_timer_cb, NULL);

    if (g_session_loaded && g_session_data) {
        session_apply_geometry(toplevel, g_session_data, "x", "y", "w", "h");
    }
    CefSettings settings;
    settings.no_sandbox = 1;
    settings.external_message_pump = 1;
    settings.command_line_args_disabled = 1;
    settings.log_severity = LOGSEVERITY_VERBOSE;
    char log_path[PATH_MAX];
    build_cwd_path(log_path, sizeof(log_path), "build/ck-browser-cef.log");
    if (log_path[0] != '\0') {
        CefString(&settings.log_file) = log_path;
    }
    if (g_resources_path[0] != '\0') {
        CefString(&settings.resources_dir_path) = g_resources_path;
    }
    const char *selected_locales = NULL;
    if (g_locales_path[0] != '\0' && dir_has_files(g_locales_path)) {
        selected_locales = g_locales_path;
        CefString(&settings.locales_dir_path) = g_locales_path;
    }
    if (g_subprocess_path[0] != '\0') {
        CefString(&settings.browser_subprocess_path) = g_subprocess_path;
    }
    char cache_base[PATH_MAX];
    build_cwd_path(cache_base, sizeof(cache_base), "build/ck-browser-cache");
    char cache_path[PATH_MAX];
    if (cache_base[0] == '\0') {
        cache_path[0] = '\0';
    } else if (!cache_suffix.empty()) {
        size_t base_len = strnlen(cache_base, sizeof(cache_base));
        size_t suffix_len = cache_suffix.size();
        if (base_len + 1 + suffix_len + 1 <= sizeof(cache_path)) {
            memcpy(cache_path, cache_base, base_len);
            cache_path[base_len] = '-';
            memcpy(cache_path + base_len + 1, cache_suffix.c_str(), suffix_len);
            cache_path[base_len + 1 + suffix_len] = '\0';
        } else {
            strncpy(cache_path, cache_base, sizeof(cache_path));
            cache_path[sizeof(cache_path) - 1] = '\0';
        }
    } else {
        strncpy(cache_path, cache_base, sizeof(cache_path));
        cache_path[sizeof(cache_path) - 1] = '\0';
    }
    if (cache_path[0] != '\0') {
        mkdir(cache_path, 0755);
        CefString(&settings.root_cache_path) = cache_path;
        fprintf(stderr, "[ck-browser] root_cache_path=%s\n", cache_path);
    }
    report_cef_resource_status(g_resources_path, selected_locales ? selected_locales : "");
    dump_cef_env_and_args(main_args.argc, main_args.argv);
    fprintf(stderr, "[ck-browser] calling CefInitialize\n");
    bool cef_ok = CefInitialize(main_args, settings, g_cef_app, nullptr);
    fprintf(stderr, "[ck-browser] CefInitialize result=%d\n", cef_ok);
    if (!cef_ok) {
        return 1;
    }
    g_cef_initialized = true;
    fprintf(stderr, "[ck-browser] CEF initialized, refreshing bookmark icons\n");
    rebuild_bookmarks_menu_items();

    XtAppMainLoop(app);
    LOG_ENTER("main loop finished, beginning shutdown");
    save_last_session_file("main loop exit");
    LOG_ENTER("saved session; shutdown requested=%d pending=%d",
              g_shutdown_requested ? 1 : 0,
              g_shutdown_pending_browsers);
    g_current_tab = NULL;
    LOG_ENTER("freeing session data %p", (void *)g_session_data);
    session_data_free(g_session_data);
    g_session_data = NULL;
    for (const auto &tab : g_browser_tabs) {
        detach_tab_clients(tab.get());
    }
    g_browser_tabs.clear();
    LOG_ENTER("about to call CefShutdown");
    CefShutdown();
    return 0;
}
