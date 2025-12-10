# Refactor To‑Do

Track the cleanup work that keeps `ck-calc.c` manageable. Check a box when the associated extraction/refactor is finished.

- [x] **Keypad layout module**
  - Extract `rebuild_keypad` (button creation, palette accents, scientific stash handling) into a new `keypad_layout.c/h`.
  - Provide helpers to build the required rows for the current mode and wire callbacks without duplicating widget names across files.
  - Keep `set_mode`, `clear_button_refs`, and button registration in the UI file, but let the new module own button creation and geometry management.

-- [x] **Scientific toggle visuals**
  - Move `refresh_second_button_labels`, `assign_sci_button_ref`, `update_second_button_state`, `set_shift_state`, and the shift/mouse handling into e.g. `sci_mode_visuals.c`.
  - Expose functions such as `sci_visuals_update(AppState *)`, `sci_visuals_register_button(AppState *, const char *name, Widget)` and `sci_visuals_handle_shift(AppState *, KeySym, Boolean)`.
  - Keep any palette-dependent styling local to that module.

- [x] **Window sizing helpers**
  - Extract `get_desired_width`, `apply_current_mode_width`, `apply_wm_hints`, and `lock_shell_dimensions` into `window_metrics.c`.
  - Rename logging helpers as needed and ensure `set_mode` calls into the new module to resize/reapply constraints.
  - Make sure widget measurement code still updates `app->chrome_dy` from the UI when the shell realizes.

- [x] **Menu/session callbacks**
  - Move `cb_toggle_thousands`, `cb_menu_*`, `capture_and_save_session`, `about_*`, and related dialog setup into `menu_handlers.c`.
  - Export setup/teardown functions so `ck-calc.c` simply registers the callbacks instead of defining them inline.
  - Verify shared helpers (session capture, about dialog) still operate on `AppState`.

- [x] **Calculator input logic**
  - Consider moving the arithmetic button callbacks (`cb_digit`, `cb_decimal`, `cb_backspace`, `cb_toggle_sign`, `cb_percent`, `cb_operator`, `cb_equals`) into an `input_handler.c` that operates on `CalcState`/`AppState`.
  - Let `ck-calc.c` focus on wiring the callbacks to buttons while the handler routines live elsewhere and reuse the display API.

- [ ] **Shared state & helpers**
  - Consolidate the `AppState` definition and related helpers (`reset_state`, `format_number`, display formatting, etc.) into their own headers/c modules so `ck-calc.c` only wires the high-level flow.
  - Keep `display_api` as the single entry point for `set_display`/`set_display_from_double`, `current_input`, `ensure_keyboard_focus` so other modules do not duplicate that logic.
  - Verify any leftover inline helpers (locale init, view state, WM hints) become the owning module’s responsibility so they are neither repeated nor hidden within `ck-calc.c`.

- [x] **Clipboard utilities**
  - Move the copy/paste flash timers and clipboard helpers out of `ck-calc.c` into `clipboard.c/h` (shared across apps if needed).
  - Have `ck-calc.c` call the exposed clipboard helpers via `AppState` and remove any duplicated buffer management/code from the main UI file.
