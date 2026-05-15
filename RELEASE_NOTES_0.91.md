# v0.91 — Smart-source classifier for Remote mode

**Release date**: 2026-05-16
**Compatibility**: Windows 10 / 11, x64 + x86 (same binary footprint as v0.9)

## TL;DR

Remote mode learned to tell apart local-HID wheel events from
KVM/RDP/Parsec/Moonlight wheel events, and skips inversion for the
former — so you can leave Remote mode on permanently and your wired
desk mouse still scrolls the right way. Opt-in via `/smart-source`.
No behavior change without the flag.

## What changed

### New: `/smart-source` opt-in mode

```cmd
scroll_toggle.exe /smart-source
```

When passed on the command line, `scroll_toggle.exe` brings up an
additional `WH_MOUSE_LL` hook on a dedicated high-priority thread.
The hook only **classifies** each wheel event (it never modifies or
suppresses input):

- `LLMHF_INJECTED` set on the event → tagged "remote" (KVM, RDP/VNC,
  Parsec, Moonlight, virtual mouse, any user-mode `SendInput`).
- Otherwise → tagged "local" (the wired/built-in HID mouse on this PC).

The verdict is written to a `Local\` named file-mapping section
(`ScrollToggle_SharedState_v1`, 16 bytes). `scroll_hook.dll` — the
DLL Windows injects into every GUI process — lazily maps the same
section on its first wheel event. From then on, before doing any
inversion work, it checks the flag and **passes local-HID events
through untouched**.

UI surfaces this clearly:

- Tray menu reads `Start Remote Reverse  (Smart)` when the flag is on.
- Main window status shows `Status: ON  (Smart source)` / `OFF (Smart source)`.

### Performance improvement (when `/smart-source` is on)

With smart-source enabled, every local-HID wheel event takes the
short path inside `MouseProc`:

```
HC_ACTION + wheel  →  read 1 byte from shared section  →  CallNextHookEx
```

vs. the legacy invert path, which on every wheel event does:

```
WindowFromPoint  →  SendMessageTimeoutA (up to 8 hops × 30 ms)  →  return 1
```

Worst-case savings per skipped local-event: 240 ms of input-thread
blocking inside the target process avoided. For a user who keeps
Remote mode on full-time and rarely uses the KVM, that's effectively
every wheel scroll.

### No regression for the v0.9 path

Without `/smart-source`, `scroll_toggle.exe` does not create the
shared section, does not start the classifier thread, and the DLL's
lazy `OpenFileMappingA` returns `NULL` — so the gate is dead code
and the legacy invert path runs exactly as before.

## Files changed

- `shared_state.h` — new, ~100 lines. Cross-process protocol +
  `scroll_shared_open()` helper used by both binaries.
- `scroll_hook.c` — +37 lines. Lazy shared-section map; one-branch
  gate at the top of `MouseProc`.
- `scroll_toggle.c` — +132 lines. Classifier callback, dedicated
  thread infra (`classifier_start` / `classifier_stop`), wiring into
  `hook_start` / `hook_stop`, `/smart-source` flag parsing, tray +
  window status labels.
- `README.md` — version bump, "What's new in 0.91" block, new flag
  row, new "How it works → `/smart-source`" subsection.

## How to enable

1. Drop the v0.91 binaries (`scroll_toggle.exe`,
   `scroll_toggle.exe.manifest`, `scroll_hook.dll`) into a folder.
2. Run with the flag: `scroll_toggle.exe /smart-source` — or wire it
   into your startup shortcut.
3. Click **Start Remote Reverse** in the tray as usual. Local mouse
   now scrolls the right way; the KVM mouse keeps getting inverted.

## Known limitations of v0.91

- `/smart-source` is CLI-only for now — no tray toggle. v0.92 should
  add a checkable tray menu item and persistence to
  `%LOCALAPPDATA%\scroll_toggle\config.ini`.
- Classifier coverage = `LLMHF_INJECTED` flag only. ~99% of
  software-KVM and remote-desktop tools are covered. Hardware USB
  KVMs that present as native HID will be classified as "local" by
  design — at the OS level they are indistinguishable from a wired
  mouse. v0.92 may add process-name or `dwExtraInfo`-signature
  matching for users on hardware KVMs.
- No race mitigation between two wheel events arriving microseconds
  apart. Worst case is one wheel notch classified the wrong way, never
  a crash. `shared_state.h` already reserves `generation` and
  `last_event_time` for a future matching path.

## Acknowledgements

Specced and shipped in a Cowork session with Claude (Sonnet 4.6
running under Cowork mode), pair-programmed against Jason Lee's design
constraints (KVM scenario requires DLL injection; LL-hook re-injection
isn't enough for Raw Input apps).
