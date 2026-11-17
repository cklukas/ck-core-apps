---
title: Screen Capture (ck-grab)
sidebar_label: Screen Capture
description: X11 and Wayland full-screen and active-window screenshot utility exporting PNG and XPM images with optional cursor and window-frame capture.
---

# Screen Capture (`ck-grab`)

`ck-grab` (`CkGrab` X application class, window title **Grab**) is a compact Motif screenshot tool for capturing either the full desktop or the currently active window, with optional capture delay, window manager frame inclusion, and pointer cursor compositing across both **X11/XLibre** (`XGetImage` + `XFixesGetCursorImage`) and **Wayland** (`zwlr_screencopy_manager_v1` under `dtwm-wayland` when `WAYLAND_DISPLAY` is set).

## Capture options

| Control / Menu item | Default | Description |
| --- | --- | --- |
| **Target** (`Full Screen` / `Active Window`) | `Full Screen` | Captures either the root window or the focused client window (`_NET_ACTIVE_WINDOW`, falling back to `XGetInputFocus`). |
| **Delay (s)** (`0`–`10`) | `0` | Countdown in seconds before reading screen pixels, allowing menus or transient UI states to be staged. |
| **Settings → Include Window Frame** | Enabled | Walks `XQueryTree` up to the top-level window-manager frame when **Target** is `Active Window`. Disabled when capturing `Full Screen`. |
| **Settings → Include Mouse Cursor** | Enabled | Composites the live pointer image (`XFixesGetCursorImage`) onto the captured screenshot. |
| **Settings → Hide Grab Window** | Enabled | Withdraws `ck-grab` before capturing so the tool does not appear in its own screenshot, then restores it afterward. |

## Saving screenshots

Clicking **Create Screenshot** (or pressing `Return`) captures the target image and opens **Save Screenshot As...** (`XmFileSelectionDialog`):

- **Output formats:** Choose **PNG** (8-bit RGBA via `libpng`) or **XPM** (`XpmWriteFileFromImage`) from the dialog's **Format** selector.
- **Automatic extension & overwrite check:** Changing the format updates the file filter (`*.png` or `*.xpm`) and appends the matching extension when omitted. Existing files prompt a modal **Overwrite File?** confirmation before writing.

## Keyboard shortcuts

| Action | Shortcut |
| --- | --- |
| **Create Screenshot** | `Return` |
| **Close** | `Escape` |
| **File → Exit** | `Alt+F4` |

## Command-line options and persistence

```sh
ck-grab [--capture <file> | -o <file>] [--window | --full-screen]
        [--include-frame | --no-frame] [--include-cursor | --no-cursor]
        [--delay <0..10>] [--format png|xpm] [-session <session_id>] [Xt options...]
```

- **Direct CLI capture (`--capture <file>` / `-o <file>`):** Captures the requested target immediately (after any `--delay`) and writes the PNG or XPM file without opening the interactive window. Under Wayland (`WAYLAND_DISPLAY`), `Full Screen` composites all active outputs via `zwlr_screencopy_manager_v1` and `Active Window` (`--window`) crops to `dtwm-wayland`'s `_DT_ACTIVE_WINDOW_BOX` (with or without its server-side CDE 3D bevel frame), capturing both native Wayland and XWayland windows cleanly.

- **User configuration:**
  - `$XDG_CONFIG_HOME/ck-core/ck-grab.view` (default `~/.config/ck-core/ck-grab.view`): persists `target`, `delay`, `include_frame`, `include_cursor`, `hide_window`, and `format`.
  - `$XDG_CONFIG_HOME/ck-core/ck-grab.paths` (default `~/.config/ck-core/ck-grab.paths`): persists `last_dir` (the last directory visited in the save dialog, defaulting to `$HOME`).
- **CDE session state:** Saves and restores window position (`x`, `y`, `w`, `h`) alongside `target`, `delay`, `include_frame`, `include_cursor`, `hide_window`, and `format`.

## Current limitations

- `ck-grab` captures either the full screen or the active window; interactive drag-rectangle region selection is not currently implemented.
- Capture delay runs synchronously on the UI thread before taking the X11 snapshot.
