/*
 * scroll_toggle.exe — toggle Windows mouse-wheel direction.
 * Author: Jason Lee   (https://github.com/themanlee7942)
 * License: MIT (c) 2026
 *
 *   LOCAL  — flips per-device FlipFlopWheel, restarts the mouse driver
 *            in-process via SetupAPI. Persistent. Auto-elevates (UAC).
 *   REMOTE — loads scroll_hook.dll, which installs a global WH_MOUSE
 *            hook so Windows injects the DLL into every GUI process.
 *            Inside each process the DLL rewrites WM_MOUSEWHEEL with
 *            the wheel delta flipped, delivered straight to the window
 *            under the cursor — no SendInput round-trip, no jitter.
 *            Session-only. No admin needed.
 *
 * Lives in the system tray. Right-click for menu, double-click to show.
 *
 * Commands:
 *     scroll_toggle.exe                  open UI, install tray icon
 *     scroll_toggle.exe /local-toggle    one-shot local toggle (used internally for UAC)
 *     scroll_toggle.exe /hook            headless hook mode
 *     scroll_toggle.exe /stop            stop any running hook
 */
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "icon_data.h"
#include "shared_state.h"

/* Load the embedded ICO at a chosen pixel size. Falls back to NULL. */
static HICON load_embedded_icon(int size) {
    int offset = LookupIconIdFromDirectoryEx((PBYTE)TRAY_ICO_BYTES, TRUE,
                                             size, size, LR_DEFAULTCOLOR);
    if (offset <= 0) return NULL;
    return CreateIconFromResourceEx((PBYTE)TRAY_ICO_BYTES + offset,
                                    (DWORD)(TRAY_ICO_SIZE - offset),
                                    TRUE, 0x00030000, size, size,
                                    LR_DEFAULTCOLOR);
}

/* ── IDs ──────────────────────────────────────────────────────────────── */
#define ID_LOCAL_BTN     1001
#define ID_REMOTE_BTN    1002
#define ID_ABOUT_BTN     1003
#define ID_HIDE_BTN      1004
#define ID_TIMER         2001

#define IDM_TOGGLE_LOCAL   3001
#define IDM_TOGGLE_REMOTE  3002
#define IDM_SHOW           3003
#define IDM_ABOUT          3004
#define IDM_GITHUB         3005
#define IDM_EXIT           3006

#define WM_TRAYICON  (WM_USER + 1)
#define APP_TITLE    "Mouse Wheel Direction Toggler"
#define APP_GITHUB   "https://github.com/themanlee7942"
#define APP_VERSION  "0.9"
#define APP_AUTHOR   "Jason Lee"
#define APP_YEAR     "2026"

/* Resource ID of the embedded icon (matches app.rc). */
#define IDI_TRAY     100

static const char *WND_CLASS = "ScrollToggleMainWnd_v4";

static HINSTANCE g_inst         = NULL;
static HWND      g_main         = NULL;
static HWND      g_localStatus  = NULL;
static HWND      g_remoteStatus = NULL;
static HWND      g_localBtn     = NULL;
static HWND      g_remoteBtn    = NULL;
static HHOOK     g_hook         = NULL;
static HFONT     g_font         = NULL;
static HFONT     g_titleFont    = NULL;
static NOTIFYICONDATAA g_nid    = {0};
static BOOL      g_trayInstalled = FALSE;

/* ── LOCAL-mode log ───────────────────────────────────────────────────
 * Set SCROLL_DEBUG_LOG to 1 (or pass -DSCROLL_DEBUG_LOG=1 to the
 * compiler) to append registry-write diagnostics to C:\scroll_debug.txt
 * on every Local toggle. Disabled by default so a normal install never
 * creates a stray file at the root of C:. Remote mode is silent in
 * both configurations — the hot path has no I/O. */
#ifndef SCROLL_DEBUG_LOG
#  define SCROLL_DEBUG_LOG 0
#endif

#if SCROLL_DEBUG_LOG
static FILE *g_log = NULL;
static void log_open(void) {
    g_log = fopen("C:\\scroll_debug.txt", "a");
    if (!g_log) return;
    time_t t = time(NULL); char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(g_log, "\n========== Local toggle @ %s ==========\n", ts);
    fflush(g_log);
}
static void log_close(void) { if (g_log) { fclose(g_log); g_log = NULL; } }
#  define LOG(...) do { if (g_log) { fprintf(g_log, __VA_ARGS__); fflush(g_log); } } while (0)
#else
#  define log_open()  ((void)0)
#  define log_close() ((void)0)
#  define LOG(...)    ((void)0)
#endif

/* ── Elevation ────────────────────────────────────────────────────────── */
static BOOL is_elevated(void) {
    BOOL admin = FALSE; HANDLE tok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION e; DWORD sz = 0;
        if (GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &sz))
            admin = e.TokenIsElevated ? TRUE : FALSE;
        CloseHandle(tok);
    }
    return admin;
}
static DWORD spawn_self_elevated(LPCSTR args, BOOL wait) {
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof(self));
    SHELLEXECUTEINFOA sei = {0};
    sei.cbSize = sizeof(sei);
    sei.fMask = wait ? SEE_MASK_NOCLOSEPROCESS : 0;
    sei.lpVerb = "runas";
    sei.lpFile = self;
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExA(&sei)) return GetLastError();
    if (wait && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 15000);
        CloseHandle(sei.hProcess);
    }
    return 0;
}

/* ── Registry helpers ────────────────────────────────────────────────── */
static int read_first_mouse_flipflop(void) {
    int result = -1;
    HDEVINFO hSet = SetupDiGetClassDevsA(&GUID_DEVCLASS_MOUSE, NULL, NULL, DIGCF_PRESENT);
    if (hSet == INVALID_HANDLE_VALUE) return -1;
    HKEY hEnum;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum",
                      0, KEY_READ, &hEnum) != ERROR_SUCCESS) {
        SetupDiDestroyDeviceInfoList(hSet); return -1;
    }
    SP_DEVINFO_DATA d = { sizeof(d) };
    if (SetupDiEnumDeviceInfo(hSet, 0, &d)) {
        char id[MAX_DEVICE_ID_LEN];
        if (CM_Get_Device_IDA(d.DevInst, id, sizeof(id), 0) == CR_SUCCESS) {
            char p[1024]; snprintf(p, sizeof(p), "%s\\Device Parameters", id);
            HKEY pk;
            if (RegOpenKeyExA(hEnum, p, 0, KEY_READ, &pk) == ERROR_SUCCESS) {
                DWORD v=0, sz=sizeof(v), ty=REG_DWORD;
                if (RegQueryValueExA(pk, "FlipFlopWheel", NULL, &ty,
                                     (LPBYTE)&v, &sz) == ERROR_SUCCESS)
                    result = (int)v;
                RegCloseKey(pk);
            }
        }
    }
    RegCloseKey(hEnum);
    SetupDiDestroyDeviceInfoList(hSet);
    return result;
}

typedef enum { OP_WRITE, OP_VERIFY } OpKind;
static void walk_class(const GUID *g, const char *name, OpKind op, DWORD target,
                       int *out_count, int *out_ok) {
    HDEVINFO hSet = SetupDiGetClassDevsA(g, NULL, NULL, DIGCF_PRESENT);
    if (hSet == INVALID_HANDLE_VALUE) return;
    HKEY hEnum;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum",
                      0, KEY_READ, &hEnum) != ERROR_SUCCESS) {
        SetupDiDestroyDeviceInfoList(hSet); return;
    }
    SP_DEVINFO_DATA did = { sizeof(did) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(hSet, i, &did); i++) {
        char id[MAX_DEVICE_ID_LEN];
        if (CM_Get_Device_IDA(did.DevInst, id, sizeof(id), 0) != CR_SUCCESS) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s\\Device Parameters", id);
        HKEY pk;
        DWORD access = (op == OP_WRITE) ? (KEY_READ | KEY_WRITE) : KEY_READ;
        if (RegOpenKeyExA(hEnum, path, 0, access, &pk) != ERROR_SUCCESS) continue;
        if (out_count) (*out_count)++;
        DWORD val = 0, size = sizeof(val), type = REG_DWORD;
        RegQueryValueExA(pk, "FlipFlopWheel", NULL, &type, (LPBYTE)&val, &size);
        if (op == OP_WRITE) {
            LONG wR = RegSetValueExA(pk, "FlipFlopWheel", 0, REG_DWORD,
                                     (LPBYTE)&target, sizeof(DWORD));
            LOG("[%s] %s | Old: %lu -> New: %lu | Result: %ld\n",
                name, path, val, target, wR);
            if (wR == ERROR_SUCCESS && out_ok) (*out_ok)++;
        } else {
            BOOL ok = (val == target);
            if (ok && out_ok) (*out_ok)++;
            (void)name;
        }
        RegCloseKey(pk);
    }
    RegCloseKey(hEnum);
    SetupDiDestroyDeviceInfoList(hSet);
}

static int restart_mouse_class(void) {
    int n = 0;
    HDEVINFO hSet = SetupDiGetClassDevsA(&GUID_DEVCLASS_MOUSE, NULL, NULL, DIGCF_PRESENT);
    if (hSet == INVALID_HANDLE_VALUE) return 0;
    SP_DEVINFO_DATA did = { sizeof(did) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(hSet, i, &did); i++) {
        SP_PROPCHANGE_PARAMS pcp = {0};
        pcp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        pcp.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        pcp.StateChange = DICS_PROPCHANGE;
        pcp.Scope = DICS_FLAG_GLOBAL;
        BOOL a = SetupDiSetClassInstallParamsA(hSet, &did,
                     (SP_CLASSINSTALL_HEADER*)&pcp, sizeof(pcp));
        if (a && SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, hSet, &did)) n++;
    }
    SetupDiDestroyDeviceInfoList(hSet);
    return n;
}

static int do_local_toggle(void) {
    log_open();
    int cur = read_first_mouse_flipflop();
    DWORD target = (cur == 1) ? 0 : 1;
    LOG("Current=%d -> Target=%lu\n", cur, target);
    int written = 0, total = 0;
    walk_class(&GUID_DEVCLASS_MOUSE,    "Mouse",    OP_WRITE, target, &total, &written);
    walk_class(&GUID_DEVCLASS_HIDCLASS, "HIDClass", OP_WRITE, target, &total, &written);
    int restarted = restart_mouse_class();
    LOG("Wrote %d/%d, Restarted %d drivers.\n", written, total, restarted);
    log_close();
    return 0;
}

/* ── Hook (remote mode) — DLL-injection path via scroll_hook.dll ─────
 *
 * Old approach was WH_MOUSE_LL + SendInput, which has unavoidable
 * round-trip latency for every wheel event. The smooth approach used
 * by AutoHotkey / WizMouse is a WH_MOUSE hook in a DLL that Windows
 * injects into every GUI process; the hook proc modifies the wheel
 * delta IN PLACE inside the target window's process, so there is no
 * SendInput round-trip and no perceptible lag.
 *
 * We load scroll_hook.dll lazily when the user starts Remote mode and
 * unload it when they stop. The DLL must live next to scroll_toggle.exe.
 */
typedef BOOL (*hook_fn)(void);

static HMODULE  g_hookDll       = NULL;
static hook_fn  g_dll_install   = NULL;
static hook_fn  g_dll_uninstall = NULL;
static BOOL     g_hookActive    = FALSE;

/* ── /smart-source state ───────────────────────────────────────────────
 *
 * When the user passes /smart-source on the command line, scroll_toggle.exe
 * installs an additional WH_MOUSE_LL hook on a dedicated thread that
 * CLASSIFIES every wheel event (LLMHF_INJECTED → "remote", else "local")
 * and writes the result into a shared-memory section. scroll_hook.dll —
 * which is what actually inverts wheel deltas inside every GUI process —
 * reads that section and bypasses inversion for local-HID events.
 *
 * Net effect for the user: in Remote mode, KVM/RDP/Parsec wheel still
 * gets inverted, but the local wired mouse passes through untouched.
 * No more "I forgot to disable Remote mode and now my desk mouse is
 * also reversed" footgun.
 *
 * Opt-in only — without /smart-source the legacy behaviour (invert
 * everything) is preserved 1:1.
 */
static BOOL              g_smartSource = FALSE;
static HANDLE            g_shmHandle   = NULL;
static ScrollSharedState *g_shm        = NULL;

static HHOOK   g_classifierHook     = NULL;
static HANDLE  g_classifierThread   = NULL;
static DWORD   g_classifierThreadId = 0;
static HANDLE  g_classifierReady    = NULL;

/* The WH_MOUSE_LL fallback below is no longer wired up by hook_start()
 * but is kept for reference / use if scroll_hook.dll is missing. */
#define SCROLL_TOGGLE_MAGIC ((ULONG_PTR)0x53435254)   /* "SCRT" */

static LRESULT CALLBACK wheel_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION)
        return CallNextHookEx(NULL, nCode, wParam, lParam);

    if (wParam == WM_MOUSEWHEEL || wParam == WM_MOUSEHWHEEL) {
        MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lParam;

        /* Real (non-synthetic) wheel events from physical HID devices
         * already work through Local mode. Skipping them here means
         * the wired mouse pays zero round-trip latency. */
        if (!(m->flags & LLMHF_INJECTED))
            return CallNextHookEx(NULL, nCode, wParam, lParam);

        /* Our own re-injection: pass through unchanged. */
        if (m->dwExtraInfo == SCROLL_TOGGLE_MAGIC)
            return CallNextHookEx(NULL, nCode, wParam, lParam);

        short delta = (short)HIWORD(m->mouseData);
        INPUT in = {0};
        in.type = INPUT_MOUSE;
        in.mi.mouseData = (DWORD)(-(INT)delta);   /* sign-extended negation */
        in.mi.dwFlags = (wParam == WM_MOUSEWHEEL)
                        ? MOUSEEVENTF_WHEEL : MOUSEEVENTF_HWHEEL;
        in.mi.dwExtraInfo = SCROLL_TOGGLE_MAGIC;
        SendInput(1, &in, sizeof(INPUT));
        return 1;   /* suppress original */
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

/* The hook lives on its OWN dedicated thread. The UI thread is busy
 * with WM_PAINT / WM_TIMER / menu work, and WH_MOUSE_LL callbacks must
 * be served by the same thread that installed the hook — running them
 * off the UI thread is the standard fix for "I can feel the hook" lag.
 */
static HANDLE g_hookThread   = NULL;
static DWORD  g_hookThreadId = 0;
static HANDLE g_hookReady    = NULL;

static DWORD WINAPI hook_thread_proc(LPVOID arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, wheel_proc, g_inst, 0);
    SetEvent(g_hookReady);
    if (!g_hook) return 1;
    /* Need a message loop — the OS dispatches hook callbacks through
     * the thread's message queue. */
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    UnhookWindowsHookEx(g_hook);
    g_hook = NULL;
    return 0;
}

/* ── /smart-source classifier ─────────────────────────────────────────
 *
 * Low-level mouse hook callback. Runs BEFORE scroll_hook.dll's WH_MOUSE
 * (LL hooks are invoked before higher-level hooks for the same event),
 * so by the time MouseProc in the DLL reads g_shm->last_was_remote it
 * already reflects this event.
 *
 * Hot-path discipline: every line here runs on the system input thread.
 * Non-wheel events return after a single branch + CallNextHookEx. For
 * wheel events we do one flag read, one struct write, one atomic inc,
 * then forward. No allocations, no syscalls beyond the chain.
 */
static LRESULT CALLBACK classifier_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION)
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    if (wParam != WM_MOUSEWHEEL && wParam != WM_MOUSEHWHEEL)
        return CallNextHookEx(NULL, nCode, wParam, lParam);

    MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lParam;
    if (g_shm) {
        BYTE remote = (m->flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED))
                       ? 1 : 0;
        g_shm->last_event_time = m->time;
        g_shm->last_was_remote = remote;
        InterlockedIncrement(&g_shm->generation);
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static DWORD WINAPI classifier_thread_proc(LPVOID arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    g_classifierHook = SetWindowsHookExW(WH_MOUSE_LL, classifier_proc,
                                         g_inst, 0);
    SetEvent(g_classifierReady);
    if (!g_classifierHook) return 1;
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    UnhookWindowsHookEx(g_classifierHook);
    g_classifierHook = NULL;
    return 0;
}

static BOOL classifier_start(void) {
    if (g_classifierThread) return TRUE;
    if (!scroll_shared_open(&g_shmHandle, &g_shm, NULL)) return FALSE;
    g_shm->smart_source_enabled = 1;
    g_classifierReady = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_classifierReady) return FALSE;
    g_classifierThread = CreateThread(NULL, 0, classifier_thread_proc,
                                      NULL, 0, &g_classifierThreadId);
    if (!g_classifierThread) {
        CloseHandle(g_classifierReady); g_classifierReady = NULL;
        return FALSE;
    }
    WaitForSingleObject(g_classifierReady, 5000);
    CloseHandle(g_classifierReady);
    g_classifierReady = NULL;
    return g_classifierHook != NULL;
}

static void classifier_stop(void) {
    if (g_classifierThread) {
        if (g_classifierThreadId)
            PostThreadMessageA(g_classifierThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_classifierThread, 5000);
        CloseHandle(g_classifierThread);
        g_classifierThread   = NULL;
        g_classifierThreadId = 0;
    }
    if (g_shm) {
        g_shm->smart_source_enabled = 0;
        UnmapViewOfFile(g_shm); g_shm = NULL;
    }
    if (g_shmHandle) { CloseHandle(g_shmHandle); g_shmHandle = NULL; }
}

/* Diagnostic info from the last failed hook_start(). */
static DWORD g_hookErr  = 0;
static char  g_hookWhy[128] = "";

static BOOL hook_start(void) {
    if (g_hookActive) return TRUE;
    if (!g_hookDll) {
#if defined(_WIN64)
        const char *dllname = "scroll_hook.dll";
#else
        const char *dllname = "scroll_hook_x86.dll";
#endif
        g_hookDll = LoadLibraryA(dllname);
        if (!g_hookDll) {
            g_hookErr = GetLastError();
            snprintf(g_hookWhy, sizeof(g_hookWhy),
                "LoadLibrary(%s) failed", dllname);
            return FALSE;
        }
        g_dll_install   = (hook_fn)GetProcAddress(g_hookDll, "install_hook");
        g_dll_uninstall = (hook_fn)GetProcAddress(g_hookDll, "uninstall_hook");
        if (!g_dll_install || !g_dll_uninstall) {
            g_hookErr = GetLastError();
            snprintf(g_hookWhy, sizeof(g_hookWhy),
                "DLL is missing install_hook/uninstall_hook exports");
            FreeLibrary(g_hookDll); g_hookDll = NULL; return FALSE;
        }
    }
    if (g_smartSource && !classifier_start()) {
        g_hookErr = GetLastError();
        snprintf(g_hookWhy, sizeof(g_hookWhy),
            "/smart-source classifier failed to start");
        return FALSE;
    }
    if (!g_dll_install()) {
        g_hookErr = GetLastError();
        snprintf(g_hookWhy, sizeof(g_hookWhy),
            "SetWindowsHookEx inside DLL failed");
        if (g_smartSource) classifier_stop();
        return FALSE;
    }
    g_hookErr = 0; g_hookWhy[0] = 0;
    g_hookActive = TRUE;
    return TRUE;
}
static void hook_stop(void) {
    if (!g_hookActive) return;
    if (g_dll_uninstall) g_dll_uninstall();
    g_hookActive = FALSE;
    /* Tear down the classifier last so any in-flight DLL MouseProc
     * read still sees a valid section. */
    classifier_stop();
    /* Keep the DLL loaded so subsequent start/stop is instant. It will
     * be freed when the process exits. */
}

/* ── System tray ─────────────────────────────────────────────────────── */
static void tray_update_tip(void) {
    int cur = read_first_mouse_flipflop();
    snprintf(g_nid.szTip, sizeof(g_nid.szTip),
             "Mouse Wheel — Local: %s, Remote: %s",
             cur == 1 ? "Reversed" : "Normal",
             g_hookActive ? "ON" : "OFF");
    if (g_trayInstalled) Shell_NotifyIconA(NIM_MODIFY, &g_nid);
}

static void tray_add(void) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = load_embedded_icon(GetSystemMetrics(SM_CXSMICON));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    strncpy(g_nid.szTip, APP_TITLE, sizeof(g_nid.szTip) - 1);
    if (Shell_NotifyIconA(NIM_ADD, &g_nid)) {
        g_trayInstalled = TRUE;
        tray_update_tip();
    }
}
static void tray_remove(void) {
    if (g_trayInstalled) {
        Shell_NotifyIconA(NIM_DELETE, &g_nid);
        g_trayInstalled = FALSE;
    }
}

static void show_main_window(void) {
    ShowWindow(g_main, SW_SHOW);
    SetForegroundWindow(g_main);
}

static void show_tray_menu(void) {
    HMENU menu = CreatePopupMenu();
    int cur = read_first_mouse_flipflop();
    char local_label[64];
    snprintf(local_label, sizeof(local_label),
             "Toggle Local Direction  (now: %s)",
             cur == 1 ? "Reversed" : "Normal");
    AppendMenuA(menu, MF_STRING, IDM_TOGGLE_LOCAL, local_label);
    {
        char remote_label[80];
        snprintf(remote_label, sizeof(remote_label),
                 "%s Remote Reverse%s",
                 g_hookActive ? "Stop" : "Start",
                 g_smartSource ? "  (Smart)" : "");
        AppendMenuA(menu, MF_STRING, IDM_TOGGLE_REMOTE, remote_label);
    }
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, IDM_SHOW,   "Show Window");
    AppendMenuA(menu, MF_STRING, IDM_ABOUT,  "About...");
    AppendMenuA(menu, MF_STRING, IDM_GITHUB, "Open GitHub Page");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, IDM_EXIT,   "Exit");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_main);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, g_main, NULL);
    DestroyMenu(menu);
}

static void show_about(void) {
    char msg[600];
    snprintf(msg, sizeof(msg),
        "%s\n"
        "Version %s\n\n"
        "Toggle Windows mouse-wheel direction without rebooting.\n"
        "Local mode (registry + driver restart) + Remote mode\n"
        "(low-level hook + SendInput).\n\n"
        "Author: %s\n"
        "GitHub: %s\n\n"
        "MIT License (c) %s\n\n"
        "Click OK, then 'Open GitHub Page' from the tray menu to visit.",
        APP_TITLE, APP_VERSION, APP_AUTHOR, APP_GITHUB, APP_YEAR);
    MessageBoxA(g_main, msg, "About " APP_TITLE,
                MB_ICONINFORMATION | MB_OK);
}

static void open_github(void) {
    ShellExecuteA(NULL, "open", APP_GITHUB, NULL, NULL, SW_SHOWNORMAL);
}

/* ── UI ──────────────────────────────────────────────────────────────── */
static void refresh_status(void) {
    int cur = read_first_mouse_flipflop();
    const char *dir = (cur == 1) ? "Reversed" : (cur == 0) ? "Normal" : "(unknown)";
    char s[128];
    snprintf(s, sizeof(s), "Direction: %s", dir);
    if (g_localStatus) SetWindowTextA(g_localStatus, s);
    if (g_remoteStatus) {
        const char *line = g_hookActive
            ? (g_smartSource ? "Status: ON  (Smart source)" : "Status: ON")
            : (g_smartSource ? "Status: OFF (Smart source)" : "Status: OFF");
        SetWindowTextA(g_remoteStatus, line);
    }
    if (g_remoteBtn)
        SetWindowTextA(g_remoteBtn, g_hookActive ? "Stop" : "Start");
    tray_update_tip();
}

static void on_local_clicked(void) {
    if (!is_elevated()) {
        DWORD err = spawn_self_elevated("/local-toggle", TRUE);
        if (err && err != ERROR_CANCELLED) {
            char m[160];
            snprintf(m, sizeof(m), "Could not elevate (err=%lu).", err);
            MessageBoxA(g_main, m, APP_TITLE, MB_ICONERROR);
        }
    } else {
        do_local_toggle();
    }
    refresh_status();
}
static void on_remote_clicked(void) {
    if (g_hookActive) hook_stop();
    else if (!hook_start()) {
        char m[400];
        snprintf(m, sizeof(m),
            "Could not start Remote mode.\n\n"
            "Reason: %s\n"
            "Win32 error: %lu\n\n"
            "Common causes:\n"
            "  - scroll_hook.dll is missing from this folder\n"
            "  - DLL bitness does not match the exe (x64 vs x86)\n"
            "  - DLL is corrupted or blocked by antivirus",
            g_hookWhy[0] ? g_hookWhy : "(unknown)", g_hookErr);
        MessageBoxA(g_main, m, APP_TITLE, MB_ICONERROR);
    }
    refresh_status();
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(w)) {
            case ID_LOCAL_BTN:        on_local_clicked();  break;
            case ID_REMOTE_BTN:       on_remote_clicked(); break;
            case ID_ABOUT_BTN:        show_about();        break;
            case ID_HIDE_BTN:         ShowWindow(h, SW_HIDE); break;
            case IDM_TOGGLE_LOCAL:    on_local_clicked();  break;
            case IDM_TOGGLE_REMOTE:   on_remote_clicked(); break;
            case IDM_SHOW:            show_main_window();  break;
            case IDM_ABOUT:           show_about();        break;
            case IDM_GITHUB:          open_github();       break;
            case IDM_EXIT:
                hook_stop(); tray_remove(); DestroyWindow(h); break;
        }
        return 0;
    case WM_TRAYICON:
        if (LOWORD(l) == WM_LBUTTONDBLCLK) show_main_window();
        else if (LOWORD(l) == WM_RBUTTONUP) show_tray_menu();
        return 0;
    case WM_CLOSE:    ShowWindow(h, SW_HIDE); return 0;  /* hide, don't quit */
    case WM_DESTROY:
        hook_stop();
        tray_remove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void apply_font(HWND ctrl, HFONT f) {
    if (f) SendMessageA(ctrl, WM_SETFONT, (WPARAM)f, TRUE);
}

static int run_ui(BOOL start_hidden) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = g_inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = WND_CLASS;
    wc.hIcon         = load_embedded_icon(GetSystemMetrics(SM_CXICON));
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassA(&wc);

    g_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         VARIABLE_PITCH, "Segoe UI");
    g_titleFont = CreateFontA(-13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              VARIABLE_PITCH, "Segoe UI");

    /* Compact window. We want a 380x230 *client area*; let
     * AdjustWindowRect add the title bar + borders on top, otherwise
     * the bottom controls end up clipped by the frame. */
    DWORD style   = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    DWORD exStyle = WS_EX_DLGMODALFRAME;
    RECT rc = {0, 0, 380, 230};
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;
    g_main = CreateWindowExA(exStyle,
        WND_CLASS, APP_TITLE, style,
        CW_USEDEFAULT, CW_USEDEFAULT, W, H,
        NULL, NULL, g_inst, NULL);

    /* --- Local row --- */
    HWND lblLocal = CreateWindowExA(0, "STATIC", "Local mouse",
        WS_CHILD | WS_VISIBLE, 16, 14, 110, 18, g_main, NULL, g_inst, NULL);
    apply_font(lblLocal, g_titleFont);

    g_localStatus = CreateWindowExA(0, "STATIC", "Direction: ...",
        WS_CHILD | WS_VISIBLE, 16, 34, 200, 16, g_main, NULL, g_inst, NULL);
    apply_font(g_localStatus, g_font);

    g_localBtn = CreateWindowExA(0, "BUTTON", "Toggle",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        280, 26, 80, 26, g_main, (HMENU)(LONG_PTR)ID_LOCAL_BTN, g_inst, NULL);
    apply_font(g_localBtn, g_font);

    HWND descLocal = CreateWindowExA(0, "STATIC",
        "Wired / built-in mouse. Persistent. Asks for admin (UAC).",
        WS_CHILD | WS_VISIBLE, 16, 56, 344, 14, g_main, NULL, g_inst, NULL);
    apply_font(descLocal, g_font);

    /* --- Separator --- */
    HWND sep = CreateWindowExA(0, "STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
        12, 80, 356, 2, g_main, NULL, g_inst, NULL);
    (void)sep;

    /* --- Remote row --- */
    HWND lblRemote = CreateWindowExA(0, "STATIC", "Remote mouse",
        WS_CHILD | WS_VISIBLE, 16, 92, 110, 18, g_main, NULL, g_inst, NULL);
    apply_font(lblRemote, g_titleFont);

    g_remoteStatus = CreateWindowExA(0, "STATIC", "Status: OFF",
        WS_CHILD | WS_VISIBLE, 16, 112, 200, 16, g_main, NULL, g_inst, NULL);
    apply_font(g_remoteStatus, g_font);

    g_remoteBtn = CreateWindowExA(0, "BUTTON", "Start",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        280, 104, 80, 26, g_main, (HMENU)(LONG_PTR)ID_REMOTE_BTN, g_inst, NULL);
    apply_font(g_remoteBtn, g_font);

    HWND descRemote = CreateWindowExA(0, "STATIC",
        "KVM / remote / virtual input. Session only. No admin.",
        WS_CHILD | WS_VISIBLE, 16, 134, 344, 14, g_main, NULL, g_inst, NULL);
    apply_font(descRemote, g_font);

    /* --- Footer --- */
    HWND footer = CreateWindowExA(0, "STATIC",
        "Closing the window keeps the app in the tray.",
        WS_CHILD | WS_VISIBLE, 16, 162, 344, 14, g_main, NULL, g_inst, NULL);
    apply_font(footer, g_font);

    /* --- Bottom buttons (placed against the bottom of the 230px client) --- */
    HWND btnAbout = CreateWindowExA(0, "BUTTON", "About",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        16, 188, 90, 30, g_main, (HMENU)(LONG_PTR)ID_ABOUT_BTN, g_inst, NULL);
    apply_font(btnAbout, g_font);

    HWND btnHide = CreateWindowExA(0, "BUTTON", "Hide to Tray",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
        260, 188, 104, 30, g_main, (HMENU)(LONG_PTR)ID_HIDE_BTN, g_inst, NULL);
    apply_font(btnHide, g_font);

    tray_add();

    refresh_status();
    if (!start_hidden) ShowWindow(g_main, SW_SHOW);
    UpdateWindow(g_main);
    /* No SetTimer here. Polling SetupAPI+Registry every second on the
     * UI thread is the main source of the "I can feel the hook" lag
     * because hook callbacks queue behind the timer work. Status now
     * refreshes only when the user actually clicks a button. */

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(g_main, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    if (g_font)      DeleteObject(g_font);
    if (g_titleFont) DeleteObject(g_titleFont);
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE p, LPSTR cmd, int show) {
    (void)p; (void)show;
    g_inst = hInst;
    BOOL wantLocal = (cmd && strstr(cmd, "/local-toggle") != NULL);
    BOOL wantHook  = (cmd && strstr(cmd, "/hook") != NULL);
    BOOL wantStop  = (cmd && strstr(cmd, "/stop") != NULL);
    BOOL wantTray  = (cmd && strstr(cmd, "/tray") != NULL);
    /* /smart-source: opt-in classifier that limits Remote-mode inversion
     * to wheel events flagged LLMHF_INJECTED (KVM/RDP/Parsec/etc). Local
     * HID mice pass through untouched. Off by default — preserves the
     * existing v0.9 Remote-mode behaviour. */
    g_smartSource  = (cmd && strstr(cmd, "/smart-source") != NULL);
    if (wantStop) {
        HWND existing = FindWindowA(WND_CLASS, NULL);
        if (existing) PostMessageA(existing, WM_COMMAND, IDM_EXIT, 0);
        return 0;
    }
    if (wantLocal) {
        if (!is_elevated()) return (int)spawn_self_elevated("/local-toggle", TRUE);
        return do_local_toggle();
    }
    if (wantHook) return run_ui(TRUE);
    return run_ui(wantTray ? TRUE : FALSE);
}
