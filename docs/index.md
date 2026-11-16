---
title: CK Core Apps
description: Native applications designed for CK Core CDE and CK Core Motif.
layout: home
hero:
  name: CK Core Apps
  text: Native tools for the maintained CDE desktop
  tagline: >
    Fourteen Motif utilities, live desktop companions, games and demos,
    built with CK Core Motif ABI 9 and integrated with CK Core CDE.
  image:
    src: images/desktop.png
    alt: CK Core Apps running together on a high-resolution CDE desktop
  actions:
    - text: Browse the apps
      link: apps.md
      theme: brand
    - text: See dynamic icons
      link: dynamic-icons.md
    - text: Build the suite
      link: build.md
features:
  - icon: ":desktop_computer:"
    title: CDE-native interaction
    details: Motif interfaces, CDE palette awareness, session save/restore and desktop-aware window behavior keep the suite at home in CDE.
    link: architecture.md
  - icon: ":bar_chart:"
    title: Live desktop information
    details: Clock, System Load, Eyes, Coins, Browser favicons and the Plasma demo render responsive 32x32 to 96x96 window icons.
    link: dynamic-icons.md
  - icon: ":art:"
    title: One toolkit cohort
    details: Build preflight and CI require CK Core Motif ABI 9 and reject an accidental system libXm.
    link: build.md
  - icon: ":toolbox:"
    title: Everyday desktop tools
    details: About, Calculator, Character Map, Screen Capture, Volume Control, Task Manager and Web Browser complement classic CDE utilities.
    link: tools/ck-calc.md
  - icon: ":video_game:"
    title: Games and demos
    details: Mines, Nibbles and multi-process animated Plasma exercise the same native Motif/X11 desktop stack.
    link: games/ck-mines.md
  - icon: ":desktop_computer:"
    title: CK Core CDE
    details: Explore the maintained desktop environment where the suite integrates with sessions, windows and desktop icons.
    link: https://cklukas.github.io/ck-core-cde/
  - icon: "🏗️"
    title: CK Core Motif
    details: Explore the maintained native Xt toolkit that supplies the suite's widgets, resources and ABI 9 contract.
    link: https://cklukas.github.io/ck-core-motif/
  - icon: ":file_folder:"
    title: ck-core-fs-lib
    details: Explore the headless, cross-platform filesystem engine and stable C ABI shared by native CK Core clients.
    link: https://cklukas.github.io/ck-core-fs-lib/
---

# A companion suite, not a CDE fork

CK Core Apps is the optional application layer for
[CK Core CDE](https://cklukas.github.io/ck-core-cde/). Keeping it in a separate
repository lets the desktop environment and the application suite evolve and
release independently, while CI verifies them together against the current
[CK Core Motif](https://cklukas.github.io/ck-core-motif/) ABI 9 toolkit.

Start with the [application catalogue](apps.md), explore the
[shared session and configuration architecture](architecture.md), or see how
[high-resolution dynamic icons](dynamic-icons.md) turn ordinary desktop icons
into compact live views.
