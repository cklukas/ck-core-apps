# ck-browser Refactor Roadmap

## Objective
- Split the gigantic `ck-browser.cpp` into focused modules so Chromium glue, tab navigation, UI, and bookmark storage each live in their own translation unit with clear APIs.
- Keep the UX and feature set unchanged while making the code easier to navigate, extend, and test.

## Phase 1 – `BrowserApp` (CEF glue)
- [x] Extract `main()`, CEF initialization/shutdown, `BrowserClient`, `CkCefApp`, `route_url_through_ck_browser`, and popup/theme-color logging into `browser_app.*`.
  - [x] Create `browser_app.h`/`browser_app.cpp` with a `BrowserApp` class that owns the lifecycle and exposes `int run()` / `static BrowserApp &instance()`.
  - [x] Move CEF entry points (`main`, `initialize_cef`, `CkCefApp`, `BrowserClient`) into the new class, leaving only bootstrap in `ck-browser.cpp`.
    - [x] Introduce `BrowserApp::run_main` delegating from `main()` as a first step toward moving the `ck_browser_run` body.
    - [x] Move `main()` into `browser_app.cpp`, keeping `ck-browser.cpp` free of the program entry point.
    - [x] Migrate the `ck_browser_run` body itself into `BrowserApp::run_main`, leaving only a thin bootstrap in `ck-browser.cpp`.
      - [x] Identify all static helpers/state used inside `ck_browser_run` and lift the necessary declarations into a private namespace or header so `browser_app.cpp` can call them without global leakage.
        - Preflight/build helpers: `load_homepage_file`, `save_homepage_file`, `normalize_url`, `parse_startup_url_arg`, `parse_cache_suffix_arg`, `find_existing_path`, `build_cwd_path`, `build_path_from_dir`, `get_exe_path`, `dir_has_files`, `has_opengl_support`, `apply_gpu_switches`, `build_cef_argv`, `dump_cef_env_and_args`, `report_cef_resource_status`.
        - Session/glue: `session_parse_argument`, `session_data_create/free`, `session_load`, `session_apply_geometry`, `session_data_get_int`, `restore_tabs_from_session_data`, `save_last_session_file`, `capture_session_state`, globals `g_session_data`, `g_session_loaded`.
        - UI/bootstrap: `create_menu_bar`, `create_toolbar`, `create_status_bar`, tab stack creation, timers `attach_tab_handlers_cb`, `bookmark_file_monitor_timer_cb`, WM callbacks (`wm_delete_cb`, `wm_save_yourself_cb`, `on_main_window_resize`), and initial tab creation (`create_tab_page`, `schedule_tab_browser_creation`, `set_current_tab`).
        - Lifecycle/teardown: `rebuild_bookmarks_menu_items`, `detach_tab_clients`, `g_browser_tabs`, `g_current_tab`, `g_cef_app`, `g_cef_initialized`, `g_shutdown_requested`, `g_shutdown_pending_browsers`.
      - [x] Localize preflight-only state (`resources_path`, `locales_path`, `subprocess_path`, GPU disable flag) within the run flow to reduce global coupling.
      - [x] Move the CEF preflight/setup portion (homepage load, path discovery, GPU toggle, session prep, CEF argv/build, `CefExecuteProcess`) into `browser_app.cpp`, keeping UI creation callbacks reachable via forward declarations.
      - [x] Move the Xt/GUI bootstrap and shutdown sequence into a helper callable from `browser_app.cpp` (e.g., `start_ui_and_cef_loop`), leaving `ck-browser.cpp` to export only that helper and UI-specific statics.
      - [x] Remove the old `ck_browser_run` definition from `ck-browser.cpp` once the class-owned flow builds and links.
    - [x] Relocate the `CkCefApp` implementation into `browser_app.cpp` with any necessary forward declarations in the header.
      - [x] Identify renderer-client responsibilities (theme-color messaging, devtools IPC) that CkCefApp currently handles and outline how they surface through BrowserApp.
      - [x] Add helper declarations for the static utilities CkCefApp relies on so they can be invoked from `browser_app.cpp`.
    - [x] Relocate the `BrowserClient` implementation into `browser_app.cpp` and expose only the minimal hooks needed by the UI layer (e.g., to show devtools or spawn windows).
      - [x] Catalog the functions and globals BrowserClient accesses (`show_devtools_for_tab`, `replacement tab creation`, `route_url_through_ck_browser`, `normalize_url`, `spawn_new_browser_window`, `open_url_in_new_tab`, `log_popup_features`, `is_devtools_url`, `extract_host_from_url`, `clear_tab_favicon`, `update_url_field_for_tab`, `update_navigation_buttons`, `update_reload_button_for_tab`, `schedule_theme_color_request`, `apply_tab_theme_colors`, `request_favicon_download`, `update_favicon_controls`, `update_tab_security_status`, `update_security_controls`, `is_tab_selected`, `focus_browser_area`, `set_current_tab`, `update_tab_label`, `update_all_tab_labels`, `set_status_label_text`, `route_url_through_ck_browser`, `g_current_tab`, `g_url_field`, `get_selected_tab`).
      - [x] Create a lightweight interface layer (either in a shared header or via BrowserApp methods) that allows BrowserClient to perform the actions it needs without dragging Xm-specific statics into `browser_app.cpp`.
    - [x] Move CEF initialization/shutdown helpers (current `CefExecuteProcess`/`CefInitialize`/`CefShutdown` flow) into `BrowserApp`, keeping UI callbacks injectable.
    - [x] Isolate `BrowserClient` and `CkCefApp` definitions into `browser_app.cpp`, with clear `OnBeforePopup`, `OnBeforeBrowse`, and renderer handler wiring.
    - [x] Make `initialize_cef_browser_cb` and tab scheduling helpers members or friends so they do not rely on scattered globals.
    - [x] Transfer `route_url_through_ck_browser`, popup logging, and theme-color request helpers into `BrowserApp`, exposing a minimal callback interface for UI/tab events.
      - [x] route_url/popup logging are now hosted by `browser_app.*`.
      - [x] Send renderer theme-color requests from `BrowserApp` rather than the UI module.
      - [x] Keep the timeout-based scheduling helper (`schedule_theme_color_request`) in the UI layer for now.
    - [x] Survey the helper functions currently called from `ck_browser_run` and document the dependencies BrowserApp will need to reach into (session/GUI, bookmark, toolbar helpers, etc.).
      - Requires homepage/config helpers (`load_homepage_file`, `save_homepage_file`, `normalize_url`, cache-suffix parsing), filesystem path utilities (`find_existing_path`, `build_cwd_path`, `build_path_from_dir`, `get_exe_path`, `dir_has_files`), and GPU capability switches (`has_opengl_support`, `apply_gpu_switches`).
      - Depends on session management (`session_parse_argument`, `session_data_create/load/apply_geometry/free`, `restore_tabs_from_session_data`, `save_last_session_file`, `capture_session_state`) plus CEF process wiring (`build_cef_argv`, `CefExecuteProcess`, `CefInitialize`, `CefShutdown`, `report_cef_resource_status`, `dump_cef_env_and_args`).
      - UI bootstrap needs Xt/Motif setup (`XtVaAppInitialize`, `DtInitialize`, widget builders `create_menu_bar`, `create_toolbar`, `create_status_bar`, tab stack/timers/WM protocol wiring, `attach_tab_handlers_cb`, `bookmark_file_monitor_timer_cb`) and tab creation (`create_tab_page`, `schedule_tab_browser_creation`, `set_current_tab`).
      - Runtime/shutdown touches bookmark refresh (`rebuild_bookmarks_menu_items`, icon refresh) and tab cleanup (`detach_tab_clients`, `g_browser_tabs.clear`, `g_current_tab = NULL`), so BrowserApp will need a clean interface to delegate lifecycle to tab/bookmark modules.
    - [x] Introduce BrowserApp-level helpers for the pre-UI setup phase (path discovery, GPU tuning, session parsing, CEF arg build) so the remaining code can be factored out without global scope pollution.
      - [x] Extract path discovery (`find_existing_path`, `build_cwd_path`, `build_path_from_dir`, `get_exe_path`, `dir_has_files`) into a small `BrowserPaths` helper owned by `BrowserApp`.
      - [x] Move GPU capability detection/toggling (`has_opengl_support`, `apply_gpu_switches`) behind `BrowserApp::configure_gpu()` invoked before CEF init.
      - [x] Wrap session argument parsing (`session_parse_argument`) and session state creation (`session_data_create`) into a `prepare_session()` helper that can be invoked before Xt/CEF startup.
      - [x] Relocate CEF argument filtering (`build_cef_argv`, cache suffix parsing) and logging (`dump_cef_env_and_args`) into a `build_cef_args()` helper that produces `CefMainArgs` for both sub- and main processes.
      - [x] Centralize cache path construction into `BrowserApp::build_cache_path` so `ck_browser_run` can focus on UI configuration.
      - [x] Once dependencies are mapped, pull the runtime orchestration (XtApp setup, widget creation, main loop, shutdown) into a helper that BrowserApp can invoke after CEF initialization.
- [x] Convert the existing global helper functions used only by CEF callbacks into private methods inside `BrowserApp`.
  - [x] Move `spawn_new_browser_window` onto `BrowserApp` (store the subprocess path and call the new instance method from BrowserClient/UI code instead of the global helper).
  - [x] Proxy renderer theme-color requests through `BrowserApp::request_theme_color_for_tab`.
  - [x] Proxy DevTools display through `BrowserApp::show_devtools_for_tab`.
  - [x] Proxy `route_url_through_ck_browser`, popup logging, and `is_devtools_url` through BrowserApp helpers and keep them out of the bridge header.
  - [x] Identify the remaining BrowserClient-only helpers that should move into `BrowserApp` by introducing BrowserApp handlers for load, status, title, favicon, loading state, and focus.
- [x] Surface events (new tab request, load finished) through a small interface so UI modules can react without depending on Xm.

-## Phase 2 – `TabManager`
- [ ] Move `BrowserTab` state, tab creation/destruction, navigation buttons, reload/stop logic, zoom helpers, and favicon updates into `tab_manager.*`.
  - [x] Move tab cleanup helpers (detach/remove/clear) into `TabManager` so `ck-browser.cpp` no longer knows about the old globals.
  - [x] Create `tab_manager.h`/`tab_manager.cpp` that owns the `BrowserTab` data structures and exposes helpers like `createTab`, `closeTab`, `selectTab`, and `getCurrentTab`.
  - [x] Move the Xm widget creation helper (`create_tab_page`) and label updates into `TabManager` so the module owns the tab data lifecycle (added `TabManager::createTab` and rerouted callers).
  - [x] Relocate the implementation of `create_tab_page` into `tab_manager.cpp` so it no longer lives inside `ck-browser.cpp`.
  - [x] Delegate tab selection (`set_current_tab`/`select_tab_page`) to `TabManager` so event code talks through the manager (added TabManager selection handler and registered `tab_selection_handler`).
  - [x] Move toolbar/tab-stack helpers (`schedule_tab_browser_creation` scheduling) into `TabManager` once the basic creation/selection APIs are in place (introduced `scheduleBrowserCreation`).
  - [ ] Relocate navigation/reload handling, zoom controls, and favicon/icon cache updates into `TabManager` methods so BrowserApp can manipulate tabs through a clean interface.
    - [x] Move navigation button wiring and the back/forward helpers into `TabManager`.
    - [x] Move reload/stop button behavior and state updates into `TabManager`.
    - [x] Move zoom controls, button updates, and polling into `TabManager`.
    - [x] Move favicon cache management and toolbar/icon updates into `TabManager`.
  - [x] Wire the new `TabManager` API into `BrowserApp`, BrowserClient, and UI callbacks, keeping the old globals behind the manager.
- [x] Provide APIs such as `loadUrl`, `openNewTab`, `currentTab`, and `selectTab` so `BrowserApp` and UI builders can work with tabs via a clean interface.
- [x] Keep toolbox helpers (status text, URL field updates, toolbar icon management) here instead of in the main file.

## Phase 3 – `BookmarkManager`
- [ ] Port the `BookmarkEntry/Group` data structures, Netscape parsing/serialization, icon cache, and the menu rebuild logic into `bookmark_manager.*`.
  - [ ] Create `bookmark_manager.h`/`bookmark_manager.cpp` that own the bookmark tree, file I/O helpers, and the core types so `ck-browser.cpp` no longer declares the global state.
  - [ ] Move the Netscape parsing/serialization, file monitor, and icon cache helpers into the new module and export a focused API for load/save/rebuild.
  - [ ] Keep UI menu/dialog wiring in `ck-browser.cpp` for now, but call `BookmarkManager` helpers instead of manipulating bookmark globals directly.
- [ ] Expose methods for adding/updating bookmarks, moving entries between folders, renaming/deleting folders, and returning favorites/groups for UI consumption.
- [ ] Keep file-path, serialization, and caching details internal; only expose high-level mutation and query APIs.

## Phase 4 – `UiBuilder`
- [ ] Factor all menu/tab bar builders, dialogs (Add Bookmark, Bookmark Manager), and widget helpers into `ui_builder.*`, using `TabManager`/`BookmarkManager` APIs for data.
- [ ] Maintain Xm-based layout code here; the module should register callbacks that call back into the managers rather than mutating globals.
- [ ] Move dialog contexts (`BookmarkDialogContext`, manager contexts) under this module so state stays alongside the widgets they own.

## Phase 5 – Integration & polish
- [ ] Wire the new modules together through thin glue in `ck-browser.cpp`, keeping that file as the bootstrap/permutation layer.
- [ ] Update build targets (Makefile) to compile the new sources and remove redundant global header clutter.
- [ ] Run `make` and quickly smoke-test bookmarks, menus, and tab navigation to ensure behavior is preserved.

## Follow-up
- [ ] Revisit any remaining static/global helpers and move them into the relevant module; aim for zero usage of `extern` globals outside configuration constants.
- [ ] Consider adding unit-test harnesses for serialization and bookmark logic once the modules are separated.
