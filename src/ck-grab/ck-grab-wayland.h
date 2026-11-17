#ifndef CK_GRAB_WAYLAND_H
#define CK_GRAB_WAYLAND_H

#include <X11/Xlib.h>

/*
 * Captures a screenshot via Wayland zwlr_screencopy_manager_v1 when
 * WAYLAND_DISPLAY is active.
 *
 * target_window: 0 = Full Screen, 1 = Active Window (_DT_ACTIVE_WINDOW_BOX / XWayland focus)
 * include_frame: 1 = include server-side CDE bevel frame when target_window == 1
 * include_cursor: 1 = ask compositor to composite pointer cursor into frame
 *
 * Returns 1 on success (*out_image owned by caller, free via XDestroyImage),
 * or 0 when Wayland screencopy is unavailable so caller can fall back to X11.
 */
int ck_grab_capture_wayland(Display *dpy,
                            int target_window,
                            int include_frame,
                            int include_cursor,
                            XImage **out_image,
                            int *out_w,
                            int *out_h);

#endif /* CK_GRAB_WAYLAND_H */
