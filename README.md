# natural-scroll-windows

[![CI](https://github.com/themanlee7942/natural-scroll-windows/actions/workflows/ci.yml/badge.svg)](https://github.com/themanlee7942/natural-scroll-windows/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/themanlee7942/natural-scroll-windows?include_prereleases&sort=semver)](https://github.com/themanlee7942/natural-scroll-windows/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform: Windows](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-blue)](#)

macOS's **Natural scrolling** for Windows — a tiny tray utility that
**reverses the mouse-wheel direction** without rebooting.

Lives in the system tray. Right-click for the menu, double-click to
open the window. Two small files, no installer, no dependencies.

**Version 0.9** — first public release.

```
                    ┌──────────────────────────────────────┐
                    │  Mouse Wheel Direction Toggler   _ ✕ │
                    ├──────────────────────────────────────┤
                    │  Local mouse                         │
                    │  Direction: Normal      [ Toggle ]   │
                    │  Wired / built-in. Persistent. UAC.  │
                    │  ────────────────────────────────────│
                    │  Remote mouse                        │
                    │  Status: OFF            [ Start  ]   │
                    │  KVM / remote / virtual. Session.    │
                    │                                      │
                    │  Closing the window keeps it in tray.│
                    │  [ About ]              [ Hide ]     │
                    └──────────────────────────────────────┘
```

Tray menu:

```
    Toggle Local Direction  (now: Normal)
    Start Remote Reverse
    ─────────────────────────────────────
    Show Window
    About...
    Open GitHub Page
    ─────────────────────────────────────
    Exit
```

---

## Download

Grab a release zip from the
[Releases page](https://github.com/themanlee7942/natural-scroll-windows/releases) —
the build workflow attaches `scroll-toggle-vX.Y-x64.zip` and
`scroll-toggle-vX.Y-x86.zip` plus a `SHA256SUMS.txt` to every tagged
release.

If you'd rather grab the files from this repo directly, they live in
[`dist/`](dist/).

## Run

Pick the architecture matching your Windows install. Almost everyone
should grab the 64-bit pair.

| Windows | Files to keep together in one folder |
|---|---|
| 64-bit (default) | `scroll_toggle.exe` + `scroll_toggle.exe.manifest` + `scroll_hook.dll` |
| 32-bit | `scroll_toggle_x86.exe` + `scroll_toggle_x86.exe.manifest` + `scroll_hook_x86.dll` |

1. Drop the three files of your architecture into any folder.
2. Double-click `scroll_toggle.exe` (or `scroll_toggle_x86.exe`).
3. A tray icon appears in the notification area.
4. Right-click the tray icon → **Toggle Local Direction** for a real
   mouse, or **Start Remote Reverse** for a KVM / RDP / remote source.
5. Closing the window does **not** exit the app — it stays in the
   tray. Use the tray menu's **Exit** to fully quit.

To launch on startup, drop a shortcut to the exe into
`shell:startup` (`Win+R` → `shell:startup`).

---

## The two modes

| Mode | Use when | Persistent? | Admin? |
|---|---|---|---|
| **Local** | Real mouse plugged into this PC | Yes (registry) | Yes (auto-elevates) |
| **Remote** | Wheel comes from another machine | No (session only) | No |

The two are independent — pick whichever matches where the wheel
events come from.

### A note on software KVMs (Synergy / Barrier / Deskflow / Mouse Without Borders)

If you share keyboard and mouse from another computer via one of
these, **first try the KVM's own "invert mouse wheel" option**. It
exists in the server-side advanced settings of all four projects, and
it produces a perfectly smooth scroll because the events never leave
the source. Use this tool's Remote mode for everything else: RDP,
VNC, Parsec, Moonlight, virtual mice, or KVMs that lack the option.

Priority order:

1. **Source-side setting** (KVM option, macOS "Natural scrolling", etc.)
2. **Local mode** (real HID mouse, persistent)
3. **Remote mode** (anything else, session-only)

---

## Command-line flags

The tray is enough for everyday use, but the exe also supports:

| Command | Behaviour |
|---|---|
| `scroll_toggle.exe` | Open UI, install tray icon |
| `scroll_toggle.exe /tray` | Start hidden, only tray icon visible |
| `scroll_toggle.exe /local-toggle` | One-shot local toggle (used internally for UAC) |
| `scroll_toggle.exe /hook` | Headless Remote-mode worker, no UI |
| `scroll_toggle.exe /stop` | Stop any running tray instance |

Bind to a hotkey: create a Start-menu shortcut, set the Shortcut key
(e.g. `Ctrl+Alt+S`), and point it at one of the above commands.

---

## How it works

**Local mode** writes `FlipFlopWheel = 1` (or `0`) under every present
mouse-class and HID-class device under
`HKLM\SYSTEM\CurrentControlSet\Enum\<InstanceId>\Device Parameters`,
then restarts the mouse driver in-process via SetupAPI
(`SetupDiSetClassInstallParams` + `SetupDiCallClassInstaller` with
`DIF_PROPERTYCHANGE`). The driver re-reads the registry on restart,
so the new direction is live within ~500 ms — no reboot, no logoff.

**Remote mode** loads `scroll_hook.dll`, which installs a `WH_MOUSE`
hook with `dwThreadId = 0`. Windows injects the DLL into every GUI
process. Inside each process, the hook callback:

1. Finds the deepest window under the cursor via `WindowFromPoint` —
   so child panes (browser viewports, list views, IDE editors) are
   targeted, not just their top-level frame.
2. `SendMessageTimeout`s a fresh `WM_MOUSEWHEEL` with the wheel delta
   negated, delivered straight to the target window. SendMessage
   bypasses the OS message queue (and therefore our own hook), so
   there is no SendInput round-trip and no event-loop feedback.
3. If the first target returns 0 (= "I didn't handle it" — common for
   chat-app input controls that let wheel events bubble), walks up
   `GetParent` and tries again, bounded to 8 hops.
4. Returns 1 to suppress the original event.

This is the same pattern AutoHotkey and WizMouse use for low-latency
wheel manipulation. It produces a scroll that feels indistinguishable
from a source-side fix on modern Windows.

---

## Build from source

A two-file project: `scroll_toggle.c` (the exe) and `scroll_hook.c`
(the DLL).

### Windows + [w64devkit](https://github.com/skeeto/w64devkit) (simplest)

```sh
# 64-bit
gcc -O2 -Wall -mwindows scroll_toggle.c \
    -lsetupapi -ladvapi32 -luser32 -lcfgmgr32 -lshell32 -lgdi32 \
    -o dist/scroll_toggle.exe
gcc -O2 -Wall -shared scroll_hook.c -luser32 \
    -o dist/scroll_hook.dll
```

### Cross-compile from Linux / macOS with [zig](https://ziglang.org/)

```sh
pip install ziglang
# 64-bit pair
python3 -m ziglang cc -target x86_64-windows-gnu -O2 -Wall \
    scroll_toggle.c \
    -lsetupapi -ladvapi32 -luser32 -lcfgmgr32 -lshell32 -lgdi32 \
    -Wl,--subsystem,windows -o dist/scroll_toggle.exe
python3 -m ziglang cc -target x86_64-windows-gnu -O2 -Wall \
    -shared scroll_hook.c -luser32 -o dist/scroll_hook.dll
# 32-bit pair
python3 -m ziglang cc -target x86-windows-gnu -O2 -Wall \
    scroll_toggle.c \
    -lsetupapi -ladvapi32 -luser32 -lcfgmgr32 -lshell32 -lgdi32 \
    -Wl,--subsystem,windows -o dist/scroll_toggle_x86.exe
python3 -m ziglang cc -target x86-windows-gnu -O2 -Wall \
    -shared scroll_hook.c -luser32 -o dist/scroll_hook_x86.dll
cp app.manifest dist/scroll_toggle.exe.manifest
cp app.manifest dist/scroll_toggle_x86.exe.manifest
```

The released binaries were built this way with zig 0.16.

---

## Caveats

- Local mode briefly freezes the cursor (~100–400 ms) during the
  driver restart.
- Some vendor "gaming" drivers ignore `FlipFlopWheel` and re-apply
  their own scroll direction — use Remote mode for those.
- Anti-cheat drivers may block DLL injection — Remote mode then has
  no effect inside the protected process.
- The exe is unsigned. Windows SmartScreen may warn on first run —
  click **More info** → **Run anyway**.
- A 64-bit `scroll_toggle.exe` cannot inject `scroll_hook.dll` into
  32-bit processes (and vice versa). Use the matching x86 build if
  you need to invert wheels inside a 32-bit application.

---

## Revert / uninstall

There's nothing to install. To restore the ori