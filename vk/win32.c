#ifdef _WIN32
#include "header.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

/* --- high-resolution clock --- */
static LARGE_INTEGER qpf = {0};
u64 pf_ticks_to_ns(u64 qpc){
    if (!qpf.QuadPart){ QueryPerformanceFrequency(&qpf); }
    return (u64)(qpc * 1000000000ULL / (u64)qpf.QuadPart);
}
u64 pf_ns_now(void){
    LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
    return pf_ticks_to_ns(qpc.QuadPart);
}
static u64 T0_ns = 0;
u64 pf_ns_start(void){ return T0_ns; }
void pf_time_reset(void){ T0_ns = pf_ns_now(); }
void pf_timestamp(char* msg){
    u64 t = pf_ns_now();
    printf("[+%7.3f ms] %s\n", (double)(i64)(t - T0_ns)/1e6, msg ? msg : "");
}

/* --- window state --- */
struct win32_window {
    HWND     hwnd;
    HINSTANCE hinst;
    int      w, h;
    int      mouse_x, mouse_y;
    int      visible;

    /* callbacks */
    KEYBOARD_CB on_key;
    MOUSE_CB    on_mouse;
    void*       cb_ud;

    /* timing (rough; default 60 Hz) */
    u64 refresh_ns;
} window;

/* forward decl for wndproc */
static LRESULT CALLBACK wndproc(HWND, UINT, WPARAM, LPARAM);

/* --- contract helpers --- */
int   pf_window_width (WINDOW p){ struct win32_window* w=(struct win32_window*)p; return w?w->w:0; }
int   pf_window_height(WINDOW p){ struct win32_window* w=(struct win32_window*)p; return w?w->h:0; }
void* pf_surface_or_hwnd(WINDOW p){ struct win32_window* w=(struct win32_window*)p; return w?(void*)w->hwnd:NULL; }
void* pf_display_or_instance(WINDOW p){ struct win32_window* w=(struct win32_window*)p; return w?(void*)w->hinst:(void*)GetModuleHandleW(NULL); }
int   pf_window_visible(WINDOW p){ struct win32_window* w=(struct win32_window*)p; return w?w->visible:0; }

/* --- event pump (non-blocking) --- */
int pf_poll_events(WINDOW p){
    struct win32_window* w = (struct win32_window*)p; if (!w) return 0;
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)){
        if (msg.message == WM_QUIT) { w->visible = 0; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 1;
}

/* --- present feedback stub (no-op on pure Win32; keep parity) --- */
void pf_request_present_feedback(WINDOW p, u64 frame_id){ (void)p; (void)frame_id; }

/* --- create fullscreen borderless window --- */
WINDOW pf_create_window(void* ud, KEYBOARD_CB key_cb, MOUSE_CB mouse_cb){
    HINSTANCE hi = GetModuleHandleW(NULL);

    /* estimate refresh (best-effort, default 60 Hz) */
    u64 refresh_ns = 16666667ull;
    DEVMODEW dm = {0}; dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 30){
        double hz = (double)dm.dmDisplayFrequency;
        refresh_ns = (u64)(1e9 / (hz > 1.0 ? hz : 60.0));
    }

    /* register class (idempotent ok) */
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.lpszClassName = L"pf_win32_cls";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);

    /* primary monitor size */
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    DWORD style  = WS_POPUP;
    DWORD exstyle= WS_EX_APPWINDOW;

    HWND hwnd = CreateWindowExW(
        exstyle, wc.lpszClassName, L"" APP_NAME, style,
        0, 0, sw, sh, NULL, NULL, hi, NULL);

    if (!hwnd){ printf("CreateWindowEx failed\n"); ExitProcess(1); }

    window.hwnd = hwnd; window.hinst = hi; window.w = sw; window.h = sh; window.visible = 0;
    window.on_key = key_cb; window.on_mouse = mouse_cb; window.cb_ud = ud;
    window.refresh_ns = refresh_ns;

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&window);

    /* make it topmost fullscreen */
    SetWindowPos(hwnd, HWND_TOP, 0, 0, sw, sh, SWP_SHOWWINDOW);
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    SetForegroundWindow(hwnd);

    /* hide decorations already absent with WS_POPUP; ensure covers taskbar */
    MONITORINFO mi = { .cbSize = sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    SetWindowPos(hwnd, HWND_TOPMOST,
                 mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    pf_time_reset();
    pf_timestamp("win32 window ready");
    pf_timestamp("set up win32");
    return &window;
}

/* --- input helpers --- */
static enum KEYBOARD_BUTTON vk_to_button(WPARAM vk){
    if (vk == VK_ESCAPE) return KEYBOARD_ESCAPE;
    return KEYBOARD_BUTTON_UNKNOWN;
}
static void emit_key(struct win32_window* w, WPARAM vk, int pressed){
    if (!w || !w->on_key) return;
    w->on_key(w->cb_ud, vk_to_button(vk), pressed ? PRESSED : RELEASED);
}
static void emit_mouse_button(struct win32_window* w, enum MOUSE_BUTTON b, int pressed){
    if (!w || !w->on_mouse) return;
    w->on_mouse(w->cb_ud, w->mouse_x, w->mouse_y, b, pressed ? PRESSED : RELEASED);
}
static void emit_mouse_move(struct win32_window* w, int x, int y){
    if (!w || !w->on_mouse) return;
    w->on_mouse(w->cb_ud, x, y, MOUSE_MOVED, RELEASED);
}

/* --- WndProc --- */
static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam){
    struct win32_window* w = (struct win32_window*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg){
        case WM_SHOWWINDOW: if (w) w->visible = (int)wParam; break;
        case WM_SIZE:
            if (w){ w->w = LOWORD(lParam); w->h = HIWORD(lParam); } break;
        case WM_MOUSEMOVE:
            if (w){
                w->mouse_x = GET_X_LPARAM(lParam);
                w->mouse_y = GET_Y_LPARAM(lParam);
                emit_mouse_move(w, w->mouse_x, w->mouse_y);
            } break;
        case WM_LBUTTONDOWN: if (w) emit_mouse_button(w, MOUSE_LEFT, 1);  break;
        case WM_LBUTTONUP:   if (w) emit_mouse_button(w, MOUSE_LEFT, 0);  break;
        case WM_RBUTTONDOWN: if (w) emit_mouse_button(w, MOUSE_RIGHT, 1); break;
        case WM_RBUTTONUP:   if (w) emit_mouse_button(w, MOUSE_RIGHT, 0); break;
        case WM_MBUTTONDOWN: if (w) emit_mouse_button(w, MOUSE_MIDDLE, 1);break;
        case WM_MBUTTONUP:   if (w) emit_mouse_button(w, MOUSE_MIDDLE, 0);break;
        case WM_KEYDOWN:     if (w) emit_key(w, wParam, 1);               break;
        case WM_KEYUP:       if (w) emit_key(w, wParam, 0);               break;
        case WM_CLOSE:       DestroyWindow(hwnd);                          return 0;
        case WM_DESTROY:     PostQuitMessage(0);                           return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
#endif
