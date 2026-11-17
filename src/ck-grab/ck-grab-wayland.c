/*
 * ck-grab-wayland.c - Wayland zwlr_screencopy_manager_v1 backend for ck-grab.
 */

#include "ck-grab-wayland.h"

#ifdef HAVE_WAYLAND_SCREENCOPY

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-screencopy-unstable-v1-client-protocol.h"
#ifdef HAVE_XDG_OUTPUT_PROTOCOL
#include "xdg-output-unstable-v1-client-protocol.h"
#endif

#define MAX_GRAB_OUTPUTS 16

typedef struct {
    struct wl_output *wl_out;
#ifdef HAVE_XDG_OUTPUT_PROTOCOL
    struct zxdg_output_v1 *xdg_out;
#endif
    int x, y;
    int logical_w, logical_h;
    int mode_w, mode_h;
    int has_xdg_pos;

    /* Screencopy frame state */
    struct zwlr_screencopy_frame_v1 *frame;
    uint32_t fmt;
    uint32_t width, height, stride;
    uint32_t flags;
    int got_buffer_info;
    int buffer_done;
    int copy_sent;
    int ready;
    int failed;

    int shm_fd;
    size_t shm_size;
    void *shm_data;
    struct wl_shm_pool *pool;
    struct wl_buffer *buffer;
} GrabWaylandOutput;

typedef struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_shm *shm;
    struct zwlr_screencopy_manager_v1 *screencopy_mgr;
    uint32_t screencopy_ver;
#ifdef HAVE_XDG_OUTPUT_PROTOCOL
    struct zxdg_output_manager_v1 *xdg_out_mgr;
#endif
    GrabWaylandOutput outputs[MAX_GRAB_OUTPUTS];
    int num_outputs;
} GrabWaylandCtx;

static int create_anon_shm_fd(size_t size)
{
    char name[64];
    static unsigned int seq = 0;
    snprintf(name, sizeof(name), "/ck-grab-%d-%u", (int)getpid(), ++seq);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    shm_unlink(name);
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* --- wl_output listener --- */
static void out_geometry(void *data, struct wl_output *wl_out, int32_t x, int32_t y,
                         int32_t phys_w, int32_t phys_h, int32_t subpixel,
                         const char *make, const char *model, int32_t transform)
{
    (void)wl_out; (void)phys_w; (void)phys_h; (void)subpixel;
    (void)make; (void)model; (void)transform;
    GrabWaylandOutput *o = (GrabWaylandOutput *)data;
    if (!o->has_xdg_pos) {
        o->x = x;
        o->y = y;
    }
}

static void out_mode(void *data, struct wl_output *wl_out, uint32_t flags,
                     int32_t width, int32_t height, int32_t refresh)
{
    (void)wl_out; (void)refresh;
    GrabWaylandOutput *o = (GrabWaylandOutput *)data;
    if ((flags & WL_OUTPUT_MODE_CURRENT) || o->mode_w == 0) {
        o->mode_w = width;
        o->mode_h = height;
    }
}

static void out_done(void *data, struct wl_output *wl_out) { (void)data; (void)wl_out; }
static void out_scale(void *data, struct wl_output *wl_out, int32_t factor) { (void)data; (void)wl_out; (void)factor; }
static void out_name(void *data, struct wl_output *wl_out, const char *name) { (void)data; (void)wl_out; (void)name; }
static void out_desc(void *data, struct wl_output *wl_out, const char *desc) { (void)data; (void)wl_out; (void)desc; }

static const struct wl_output_listener out_listener = {
    .geometry = out_geometry,
    .mode = out_mode,
    .done = out_done,
    .scale = out_scale,
    .name = out_name,
    .description = out_desc,
};

#ifdef HAVE_XDG_OUTPUT_PROTOCOL
static void xdg_out_pos(void *data, struct zxdg_output_v1 *xo, int32_t x, int32_t y)
{
    (void)xo;
    GrabWaylandOutput *o = (GrabWaylandOutput *)data;
    o->x = x;
    o->y = y;
    o->has_xdg_pos = 1;
}

static void xdg_out_size(void *data, struct zxdg_output_v1 *xo, int32_t w, int32_t h)
{
    (void)xo;
    GrabWaylandOutput *o = (GrabWaylandOutput *)data;
    o->logical_w = w;
    o->logical_h = h;
}

static void xdg_out_done(void *data, struct zxdg_output_v1 *xo) { (void)data; (void)xo; }
static void xdg_out_name(void *data, struct zxdg_output_v1 *xo, const char *n) { (void)data; (void)xo; (void)n; }
static void xdg_out_desc(void *data, struct zxdg_output_v1 *xo, const char *d) { (void)data; (void)xo; (void)d; }

static const struct zxdg_output_v1_listener xdg_out_listener = {
    .logical_position = xdg_out_pos,
    .logical_size = xdg_out_size,
    .done = xdg_out_done,
    .name = xdg_out_name,
    .description = xdg_out_desc,
};
#endif

/* --- zwlr_screencopy_frame_v1 listener --- */
static void start_frame_copy(GrabWaylandCtx *ctx, GrabWaylandOutput *o)
{
    if (o->copy_sent || o->failed || !o->got_buffer_info) return;
    o->shm_size = (size_t)o->stride * (size_t)o->height;
    if (o->shm_size == 0) {
        o->failed = 1;
        return;
    }
    o->shm_fd = create_anon_shm_fd(o->shm_size);
    if (o->shm_fd < 0) {
        o->failed = 1;
        return;
    }
    o->shm_data = mmap(NULL, o->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, o->shm_fd, 0);
    if (o->shm_data == MAP_FAILED) {
        o->shm_data = NULL;
        close(o->shm_fd);
        o->shm_fd = -1;
        o->failed = 1;
        return;
    }
    o->pool = wl_shm_create_pool(ctx->shm, o->shm_fd, (int32_t)o->shm_size);
    o->buffer = wl_shm_pool_create_buffer(
        o->pool, 0, (int32_t)o->width, (int32_t)o->height, (int32_t)o->stride, o->fmt
    );
    o->copy_sent = 1;
    zwlr_screencopy_frame_v1_copy(o->frame, o->buffer);
}

static void sc_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
                      uint32_t format, uint32_t width, uint32_t height, uint32_t stride)
{
    (void)frame;
    GrabWaylandOutput *o = (GrabWaylandOutput *)data;
    if (format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_XRGB8888 ||
        format == WL_SHM_FORMAT_ABGR8888 || format == WL_SHM_FORMAT_XBGR8888) {
        o->fmt = format;
        o->width = width;
        o->height = height;
        o->stride = stride;
        o->got_buffer_info = 1;
    }
}

static void sc_flags(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags)
{
    (void)frame;
    GrabWaylandOutput *o = (GrabWaylandOutput *)data;
    o->flags = flags;
}

static void sc_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
                     uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
    (void)frame; (void)tv_sec_hi; (void)tv_sec_lo; (void)tv_nsec;
    GrabWaylandOutput *o = (GrabWaylandOutput *)data;
    o->ready = 1;
}

static void sc_failed(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
    (void)frame;
    GrabWaylandOutput *o = (GrabWaylandOutput *)data;
    o->failed = 1;
}

static void sc_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
                      uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    (void)data; (void)frame; (void)x; (void)y; (void)w; (void)h;
}

static void sc_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *frame,
                      uint32_t fmt, uint32_t w, uint32_t h)
{
    (void)data; (void)frame; (void)fmt; (void)w; (void)h;
}

static void sc_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
    (void)frame;
    GrabWaylandOutput *o = (GrabWaylandOutput *)data;
    o->buffer_done = 1;
}

static const struct zwlr_screencopy_frame_v1_listener sc_listener = {
    .buffer = sc_buffer,
    .flags = sc_flags,
    .ready = sc_ready,
    .failed = sc_failed,
    .damage = sc_damage,
    .linux_dmabuf = sc_dmabuf,
    .buffer_done = sc_buffer_done,
};

/* --- wl_registry --- */
static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version)
{
    GrabWaylandCtx *ctx = (GrabWaylandCtx *)data;
    if (strcmp(iface, wl_shm_interface.name) == 0) {
        ctx->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        uint32_t v = version < 3 ? version : 3;
        ctx->screencopy_ver = v;
        ctx->screencopy_mgr = wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface, v);
    } else if (strcmp(iface, wl_output_interface.name) == 0 && ctx->num_outputs < MAX_GRAB_OUTPUTS) {
        uint32_t v = version < 4 ? version : 4;
        GrabWaylandOutput *o = &ctx->outputs[ctx->num_outputs++];
        memset(o, 0, sizeof(*o));
        o->shm_fd = -1;
        o->wl_out = wl_registry_bind(reg, name, &wl_output_interface, v);
        wl_output_add_listener(o->wl_out, &out_listener, o);
    }
#ifdef HAVE_XDG_OUTPUT_PROTOCOL
    else if (strcmp(iface, zxdg_output_manager_v1_interface.name) == 0) {
        uint32_t v = version < 3 ? version : 3;
        ctx->xdg_out_mgr = wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, v);
    }
#endif
}

static void reg_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener reg_listener = {
    .global = reg_global,
    .global_remove = reg_remove,
};

static int lowest_bit_ul(unsigned long v)
{
    int s = 0;
    if (!v) return 0;
    while ((v & 1UL) == 0) { v >>= 1; s++; }
    return s;
}

static int count_bits_ul(unsigned long v)
{
    int c = 0;
    while (v) { c += (int)(v & 1UL); v >>= 1; }
    return c;
}

static unsigned long pack_rgb_for_ximage(const XImage *img, unsigned char r, unsigned char g, unsigned char b)
{
    int rs = lowest_bit_ul(img->red_mask), rb = count_bits_ul(img->red_mask);
    int gs = lowest_bit_ul(img->green_mask), gb = count_bits_ul(img->green_mask);
    int bs = lowest_bit_ul(img->blue_mask), bb = count_bits_ul(img->blue_mask);
    unsigned long rv = rb > 0 ? ((unsigned long)r * ((1UL << rb) - 1UL) + 127U) / 255U : 0;
    unsigned long gv = gb > 0 ? ((unsigned long)g * ((1UL << gb) - 1UL) + 127U) / 255U : 0;
    unsigned long bv = bb > 0 ? ((unsigned long)b * ((1UL << bb) - 1UL) + 127U) / 255U : 0;
    return ((rv << rs) & img->red_mask) | ((gv << gs) & img->green_mask) | ((bv << bs) & img->blue_mask);
}

static int query_active_window_crop(Display *dpy, int include_frame,
                                    int *cx, int *cy, int *cw, int *ch)
{
    if (!dpy) return 0;
    Window root = DefaultRootWindow(dpy);

    /* 1. Prefer dtwm-wayland _DT_ACTIVE_WINDOW_BOX (covers both native Wayland and XWayland windows) */
    Atom box_atom = XInternAtom(dpy, "_DT_ACTIVE_WINDOW_BOX", True);
    if (box_atom != None) {
        Atom actual = None;
        int fmt = 0;
        unsigned long nitems = 0, after = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, root, box_atom, 0, 8, False, XA_CARDINAL,
                               &actual, &fmt, &nitems, &after, &data) == Success && data) {
            if (fmt == 32 && nitems >= 8) {
                const long *vals = (const long *)data;
                if (include_frame) {
                    *cx = (int)vals[0]; *cy = (int)vals[1];
                    *cw = (int)vals[2]; *ch = (int)vals[3];
                } else {
                    *cx = (int)vals[4]; *cy = (int)vals[5];
                    *cw = (int)vals[6]; *ch = (int)vals[7];
                }
                XFree(data);
                return (*cw > 0 && *ch > 0) ? 1 : 0;
            }
            XFree(data);
        }
    }

    /* 2. Fallback: XWayland focused window */
    Window focus = None;
    int revert = 0;
    XGetInputFocus(dpy, &focus, &revert);
    if (focus != None && focus != PointerRoot && focus != root) {
        XWindowAttributes attr;
        Window child = None;
        int rx = 0, ry = 0;
        if (XGetWindowAttributes(dpy, focus, &attr) && attr.map_state == IsViewable &&
            XTranslateCoordinates(dpy, focus, root, 0, 0, &rx, &ry, &child)) {
            *cx = rx; *cy = ry;
            *cw = attr.width; *ch = attr.height;
            return (*cw > 0 && *ch > 0) ? 1 : 0;
        }
    }
    return 0;
}

static void cleanup_wayland_ctx(GrabWaylandCtx *ctx)
{
    for (int i = 0; i < ctx->num_outputs; ++i) {
        GrabWaylandOutput *o = &ctx->outputs[i];
        if (o->frame) zwlr_screencopy_frame_v1_destroy(o->frame);
        if (o->buffer) wl_buffer_destroy(o->buffer);
        if (o->pool) wl_shm_pool_destroy(o->pool);
        if (o->shm_data) munmap(o->shm_data, o->shm_size);
        if (o->shm_fd >= 0) close(o->shm_fd);
#ifdef HAVE_XDG_OUTPUT_PROTOCOL
        if (o->xdg_out) zxdg_output_v1_destroy(o->xdg_out);
#endif
        if (o->wl_out) wl_output_destroy(o->wl_out);
    }
#ifdef HAVE_XDG_OUTPUT_PROTOCOL
    if (ctx->xdg_out_mgr) zxdg_output_manager_v1_destroy(ctx->xdg_out_mgr);
#endif
    if (ctx->screencopy_mgr) zwlr_screencopy_manager_v1_destroy(ctx->screencopy_mgr);
    if (ctx->shm) wl_shm_destroy(ctx->shm);
    if (ctx->registry) wl_registry_destroy(ctx->registry);
    if (ctx->display) wl_display_disconnect(ctx->display);
}

int ck_grab_capture_wayland(Display *dpy,
                            int target_window,
                            int include_frame,
                            int include_cursor,
                            XImage **out_image,
                            int *out_w,
                            int *out_h)
{
    const char *wl_env = getenv("WAYLAND_DISPLAY");
    if (!wl_env || !wl_env[0] || !dpy || !out_image) {
        return 0;
    }

    GrabWaylandCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.display = wl_display_connect(NULL);
    if (!ctx.display) {
        return 0;
    }

    ctx.registry = wl_display_get_registry(ctx.display);
    wl_registry_add_listener(ctx.registry, &reg_listener, &ctx);
    wl_display_roundtrip(ctx.display);

    if (!ctx.shm || !ctx.screencopy_mgr || ctx.num_outputs <= 0) {
        cleanup_wayland_ctx(&ctx);
        return 0;
    }

#ifdef HAVE_XDG_OUTPUT_PROTOCOL
    if (ctx.xdg_out_mgr) {
        for (int i = 0; i < ctx.num_outputs; ++i) {
            ctx.outputs[i].xdg_out = zxdg_output_manager_v1_get_xdg_output(
                ctx.xdg_out_mgr, ctx.outputs[i].wl_out
            );
            zxdg_output_v1_add_listener(ctx.outputs[i].xdg_out, &xdg_out_listener, &ctx.outputs[i]);
        }
    }
#endif
    wl_display_roundtrip(ctx.display);

    /* Request screencopy on every output */
    for (int i = 0; i < ctx.num_outputs; ++i) {
        GrabWaylandOutput *o = &ctx.outputs[i];
        o->frame = zwlr_screencopy_manager_v1_capture_output(
            ctx.screencopy_mgr, include_cursor ? 1 : 0, o->wl_out
        );
        zwlr_screencopy_frame_v1_add_listener(o->frame, &sc_listener, o);
    }

    for (;;) {
        int all_done = 1;
        for (int i = 0; i < ctx.num_outputs; ++i) {
            GrabWaylandOutput *o = &ctx.outputs[i];
            if (!o->copy_sent && o->got_buffer_info && (ctx.screencopy_ver < 3 || o->buffer_done)) {
                start_frame_copy(&ctx, o);
            }
            if (!o->ready && !o->failed) {
                all_done = 0;
            }
        }
        if (all_done) break;
        if (wl_display_dispatch(ctx.display) < 0) {
            cleanup_wayland_ctx(&ctx);
            return 0;
        }
    }

    /* Compute desktop bounding box across ready outputs */
    int desk_w = 0, desk_h = 0;
    int auto_x = 0;
    for (int i = 0; i < ctx.num_outputs; ++i) {
        GrabWaylandOutput *o = &ctx.outputs[i];
        if (!o->ready || !o->shm_data) continue;
        if (i > 0 && !o->has_xdg_pos && o->x == 0 && o->y == 0) {
            o->x = auto_x;
        }
        int ow = (int)o->width;
        int oh = (int)o->height;
        auto_x = o->x + ow;
        if (o->x + ow > desk_w) desk_w = o->x + ow;
        if (o->y + oh > desk_h) desk_h = o->y + oh;
    }
    if (desk_w <= 0 || desk_h <= 0) {
        cleanup_wayland_ctx(&ctx);
        return 0;
    }

    int crop_x = 0, crop_y = 0, crop_w = desk_w, crop_h = desk_h;
    if (target_window) {
        int wx = 0, wy = 0, ww = 0, wh = 0;
        if (!query_active_window_crop(dpy, include_frame, &wx, &wy, &ww, &wh)) {
            cleanup_wayland_ctx(&ctx);
            return 0;
        }
        if (wx < 0) { ww += wx; wx = 0; }
        if (wy < 0) { wh += wy; wy = 0; }
        if (wx + ww > desk_w) ww = desk_w - wx;
        if (wy + wh > desk_h) wh = desk_h - wy;
        if (ww <= 0 || wh <= 0) {
            cleanup_wayland_ctx(&ctx);
            return 0;
        }
        crop_x = wx; crop_y = wy; crop_w = ww; crop_h = wh;
    }

    int scr = DefaultScreen(dpy);
    Visual *vis = DefaultVisual(dpy, scr);
    int depth = DefaultDepth(dpy, scr);
    char *img_bytes = (char *)calloc((size_t)crop_w * (size_t)crop_h, 4);
    if (!img_bytes) {
        cleanup_wayland_ctx(&ctx);
        return 0;
    }

    XImage *ximg = XCreateImage(dpy, vis, (unsigned int)depth, ZPixmap, 0,
                                img_bytes, (unsigned int)crop_w, (unsigned int)crop_h, 32, 0);
    if (!ximg) {
        free(img_bytes);
        cleanup_wayland_ctx(&ctx);
        return 0;
    }

    for (int i = 0; i < ctx.num_outputs; ++i) {
        GrabWaylandOutput *o = &ctx.outputs[i];
        if (!o->ready || !o->shm_data) continue;
        int y_inv = (o->flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
        int is_bgr = (o->fmt == WL_SHM_FORMAT_ABGR8888 || o->fmt == WL_SHM_FORMAT_XBGR8888);

        for (uint32_t sy = 0; sy < o->height; ++sy) {
            int dy = o->y + (int)sy - crop_y;
            if (dy < 0 || dy >= crop_h) continue;
            uint32_t src_row_idx = y_inv ? (o->height - 1u - sy) : sy;
            const uint32_t *srow = (const uint32_t *)((const uint8_t *)o->shm_data + (size_t)src_row_idx * o->stride);

            for (uint32_t sx = 0; sx < o->width; ++sx) {
                int dx = o->x + (int)sx - crop_x;
                if (dx < 0 || dx >= crop_w) continue;
                uint32_t px = srow[sx];
                unsigned char r, g, b;
                if (is_bgr) {
                    r = (unsigned char)(px & 0xFFu);
                    g = (unsigned char)((px >> 8) & 0xFFu);
                    b = (unsigned char)((px >> 16) & 0xFFu);
                } else {
                    r = (unsigned char)((px >> 16) & 0xFFu);
                    g = (unsigned char)((px >> 8) & 0xFFu);
                    b = (unsigned char)(px & 0xFFu);
                }
                XPutPixel(ximg, dx, dy, pack_rgb_for_ximage(ximg, r, g, b));
            }
        }
    }

    cleanup_wayland_ctx(&ctx);
    *out_image = ximg;
    if (out_w) *out_w = crop_w;
    if (out_h) *out_h = crop_h;
    return 1;
}

#else /* !HAVE_WAYLAND_SCREENCOPY */

int ck_grab_capture_wayland(Display *dpy,
                            int target_window,
                            int include_frame,
                            int include_cursor,
                            XImage **out_image,
                            int *out_w,
                            int *out_h)
{
    (void)dpy; (void)target_window; (void)include_frame; (void)include_cursor;
    (void)out_image; (void)out_w; (void)out_h;
    return 0;
}

#endif /* HAVE_WAYLAND_SCREENCOPY */
