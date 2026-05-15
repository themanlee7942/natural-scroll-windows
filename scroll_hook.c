/*
 * scroll_hook.dll — global mouse-wheel direction inverter.
 *
 * Loaded on demand by scroll_toggle.exe when the user activates
 * "Remote Reverse". Installs a WH_MOUSE hook with dwThreadId = 0 so
 * Windows injects this DLL into every GUI process. Inside each
 * process, the hook callback intercepts WM_MOUSEWHEEL / WM_MOUSEHWHEEL,
 * suppresses the original message, and synthesises a fresh message
 * with the wheel delta sign flipped, delivered straight to the window
 * under the cursor via SendMessage. This bypasses the OS message queue
 * (and therefore our own hook), so there is no SendInput round-trip
 * and no event-loop feedback. It is the same pattern used by
 * AutoHotkey and WizMouse for smooth wheel manipulation.
 *
 * When scroll_toggle.exe is started with /smart-source it also brings
 * up a WH_MOUSE_LL classifier and publishes the verdict (remote vs
 * local) into a Local\ named file-mapping section. This DLL maps that
 * section lazily and skips inversion for local-HID events.
 *
 * Author : Jason Lee   <https://github.com/themanlee7942>
 * License: MIT (c) 2026
 */
#include <windows.h>
#include "shared_state.h"

/* MinGW's headers usually expose MOUSEHOOKSTRUCTEX, but define a local
 * fallback so the file builds with any reasonable Windows SDK. */
typedef struct {
    MOUSEHOOKSTRUCT mhs;
    DWORD           mouseData;
} MWH_STRUCT;

static HHOOK              g_hook       = NULL;
static HINSTANCE          g_dllInst    = NULL;

/* Shared-state mapping. Opened lazily on the first wheel event we see
 * (rather than in DllMain) so we never run loader-lock-unsafe code in
 * DLL_PROCESS_ATTACH. If the section doesn't exist (scroll_toggle.exe
 * isn't running smart-source mode) we just behave like the legacy
 * Remote mode and invert everything. */
static HANDLE             g_shmHandle  = NULL;
static ScrollSharedState *g_shm        = NULL;
static volatile LONG      g_shmTried   = 0;   /* 1 once we've attempted open */

__declspec(dllexport)
LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam);

/* Public: install/remove the global wheel-inversion hook. */
__declspec(dllexport)
BOOL install_hook(void) {
    if (g_hook) return TRUE;
    g_hook = SetWindowsHookExA(WH_MOUSE, MouseProc, g_dllInst, 0);
    return g_hook != NULL;
}

__declspec(dllexport)
BOOL uninstall_hook(void) {
    if (!g_hook) return TRUE;
    BOOL ok = UnhookWindowsHookEx(g_hook);
    g_hook = NULL;
    return ok;
}

/* Open the shared-state section if it exists. Safe to call from any
 * thread; idempotent via InterlockedExchange guard. Called from the
 * hot path on first wheel event so we never block DLL_PROCESS_ATTACH. */
static void try_open_shared_state(void) {
    if (InterlockedExchange(&g_shmTried, 1) != 0) return;
    HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, SCROLL_SHM_NAME);
    if (!h) return;
    ScrollSharedState *p = (ScrollSharedState *)MapViewOfFile(
        h, FILE_MAP_READ, 0, 0, sizeof(ScrollSharedState));
    if (!p) { CloseHandle(h); return; }
    g_shmHandle = h;
    g_shm       = p;
}

/* The hook callback. Exported so SetWindowsHookEx can find it.
 *
 * Strategy:
 *   0. If /smart-source mode is enabled and the most recent classification
 *      from scroll_toggle.exe's LL hook says "this event came from a
 *      local HID device", do nothing and let it through unchanged. The
 *      registry-based Local mode (or the user's normal scroll direction)
 *      then takes over.
 *   1. Otherwise (legacy behaviour or remote-source event), for each
 *      WM_MOUSEWHEEL / WM_MOUSEHWHEEL event, find the deepest window
 *      under the cursor (so child panes — browser viewports, list views,
 *      IDE editors — receive the message, not just their top-level frame).
 *   2. SendMessageTimeout the inverted-delta message to that window.
 *      If it returns 0 (= "I didn't handle it"), walk up GetParent and
 *      try again. Bounded to 8 hops so we never spin.
 *   3. Return 1 to suppress the original event.
 */
__declspec(dllexport)
LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0)
        return CallNextHookEx(NULL, code, wParam, lParam);

    if (code == HC_ACTION
        && (wParam == WM_MOUSEWHEEL || wParam == WM_MOUSEHWHEEL)) {

        if (!g_shmTried) try_open_shared_state();
        if (g_shm && g_shm->smart_source_enabled && !g_shm->last_was_remote) {
            return CallNextHookEx(NULL, code, wParam, lParam);
        }

        MWH_STRUCT *mh    = (MWH_STRUCT *)lParam;
        short       delta = (short)HIWORD(mh->mouseData);

        WPARAM newWp = ((WPARAM)(WORD)(short)(-delta)) << 16;
        LPARAM newLp = MAKELPARAM(mh->mhs.pt.x, mh->mhs.pt.y);

        HWND t = WindowFromPoint(mh->mhs.pt);
        if (!t) t = mh->mhs.hwnd;

        for (int hops = 0; hops < 8 && t; hops++) {
            DWORD_PTR result = 0;
            BOOL ok = SendMessageTimeoutA(t, (UINT)wParam, newWp, newLp,
                                          SMTO_ABORTIFHUNG | SMTO_NORMAL,
                                          30, &result);
            if (ok && result == 0) break;
            t = GetParent(t);
        }

        return 1;
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_dllInst = hInst;
        DisableThreadLibraryCalls(hInst);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_shm)       { UnmapViewOfFile(g_shm); g_shm = NULL; }
        if (g_shmHandle) { CloseHandle(g_shmHandle); g_shmHandle = NULL; }
    }
    return TRUE;
}
