---
title: Task Manager (ck-tasks)
sidebar_label: Task Manager
description: Tabbed Motif system monitor inspecting X11 windows, Linux processes, init services, CPU/memory/load meters, and logged-in user sessions.
---

# Task Manager (`ck-tasks`)

`ck-tasks` (`TasksManager` X application class, window title **Task Manager**) is a multi-tab system and desktop monitor built on Motif's `XmTabStack` and the shared `CkTable` / `VerticalMeter` widgets.

## Notebook tabs

`ck-tasks` organizes system state across six tabs (opening on **Processes** by default) above a persistent four-segment status bar (`Processes`, `CPU Usage`, `Physical Memory`, and status text):

| Tab | Data sources | Capabilities |
| --- | --- | --- |
| **Applications** | Root X11 client properties (`_NET_CLIENT_LIST*`, `_DT_WM_CLIENT_LIST`, `XQueryTree`) + `/proc` | Lists top-level desktop windows (`Application`, `PID`, `Windows`, `Command`, `Class`) with live text filtering and a **Close Window** button (`WM_DELETE_WINDOW`). |
| **Processes** | `/proc/<pid>/{stat,status,cmdline,statm}` & `/proc/uptime` | Virtualized, sortable table (`CkTable` in `CK_TABLE_MODE_VIRTUAL`) showing `Image Name`, `PID`, `CPU (%)`, `Memory (MB)`, `Threads`, and `User/Session`, with **Show only my processes** (enabled by default) and substring filtering. |
| **Services** | Filesystem inspection of `systemd`, `BSD rc`, or `SysV` init trees | Detects the active init layout, lists services (`Order`, `Service`, `State`) with a **Show disabled services** toggle, and opens a bottom detail pane showing symlink paths and LSB `INIT INFO` headers when a row is clicked (clicking pauses background table refresh until deselected). |
| **Performance** | `/proc/stat`, `/proc/meminfo`, `/proc/loadavg` | Displays five segmented `VerticalMeter` columns (`CPU`, `Memory`, `Load 1m`, `Load 5m`, `Load 15m`, normalized per online core and auto-scaling above `100%`). |
| **Networking** | Static UI preview | Layout preview tab (currently populated with static sample interface entries). |
| **Users** | `utmp` (`USER_PROCESS`), `/dev/<tty>`, `/proc/<pid>/stat` | Lists active login sessions (`User`, `TTY`, `Login Time`, `Idle`, `Host`, `PID`) and provides **Logout user (SIGHUP)** with a confirmation dialog listing affected foreground-group processes (`kill(-pgrp, SIGHUP)`). |

## Menus

- **File:** **New Window** (spawns another `ck-tasks` process), **Connect to Remote...** *(placeholder)*, **Exit**.
- **Options:** **Always on Top** *(placeholder status toggle)*, **Update Frequency** (`1s`, `2s` default, `5s`), and **Filter by User** (synchronized with the Processes tab checkbox).
- **View:** **Refresh** (forces an immediate poll) and quick tab switches (**Show Processes**, **Show Performance**, **Show Networking**).
- **Help:** **View Help** and **About**.

## Command-line options and session state

```sh
ck-tasks [-session <session_id>] [Xt options...]
```

- **CDE session state:** Saves and restores window geometry (`x`, `y`, `w`, `h`) and the active tab (`current_tab`) across CDE sessions via `DtSessionSavePath` / `DtSessionRestorePath`.

## Current limitations

- **Processes** and **Services** tables are currently read-only (process termination and service start/stop controls are not yet wired; **Applications → Close Window** and **Users → Logout user (SIGHUP)** are active).
- **Networking**, the **Performance** CPU history frame, **Connect to Remote...**, and **Always on Top** are UI placeholders reserved for upcoming iterations.
- On `systemd` hosts, service inspection reads unit directories (`/run/systemd/system`, `/etc/systemd/system`, `/usr/lib/systemd/system`) directly without querying D-Bus, so unit enablement is reported primarily as `enabled` / `disabled`.
