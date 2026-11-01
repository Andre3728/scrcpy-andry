# scrcpy-gui — plan

A fork of [scrcpy](https://github.com/Genymobile/scrcpy) that adds a
desktop GUI: a macOS-inspired sidebar on the left and the device mirror on the
right, with first-class ADB features (APK install, file transfer, …) and
drag & drop everywhere.

Status: **M1–M6 implemented, M7 partly** (see Progress). Decisions below were
settled in a design session (2026-11-02). Change them deliberately, not
silently.


## Product decisions

### Identity and launch

- Binary: `scrcpy-gui.exe`. Config dir: `%APPDATA%\scrcpy-gui\`
  (XDG config dir on Linux/macOS). Upstream license and notices are kept.
- No arguments → GUI mode.
- Any upstream CLI flag → behaves exactly like upstream (no panel).
- `--gui` forces GUI mode; CLI flags given with it become initial settings.
- Distributed as a folder (exe + adb + DLLs + server) at first; a true
  single-file exe is a packaging step in M7.

### App lifecycle

- The app outlives a mirroring session: it opens with no device, shows the
  device list, and returns to it on disconnect instead of exiting.
- One device per window in v1.

### Layout

- macOS-style sidebar (Finder / System Settings feel): icon + label rows,
  tinted background, toggle button hides it entirely.
- Sidebar pages: **Devices**, **Files**, **Apps**, **Settings**.
- Pages use grouped inset lists, toggle switches, segmented controls, popup
  buttons.
- Mirror on the right, aspect-fit. Rotation to landscape widens the window;
  the sidebar stays left. Fullscreen hides the sidebar.
- Floating toolbar under the mirror (always visible, even with sidebar
  hidden): Back, Home, Recents, Volume −/+, Power, Rotate, Screenshot, Record,
  Transfers (popover, Safari-downloads style), `…` overflow (notification
  panel, settings panel, …).
- v1 keeps the native Windows title bar (dark-mode aware via DWM). A unified
  custom title bar (borderless + `SDL_SetWindowHitTest`, Windows-style
  caption buttons, no traffic lights) is M7 polish.

### Look and feel

- macOS-inspired. Dark by default, follows system light/dark.
- Inter font + Lucide icon font, embedded in the exe. One accent color,
  rounded corners, generous padding. HiDPI aware.
- Toasts for job results. Raw logs live in a hidden Log drawer.
- English only; all strings in one place for future translation.

### Input routing

- Keyboard goes to the device only while the mirror has focus (click it).
  Clicking the sidebar or a text field gives focus to the GUI.
- Mouse goes to whatever is under the cursor.
- A focus ring around the mirror shows when it captures input.
- The scrcpy shortcut modifier (Alt) keeps working globally.

### Drag & drop

- Drop on the mirror: `.apk` → install; other files → `/sdcard/Download/`.
- Drop on a folder in the file manager → push there.
- Folders are pushed recursively. Mixed drops: APKs install, the rest push.
- During drag, a highlighted overlay on the target says what will happen
  ("Install 2 APKs", "Copy to /sdcard/DCIM").
- Split APKs (`.apks`/`.xapk`) → v2.

### Devices and connection

- Devices page: connected + remembered devices.
- Wireless (v1): one-click "Switch to Wi-Fi" for a USB device
  (`adb tcpip` + auto-connect), Android 11+ pair with code, manual ip:port.
  QR pairing → v2.
- Empty state: onboarding card ("enable USB debugging", 3 steps, "Connect
  over Wi-Fi"). Clear states with fix-it hints for unauthorized, offline,
  server-failed.
- adb resolution: `ADB` env var, then bundled adb (upstream behavior), plus
  a Settings → adb path override. Warn on adb version mismatch.

### Settings

- Global defaults + per-device profiles keyed by serial, in
  `config.json`. Also stores last device, window size, sidebar state, paired
  wireless devices.
- Live settings (screen off, stay awake, show touches) apply instantly.
- Restart-only settings (resolution, bitrate, FPS, codec, audio) are marked
  ↻; **Apply** restarts the session on the same device.
- Settings → Advanced: "Additional arguments" text field for raw scrcpy flags,
  validated through `cli.c`.

### File manager (v1)

- Single-pane device browser: breadcrumb path bar, list view
  (Name / Size / Modified), sorting, back / forward / up, quick-access
  shortcuts (Internal storage, DCIM, Download, Documents).
- Push: drag from Explorer. Pull: right-click → "Save to…", or double-click to
  download to the default folder. Delete, new folder.
- Drag-out to Explorer (Windows OLE `DoDragDrop`) → v2.
- Transfers go through a queue with per-file progress, cancel, summary toast.

### Capture

- Screenshot: grab the currently decoded frame (mirror resolution, instant)
  → PNG in `Pictures\scrcpy\` + clipboard.
- Recording: start/stop mid-session by attaching the recorder at runtime and
  requesting a keyframe (reset-video control message). Fallback if too
  invasive: restart the session with `--record`.


## Scope

| Version | Features |
|---------|----------|
| v1 | Device picker, Wi-Fi connect (switch / pair code / manual), mirror toolbar, settings + persistence, APK install with progress, file manager, screenshot, recording |
| v2 | App manager (list / launch / uninstall / force-stop / clear data / extract APK), device info, QR pairing, split APKs, drag-out to Explorer, multi-device |
| v3 | Logcat viewer, shell console |


## Architecture

- **UI:** Dear ImGui, rendered into the existing SDL3 window using the
  official `imgui_impl_sdl3` + `imgui_impl_sdlrenderer3` backends (scrcpy
  already renders with `SDL_Renderer`). Vendored under `app/deps/` or
  `subprojects/`.
- **Language:** the `app/src/gui/` module is C++ and exposes a thin
  `extern "C"` interface. The core stays C. Meson builds both.
- **Upstream friendliness:** GUI code is additive in `app/src/gui/`. Core
  touch points are kept small and explicit (listed below). CLI mode is
  byte-for-byte upstream behavior.
- **Concurrency:** all adb / device operations run on a background worker
  pool (reusing `sc_thread`, `sc_intr` for cancellation). Jobs expose
  pending / progress / done / failed; results are posted to the SDL event
  loop as custom events. The UI never blocks on adb.
- **File ops:** adb only (`shell ls -la` / `stat`, `push`, `pull`, progress
  parsed from adb output). No changes to the Java server.
- **Config:** JSON via vendored cJSON.

### Core touch points (known so far)

Findings from reading the current code:

- `scrcpy()` in `app/src/scrcpy.c` is a one-shot session: it creates the
  `sc_screen` (and thus the SDL window + renderer) and runs `event_loop()`
  until the device goes away. **The GUI needs the window to outlive the
  session**, so the window/renderer must become owned by the GUI app and
  lent to `sc_screen` (new init param), and the session must be startable /
  stoppable from a GUI-owned event loop.
- `sc_screen_render()` (`app/src/screen.c`) clears, draws the video texture
  into `screen->rect`, then presents. The GUI must: offset/shrink the content
  rect by the sidebar width + toolbar height (`compute_content_rect` input),
  draw ImGui after the video texture, and own the present call.
- `sc_screen_handle_event()` and the input manager receive all events. They
  must be filtered by ImGui's `WantCaptureMouse/Keyboard` and the mirror
  focus state, and mouse coordinates must account for the content offset.
- `app/src/file_pusher.c` already handles `SDL_EVENT_DROP_FILE` (APK install
  or push to `/sdcard/Download/`). It will be generalized into the job queue
  with a destination path and progress reporting.
- `app/src/events.h` holds the custom SDL event enum; GUI job events get
  added there (or in a GUI-owned range).


## Milestones

Each milestone is usable and mergeable on its own.

1. **M1 — Shell.** Vendor ImGui; mixed C/C++ meson build; GUI mode switch
   (`scrcpy-gui.exe` with no args / `--gui`); sidebar + mirror split layout
   in the existing window; floating toolbar wired to existing actions (back,
   home, recents, volume, power, rotate); input routing and focus ring;
   theme, fonts, icons.
2. **M2 — Device lifecycle.** GUI-owned window and event loop; session
   start/stop; Devices page; reconnect on disconnect; empty/error states;
   Wi-Fi switch / pair / connect.
3. **M3 — Settings.** cJSON config, per-device profiles, Settings page, live
   vs ↻ settings with Apply/restart, Additional arguments, adb path override.
4. **M4 — Jobs and drop.** Worker pool; drop zones with hover overlay; APK
   install with progress; transfer queue + Transfers popover; toasts.
5. **M5 — File manager.** Files page: browse, push into folder, pull, delete,
   mkdir, quick access.
6. **M6 — Capture.** Screenshot (PNG + clipboard), mid-session recording.
7. **M7 — Polish and packaging.** Unified title bar, theming pass, single-file
   exe, release CI.


## Progress

### M1 — Shell (implemented, needs hands-on testing)

- Dear ImGui v1.93.0 vendored in `app/src/gui/third_party/imgui/` (core +
  SDL3 / SDLRenderer3 backends, unmodified). Inter 4.1 and Lucide 1.59.0
  fonts in `app/data/fonts/`, embedded at build time by `app/src/gui/embed.py`.
- The executable is now `scrcpy-gui`. No arguments or `--gui` → GUI mode.
- Layout decision made during M1: the left side is **two columns**, a
  narrow macOS-style navigation sidebar (Devices / Files / Apps / Settings)
  and the selected page next to it, then the mirror. The toolbar's first
  button hides/shows both (the window shrinks/grows so the mirror keeps its
  size and position).
- Core touch points: `screen.c` (video area = window minus GUI insets,
  GUI drawn before present, events routed through the GUI first),
  `input_manager.c` (`sc_input_manager_handle_gui_action()`), `--gui`
  option, `SC_EVENT_GUI_REDRAW`.
- Pages are placeholders except Settings → Appearance (System/Light/Dark).
- Known gaps: the disconnection screen (`sc_screen_handle_disconnection`)
  does not redraw the GUI; that loop is replaced in M2.

### M2 — Device lifecycle (implemented)

- `app/src/gui_app.c` (`scrcpy_gui()`): owns the window, renderer and GUI;
  runs sessions with `scrcpy_run(options, host)` and returns to the device
  list when a session ends. Used for no-arg launch and `--gui`.
- `struct scrcpy_gui_host` (`scrcpy.h`): the session borrows the app's window,
  renderer and GUI ("hosted" screen), and forwards events to the app while
  waiting for the server so the UI stays live during connection.
- Worker thread: polls `adb devices -l` every 2 s and runs device tasks
  (Use Wi-Fi = `adb tcpip 5555` + connect, pair with code, connect,
  disconnect). Results come back as `SC_EVENT_GUI_DEVICES` /
  `SC_EVENT_GUI_TASK_DONE`.
- Auto-start: the `-s` device, else the only ready device. A device stopped by
  the user or that failed is not auto-started again until it disappears.
- Auto-reconnect after a disconnection (verified with `adb kill-server`
  mid-session); after "Use Wi-Fi", the app reconnects to the Wi-Fi address.
- Devices page: device rows (USB/Wi-Fi, state, unauthorized/offline hints,
  Mirror / Stop / Use Wi-Fi / Disconnect), onboarding card when empty,
  Wi-Fi card (pair with code / connect). Placeholder in the mirror area when
  idle or connecting. Toasts for results.
- GUI callbacks are deferred out of rendering (dispatched on the next event).
- Core fixes: `sc_adb_init()` is reference-counted; `sc_cond_timedwait()`
  waits again when the OS reports a timeout before the deadline (it asserted
  on Windows); the screen removes its event watcher on destroy.
- Not verified yet (needs hands-on): Stop/Mirror buttons, Use Wi-Fi, pairing.

### M3 — Settings (implemented)

- `app/src/gui_config.c`: `config.json` in the user config directory
  (`%APPDATA%\scrcpy-gui\` on Windows, via `SDL_GetPrefPath`), cJSON
  v1.7.19 vendored in `app/src/gui/third_party/cjson/`. Stores appearance,
  sidebar visibility, adb path, last device, default profile and per-device
  profiles. Written atomically (temp file + rename).
- Settings page: Resolution, Video bitrate, Max frame rate, Codec, Audio,
  Stay awake, Show touches (all ↻), Turn screen off (live), Additional
  arguments (↻, parsed with the scrcpy CLI parser on top of the profile).
  While mirroring, ↻ changes show "Apply & restart" / "Revert"; without a
  session, changes are saved for the next session (to the defaults when no
  device is mirrored).
- Deviation from the plan: Stay awake and Show touches are ↻ (restart)
  rather than live, because the server sets and restores them itself.
- adb path override (Settings > Advanced), applied at the next launch; the
  `ADB` environment variable wins. Version mismatch warning: not done.
- Unit test `test_gui_config` (JSON round-trip, invalid values, argument
  splitting). Verified on device: a profile with max size 800 and
  `--no-audio --max-fps=30` gives a 384x800 stream without audio.

### M4 — Drop, install and transfers (implemented, drag & drop needs hands-on)

- In GUI mode the GUI handles the SDL drop events itself (the upstream file
  pusher is bypassed): drop anywhere in the window while mirroring. APKs are
  installed (`adb install -r`), other files and folders are copied
  (`adb push`, recursive) to `--push-target` (default `/sdcard/Download/`).
- Overlay during the drag (card over the mirror area). SDL only provides the
  file names on drop, so the overlay describes the rule ("APKs are installed,
  other files go to /sdcard/Download") rather than counting files.
- Transfer thread (separate from the device poller): one transfer at a time,
  cancel (pending: dequeued; running: the adb process is killed). Progress
  is per file (waiting / installing / done / failed): adb does not print
  byte progress when its output is not a terminal.
- Transfers popover on the toolbar (badge with the active count, opens
  automatically on drop, Clear), summary toast per batch, readable install
  errors (`INSTALL_FAILED_VERSION_DOWNGRADE` → "A newer version is already
  installed", ...).
- `sc_adb_run_capture()` (adb.c) captures stdout + stderr of an adb command.
- Pure helpers in `gui_util.c` with unit test `test_gui_util` (error
  extraction checked against real `adb install` output from the device).

### M5 — File manager (implemented, actions need hands-on)

- Files page: back / forward / up / refresh / new folder / locations
  (Internal storage, DCIM, Download, Documents, Pictures, Movies, Music,
  root), clickable breadcrumb, sort by name / date / size (folders first),
  two-line rows (name, size or "Folder", date) to fit the narrow pane.
- Click selects, Ctrl+click multi-selects, double-click opens a folder or
  downloads a file. Context menu: Open, Download, Save to..., Delete. A footer
  with the same actions appears when files are selected.
- Download goes to the computer's Downloads folder, or to a folder chosen in
  the native dialog (`SDL_ShowOpenFolderDialog`), through the transfer queue
  (`adb pull`).
- Delete asks for confirmation (`rm -rf`, quoted; `/`, `/sdcard` and
  `/storage/emulated/0` are refused). New folder: `mkdir -p`.
- Drop on a folder row copies into it, drop on the list copies into the
  current folder (highlighted); the mirror overlay is hidden in that case.
- Listing: `adb shell ls -la '<dir>/'` on the device worker, parsed by
  `sc_gui_parse_ls()` (unit tested with real toybox output, spaces,
  symlinks, devices, errors). The device poll no longer runs after every
  file operation.
- The last opened page is remembered (`"page"` in config.json).
- Verified on device: /sdcard/ listing rendered correctly.

### M6 — Capture (implemented, needs hands-on)

- Screenshot (toolbar camera button): `sc_screen_capture()` renders the
  current video texture into an offscreen target at the video resolution
  (with the client orientation) and reads it back; saved as PNG
  (`SDL_SavePNG`) to `Pictures\scrcpy\Screenshot_<date>_<time>.png` and copied
  to the clipboard (Windows: CF_DIB; other platforms: not yet). Flash
  feedback on the mirror.
- Recording (toolbar record button): **deviation from the plan** — the
  upstream recorder is a packet sink attached when the demuxers start, and
  attaching it to running demuxers needs invasive threading changes in the
  core. As agreed in the fallback, starting/stopping a recording restarts
  the session with `--record=<Videos>\scrcpy\Recording_<date>_<time>.mp4`
  (about one second of interruption, stated in the tooltip). The sidebar
  header shows a blinking "Recording mm:ss". A toast gives the file path when
  it is finalized. A reconnection does not resume recording (it would
  overwrite the file).
- Toolbar: 12 buttons (32 px) in 4 groups: sidebar | back, home, recents |
  volume, power, rotate | screenshot, record, transfers, more.

### M7 — Polish and packaging (partly done)

Done:
- Native title bar matched to the theme (Windows: dark mode attribute +
  caption/text colors = sidebar color on Windows 11), instead of a custom
  borderless title bar: same unified look, keeps Snap Layouts and native
  behavior.
- Double-click launch in GUI mode frees the console window; fatal startup
  errors are shown in a message box.
- The C++ runtime and winpthread are linked statically on Windows
  (`scrcpy-gui.exe` only imports SDL3, FFmpeg and system DLLs).
- `release/build_gui_windows.sh`: portable build with MSYS2 (exe, DLLs,
  adb 37.0.1, prebuilt server, licenses) + zip, with a missing-DLL check.
  Verified: the package runs with only `C:\Windows\System32` in `PATH`.
- Upstream release scripts and `release.yml` updated for the `scrcpy-gui`
  binary name; `app/deps/_init` falls back to `sha256sum` when `shasum` is
  missing.
- `.github/workflows/gui.yml`: Windows build + unit tests + package artifact
  (not run yet: needs a push to GitHub).
- User documentation: `doc/gui.md`, fork notice in `README.md`.
- Disabled controls are more visibly disabled.

Not done (follow-ups):
- **Single-file exe.** The MSYS2 package is a folder of ~115 files (59 MB
  zipped) because MSYS2's FFmpeg is a full build. Next step: build with the
  upstream release pipeline (`release/build_windows.sh`, minimal FFmpeg/SDL
  built from source, ~10 files), then embed the DLLs, adb and server as
  resources extracted on first run (or static-link SDL/FFmpeg).
- Custom unified title bar (borderless + hit testing): replaced by the native
  title bar coloring above; revisit only if needed.
- Clipboard image copy on Linux/macOS, adb version mismatch warning.


## Build and test

- Daily development: native Windows with **MSYS2 UCRT64** (gcc, meson,
  ninja, SDL3, FFmpeg from pacman).
- Releases: GitHub Actions, keeping upstream's Linux → Windows cross-compile
  (`cross_win64.txt`) working.
- Unit tests (meson `test`, like `app/tests/`) for pure logic: `ls -la`
  parsing, config JSON round-trip, drop routing, adb progress parsing.
- GUI: manual testing on a real device + emulator, with a short checklist per
  milestone.
