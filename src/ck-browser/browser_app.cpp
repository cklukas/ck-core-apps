#include "browser_app.h"
#include "browser_tab.h"
#include "browser_ui_bridge.h"

#include <cstdio>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>

#include <cstring>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <functional>
#include <include/cef_app.h>
#include <include/cef_render_process_handler.h>
#include <include/cef_browser.h>
#include <include/cef_client.h>
#include <include/cef_context_menu_handler.h>
#include <include/cef_display_handler.h>
#include <include/cef_focus_handler.h>
#include <include/cef_frame.h>
#include <include/cef_life_span_handler.h>
#include <include/cef_load_handler.h>
#include <include/cef_menu_model.h>
#include <include/cef_popup_features.h>
#include <include/cef_process_message.h>
#include <include/cef_v8.h>
#include <include/cef_dictionary_value.h>
#include <include/cef_list_value.h>
#include <include/cef_request.h>
#include <include/cef_request_handler.h>
#include <include/cef_command_line.h>
#include <include/cef_string.h>
extern "C" {
#include "../shared/session_utils.h"
}

extern std::string load_homepage_file();
extern bool parse_startup_url_arg(int argc, char *argv[], std::string *out_url);

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


int main(int argc, char *argv[])
{
    return BrowserApp::instance().run(argc, argv);
}

BrowserApp &BrowserApp::instance()
{
    static BrowserApp app;
    return app;
}

int BrowserApp::run(int argc, char *argv[])
{
    return run_main(argc, argv);
}

int BrowserApp::run_main(int argc, char *argv[])
{
    return start_ui_and_cef_loop(argc, argv, *this);
}

static void log_popup_features(const CefPopupFeatures &features)
{
    fprintf(stderr,
            "[ck-browser] popup features x=%d(xSet=%d) y=%d(ySet=%d) "
            "w=%d(wSet=%d) h=%d(hSet=%d) isPopup=%d\n",
            features.x,
            features.xSet,
            features.y,
            features.ySet,
            features.width,
            features.widthSet,
            features.height,
            features.heightSet,
            features.isPopup);
}

bool route_url_through_ck_browser(CefRefPtr<CefBrowser> browser,
                                  const std::string &url,
                                  bool allow_existing_tab)
{
    if (url.empty()) return false;
    std::string normalized = normalize_url(url.c_str());
    if (normalized.empty()) return false;
    fprintf(stderr,
            "[ck-browser] route_url_through_ck_browser url=%s normalized=%s allow_existing=%d\n",
            url.c_str(),
            normalized.c_str(),
            allow_existing_tab ? 1 : 0);
    if (browser) {
        CefRefPtr<CefBrowserHost> host = browser->GetHost();
        if (host) {
            host->SetFocus(true);
        }
    }
    if (allow_existing_tab) {
        BrowserTab *tab = get_selected_tab();
        if (tab && tab->browser) {
            load_url_for_tab(tab, normalized);
            select_tab_page(tab);
            set_current_tab(tab);
            return true;
        }
    }
    open_url_in_new_tab(normalized, true);
    return true;
}

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
    tab_->theme_color_retry_count = 0;
    tab_->theme_color_ready_retry_count = 0;
    if (tab_ == get_current_tab()) {
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
    bool load_in_current = (target_disposition == CEF_WOD_CURRENT_TAB ||
                            target_disposition == CEF_WOD_SWITCH_TO_TAB ||
                            target_disposition == CEF_WOD_SINGLETON_TAB);
    return route_url_through_ck_browser(browser, url, load_in_current);
  }

  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                      CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request,
                      bool user_gesture,
                      bool is_redirect) override {
    CEF_REQUIRE_UI_THREAD();
    (void)user_gesture;
    (void)is_redirect;
    if (!frame || frame->IsMain() || !request) return false;
    std::string url = request->GetURL();
#if defined(TRANSITION_AUTO_SUBFRAME) && defined(TRANSITION_MANUAL_SUBFRAME)
    int transition = request->GetTransitionType();
    const int kSubframeMask = TRANSITION_AUTO_SUBFRAME | TRANSITION_MANUAL_SUBFRAME;
    if (transition & kSubframeMask) return false;
#endif
    if (url.empty()) return false;
    std::string frame_name = frame->GetName();
    fprintf(stderr,
            "[ck-browser] OnBeforeBrowse intercepted subframe name=%s method=%s url=%s\n",
            frame_name.empty() ? "(frame)" : frame_name.c_str(),
            request->GetMethod().ToString().c_str(),
            url.c_str());
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
    log_popup_features(popupFeatures);
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

    bool is_small_popup = (popupFeatures.widthSet && popupFeatures.heightSet &&
                           popupFeatures.width > 0 && popupFeatures.height > 0 &&
                           popupFeatures.width <= 640 && popupFeatures.height <= 480);
    bool prefer_native_popup = popupFeatures.isPopup || is_small_popup;
    if (prefer_native_popup) {
      fprintf(stderr,
              "[ck-browser] OnBeforePopup letting native popup (native=%d width=%d height=%d)\n",
              prefer_native_popup ? 1 : 0,
              popupFeatures.width,
              popupFeatures.height);
      return false;
    }
    if (target_disposition == CEF_WOD_NEW_WINDOW ||
        target_disposition == CEF_WOD_OFF_THE_RECORD ||
        target_disposition == CEF_WOD_NEW_POPUP ||
        target_disposition == CEF_WOD_NEW_FOREGROUND_TAB ||
        target_disposition == CEF_WOD_NEW_BACKGROUND_TAB) {
      return route_url_through_ck_browser(browser, url, false);
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
    if (!args || args->GetSize() < 6) return true;
    fprintf(stderr,
            "[ck-browser] theme color args size=%zu types=%d,%d,%d,%d,%d,%d\n",
            args->GetSize(),
            args->GetType(0),
            args->GetType(1),
            args->GetType(2),
            args->GetType(3),
            args->GetType(4),
            args->GetType(5));
    fprintf(stderr,
            "[ck-browser] theme color raw values r=%d g=%d b=%d source='%s' raw='%s' ready_raw='%s'\n",
            args->GetInt(0),
            args->GetInt(1),
            args->GetInt(2),
            args->GetString(3).ToString().c_str(),
            args->GetString(4).ToString().c_str(),
            args->GetString(5).ToString().c_str());
    int r = args->GetInt(0);
    int g = args->GetInt(1);
    int b = args->GetInt(2);
    std::string source;
    CefString source_val = args->GetString(3);
    if (!source_val.empty()) {
        source = source_val.ToString();
    }
    std::string raw_color;
    CefString raw_val = args->GetString(4);
    if (!raw_val.empty()) {
        raw_color = raw_val.ToString();
    }
    std::string ready_state;
    CefString ready_val = args->GetString(5);
    if (!ready_val.empty()) {
        ready_state = ready_val.ToString();
    }
    bool ready_complete = (ready_state == "complete" || ready_state == "interactive");
    std::string frame_url = frame ? frame->GetURL().ToString() : std::string();
    if (frame_url.empty()) frame_url = "(none)";
    fprintf(stderr,
            "[ck-browser] theme color tab=%s (%p) frame=%s url=%s rgb=%d,%d,%d source=%s raw='%s' readyState=%s\n",
            tab_->base_title.empty() ? "Tab" : tab_->base_title.c_str(),
            (void *)tab_,
            frame_url.c_str(),
            tab_->pending_url.empty() ? "(none)" : tab_->pending_url.c_str(),
            r, g, b,
            source.empty() ? "unknown" : source.c_str(),
            raw_color.empty() ? "(empty)" : raw_color.c_str(),
            ready_state.empty() ? "unknown" : ready_state.c_str());
    if (!ready_complete) {
      if (tab_->theme_color_ready_retry_count < kThemeColorReadyRetryLimit) {
        schedule_theme_color_request(tab_, 250);
        tab_->theme_color_ready_retry_count++;
        fprintf(stderr,
                "[ck-browser] theme color readyState not complete, retry %d for tab=%s\n",
                tab_->theme_color_ready_retry_count,
                tab_->base_title.empty() ? "Tab" : tab_->base_title.c_str());
      } else {
        fprintf(stderr,
                "[ck-browser] theme color readyState retry limit reached for tab=%s\n",
                tab_->base_title.empty() ? "Tab" : tab_->base_title.c_str());
      }
      return true;
    }
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
    if (tab_ == get_current_tab()) {
      apply_tab_theme_colors(tab_, true);
    }
    tab_->theme_color_ready_retry_count = 0;
    bool fallback = (source.empty() || source == "fallback" || raw_color.empty() || raw_color == "#ffffff");
    if (fallback && tab_) {
      if (tab_->theme_color_retry_count < kThemeColorRetryLimit) {
        schedule_theme_color_request(tab_, 250);
        tab_->theme_color_retry_count++;
        fprintf(stderr,
                "[ck-browser] theme color fallback detected, retry %d for tab=%s\n",
                tab_->theme_color_retry_count,
                tab_->base_title.empty() ? "Tab" : tab_->base_title.c_str());
      } else {
        fprintf(stderr,
                "[ck-browser] theme color fallback retry limit reached for tab=%s\n",
                tab_->base_title.empty() ? "Tab" : tab_->base_title.c_str());
      }
    } else if (!fallback && tab_) {
      tab_->theme_color_retry_count = 0;
      tab_->theme_color_ready_retry_count = 0;
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
    BrowserTab *current = get_current_tab();
    if (tab_ == current) {
      update_security_controls(tab_);
      update_url_field_for_tab(tab_);
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
    BrowserTab *current = get_current_tab();
    if (is_tab_selected(tab_) && current != tab_) {
        set_current_tab(tab_);
        current = tab_;
    }
    bool is_current = (tab_ == current);
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
    if (tab_ == get_current_tab()) {
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
    BrowserTab *current = get_current_tab();
    tab_->can_go_back = canGoBack;
    tab_->can_go_forward = canGoForward;
    tab_->loading = isLoading;
    if (!isLoading) {
      update_tab_security_status(tab_);
      if (tab_ == current) {
        update_security_controls(tab_);
      }
      if (!tab_->current_url.empty() && tab_->current_url == tab_->pending_url) {
        schedule_theme_color_request(tab_, 50);
        fprintf(stderr,
                "[ck-browser] scheduling theme color request (loading finished) url=%s ready_state=complete\n",
                tab_->pending_url.c_str());
      } else {
        fprintf(stderr,
                "[ck-browser] skipping theme color request because current_url=%s pending_url=%s\n",
                tab_->current_url.empty() ? "(none)" : tab_->current_url.c_str(),
                tab_->pending_url.empty() ? "(none)" : tab_->pending_url.c_str());
      }
    }
    if (tab_ == current) {
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
    fprintf(stderr,
            "[ck-renderer] got message=%s frame=%s url=%s\n",
            name.c_str(),
            frame->IsMain() ? "main" : "sub",
            frame->GetURL().ToString().c_str());
    fflush(stderr);
    if (name != "ck_request_theme_color") return false;

    CefRefPtr<CefV8Context> ctx = frame->GetV8Context();
    if (!ctx || !ctx->IsValid()) return true;
    if (!ctx->Enter()) return true;

    CefRefPtr<CefV8Value> retval;
    CefRefPtr<CefV8Exception> exception;

const char *code = R"JS(
(function(){
  function parseColorString(value){
    if(!value) return null;
    var text=String(value).trim();
    if(!text) return null;
    var hex=text.match(/^#([0-9a-f]{3}|[0-9a-f]{6})$/i);
    if(hex){
      var digits=hex[1];
      if(digits.length===3){
        digits=digits[0]+digits[0]+digits[1]+digits[1]+digits[2]+digits[2];
      }
      return [
        parseInt(digits.slice(0,2),16),
        parseInt(digits.slice(2,4),16),
        parseInt(digits.slice(4,6),16)
      ];
    }
    var rgb=text.match(/^rgba?\(([^)]+)\)/i);
    if(rgb){
      var parts=rgb[1].split(',').map(function(p){ return p.trim(); });
      if(parts.length>=3){
        var r=parseInt(parts[0],10), g=parseInt(parts[1],10), b=parseInt(parts[2],10);
        if(!isNaN(r)&&!isNaN(g)&&!isNaN(b)) return [r,g,b];
      }
    }
    return null;
  }

  function norm(c){
    var parsed=parseColorString(c);
    if(parsed) return parsed;
    var d=document.createElement('div');
    d.style.color=String(c||'');
    (document.body||document.documentElement).appendChild(d);
    var s=getComputedStyle(d).color||'';
    if(d.parentNode) d.parentNode.removeChild(d);
    var m=s.match(/rgba?\((\d+)\s*,\s*(\d+)\s*,\s*(\d+)/);
    if(m) return [parseInt(m[1],10),parseInt(m[2],10),parseInt(m[3],10)];
    return null;
  }

  function isTransparent(value){
    if(value==null) return true;
    var s=String(value).trim().toLowerCase();
    if(!s) return true;
    if(s==='transparent') return true;
    if(s.indexOf('rgba(')===0){
      var inner=s.slice(5,-1).split(',');
      if(inner.length===4){
        var a=parseFloat(inner[3]);
        return !isFinite(a) ? false : a<=0;
      }
    }
    if(s.indexOf('/')!==-1 && s.indexOf('rgb')===0){
      var slash=s.split('/');
      if(slash.length===2){
        var a2=parseFloat(slash[1]);
        if(isFinite(a2)) return a2<=0;
      }
    }
    return false;
  }

  var logs=[];
  logs.push('readyState:'+(document.readyState||'unknown'));

  var chosen = { value: null, source: 'fallback' };

  function consider(value, sourceLabel){
    if(chosen.value!=null) return;
    if(value==null) return;
    var v=String(value).trim();
    if(!v) return;
    if(isTransparent(v)) {
      logs.push(sourceLabel+' ignored (transparent)');
      return;
    }
    chosen.value=v;
    chosen.source=sourceLabel;
  }

  var meta=document.querySelector('meta[name="theme-color"]');
  if(meta && meta.content!=null) {
    logs.push('meta='+meta.content);
    consider(meta.content,'meta');
  }

  var htmlBg=getComputedStyle(document.documentElement).backgroundColor||'';
  logs.push('htmlBackground:'+htmlBg);
  consider(htmlBg,'html');

  if(document.body){
    var bodyBg=getComputedStyle(document.body).backgroundColor||'';
    logs.push('bodyBackground:'+bodyBg);
    consider(bodyBg,'body');
  }

  var used = (chosen.value!=null) ? chosen.value : '#ffffff';
  var source = chosen.source;

  var rgb = norm(used) || [255,255,255];
  logs.push('used='+used);

  if(window.console && console.log){
    console.log('[ck-theme-color] '+logs.join(' | '));
  }

  rgb.push(source);
  rgb.push(used);
  rgb.push(document.readyState||'unknown');
  return rgb;
})()
)JS";



    bool ok = ctx->Eval(code, "ck_theme_color.js", 1, retval, exception);
    const char *result_type = "null";
    if (retval) {
      if (retval->IsArray()) {
        result_type = "array";
      } else if (retval->IsString()) {
        result_type = "string";
      } else {
        result_type = "other";
      }
    }
    fprintf(stderr,
            "[ck-renderer] Eval ok=%d resultType=%s hasException=%d\n",
            ok ? 1 : 0,
            result_type,
            exception ? 1 : 0);
    if (exception) {
      fprintf(stderr,
              "[ck-renderer] JS exception: %s @ %s:%d\n",
              exception->GetMessage().ToString().c_str(),
              exception->GetScriptResourceName().ToString().c_str(),
              exception->GetLineNumber());
    }
    fflush(stderr);

    if (!ok || !retval || !retval->IsArray()) {
      ctx->Exit();
      return true;
    }

    int r = 255, g = 255, b = 255;
    CefRefPtr<CefV8Value> v0 = retval->GetValue(0);
    CefRefPtr<CefV8Value> v1 = retval->GetValue(1);
    CefRefPtr<CefV8Value> v2 = retval->GetValue(2);
    auto extract_number = [](CefRefPtr<CefV8Value> value, int fallback) {
      if (!value) {
        fprintf(stderr,
                "[ck-renderer] extract_number: value is null, returning fallback=%d\n",
                fallback);
        return fallback;
      }
      if (value->IsInt()) return value->GetIntValue();
      if (value->IsDouble()) return (int)std::round(value->GetDoubleValue());
      if (value->IsBool()) return value->GetBoolValue() ? 1 : 0;
      if (value->IsString()) {
        try {
          double parsed = std::stod(value->GetStringValue().ToString());
          return (int)std::round(parsed);
        } catch (...) {
          return fallback;
        }
      }
      return fallback;
    };
    r = extract_number(v0, r);
    g = extract_number(v1, g);
    b = extract_number(v2, b);
    std::string source_str;
    CefRefPtr<CefV8Value> v3 = retval->GetValue(3);
    if (v3 && v3->IsString()) {
        // log the actual string
        fprintf(stderr,
                "[ck-renderer] theme color raw source string='%s' (v3)\n",
                v3->GetStringValue().ToString().c_str());
      source_str = v3->GetStringValue().ToString();
    }
    std::string raw_str;
    CefRefPtr<CefV8Value> v4 = retval->GetValue(4);
    if (v4 && v4->IsString()) {
        fprintf(stderr,
                "[ck-renderer] theme color raw color string='%s' (v4)\n",
                v4->GetStringValue().ToString().c_str());
      raw_str = v4->GetStringValue().ToString();
    }
    std::string ready_state;
    CefRefPtr<CefV8Value> v5 = retval->GetValue(5);
    if (v5 && v5->IsString()) {
        fprintf(stderr,
                "[ck-renderer] theme color ready state string='%s' (v5)\n",
                v5->GetStringValue().ToString().c_str());
      ready_state = v5->GetStringValue().ToString();
    }
    fprintf(stderr,
            "[ck-browser] renderer theme color rgb=%d,%d,%d source=%s raw='%s' readyState=%s\n",
            r,
            g,
            b,
            source_str.empty() ? "fallback" : source_str.c_str(),
            raw_str.empty() ? "(empty)" : raw_str.c_str(),
            ready_state.empty() ? "unknown" : ready_state.c_str());
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;

    ctx->Exit();


    CefRefPtr<CefProcessMessage> reply = CefProcessMessage::Create("ck_theme_color");
    CefRefPtr<CefListValue> args = reply->GetArgumentList();
    args->SetInt(0, r);
    args->SetInt(1, g);
    args->SetInt(2, b);
    args->SetString(3, source_str.empty() ? "fallback" : source_str.c_str());
    args->SetString(4, raw_str.empty() ? "#ffffff" : raw_str.c_str());
    args->SetString(5, ready_state.empty() ? "unknown" : ready_state.c_str());
    frame->SendProcessMessage(PID_BROWSER, reply);
    return true;
  }

 private:
  IMPLEMENT_REFCOUNTING(CkCefApp);
};

 : 0;
      if (value->IsString()) {
        try {
          double parsed = std::stod(value->GetStringValue().ToString());
          return (int)std::round(parsed);
        } catch (...) {
          return fallback;
        }
      }
      return fallback;
    };
    r = extract_number(v0, r);
    g = extract_number(v1, g);
    b = extract_number(v2, b);
    std::string source_str;
    CefRefPtr<CefV8Value> v3 = retval->GetValue(3);
    if (v3 && v3->IsString()) {
        // log the actual string
        fprintf(stderr,
                "[ck-renderer] theme color raw source string='%s' (v3)\n",
                v3->GetStringValue().ToString().c_str());
      source_str = v3->GetStringValue().ToString();
    }
    std::string raw_str;
    CefRefPtr<CefV8Value> v4 = retval->GetValue(4);
    if (v4 && v4->IsString()) {
        fprintf(stderr,
                "[ck-renderer] theme color raw color string='%s' (v4)\n",
                v4->GetStringValue().ToString().c_str());
      raw_str = v4->GetStringValue().ToString();
    }
    std::string ready_state;
    CefRefPtr<CefV8Value> v5 = retval->GetValue(5);
    if (v5 && v5->IsString()) {
        fprintf(stderr,
                "[ck-renderer] theme color ready state string='%s' (v5)\n",
                v5->GetStringValue().ToString().c_str());
      ready_state = v5->GetStringValue().ToString();
    }
    fprintf(stderr,
            "[ck-browser] renderer theme color rgb=%d,%d,%d source=%s raw='%s' readyState=%s\n",
            r,
            g,
            b,
            source_str.empty() ? "fallback" : source_str.c_str(),
            raw_str.empty() ? "(empty)" : raw_str.c_str(),
            ready_state.empty() ? "unknown" : ready_state.c_str());
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;

    ctx->Exit();


    CefRefPtr<CefProcessMessage> reply = CefProcessMessage::Create("ck_theme_color");
    CefRefPtr<CefListValue> args = reply->GetArgumentList();
    args->SetInt(0, r);
    args->SetInt(1, g);
    args->SetInt(2, b);
    args->SetString(3, source_str.empty() ? "fallback" : source_str.c_str());
    args->SetString(4, raw_str.empty() ? "#ffffff" : raw_str.c_str());
    args->SetString(5, ready_state.empty() ? "unknown" : ready_state.c_str());
    frame->SendProcessMessage(PID_BROWSER, reply);
    return true;
  }

private:
  IMPLEMENT_REFCOUNTING(CkCefApp);
};

CefRefPtr<CkCefApp> g_cef_app;

CefRefPtr<BrowserClient> create_browser_client(BrowserTab *tab)
{
    return new BrowserClient(tab);
}

void detach_browser_client(const CefRefPtr<BrowserClient> &client)
{
    if (client) {
        client->detach_tab();
    }
}
bool BrowserApp::build_path_from_dir(const char *dir, const char *suffix, char *buffer, size_t buffer_len) const
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

void BrowserApp::build_cwd_path(char *buffer, size_t buffer_len, const char *suffix) const
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

bool BrowserApp::dir_has_files(const char *path) const
{
    if (!path || path[0] == '\0') return false;
    DIR *dir = opendir(path);
    if (!dir) return false;
    struct dirent *entry = NULL;
    bool has_files = false;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        has_files = true;
        break;
    }
    closedir(dir);
    return has_files;
}

bool BrowserApp::file_exists(const char *path) const
{
    if (!path || path[0] == '\0') return false;
    return access(path, R_OK) == 0;
}

void BrowserApp::get_exe_path(char *buffer, size_t buffer_len) const
{
    if (!buffer || buffer_len == 0) return;
    ssize_t len = readlink("/proc/self/exe", buffer, buffer_len - 1);
    if (len < 0) {
        buffer[0] = '\0';
        return;
    }
    buffer[len] = '\0';
}

bool BrowserApp::find_existing_path(char *buffer, size_t buffer_len, const char *suffix) const
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

BrowserPaths BrowserApp::discover_cef_paths() const
{
    BrowserPaths paths;
    char resources_path[PATH_MAX];
    if (!find_existing_path(resources_path, sizeof(resources_path),
                            "third_party/cef/resources")) {
        build_cwd_path(resources_path, sizeof(resources_path), "third_party/cef/resources");
    }
    if (resources_path[0]) {
        paths.resources_path = resources_path;
    }

    char locales_path[PATH_MAX];
    if (!find_existing_path(locales_path, sizeof(locales_path),
                            "third_party/cef/locales")) {
        build_cwd_path(locales_path, sizeof(locales_path), "third_party/cef/locales");
    }
    if (!dir_has_files(locales_path)) {
        if (!find_existing_path(locales_path, sizeof(locales_path),
                                "third_party/cef/resources/locales")) {
            build_cwd_path(locales_path, sizeof(locales_path),
                           "third_party/cef/resources/locales");
        }
    }
    if (locales_path[0]) {
        paths.locales_path = locales_path;
    }

    char exe_path[PATH_MAX];
    get_exe_path(exe_path, sizeof(exe_path));
    if (exe_path[0]) {
        paths.subprocess_path = exe_path;
    }

    return paths;
}

void BrowserApp::report_cef_resource_status(const char *resources_path, const char *locales_path) const
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

void BrowserApp::dump_cef_env_and_args(int argc, char *argv[]) const
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

bool BrowserApp::parse_cache_suffix_arg(int argc, char *argv[], std::string *out_suffix) const
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

void BrowserApp::build_cef_argv(int argc, char *argv[], std::vector<char *> *out_argv) const
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

bool BrowserApp::has_opengl_support() const
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

void BrowserApp::apply_gpu_switches(bool disable_gpu) const
{
    if (!disable_gpu) return;
    CefRefPtr<CefCommandLine> global = CefCommandLine::GetGlobalCommandLine();
    if (!global) return;
    global->AppendSwitch("disable-gpu");
    global->AppendSwitch("disable-software-rasterizer");
    global->AppendSwitch("disable-gpu-compositing");
}

std::string BrowserApp::build_cache_path(const std::string &suffix) const
{
    char cache_base[PATH_MAX];
    build_cwd_path(cache_base, sizeof(cache_base), "build/ck-browser-cache");
    if (cache_base[0] == '\0') {
        return std::string();
    }
    std::string cache_path(cache_base);
    if (!suffix.empty()) {
        cache_path.push_back('-');
        cache_path.append(suffix);
    }
    mkdir(cache_path.c_str(), 0755);
    return cache_path;
}

int BrowserApp::run_cef_preflight(int argc, char *argv[], CefRefPtr<CefApp> cef_app, BrowserPreflightState *state) const
{
    if (!state) return -1;
    state->homepage_url = load_homepage_file();
    state->cef_paths = discover_cef_paths();
    bool force_disable_gpu = !has_opengl_support();
    state->force_disable_gpu = force_disable_gpu;
    if (force_disable_gpu) {
        fprintf(stderr,
                "[ck-browser] OpenGL stack missing (libGL), forcing --disable-gpu + software fallback\n");
    } else {
        fprintf(stderr, "[ck-browser] OpenGL stack present, GPU acceleration enabled\n");
    }
    apply_gpu_switches(force_disable_gpu);
    state->session_data = prepare_session(argc, argv);

    build_cef_argv(argc, argv, &state->cef_argv);
    fprintf(stderr, "[ck-browser] cef args:");
    for (size_t i = 0; i < state->cef_argv.size(); ++i) {
        fprintf(stderr, " %s", state->cef_argv[i] ? state->cef_argv[i] : "(null)");
    }
    fprintf(stderr, "\n");
    CefMainArgs main_args((int)state->cef_argv.size(), state->cef_argv.data());
    if (force_disable_gpu) {
        apply_gpu_switches(force_disable_gpu);
    }
    fprintf(stderr, "[ck-browser] calling cef_execute_process\n");
    int exit_code = CefExecuteProcess(main_args, cef_app, nullptr);
    fprintf(stderr, "[ck-browser] cef_execute_process result=%d\n", exit_code);
    if (exit_code >= 0) {
        return exit_code;
    }

    state->has_startup_url = parse_startup_url_arg(argc, argv, &state->startup_url);
    parse_cache_suffix_arg(argc, argv, &state->cache_suffix);
    if (state->cache_suffix.empty() && state->has_startup_url) {
        state->cache_suffix = std::to_string((long)getpid());
    }
    return -1;
}

bool BrowserApp::initialize_cef(const BrowserPreflightState &preflight,
                                const char *resources_path,
                                const char *locales_path,
                                const char *subprocess_path)
{
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
    if (resources_path && resources_path[0] != '\0') {
        CefString(&settings.resources_dir_path) = resources_path;
    }
    const char *selected_locales = NULL;
    if (locales_path && locales_path[0] != '\0' && dir_has_files(locales_path)) {
        selected_locales = locales_path;
        CefString(&settings.locales_dir_path) = locales_path;
    }
    if (subprocess_path && subprocess_path[0] != '\0') {
        CefString(&settings.browser_subprocess_path) = subprocess_path;
    }

    std::string cache_path = build_cache_path(preflight.cache_suffix);
    if (!cache_path.empty()) {
        CefString(&settings.root_cache_path) = cache_path;
        fprintf(stderr, "[ck-browser] root_cache_path=%s\n", cache_path.c_str());
    }
    report_cef_resource_status(resources_path, selected_locales ? selected_locales : "");
    CefMainArgs main_args((int)preflight.cef_argv.size(), preflight.cef_argv.data());
    dump_cef_env_and_args(main_args.argc, main_args.argv);
    fprintf(stderr, "[ck-browser] calling CefInitialize\n");
    bool cef_ok = CefInitialize(main_args, settings, g_cef_app, nullptr);
    fprintf(stderr, "[ck-browser] CefInitialize result=%d\n", cef_ok ? 1 : 0);
    return cef_ok;
}

void BrowserApp::shutdown_cef() const
{
    fprintf(stderr, "[ck-browser] calling CefShutdown\n");
    CefShutdown();
}

SessionData *BrowserApp::prepare_session(int &argc, char **argv) const
{
    char *session_id = session_parse_argument(&argc, argv);
    SessionData *data = session_data_create(session_id);
    free(session_id);
    return data;
}
