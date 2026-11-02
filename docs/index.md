---
title: CK Core Apps
description: Native applications designed for CK Core CDE and CK Core Motif.
layout: home
hero:
  name: CK Core Apps
  text: Native tools for the maintained CDE desktop
  tagline: >
    A growing suite of Motif utilities, desktop companions, games and demos,
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
    details: Motif interfaces, CDE session handling and desktop-aware window behavior keep the suite at home in CDE.
    link: apps.md
  - icon: ":bar_chart:"
    title: Live desktop information
    details: Clock, load, eyes, market data, browser favicons and the plasma demo can update their high-resolution window icons.
    link: dynamic-icons.md
  - icon: ":art:"
    title: One toolkit cohort
    details: Build preflight and CI require CK Core Motif ABI 9 and reject an accidental system libXm.
    link: build.md
  - icon: ":toolbox:"
    title: Useful everyday tools
    details: Calculator, character map, screen capture, mixer, task manager and browser complement the classic CDE tools.
    link: apps.md
  - icon: ":video_game:"
    title: Games and demos
    details: Mines, Nibbles and animated plasma exercise the same native desktop stack.
    link: apps.md
  - icon: ":desktop_computer:"
    title: CK Core CDE
    details: Explore the maintained desktop environment where the suite integrates with sessions, windows and desktop icons.
    link: https://cklukas.github.io/ck-core-cde/
  - icon: "🏗️"
    title: CK Core Motif
    details: Explore the maintained native Xt toolkit that supplies the suite's widgets, resources and ABI.
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
release independently, while CI verifies them against the same current
CK Core Motif ABI 9 toolkit.

Start with the [application catalogue](apps.md), or see how
[high-resolution dynamic icons](dynamic-icons.md) turn ordinary desktop icons
into compact live views.
