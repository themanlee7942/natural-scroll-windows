/*
 * shared_state.h — shared-memory protocol between scroll_toggle.exe and
 * scroll_hook.dll for the /smart-source feature.
 *
 * scroll_toggle.exe owns a WH_MOUSE_LL hook that classifies each wheel
 * event as "remote source" (LLMHF_INJECTED) or "local HID". The result
 * is written into a Local\ named file-mapping section. scroll_hook.dll
 * (which is injected into every GUI process by Windows when WH_MOUSE is
 * installed) maps the same section and consults the flag before
 * inverting the wheel.
 *
 * Ordering: WH_MOUSE_LL callbacks fire BEFORE WH_MOUSE callbacks for the
 * same physical event, so by the time MouseProc reads last_was_remote
 * the classifier has already written it. A tiny race remains if two
 * wheel events arrive within microseconds; the DLL mitigates by also
 * matching MOUSEHOOKSTRUCTEX::pt time-ordering — see scroll_hook.c.
 *
 * Author : Jason Lee   <https://github.com/themanlee7942>
 * License: MIT (c) 2026
 */
#ifndef SCROLL_SHARED_STATE_H
#define SCROLL_SHARED_STATE_H

#include <windows.h>

/* Local\ namespace = per-session, no admin required. Matches the
 * "session-only" promise of Remote mode in README.md. */
#define SCROLL_SHM_NAME "Local\\ScrollToggle_SharedState_v1"

#pragma pack(push, 8)
typedef struct {
    /* Bumped by the classifier on every wheel event it sees. The DLL can
     * compare against a remembered value to detect "did the classifier
     * run since I last looked?" — useful for the race-mitigation path. */
    volatile LONG  generation;

    /* MSLLHOOKSTRUCT::time on the last wheel event the classifier saw.
     * The DLL compares this against its own GetMessageTime() to decide
     * whether the classification is fresh enough to trust. */
    volatile DWORD last_event_time;

    /* 1 = the most recently classified wheel event had LLMHF_INJECTED
     * set (KVM / RDP / Parsec / Moonlight / virtual mouse / SendInput
     * from any app). 0 = local HID hardware mouse. */
    volatile BYTE  last_was_remote;

    /* 1 = DLL must honor last_was_remote and skip inversion for local
     * events. 0 = DLL inverts every wheel event regardless (legacy
     * Remote mode behavior, the default). Toggled by scroll_toggle.exe
     * when the user passes /smart-source or activates the tray option. */
    volatile BYTE  smart_source_enabled;

    /* Pad to 16 bytes so the struct stays on a clean cache line. */
    BYTE  _reserved[6];
} ScrollSharedState;
#pragma pack(pop)

/* Create-or-open the shared section.
 *
 *   out_handle         — receives the CreateFileMapping handle; caller
 *                        must CloseHandle on shutdown (the kernel
 *                        unmaps the section when the last handle dies)
 *   out_state          — receives the mapped pointer
 *   out_first_creator  — optional; set to TRUE iff THIS call was the
 *                        one that actually created the section (rather
 *                        than opening an existing one). The first
 *                        creator should zero-initialise the struct.
 *
 * Returns FALSE on failure; *out_state is NULL.
 */
static __inline BOOL scroll_shared_open(HANDLE *out_handle,
                                        ScrollSharedState **out_state,
                                        BOOL *out_first_creator) {
    if (out_handle)        *out_handle = NULL;
    if (out_state)         *out_state = NULL;
    if (out_first_creator) *out_first_creator = FALSE;

    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                  0, sizeof(ScrollSharedState),
                                  SCROLL_SHM_NAME);
    if (!h) return FALSE;
    BOOL first = (GetLastError() != ERROR_ALREADY_EXISTS);

    ScrollSharedState *p = (ScrollSharedState *)MapViewOfFile(
        h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(ScrollSharedState));
    if (!p) { CloseHandle(h); return FALSE; }

    if (first) {
        /* Defensive zero-init. CreateFileMapping does this already for a
         * fresh section, but be explicit so a future change to the
         * struct layout doesn't accidentally leak uninitialised flags. */
        p->generation           = 0;
        p->last_event_time      = 0;
        p->last_was_remote      = 0;
        p->smart_source_enabled = 0;
    }

    if (out_handle)        *out_handle = h;
    if (out_state)         *out_state = p;
    if (out_first_creator) *out_first_creator = first;
    return TRUE;
}

#endif /* SCROLL_SHARED_STATE_H */
