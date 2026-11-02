# CK Core Apps agent rules

These rules apply to the entire repository.

- Commit independently reviewable topics separately. Keep implementation,
  tests, `CHANGES.md`, documentation and directly related CI changes together.
- Update `CHANGES.md` for user-visible behavior, dependencies, build/test
  contracts, CI, packaging and substantial documentation.
- Document major application behavior on the relevant page under `docs/`, and
  add new pages to `ckdocs.yml` navigation.
- Build with the latest CK Core Motif ABI 9 and a CDE build made against the
  same toolkit. Never accept a system `libXm` as an implicit substitute.
- Verify visible changes in a freshly built application under CK Core CDE;
  screenshots must show the real process and record the Motif/CDE revisions.
- Use at most six parallel build or test jobs per task.
- Commit locally when a topic is complete. Do not push or publish unless the
  user explicitly requests that separate action.
