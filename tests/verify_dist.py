#!/usr/bin/env python3
"""
Static smoke test for dist/.

Parses each shipped PE file and asserts:
  * the machine architecture matches the filename (x64 vs x86),
  * the subsystem is Windows GUI,
  * every required Windows API the C source calls is actually present
    in the import table.

Run locally:    python3 tests/verify_dist.py
Run in CI:      same command after the build step. Exit code 1 on any
                failure, so the workflow stays red and Release publishing
                is skipped.
"""
from __future__ import annotations
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DIST = REPO / "dist"

MACHINE_X64 = 0x8664
MACHINE_X86 = 0x014C
SUBSYSTEM_GUI = 2

# What each file must satisfy.  (path, expected_machine, required_imports)
EXE_BASE_IMPORTS = {
    # tray + window
    "Shell_NotifyIconA", "CreatePopupMenu", "TrackPopupMenu",
    "CreateWindowExA", "RegisterClassA", "ShowWindow",
    # local mode (registry + driver restart)
    "SetupDiGetClassDevsA", "SetupDiEnumDeviceInfo",
    "SetupDiCallClassInstaller", "RegSetValueExA", "RegOpenKeyExA",
    # remote mode loader
    "LoadLibraryA", "GetProcAddress", "FreeLibrary",
    # elevation
    "ShellExecuteExA", "OpenProcessToken", "GetTokenInformation",
    # icon (embedded resource)
    "CreateIconFromResourceEx", "LookupIconIdFromDirectoryEx",
}

DLL_REQUIRED_IMPORTS = {
    "SetWindowsHookExA", "UnhookWindowsHookEx", "CallNextHookEx",
    "SendMessageTimeoutA", "WindowFromPoint", "GetParent",
}

EXPECTED = [
    ("dist/scroll_toggle.exe",     MACHINE_X64, EXE_BASE_IMPORTS),
    ("dist/scroll_toggle_x86.exe", MACHINE_X86, EXE_BASE_IMPORTS),
    ("dist/scroll_hook.dll",       MACHINE_X64, DLL_REQUIRED_IMPORTS),
    ("dist/scroll_hook_x86.dll",   MACHINE_X86, DLL_REQUIRED_IMPORTS),
]

MACHINE_NAME = {MACHINE_X64: "x86-64", MACHINE_X86: "i386"}


def parse_pe(path: Path) -> tuple[int, int, set[str]]:
    """Return (machine, subsystem, {imported names}) for a PE file."""
    data = path.read_bytes()
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\x00\x00":
        raise ValueError(f"{path}: not a PE file")
    machine = struct.unpack_from("<H", data, pe_off + 4)[0]
    nsec = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    opt_off = pe_off + 24
    magic = struct.unpack_from("<H", data, opt_off)[0]
    is_plus = magic == 0x20B
    # Subsystem offset: 0x44 (PE32+) or 0x44 also (PE32). Both put it at the same place.
    subsystem = struct.unpack_from("<H", data, opt_off + 0x44)[0]
    # Data directory: 0x70 (PE32+) or 0x60 (PE32) from opt_off.
    ddir_off = opt_off + (0x70 if is_plus else 0x60)
    imp_rva = struct.unpack_from("<I", data, ddir_off + 8)[0]

    # Section table starts after the optional header.
    sec_off = opt_off + opt_size
    sections = []
    for i in range(nsec):
        o = sec_off + i * 40
        sections.append((
            struct.unpack_from("<I", data, o + 8)[0],   # VirtualSize
            struct.unpack_from("<I", data, o + 12)[0],  # VirtualAddress
            struct.unpack_from("<I", data, o + 16)[0],  # SizeOfRawData
            struct.unpack_from("<I", data, o + 20)[0],  # PointerToRawData
        ))

    def r2o(rva: int) -> int | None:
        for vsz, vrva, rsz, roff in sections:
            if vrva <= rva < vrva + max(vsz, rsz):
                return roff + (rva - vrva)
        return None

    imports: set[str] = set()
    if imp_rva == 0:
        return machine, subsystem, imports

    desc_off = r2o(imp_rva)
    if desc_off is None:
        return machine, subsystem, imports
    ptr_size = 8 if is_plus else 4
    high_bit = 1 << (ptr_size * 8 - 1)

    while True:
        oft = struct.unpack_from("<I", data, desc_off)[0]
        name_rva = struct.unpack_from("<I", data, desc_off + 12)[0]
        ft = struct.unpack_from("<I", data, desc_off + 16)[0]
        if name_rva == 0:
            break
        thunk_rva = oft or ft
        thunk_off = r2o(thunk_rva)
        if thunk_off is None:
            break
        while True:
            if is_plus:
                v = struct.unpack_from("<Q", data, thunk_off)[0]
            else:
                v = struct.unpack_from("<I", data, thunk_off)[0]
            if v == 0:
                break
            if not (v & high_bit):
                hint_off = r2o(v)
                if hint_off is not None:
                    end = data.index(b"\x00", hint_off + 2)
                    imports.add(data[hint_off + 2:end].decode("ascii", "replace"))
            thunk_off += ptr_size
        desc_off += 20

    return machine, subsystem, imports


def main() -> int:
    failed = 0
    for rel, expected_machine, required in EXPECTED:
        path = REPO / rel
        print(f"\n[check] {rel}")
        if not path.exists():
            print(f"  FAIL  file missing: {path}")
            failed += 1
            continue

        try:
            machine, subsystem, imports = parse_pe(path)
        except Exception as e:
            print(f"  FAIL  unable to parse: {e}")
            failed += 1
            continue

        mname = MACHINE_NAME.get(machine, f"0x{machine:04x}")
        exp_name = MACHINE_NAME[expected_machine]
        if machine != expected_machine:
            print(f"  FAIL  machine is {mname}, expected {exp_name}")
            failed += 1
        else:
            print(f"  ok    machine = {mname}")

        if subsystem != SUBSYSTEM_GUI:
            print(f"  FAIL  subsystem = {subsystem}, expected {SUBSYSTEM_GUI} (GUI)")
            failed += 1
        else:
            print("  ok    subsystem = GUI")

        missing = sorted(required - imports)
        if missing:
            print(f"  FAIL  missing imports: {missing}")
            failed += 1
        else:
            print(f"  ok    all {len(required)} required imports present")

    print()
    if failed:
        print(f"=== {failed} check(s) failed ===")
        return 1
    print("=== all checks passed ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
