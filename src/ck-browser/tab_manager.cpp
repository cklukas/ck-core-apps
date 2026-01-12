#include "tab_manager.h"

#include <Xm/DrawingA.h>
#include <Xm/Form.h>
#include <Xm/TabStack.h>
#include <Xm/TextF.h>
#include <Xm/Xm.h>
#include <X11/Intrinsic.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <include/cef_image.h>

#include <cstdio>

#include <algorithm>
#include <list>
#include <unordered_map>
#include <vector>
#include <ctime>
#include <cstdlib>
#include <cstring>

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

int desired_favicon_size()
{
    int size = get_url_field_height();
    if (size <= 0) size = 20;
    if (size < 16) size = 16;
    if (size > 32) size = 32;
    return size;
}

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

namespace {
const size_t kFaviconCacheLimit = 500;
const time_t kFaviconCacheTTL_SECONDS = 6 * 60 * 60;

struct FaviconCacheEntry {
    std::vector<unsigned char> data;
    std::vector<unsigned char> png_data;
    int width = 0;
    int height = 0;
    time_t saved_time = 0;
    std::list<std::string>::iterator order_it;
};

std::unordered_map<std::string, FaviconCacheEntry> favicon_cache_;
std::list<std::string> favicon_cache_order_;

int desired_window_icon_size()
{
    return 96;
}

bool tab_is_alive(const BrowserTab *tab)
{
    if (!tab) return false;
    for (const auto &entry : TabManager::instance().tabs()) {
        if (entry.get() == tab) return true;
    }
    return false;
}

const char *display_url_for_tab(const BrowserTab *tab)
{
    if (!tab) return kInitialBrowserUrl;
    if (!tab->current_url.empty()) return tab->current_url.c_str();
    if (!tab->pending_url.empty()) return tab->pending_url.c_str();
    return kInitialBrowserUrl;
}

void remove_favicon_cache_entry(const std::string &key)
{
    auto it = favicon_cache_.find(key);
    if (it == favicon_cache_.end()) return;
    if (it->second.order_it != favicon_cache_order_.end()) {
        favicon_cache_order_.erase(it->second.order_it);
    }
    favicon_cache_.erase(it);
}

void cleanup_favicon_cache()
{
    time_t now = time(NULL);
    std::vector<std::string> expired;
    for (const auto &pair : favicon_cache_) {
        if (now - pair.second.saved_time > kFaviconCacheTTL_SECONDS) {
            expired.push_back(pair.first);
        }
    }
    for (const auto &key : expired) {
        remove_favicon_cache_entry(key);
    }
}

void mask_shift_bits(unsigned long mask, int *shift_out, int *bits_out)
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

unsigned long pack_rgb_pixel(Visual *visual, unsigned char r, unsigned char g, unsigned char b)
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

bool query_pixel_rgb(Display *display,
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

void update_wm_icon_pixmap(Display *display, Window window, Pixmap pixmap, Pixmap mask)
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

Pixmap create_scaled_toolbar_pixmap_from_bgra_impl(Display *display,
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

bool create_scaled_window_icon_from_bgra_impl(Display *display,
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

bool apply_favicon_to_tab_from_raw(BrowserTab *tab,
                                   const unsigned char *raw,
                                   int raw_w,
                                   int raw_h,
                                   int toolbar_size,
                                   int window_size,
                                   Pixel toolbar_bg_pixel)
{
    Widget toplevel = get_toplevel_widget();
    Display *display = get_browser_display();
    if (!tab || !toplevel || !display || !raw || raw_w <= 0 || raw_h <= 0) return false;
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

void store_favicon_in_cache(const char *url,
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
    auto it = favicon_cache_.find(key);
    if (it != favicon_cache_.end()) {
        it->second.data = std::move(copy);
        it->second.width = width;
        it->second.height = height;
        it->second.saved_time = now;
        it->second.png_data = std::move(png_copy);
        if (it->second.order_it != favicon_cache_order_.end()) {
            favicon_cache_order_.erase(it->second.order_it);
        }
        favicon_cache_order_.push_back(key);
        auto order_it = favicon_cache_order_.end();
        --order_it;
        it->second.order_it = order_it;
    } else {
        FaviconCacheEntry entry;
        entry.data = std::move(copy);
        entry.width = width;
        entry.height = height;
        entry.saved_time = now;
        entry.png_data = std::move(png_copy);
        favicon_cache_order_.push_back(key);
        auto order_it = favicon_cache_order_.end();
        --order_it;
        entry.order_it = order_it;
        favicon_cache_.emplace(key, std::move(entry));
    }
    while (favicon_cache_.size() > kFaviconCacheLimit && !favicon_cache_order_.empty()) {
        remove_favicon_cache_entry(favicon_cache_order_.front());
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
      if (tab_ == TabManager::instance().currentTab()) {
        TabManager::instance().updateFaviconControls(tab_);
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
}  // namespace

Pixmap create_scaled_toolbar_pixmap_from_bgra(Display *display,
                                              int screen,
                                              const unsigned char *bgra,
                                              int src_w,
                                              int src_h,
                                              int target_size,
                                              Pixel bg_pixel)
{
    return create_scaled_toolbar_pixmap_from_bgra_impl(display,
                                                       screen,
                                                       bgra,
                                                       src_w,
                                                       src_h,
                                                       target_size,
                                                       bg_pixel);
}

bool create_scaled_window_icon_from_bgra(Display *display,
                                         int screen,
                                         const unsigned char *bgra,
                                         int src_w,
                                         int src_h,
                                         int target_size,
                                         Pixmap *out_pixmap,
                                         Pixmap *out_mask)
{
    return create_scaled_window_icon_from_bgra_impl(display,
                                                    screen,
                                                    bgra,
                                                    src_w,
                                                    src_h,
                                                    target_size,
                                                    out_pixmap,
                                                    out_mask);
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
    Widget stack = tab_stack ? tab_stack : tab_stack_;
    if (!stack) return nullptr;
    Widget page = XmCreateForm(stack, (String)name, NULL, 0);
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

void TabManager::setTabStack(Widget stack)
{
    tab_stack_ = stack;
}

BrowserTab *TabManager::openNewTab(const std::string &url, bool select)
{
    if (url.empty() || !tab_stack_) return nullptr;
    BrowserApp::instance().notify_new_tab_request(url, select);
    const char *base = "New Tab";
    int count = countTabsWithBaseTitle(base);
    char name[32];
    snprintf(name, sizeof(name), "tabNew%d", count + 1);
    char tab_title[64];
    snprintf(tab_title, sizeof(tab_title), "%s (%d)", base, count + 1);
    BrowserTab *tab = createTab(tab_stack_, name, tab_title, base, url.c_str());
    if (!tab) return nullptr;
    scheduleBrowserCreation(tab);
    if (select) {
        if (tab->page) {
            XmTabStackSelectTab(tab->page, True);
        }
        selectTab(tab);
    }
    return tab;
}

void TabManager::registerUrlField(Widget url_field)
{
    url_field_ = url_field;
}

void TabManager::registerStatusLabel(Widget status_label)
{
    status_label_ = status_label;
}

void TabManager::updateUrlField(BrowserTab *tab)
{
    if (!url_field_ || !tab || tab != current_tab_) return;
    const char *value = display_url_for_tab(tab);
    XmTextFieldSetString(url_field_, const_cast<char *>(value ? value : ""));
}

void TabManager::setStatusText(const char *text)
{
    if (!status_label_) return;
    const char *display = text ? text : "";
    XmString xm_text = XmStringCreateLocalized(const_cast<char *>(display));
    XtVaSetValues(status_label_, XmNlabelString, xm_text, NULL);
    XmStringFree(xm_text);
}

int TabManager::getUrlFieldHeight() const
{
    if (!url_field_) return 0;
    Dimension height = 0;
    XtVaGetValues(url_field_, XmNheight, &height, NULL);
    return (int)height;
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
    XmString xm_label = XmStringCreateLocalized((String)label);
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

void TabManager::registerFaviconLabel(Widget favicon_label)
{
    favicon_label_ = favicon_label;
    updateFaviconControls(current_tab_);
}

void TabManager::updateFaviconControls(BrowserTab *tab)
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
    if (favicon_label_) {
        Pixmap pix = (tab && tab->favicon_toolbar_pixmap != None) ? tab->favicon_toolbar_pixmap : XmUNSPECIFIED_PIXMAP;
        int size = desired_favicon_size();
        XtVaSetValues(favicon_label_,
                      XmNlabelType, XmPIXMAP,
                      XmNlabelPixmap, pix,
                      XmNwidth, size,
                      XmNheight, size,
                      NULL);
    }
    Widget toplevel = get_toplevel_widget();
    Display *display = get_browser_display();
    if (toplevel && tab && tab->favicon_window_pixmap != None) {
        XtVaSetValues(toplevel,
                      XmNiconPixmap, tab->favicon_window_pixmap,
                      XmNiconMask, tab->favicon_window_mask,
                      NULL);
        if (XtIsRealized(toplevel)) {
            update_wm_icon_pixmap(display,
                                  XtWindow(toplevel),
                                  tab->favicon_window_pixmap,
                                  tab->favicon_window_mask);
        }
    }
    if (tab && tab->favicon_toolbar_pixmap == None && tab->favicon_window_pixmap == None && !tab->favicon_url.empty()) {
        requestFaviconDownload(tab, "update_favicon_controls");
    }
}

void TabManager::requestFaviconDownload(BrowserTab *tab, const char *reason)
{
    if (!tab || !tab->browser) return;
    if (tab->favicon_url.empty()) return;
    CefRefPtr<CefBrowserHost> host = tab->browser->GetHost();
    if (!host) return;

    int toolbar_size = desired_favicon_size();
    int window_size = desired_window_icon_size();
    int max_size = toolbar_size > window_size ? toolbar_size : window_size;

    Pixel bg_pixel = 0;
    Widget toplevel = get_toplevel_widget();
    if (favicon_label_) {
        XtVaGetValues(favicon_label_, XmNbackground, &bg_pixel, NULL);
    } else if (toplevel) {
        XtVaGetValues(toplevel, XmNbackground, &bg_pixel, NULL);
    }

    cleanup_favicon_cache();
    time_t now = time(NULL);
    auto cache_it = favicon_cache_.find(tab->favicon_url);
    if (cache_it != favicon_cache_.end()) {
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
            if (tab == current_tab_) {
                updateFaviconControls(tab);
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

void TabManager::clearTabFavicon(BrowserTab *tab)
{
    fprintf(stderr,
            "[ck-browser] clear_tab_favicon tab=%p host=%s url=%s\n",
            (void *)tab,
            tab ? (tab->current_host.empty() ? "(none)" : tab->current_host.c_str()) : "(none)",
            tab ? (tab->favicon_url.empty() ? "(none)" : tab->favicon_url.c_str()) : "(none)");
    if (!tab) return;
    Display *display = get_browser_display();
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
    if (tab == current_tab_) {
        updateFaviconControls(tab);
    }
}

void TabManager::loadUrl(BrowserTab *tab, const std::string &url)
{
    if (!tab || url.empty()) return;
    const std::string normalized = normalize_url(url.c_str());
    if (normalized.empty()) return;
    std::string host = extract_host_from_url(normalized);
    if (host != tab->current_host) {
        tab->current_host = host;
        clearTabFavicon(tab);
    }
    tab->pending_url = normalized;
    if (!tab->browser) {
        scheduleBrowserCreation(tab);
    }
    if (tab->browser) {
        CefRefPtr<CefFrame> frame = tab->browser->GetMainFrame();
        if (frame) {
            frame->LoadURL(normalized);
        }
    }
    updateUrlField(tab);
}

bool TabManager::getCachedFavicon(const std::string &url,
                                  std::vector<unsigned char> *raw_data,
                                  int *width,
                                  int *height,
                                  std::vector<unsigned char> *png_data)
{
    if (url.empty()) return false;
    auto it = favicon_cache_.find(url);
    if (it == favicon_cache_.end()) return false;
    const FaviconCacheEntry &entry = it->second;
    if (raw_data) {
        raw_data->assign(entry.data.begin(), entry.data.end());
    }
    if (png_data) {
        png_data->assign(entry.png_data.begin(), entry.png_data.end());
    }
    if (width) *width = entry.width;
    if (height) *height = entry.height;
    return true;
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
