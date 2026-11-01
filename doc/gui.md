# scrcpy-gui

scrcpy-gui is scrcpy with a control panel: the device mirror on the right, a
sidebar on the left with devices, files, apps and settings, and a toolbar
under the mirror.


## Launch

Double-click `scrcpy-gui.exe` (or run it without arguments). No console window
is opened. The window stays open between mirroring sessions:

 - if a single device is ready, it is mirrored automatically;
 - with several devices, click **Mirror** next to one in **Devices**;
 - when the device is disconnected, it is mirrored again automatically when
   it comes back.

With command line arguments, `scrcpy-gui` behaves exactly like `scrcpy` (no
panel). Run from a terminal, it prints its output to that terminal (the prompt
may come back before the output, because it is a GUI application). Add
`--gui` to get the panel and use the arguments as defaults:

```bash
scrcpy-gui --gui --max-size=1024
scrcpy-gui --gui -s 0123456789abcdef   # mirror this device first
```


## Devices

 - **Mirror** / **Stop**: start or stop mirroring a device.
 - **Use Wi-Fi**: switch a USB device to wireless debugging (port 5555), then
   unplug the cable; mirroring continues over Wi-Fi.
 - **Pair with code** (Android 11+): on the device, open _Developer options >
   Wireless debugging > Pair device with pairing code_, then enter the IP
   address, port and code.
 - **Connect**: connect to `IP:port` (shown under _Wireless debugging_).


## Connection log

The bottom of the left panel shows the last log message (connection, server,
errors). Click it to expand the full log (with the messages of the scrcpy
server running on the device), copy it, or clear it. A red dot indicates new
errors while it is collapsed.


## Toolbar

| Button | Action |
|--------|--------|
| Sidebar | Show or hide the panel (the mirror keeps its size) |
| Back, Home, Recents | Navigation buttons |
| Volume −/+, Power, Rotate | Hardware buttons, rotate the device |
| Camera | Screenshot: saved to `Pictures\scrcpy\`, copied to the clipboard |
| Record | Start/stop recording to `Videos\scrcpy\` (mirroring restarts) |
| Transfers | Installs, copies and downloads, with errors |
| ⋯ | Notification panel, quick settings, screen off/on |

The keyboard goes to the device only after clicking the mirror (a blue ring
shows it). Click anywhere else to type in the panel. scrcpy shortcuts
(`Alt`+…) always work.


## Drag & drop

While mirroring, drop files anywhere on the window:

 - `.apk` files are installed;
 - other files and folders are copied to `/sdcard/Download/` (or
   `--push-target`).

In **Files**, drop on a folder to copy into it, or on the list to copy into
the current folder. Progress and errors are listed in **Transfers**.


## Files

Browse the device storage: back/forward/up, locations, breadcrumb, sort by
name, date or size. Double-click a folder to open it, or a file to download it
to the computer's Downloads folder. Right-click for **Download**, **Save
to...** and **Delete**. Ctrl+click selects several files.


## Settings

Per-device mirroring settings (resolution, bitrate, frame rate, codec, audio,
stay awake, show touches, additional scrcpy arguments). Settings marked ↻ are
applied by restarting mirroring (**Apply & restart**). _Turn screen off_ is
applied immediately.

The configuration is stored in `%APPDATA%\scrcpy-gui\config.json`.

To use a specific adb (for example the one of Android Studio, to avoid adb
version conflicts), set it in **Settings > Advanced**, or set the `ADB`
environment variable.


## Single executable

`scrcpy-gui-<version>.exe` contains everything (SDL, FFmpeg, adb, the
scrcpy server). On first launch, adb, the server and the icons are extracted to
`%LOCALAPPDATA%\scrcpy-gui\runtime\<hash>\` (a few MB, reused by the next
launches, and replaced when the executable is updated).


## Build

See [gui-plan.md](gui-plan.md#build-and-test). From the MSYS2 UCRT64 shell:

```bash
release/build_gui_windows_onefile.sh  # single executable (dist/*.exe)
release/build_gui_windows.sh          # portable folder + zip (MSYS2 DLLs)
```
