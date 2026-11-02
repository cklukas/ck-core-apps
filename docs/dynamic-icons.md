---
title: High-resolution dynamic icons
description: Live application state rendered into CK Core CDE desktop icons.
---

# High-resolution dynamic icons

CK Core Apps can publish a newly rendered `WM_HINTS` icon pixmap whenever
application state changes. CK Core CDE's open-window icon mode keeps that
client-supplied image visible on the desktop while the normal window is open,
and its larger icon-size setting gives the live view enough space to remain
legible.

This is an X11 window-icon contract, not a separate widget layer or a Front
Panel embedding mechanism. The owning application renders the pixels; dtwm
places and refreshes the icon.

## Implemented applications

| Application | Live icon content | Update trigger |
| --- | --- | --- |
| `ck-clock` | Analog clock face | Minute change and icon-state refresh, including open-window icons |
| `ck-load` | CPU and RAM bars | Every second, including open-window icons |
| `ck-eyes` | Eyes and pupils following the pointer | Every 50 ms while minimized |
| `ck-browser` | Favicon of the active browser tab | Active tab or page favicon change, including open-window icons |
| `ck-coins` | Selected coin symbol and price | Selection and market-data refresh, including open-window icons |
| `ck-plasma-1` | Animated plasma frame | Each generated frame while minimized |

`ck-clock`, `ck-load`, `ck-eyes`, `ck-browser` and `ck-coins` are user-facing
applications. `ck-plasma-1` is a demo of the same update contract.

Eyes and Plasma render their compact path when the client enters the minimized
state. Clock, Load, Browser and Coins update their icon pixmaps while their
normal windows remain open, so CK Core CDE's open-window icons can act as live
desktop views without minimizing those applications.

![CK Core CDE desktop with live Eyes, Clock, Plasma, System Load and Coins icons beside their application windows](images/dynamic-icons-desktop.png)

*A fresh uConsole smoke session running CK Core CDE, CK Core Motif ABI 9 and
five CK Core Apps. Eyes and Plasma are minimized; Clock, System Load and Coins
continue updating their desktop icons while their normal windows are open.*

### Motion in the minimized icon path

The two frames below were captured four seconds apart after moving the pointer.
They show that Eyes redraws its pupils and Plasma publishes a new animation
frame through the minimized-window icon path.

| Earlier frame | Later frame |
| --- | --- |
| ![Eyes and Plasma dynamic icons, earlier frame](images/dynamic-icons-motion-a.png) | ![Eyes and Plasma dynamic icons, later frame](images/dynamic-icons-motion-b.png) |

![Clock rendered as a live desktop icon](images/ck-clock-3.png)

![CPU and memory rendered in the System Load desktop icon](images/ck-load-2.png)

## CDE settings

In CK Core CDE, enable **Show Icons for Open Windows** and select the larger
window-icon size in Style Manager. The corresponding dtwm resource is
`Dtwm*showOpenWindowIcons`.

The implementation uses the icon geometry advertised by dtwm when available,
then publishes `IconPixmapHint` and, where needed, `IconMaskHint`. This keeps
the apps independent of dtwm internals while allowing CK Core CDE to present
their high-resolution output.

See [Window-aware desktop icons](https://cklukas.github.io/ck-core-cde/improvements/window-state.html)
for the window-manager side of the feature.
