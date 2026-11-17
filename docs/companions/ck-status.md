---
title: System Status & Notifications (ck-status)
sidebar_label: System Status
description: Workstation and laptop power/battery, Wi-Fi/network, Bluetooth, FreeDesktop Notifications server, StatusNotifier system tray, and Front Panel live gauge for CK Core CDE.
---

# System Status & Notifications (`ck-status`)

`ck-status` (`CkStatus` X application class) provides modern Linux desktop notifications, `StatusNotifier` background tray hosting, and laptop/workstation hardware status controls in an authentic 1995 SunSoft / IBM CDE Motif interface that runs identically on **CDE (Wayland)** and **CDE (X11 / XLibre)**.

## Subsystems

| Subsystem | Data source / D-Bus interface | Capabilities |
| --- | --- | --- |
| **Desktop Notifications** | `org.freedesktop.Notifications` (`/org/freedesktop/Notifications`) | Hosts standard Linux notifications (`notify-send`, browsers, mailers), displays 3D-beveled CDE toast banners above the Front Panel, supports *Do Not Disturb*, maintains a session history list, and toggles `$HOME/.dt/notifications.unread` for the Front Panel's central Switch indicator lamp. |
| **System Tray (`SNI`)** | `org.kde.StatusNotifierWatcher` & `org.freedesktop.StatusNotifierWatcher` (`/StatusNotifierWatcher`) | Hosts modern Linux background tray items, decodes network-byte-order ARGB32 `IconPixmap` icons onto Motif controls, tracks `NameOwnerChanged` disconnects, and dispatches `Activate`, `SecondaryActivate`, and `ContextMenu`. |
| **Power & Battery** | `/sys/class/power_supply/*` + `/sys/firmware/acpi/platform_profile` | Displays a 10-cell segmented Fuel-Gauge bar, AC/battery status, charge percentage, watt draw, estimated runtime/charge time, and platform power profile. |
| **Network & Wi-Fi** | `org.freedesktop.NetworkManager` + `/sys/class/net/*` + `/proc/net/wireless` + `getifaddrs()` | Displays primary link status, IPv4/IPv6 addresses, RX/TX byte counters, Wi-Fi radio enable toggle, scanned SSIDs with signal bars, and one-click connection setup (`nm-connection-editor` or `dtterm -e nmtui`). |
| **Bluetooth** | `org.bluez` (`ObjectManager`) + `/sys/class/bluetooth/*` | Displays adapter power state, toggles `org.bluez.Adapter1.Powered`, lists paired/discovered devices, and invokes `Connect` / `Disconnect`. |

## Front Panel integration and modes

`ck-status` integrates with the genuine CDE Front Panel (`dtwm` and `dtwm-panel`) in three complementary ways:

1. **Central Switch Notification Lamp (`dtwm.fp`):** Slot 3 of the central workspace Switch monitors `$HOME/.dt/notifications.unread` (`MONITOR_TYPE file`), lighting up automatically when unread notifications arrive and launching `ck-status --tab notifications` when clicked.
2. **Slide-Up *System & Hardware* Subpanel (`SystemSubpanel`):** Provides dedicated controls for *System Status*, *Power & Battery*, *Network & Wi-Fi*, *Bluetooth*, *Audio Mixer* (`ck-mixer`), and *Monitors & Display*. Right-clicking any subpanel item and choosing **Copy To Main Panel** promotes it permanently onto the main Front Panel.
3. **Interactive 4-Quadrant Live Gauge (`ck-status --gauge`):** Embeds a `54×48` `TYPE client` (`CLIENT_NAME ck-status-gauge`) instrument cluster in `~/.dt/types/ckstatus_gauge.fp` showing Wi-Fi signal bars, Bluetooth + notification/SNI lamp, and segmented battery/AC fuel gauge. Clicking individual quadrants opens the corresponding tab directly.

## Command-line options

```sh
ck-status [--tab overview|notifications|power|network|bluetooth]
          [--daemon] [--gauge] [--pin-gauge | --unpin-gauge] [--standalone]
```

- `--daemon`: Starts unmapped as the session notification and `StatusNotifierWatcher` service (automatically invoked by `Xsession.wayland`); subsequent `ck-status --tab <name>` invocations raise and switch the running instance over `org.ckcore.Status`.
- `--gauge`: Runs as a compact `54×48` Front Panel `TYPE client` instrument cluster (`WM_CLASS` `ck-status-gauge`).
- `--pin-gauge` / `--unpin-gauge`: Installs or removes `~/.dt/types/ckstatus_gauge.fp` and reloads the Front Panel.
