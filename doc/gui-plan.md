# scrcpy-gui — plan

A fork of [scrcpy](https://github.com/Genymobile/scrcpy) that adds a
desktop GUI: a macOS-inspired sidebar on the left and the device mirror on the
right, with first-class ADB features (APK install, file transfer, …) and
drag & drop everywhere.

Status: **planned** — decisions below were settled in a design session
(2026-11-02). Change them deliberately, not silently.


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


## Build and test

- Daily development: native Windows with **MSYS2 UCRT64** (gcc, meson,
  ninja, SDL3, FFmpeg from pacman).
- Releases: GitHub Actions, keeping upstream's Linux → Windows cross-compile
  (`cross_win64.txt`) working.
- Unit tests (meson `test`, like `app/tests/`) for pure logic: `ls -la`
  parsing, config JSON round-trip, drop routing, adb progress parsing.
- GUI: manual testing on a real device + emulator, with a short checklist per
  milestone.
