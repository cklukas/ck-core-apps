## 0. Overall vision

* **Backend**: ALSA mixer API (per-card mixer, elements, volumes, switches, enums).
* **UI style**: Win95-like:

  * Top-left: **device combo box** + **“Use this device as default”** checkbox.
  * Below: **horizontal row of vertical columns**, one column per mixer element:

    * label at top (e.g. “Master”)
    * current volume text (e.g. “75%”)
    * vertical slider
    * mute / select / capture checkboxes at bottom (if applicable).
* **Sync with external changes**:

  * If the mixer state changes elsewhere (other app, second instance), your UI should update.

---

## 1. ALSA backend design

### 1.1 Decide ALSA scope & dependencies

* Use **ALSA “simple element” API** (`snd_mixer_selem_*`) to keep it manageable.
* Start with **playback controls** only (Master, PCM, Headphone, etc.), later add capture.

### 1.2 Define core data structures

Design a few C structs (conceptual):

* `MixerDevice`:

  * `char name[...];` (human-readable name, e.g. “hw:0 – HDA Intel”)
  * `char alsa_name[...];` (e.g. `"hw:0"`, `"hw:1"`)
  * `snd_mixer_t *handle;`
  * `GList *controls;` or a simple array/vector if you prefer C++ later.
* `MixerControl` (one column in the UI):

  * `snd_mixer_elem_t *elem;`
  * flags: `is_playback`, `has_volume`, `has_switch`, `is_capture`, `is_enum`, `stereo`, etc.
  * min/max volume, dB info if you want later.
  * **pointers to widgets**:

    * `Widget label;`
    * `Widget value_label;`
    * `Widget scale;` (vertical slider)
    * `Widget mute_toggle;`
    * `Widget capture_toggle;`
    * `Widget enum_menu;` (if needed later)

This struct is the bridge between ALSA and Motif.

### 1.3 Implement basic ALSA card enumeration

* Use ALSA to enumerate mixer devices:

  * Either use `snd_card_next()` to find cards and build `"hw:%d"` strings.
  * Or use `snd_device_name_hint(-1, "ctl", ...)` to get a list of mixer names.
* For each device:

  * `snd_mixer_open(&handle, 0);`
  * `snd_mixer_attach(handle, "hw:0");` (or corresponding name)
  * `snd_mixer_selem_register(handle, NULL, NULL);`
  * `snd_mixer_load(handle);`
* Collect these into an array/list of `MixerDevice`.

### 1.4 Implement element discovery per device

For each `MixerDevice`:

* Iterate elements:

  * `snd_mixer_elem_t *elem = snd_mixer_first_elem(handle);`
  * `for (; elem; elem = snd_mixer_elem_next(elem)) { ... }`
* Filter to “active” elements:

  * `snd_mixer_selem_is_active(elem)`
* Decide which elements to show:

  * **Simple heuristic** for v1:

    * Only elements with playback volume: `snd_mixer_selem_has_playback_volume(elem)`
    * Later: capture elements, switches, enums.
* For each chosen element, create a `MixerControl` and fill:

  * `snd_mixer_selem_get_playback_volume_range(elem, &min, &max);`
  * Flags: has_switch, stereo channels, etc.

Test all this **without UI** first using a small CLI that prints the detected controls.

---

## 2. Motif UI layout planning (Win95 style)

### 2.1 Top-level window layout

* `XmForm` as main container.
* At the **top** (attached to top/left of the Form):

  * `XmComboBox` or `XmDropDown` (or `XmRowColumn`+`XmOptionMenu`) for device selection.
  * Next to it: `XmToggleButton` labeled “Use this device as default”.
* Below that, filling the rest:

  * A `XmRowColumn` in **horizontal orientation**:

    * Each child will be a “control column” `XmForm` or `XmRowColumn` arranged vertically.

### 2.2 Control column layout (per MixerControl)

Each column (for one control, e.g. Master):

* Container: `Widget column = XmFormCreate(...)` or a `XmRowColumn` (vertical orientation).
* Inside:

  1. **Label at top**: `XmLabel` with control name.
  2. **Current value label** under the name: `XmLabel` (e.g. “75%”).
  3. **Vertical slider**:

     * `XmScale` with `XmVERTICAL` orientation.
     * Value range mapped 0–100 or 0–(max-min).
  4. At bottom: one or more `XmToggleButton`s:

     * “Mute” (if playback switch exists).
     * “Select” / “Capture” for capture controls.

Arrange with attachments/packing so it visually resembles Win95 sndvol32.

### 2.3 Status bar (optional but nice)

* At the bottom of main Form: a `XmLabel` acting as a status bar:

  * Show hints like: “Device: HDA Intel (hw:0)”, or “Synced with ALSA”.

---

## 3. Application state & event connections

### 3.1 Global application context

* Store:

  * List of `MixerDevice`s.
  * Index / pointer to currently active device.
  * Pointer to the `XmRowColumn` that holds the control columns.

### 3.2 Device combo box behavior

* When user selects a device:

  * Close / detach old device’s mixer handle (or keep alive, up to you).
  * Set “current device” pointer.
  * Rebuild control-columns UI:

    * Destroy all children of the controls container.
    * For each `MixerControl` in the selected device:

      * Create column widgets and attach them.
      * Initialize scale & toggles from ALSA current values.

### 3.3 Control callbacks

* `XmScale` ValueChanged or Drag callbacks:

  * Map scale value to ALSA volume range: `user_value -> [min,max]`.
  * Use `snd_mixer_selem_set_playback_volume_all(elem, value)` (and capture analogs).
  * Update `value_label` to show new percentage.
* Mute / Capture toggles:

  * On toggle: call `snd_mixer_selem_set_playback_switch_all(elem, on/off)` or capture equivalent.
  * Optionally update slider appearance (e.g. greyed out) if muted.

---

## 4. Background monitoring & live updates

You want the UI to reflect changes made elsewhere.

### 4.1 Using ALSA’s poll descriptors + Xt

* ALSA allows you to integrate its fd into your loop:

  * `snd_mixer_poll_descriptors_count(handle)` to know how many fds.
  * `snd_mixer_poll_descriptors(handle, pfds, count)` to fill an array of `struct pollfd`.
* For each fd, register with Xt:

  * `XtAppAddInput(app_context, pfds[i].fd, (XtPointer)XtInputReadMask, mixer_fd_callback, (XtPointer)device);`
* In `mixer_fd_callback`:

  * Call `snd_mixer_handle_events(handle);`
  * Then:

    * Either refresh all controls for that device (simple but brute-force).
    * Or use ALSA element callbacks:

      * `snd_mixer_elem_set_callback(elem, on_elem_change);`
      * In callback: mark that this element changed, then update only that control’s widgets.

### 4.2 Updating the UI safely

* In `on_elem_change` or after `snd_mixer_handle_events`:

  * Read new values:

    * `snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &v);`
    * `snd_mixer_selem_get_playback_switch(elem, ...)`.
  * Update:

    * Scale position
    * Value label text
    * Toggle state.
* Ensure you **do not recurse**:

  * When updating widgets from ALSA callbacks, temporarily disable widget callbacks or use a “in_sync” flag to ignore internal changes.

---

## 5. “Use this device as default” behavior

This is more policy than tech, but plan it now:

### 5.1 Decide scope of "default"

* ALSA itself doesn’t have a universal “default” switch.
* Common approach: `~/.asoundrc` or `/etc/asound.conf` decides default `pcm` and `ctl`.
* Options for your checkbox:

  1. **App-local default**:

     * Store user preference in a config file, e.g. `~/.motif_mixer.conf`.
     * On startup, select that device automatically.
     * Does **not** change system behavior, only your app.
  2. **System / user default**:

     * When toggled:

       * Write/update `~/.asoundrc` with something like:

         ```txt
         defaults.pcm.card <cardnumber>
         defaults.ctl.card <cardnumber>
         ```
       * This will affect many ALSA clients that use “default” pcm/ctl.
     * You must:

       * Read existing file carefully.
       * Avoid destroying other user config.
       * Maybe offer a confirmation dialog.

For safety, I’d plan:

* Phase 1–3: Implement only **app-local default**.
* Phase 4: Optionally implement `~/.asoundrc` integration as a separate feature.

### 5.2 UI logic for default checkbox

* On device change:

  * If current device == app-local default -> check the box, else uncheck.
* On checkbox toggled:

  * If checked:

    * Set app-local default = current device in config.
    * (Optionally) write `~/.asoundrc` or show dialog.
  * If unchecked:

    * Remove from config, or revert to ALSA’s implicit default.

---

## 6. Implementation roadmap (step-by-step ToDo list)

Here’s the concrete to-do list you can literally work down:

### Phase 1 – Backend basics (no GUI yet)

1. Create a new project `motif_mixer` with ALSA + Motif includes and a simple `main.c`.
2. Implement ALSA card enumeration:

   * Use `snd_device_name_hint` or `snd_card_next`.
   * Print all found mixer devices to stdout.
3. For the first card, open `snd_mixer_t`, register, load.
4. Enumerate all simple elements and print:

   * Name
   * Flags: has playback volume / switch / capture / enum.
   * Playback volume range.
5. Decide a filter rule (“show only these controls”).

### Phase 2 – Minimal Motif window + single slider demo

6. Add basic Motif boilerplate:

   * `XtVaAppInitialize`, `XmCreateForm`, main window, show window.
7. Create a dummy control column:

   * Vertical `XmScale` with label above and value label below.
   * A mute toggle at bottom.
8. Implement callbacks for the dummy slider:

   * Update the value label when slider changes.

Verify look & feel (Win95-ish enough? Adjust fonts/margins as desired).

### Phase 3 – Real mixer integration for one device

9. Integrate ALSA into the Motif program:

   * On startup, open first mixer device, build `MixerDevice` with its `MixerControl`s.
10. Create controls container:

    * A horizontal `XmRowColumn` in the main form.
11. For each selected `MixerControl`:

    * Create a column Form / RowColumn.
    * Create label, value label, vertical scale, mute checkbox (if applicable).
    * Initialize their values from ALSA (read current volumes/switches).
12. Implement callbacks:

    * Slider → ALSA `set_playback_volume_all`.
    * Mute toggle → ALSA `set_playback_switch_all`.
    * After changing ALSA, update value label & maybe slider if clamped.

Test: changing slider should affect real audio.

### Phase 4 – Device selection UI

13. Implement device combo box at top:

    * Populate with detected `MixerDevice` names.
14. On combo selection change:

    * Switch current device pointer.
    * Rebuild the control columns (destroy old children, create new).
    * Initialize all controls from the new mixer.
15. Add “Use this device as default” checkbox:

    * Implement **app-local** default in a simple config file.
    * On startup, read config and select that device if available.

### Phase 5 – Live updates / monitoring

16. From the mixer handle, get its poll descriptors:

    * `count = snd_mixer_poll_descriptors_count(handle)`
    * `snd_mixer_poll_descriptors(handle, pfds, count)`
17. For each fd:

    * Register with `XtAppAddInput(app, fd, XtInputReadMask, mixer_fd_callback, device_ptr);`
18. In `mixer_fd_callback`:

    * Call `snd_mixer_handle_events(handle);`
    * Then refresh controls:

      * Easiest: for each `MixerControl`, re-read volume/switch and update widgets.
      * Optimize later if needed.
19. Add ALSA element callbacks (optional refinement):

    * `snd_mixer_elem_set_callback(elem, elem_changed_cb);`
    * In callback, flag that this element changed.
    * In `mixer_fd_callback`, update only flagged controls.

### Phase 6 – Optional: system default

20. Implement reading/writing of `~/.asoundrc`:

    * Parse minimal subset relevant to `defaults.pcm.card` and `defaults.ctl.card`.
    * On “Use this device” checked, offer dialog:

      * “Also set this as ALSA default in ~/.asoundrc?”
    * If yes, update file and maybe signal the user to restart apps.
21. Provide a menu item / dialog for “Reset default” or “Show current ALSA default”.
