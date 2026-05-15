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
 * Author : Jason Lee   <https://github.com/themanlee7942>
 * License: MIT (c) 2026
 */
#include <windows.h>

/* MinGW's headers usually expose MOUSEHOOKSTRUCTEX, but define a local
 * fallback so the file builds with any reasonable Windows SDK. */
typedef struct {
    MOUSEHOOKSTRUCT mhs;
    DWORD           mouseData;
} MWH_STRUCT;

static HHOOK     g_hook    = NULL;
static HINSTANCE g_dllInst = NULL;

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

/* The hook callback. Exported so SetWindowsHookEx can find it.
 *
 * Strategy:
 *   1. For each WM_MOUSEWHEEL / WM_MOUSEHWHEEL event, find the deepest
 *      window under the cursor (so child panes — browser viewports,
 *      list views, IDE editors — receive the message, not just their
 *      top-level frame).
 *   2. SendMessageTimeout the inverted-delta message to that window.
 *      If it returns 0 (= "I didn't handle it" — common for chat-app
 *      input boxes that let wheel events bubble to a parent scroll
 *      view), walk up GetParent and try again. Bounded to 8 hops so
 *      we never spin.
 *   3. Return 1 to suppress the original event.
 *
 * SendMessage does NOT go through the queue, so our hook does not see
 * the messages we generate — no infinite loop, no marker needed.
 */
__declspec(dllexport)
LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0)
        return CallNextHookEx(NULL, code, wParam, lParam);

    if (code == HC_ACTION
        && (wParam == WM_MOUSEWHEEL || wParam == WM_MOUSEHWHEEL)) {

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
            if (ok && result == 0) break;     /* handled */
            t = GetParent(t);                 /* let parent try */
        }

        return 1;   /* suppress the original */
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_dllInst = hInst;
        DisableThreadLibraryCalls(hInst);
    }
    return TRUE;
}
