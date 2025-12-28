/*
 * ck-plasma-1.c - Motif plasma/checker animation demo.
 *
 * Based on the simple checker animation idea from:
 * https://gist.github.com/rexim/ef86bf70918034a5a57881456c0a0ccf
 *
 * Build example (Debian/Devuan):
 *   gcc -O2 -Wall -o ck-plasma-1 ck-plasma-1.c -lX11 -lXm -lXt
 */

#include <Xm/DrawingA.h>
#include <Xm/Protocols.h>
#include <Xm/Xm.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <Dt/Dt.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "../../shared/session_utils.h"
#include "plasma_renderer.h"

#define CK_PLASMA_TITLE "Plasma Animation"
#define CK_PLASMA_MAX_FPS 30
#define CK_PLASMA_MAX_PENDING 2
#define CK_PLASMA_ICON_SIZE 96

typedef enum {
    CK_PLASMA_FRAME_WINDOW = 0,
    CK_PLASMA_FRAME_ICON = 1
} CkPlasmaFrameType;

typedef struct {
    int generation;
    int frame_index;
    int width;
    int height;
    int type;
} CkPlasmaTask;

typedef struct {
    int generation;
    int frame_index;
    int width;
    int height;
    int type;
    int data_size;
} CkPlasmaResultHeader;

typedef struct CkPlasmaFrame {
    int generation;
    int frame_index;
    int width;
    int height;
    int type;
    unsigned char *data;
    struct CkPlasmaFrame *next;
} CkPlasmaFrame;

typedef struct {
    pid_t pid;
    int to_child;
    int from_child;
    XtInputId input_id;
} CkPlasmaWorker;

typedef struct {
    XtAppContext app_ctx;
    Widget toplevel;
    Widget drawing_area;
    Display *display;
    int screen;
    GC gc;

    Pixmap pixmap;
    int pixmap_w;
    int pixmap_h;

    Pixmap icon_pixmap;
    int icon_w;
    int icon_h;

    int target_w;
    int target_h;
    int iconified;
    int generation;

    int next_request_frame;
    int next_display_frame;
    int outstanding;
    int next_worker;

    int num_workers;
    CkPlasmaWorker *workers;
    CkPlasmaFrame *frames;
    int queued_frames;
    SessionData *session_data;
    char exec_path[PATH_MAX];
} CkPlasmaApp;

static void ck_plasma_schedule_tasks(CkPlasmaApp *app);
static void ck_plasma_clear_frames(CkPlasmaApp *app);

static int read_full(int fd, void *buf, size_t len)
{
    unsigned char *dst = (unsigned char *)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t got = read(fd, dst + total, len - total);
        if (got == 0) return 0;
        if (got < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)got;
    }
    return 1;
}

static int write_full(int fd, const void *buf, size_t len)
{
    const unsigned char *src = (const unsigned char *)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t sent = write(fd, src + total, len - total);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)sent;
    }
    return 1;
}

static void worker_loop(int read_fd, int write_fd)
{
    for (;;) {
        CkPlasmaTask task;
        int status = read_full(read_fd, &task, sizeof(task));
        if (status <= 0) break;
        if (task.width <= 0 || task.height <= 0) continue;

        size_t data_size = (size_t)task.width * (size_t)task.height * 4;
        unsigned char *buffer = (unsigned char *)malloc(data_size);
        if (!buffer) continue;

        int sequence_id = task.frame_index / CK_PLASMA_RENDERER_TIME_STEPS;
        ck_plasma_render_frame(buffer, task.width, task.height, task.frame_index, sequence_id);

        CkPlasmaResultHeader header;
        header.generation = task.generation;
        header.frame_index = task.frame_index;
        header.width = task.width;
        header.height = task.height;
        header.type = task.type;
        header.data_size = (int)data_size;

        if (write_full(write_fd, &header, sizeof(header)) <= 0 ||
            write_full(write_fd, buffer, data_size) <= 0) {
            free(buffer);
            break;
        }
        free(buffer);
    }
    _exit(0);
}

static int ck_plasma_window_is_iconified(const CkPlasmaApp *app)
{
    if (!app || !app->toplevel || !XtIsRealized(app->toplevel)) return 0;
    Display *display = XtDisplay(app->toplevel);
    if (!display) return 0;
    Window window = XtWindow(app->toplevel);
    if (!window) return 0;

    Atom prop = XInternAtom(display, "WM_STATE", True);
    if (prop == None) return 0;

    Atom actual;
    int format;
    unsigned long nitems;
    unsigned long bytes_after;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(display, window, prop, 0, 2, False,
                                    AnyPropertyType, &actual,
                                    &format, &nitems, &bytes_after, &data);
    if (status != Success || !data) return 0;

    long state = -1;
    if (nitems >= 1) {
        long *longs = (long *)data;
        state = longs[0];
    }
    XFree(data);
    return (state == IconicState);
}

static void ck_plasma_update_wm_icon_pixmap(CkPlasmaApp *app)
{
    if (!app || !app->display || !app->toplevel || !XtIsRealized(app->toplevel)) return;
    Window window = XtWindow(app->toplevel);
    if (!window) return;

    XWMHints *hints = XGetWMHints(app->display, window);
    XWMHints local;
    if (hints) {
        local = *hints;
        XFree(hints);
    } else {
        memset(&local, 0, sizeof(local));
    }
    local.flags |= IconPixmapHint;
    local.icon_pixmap = app->icon_pixmap;
    XSetWMHints(app->display, window, &local);
}

static void ck_plasma_present_pixmap(CkPlasmaApp *app)
{
    if (!app || !app->display || !app->drawing_area || !XtIsRealized(app->drawing_area)) return;
    if (app->pixmap == None || app->pixmap_w <= 0 || app->pixmap_h <= 0) return;
    Window win = XtWindow(app->drawing_area);
    if (!win) return;

    XCopyArea(app->display, app->pixmap, win, app->gc,
              0, 0, (unsigned int)app->pixmap_w, (unsigned int)app->pixmap_h, 0, 0);
}

static void ck_plasma_store_frame_sorted(CkPlasmaApp *app, CkPlasmaFrame *frame)
{
    if (!app || !frame) return;
    CkPlasmaFrame **cursor = &app->frames;
    while (*cursor && (*cursor)->frame_index < frame->frame_index) {
        cursor = &(*cursor)->next;
    }
    frame->next = *cursor;
    *cursor = frame;
    app->queued_frames++;
}

static CkPlasmaFrame *ck_plasma_pop_oldest_frame(CkPlasmaApp *app)
{
    if (!app || !app->frames) return NULL;
    CkPlasmaFrame *out = app->frames;
    app->frames = out->next;
    out->next = NULL;
    if (app->queued_frames > 0) {
        app->queued_frames--;
    }
    return out;
}

static void ck_plasma_clear_frames(CkPlasmaApp *app)
{
    if (!app) return;
    CkPlasmaFrame *frame = app->frames;
    while (frame) {
        CkPlasmaFrame *next = frame->next;
        free(frame->data);
        free(frame);
        frame = next;
    }
    app->frames = NULL;
    app->queued_frames = 0;
}

static void ck_plasma_consume_frame(CkPlasmaApp *app, CkPlasmaFrame *frame)
{
    if (!app || !frame) return;

    if (frame->type == CK_PLASMA_FRAME_ICON) {
        if (app->icon_pixmap != None) {
            XFreePixmap(app->display, app->icon_pixmap);
        }
        Pixmap icon_pix = XCreatePixmap(app->display,
                                        RootWindow(app->display, app->screen),
                                        (unsigned int)frame->width,
                                        (unsigned int)frame->height,
                                        DefaultDepth(app->display, app->screen));
        if (icon_pix != None) {
            XImage *image = XCreateImage(app->display,
                                         DefaultVisual(app->display, app->screen),
                                         DefaultDepth(app->display, app->screen),
                                         ZPixmap, 0,
                                         (char *)frame->data,
                                         (unsigned int)frame->width,
                                         (unsigned int)frame->height,
                                         32, 0);
            if (image) {
                XPutImage(app->display, icon_pix, app->gc, image, 0, 0, 0, 0,
                          (unsigned int)frame->width,
                          (unsigned int)frame->height);
                XDestroyImage(image);
                app->icon_pixmap = icon_pix;
                XtVaSetValues(app->toplevel, XmNiconPixmap, app->icon_pixmap, NULL);
                ck_plasma_update_wm_icon_pixmap(app);
            } else {
                XFreePixmap(app->display, icon_pix);
                free(frame->data);
            }
        } else {
            free(frame->data);
        }
    } else {
        if (app->pixmap != None) {
            XFreePixmap(app->display, app->pixmap);
        }
        app->pixmap_w = frame->width;
        app->pixmap_h = frame->height;
        Pixmap pix = XCreatePixmap(app->display,
                                   RootWindow(app->display, app->screen),
                                   (unsigned int)frame->width,
                                   (unsigned int)frame->height,
                                   DefaultDepth(app->display, app->screen));
        if (pix != None) {
            XImage *image = XCreateImage(app->display,
                                         DefaultVisual(app->display, app->screen),
                                         DefaultDepth(app->display, app->screen),
                                         ZPixmap, 0,
                                         (char *)frame->data,
                                         (unsigned int)frame->width,
                                         (unsigned int)frame->height,
                                         32, 0);
            if (image) {
                XPutImage(app->display, pix, app->gc, image, 0, 0, 0, 0,
                          (unsigned int)frame->width,
                          (unsigned int)frame->height);
                XDestroyImage(image);
                app->pixmap = pix;
                ck_plasma_present_pixmap(app);
            } else {
                XFreePixmap(app->display, pix);
                free(frame->data);
            }
        } else {
            free(frame->data);
        }
    }

    free(frame);
}

static void ck_plasma_worker_input(XtPointer client_data, int *source, XtInputId *id)
{
    (void)source;
    (void)id;
    CkPlasmaApp *app = (CkPlasmaApp *)client_data;
    if (!app) return;

    CkPlasmaResultHeader header;
    int status = read_full(*source, &header, sizeof(header));
    if (status <= 0) return;

    if (app->outstanding > 0) {
        app->outstanding--;
    }

    size_t expected_size = (size_t)header.width * (size_t)header.height * 4;
    if (header.data_size <= 0 || (size_t)header.data_size != expected_size) {
        return;
    }

    unsigned char *data = (unsigned char *)malloc((size_t)header.data_size);
    if (!data) return;

    if (read_full(*source, data, (size_t)header.data_size) <= 0) {
        free(data);
        return;
    }

    if (header.generation != app->generation) {
        free(data);
        return;
    }

    CkPlasmaFrame *frame = (CkPlasmaFrame *)calloc(1, sizeof(CkPlasmaFrame));
    if (!frame) {
        free(data);
        return;
    }
    frame->generation = header.generation;
    frame->frame_index = header.frame_index;
    frame->width = header.width;
    frame->height = header.height;
    frame->type = header.type;
    frame->data = data;
    ck_plasma_store_frame_sorted(app, frame);
    ck_plasma_schedule_tasks(app);
}

static void ck_plasma_schedule_tasks(CkPlasmaApp *app)
{
    if (!app || app->num_workers <= 0) return;
    if (app->target_w <= 0 || app->target_h <= 0) return;

    /* Cap the queue to the configured FPS target so we don't build up work we can't display. */
    int max_outstanding = CK_PLASMA_MAX_FPS;
    if (max_outstanding <= 0) max_outstanding = 1;
    if (app->num_workers > 0 && max_outstanding > app->num_workers) {
        max_outstanding = app->num_workers;
    }
    int max_pending = CK_PLASMA_MAX_PENDING;
    if (max_pending <= 0) max_pending = 1;
    while (app->outstanding < max_outstanding &&
           app->outstanding + app->queued_frames < max_pending) {
        CkPlasmaTask task;
        task.generation = app->generation;
        task.frame_index = app->next_request_frame;
        task.width = app->target_w;
        task.height = app->target_h;
        task.type = app->iconified ? CK_PLASMA_FRAME_ICON : CK_PLASMA_FRAME_WINDOW;

        CkPlasmaWorker *worker = &app->workers[app->next_worker];
        if (write_full(worker->to_child, &task, sizeof(task)) <= 0) {
            return;
        }
        app->outstanding++;
        app->next_request_frame++;
        app->next_worker = (app->next_worker + 1) % app->num_workers;
    }
}

static void ck_plasma_timer_cb(XtPointer client_data, XtIntervalId *id)
{
    (void)id;
    CkPlasmaApp *app = (CkPlasmaApp *)client_data;
    if (!app) return;

    int now_iconified = ck_plasma_window_is_iconified(app);
    if (now_iconified != app->iconified) {
        app->iconified = now_iconified;
        app->generation++;
        app->next_request_frame = 0;
        app->next_display_frame = 0;
        ck_plasma_clear_frames(app);
        if (app->iconified) {
            app->target_w = app->icon_w;
            app->target_h = app->icon_h;
        }
    }

    if (app->iconified) {
        app->target_w = app->icon_w;
        app->target_h = app->icon_h;
    } else if (app->drawing_area) {
        Dimension width = 0;
        Dimension height = 0;
        XtVaGetValues(app->drawing_area, XmNwidth, &width, XmNheight, &height, NULL);
        int new_w = (int)width;
        int new_h = (int)height;
        if (new_w > 0 && new_h > 0 &&
            (new_w != app->target_w || new_h != app->target_h)) {
            app->target_w = new_w;
            app->target_h = new_h;
            app->generation++;
            app->next_request_frame = 0;
            app->next_display_frame = 0;
            ck_plasma_clear_frames(app);
        }
    }

    CkPlasmaFrame *frame = app->frames;
    if (frame) {
        if (frame->frame_index > app->next_display_frame) {
            app->next_display_frame = frame->frame_index;
        }
        frame = ck_plasma_pop_oldest_frame(app);
        if (frame) {
            ck_plasma_consume_frame(app, frame);
            app->next_display_frame++;
        }
    }

    ck_plasma_schedule_tasks(app);

    if (app->app_ctx) {
        XtAppAddTimeOut(app->app_ctx, 1000 / CK_PLASMA_MAX_FPS, ck_plasma_timer_cb, app);
    }
}

static void ck_plasma_expose_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    CkPlasmaApp *app = (CkPlasmaApp *)client_data;
    ck_plasma_present_pixmap(app);
}

static void ck_plasma_resize_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)call_data;
    CkPlasmaApp *app = (CkPlasmaApp *)client_data;
    if (!app || !w) return;

    Dimension width = 0;
    Dimension height = 0;
    XtVaGetValues(w, XmNwidth, &width, XmNheight, &height, NULL);

    int new_w = (int)width;
    int new_h = (int)height;
    if (new_w <= 0 || new_h <= 0) return;

    if (new_w != app->target_w || new_h != app->target_h) {
        app->target_w = new_w;
        app->target_h = new_h;
        if (!app->iconified) {
            app->generation++;
            app->next_request_frame = 0;
            app->next_display_frame = 0;
            ck_plasma_clear_frames(app);
        }
    }
}

static void ck_plasma_wm_delete_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    CkPlasmaApp *app = (CkPlasmaApp *)client_data;
    if (!app) return;
    if (app->app_ctx) {
        XtAppSetExitFlag(app->app_ctx);
    }
}

static void ck_plasma_session_save_cb(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)call_data;
    CkPlasmaApp *app = (CkPlasmaApp *)client_data;
    if (!app || !app->session_data) return;
    session_capture_geometry(w, app->session_data, "x", "y", "w", "h");
    const char *exec_path = app->exec_path[0] ? app->exec_path : NULL;
    session_save(w, app->session_data, exec_path);
}

static int ck_plasma_spawn_workers(CkPlasmaApp *app)
{
    int workers = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (workers <= 0) workers = 1;

    app->workers = (CkPlasmaWorker *)calloc((size_t)workers, sizeof(CkPlasmaWorker));
    if (!app->workers) return 0;

    app->num_workers = workers;

    for (int i = 0; i < workers; ++i) {
        int to_child[2];
        int from_child[2];
        if (pipe(to_child) != 0) return 0;
        if (pipe(from_child) != 0) return 0;

        pid_t pid = fork();
        if (pid < 0) return 0;
        if (pid == 0) {
            close(to_child[1]);
            close(from_child[0]);
            worker_loop(to_child[0], from_child[1]);
        }

        close(to_child[0]);
        close(from_child[1]);
        app->workers[i].pid = pid;
        app->workers[i].to_child = to_child[1];
        app->workers[i].from_child = from_child[0];
        app->workers[i].input_id = XtAppAddInput(app->app_ctx, from_child[0],
                                                 (XtPointer)XtInputReadMask,
                                                 ck_plasma_worker_input, app);
    }
    return 1;
}

static void ck_plasma_shutdown(CkPlasmaApp *app)
{
    if (!app) return;
    for (int i = 0; i < app->num_workers; ++i) {
        if (app->workers[i].to_child >= 0) {
            close(app->workers[i].to_child);
        }
        if (app->workers[i].from_child >= 0) {
            close(app->workers[i].from_child);
        }
    }
    free(app->workers);
    app->workers = NULL;
    app->num_workers = 0;

    ck_plasma_clear_frames(app);

    if (app->pixmap != None && app->display) {
        XFreePixmap(app->display, app->pixmap);
    }
    if (app->icon_pixmap != None && app->display) {
        XFreePixmap(app->display, app->icon_pixmap);
    }
    if (app->session_data) {
        session_data_free(app->session_data);
        app->session_data = NULL;
    }
}

static void ck_plasma_init_exec_path(CkPlasmaApp *app, const char *argv0)
{
    if (!app) return;

    ssize_t len = readlink("/proc/self/exe", app->exec_path, sizeof(app->exec_path) - 1);
    if (len > 0) {
        if (len >= (ssize_t)sizeof(app->exec_path)) {
            len = (ssize_t)sizeof(app->exec_path) - 1;
        }
        app->exec_path[len] = '\0';
        return;
    }

    if (!argv0 || !argv0[0]) {
        app->exec_path[0] = '\0';
        return;
    }

    if (argv0[0] == '/') {
        strncpy(app->exec_path, argv0, sizeof(app->exec_path) - 1);
        app->exec_path[sizeof(app->exec_path) - 1] = '\0';
        return;
    }

    if (strchr(argv0, '/')) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            size_t cwd_len = strlen(cwd);
            size_t argv_len = strlen(argv0);
            if (cwd_len + 1 + argv_len < sizeof(app->exec_path)) {
                memcpy(app->exec_path, cwd, cwd_len);
                app->exec_path[cwd_len] = '/';
                memcpy(app->exec_path + cwd_len + 1, argv0, argv_len);
                app->exec_path[cwd_len + 1 + argv_len] = '\0';
                return;
            }
        }
    }

    strncpy(app->exec_path, argv0, sizeof(app->exec_path) - 1);
    app->exec_path[sizeof(app->exec_path) - 1] = '\0';
}

int main(int argc, char **argv)
{
    CkPlasmaApp app;
    memset(&app, 0, sizeof(app));
    char *session_id = session_parse_argument(&argc, argv);
    app.session_data = session_data_create(session_id);
    free(session_id);
    ck_plasma_init_exec_path(&app, argv[0]);
    app.icon_w = CK_PLASMA_ICON_SIZE;
    app.icon_h = CK_PLASMA_ICON_SIZE;
    app.generation = 1;

    app.toplevel = XtAppInitialize(&app.app_ctx, "CkPlasma",
                                   NULL, 0, &argc, argv, NULL, NULL, 0);
    if (!app.toplevel) {
        fprintf(stderr, "ck-plasma-1: XtAppInitialize failed\n");
        return 1;
    }

    DtInitialize(XtDisplay(app.toplevel), app.toplevel, "CkPlasma", "CkPlasma");

    XtVaSetValues(app.toplevel,
                  XmNtitle, CK_PLASMA_TITLE,
                  XmNiconName, CK_PLASMA_TITLE,
                  XmNwidth, 640,
                  XmNheight, 480,
                  NULL);

    app.drawing_area = XmCreateDrawingArea(app.toplevel, "drawing", NULL, 0);
    XtVaSetValues(app.drawing_area,
                  XmNwidth, 640,
                  XmNheight, 480,
                  NULL);
    XtAddCallback(app.drawing_area, XmNexposeCallback, ck_plasma_expose_cb, &app);
    XtAddCallback(app.drawing_area, XmNresizeCallback, ck_plasma_resize_cb, &app);
    XtManageChild(app.drawing_area);

    if (app.session_data && session_load(app.toplevel, app.session_data)) {
        session_apply_geometry(app.toplevel, app.session_data, "x", "y", "w", "h");
    }

    Atom wm_delete = XmInternAtom(XtDisplay(app.toplevel), "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(app.toplevel, wm_delete, ck_plasma_wm_delete_cb, &app);
    XmActivateWMProtocol(app.toplevel, wm_delete);

    Atom wm_save = XmInternAtom(XtDisplay(app.toplevel), "WM_SAVE_YOURSELF", False);
    XmAddWMProtocolCallback(app.toplevel, wm_save, ck_plasma_session_save_cb, &app);
    XmActivateWMProtocol(app.toplevel, wm_save);

    XtRealizeWidget(app.toplevel);

    app.display = XtDisplay(app.toplevel);
    app.screen = DefaultScreen(app.display);
    app.gc = XCreateGC(app.display, RootWindow(app.display, app.screen), 0, NULL);

    Dimension w = 0;
    Dimension h = 0;
    XtVaGetValues(app.drawing_area, XmNwidth, &w, XmNheight, &h, NULL);
    app.target_w = (int)w;
    app.target_h = (int)h;

    if (!ck_plasma_spawn_workers(&app)) {
        fprintf(stderr, "ck-plasma-1: failed to spawn workers\n");
        return 1;
    }

    ck_plasma_schedule_tasks(&app);
    XtAppAddTimeOut(app.app_ctx, 1000 / CK_PLASMA_MAX_FPS, ck_plasma_timer_cb, &app);

    XtAppMainLoop(app.app_ctx);

    ck_plasma_shutdown(&app);
    if (app.gc && app.display) {
        XFreeGC(app.display, app.gc);
    }
    return 0;
}
