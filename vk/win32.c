#ifdef _WIN32
#include "header.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif

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
static enum BUTTON vk_to_button(WPARAM vk){
    switch (vk){
        case VK_ESCAPE:       return KEYBOARD_ESCAPE;

        /* main editing / whitespace */
        case VK_RETURN:       return KEYBOARD_ENTER;      /* numpad Enter is also VK_RETURN with extended flag */
        case VK_BACK:         return KEYBOARD_BACKSPACE;
        case VK_TAB:          return KEYBOARD_TAB;
        case VK_SPACE:        return KEYBOARD_SPACE;

        /* arrows */
        case VK_LEFT:         return KEYBOARD_LEFT;
        case VK_RIGHT:        return KEYBOARD_RIGHT;
        case VK_UP:           return KEYBOARD_UP;
        case VK_DOWN:         return KEYBOARD_DOWN;

        /* navigation / editing */
        case VK_HOME:         return KEYBOARD_HOME;
        case VK_END:          return KEYBOARD_END;
        case VK_PRIOR:        return KEYBOARD_PAGEUP;     /* Page Up */
        case VK_NEXT:         return KEYBOARD_PAGEDOWN;   /* Page Down */
        case VK_INSERT:       return KEYBOARD_INSERT;
        case VK_DELETE:       return KEYBOARD_DELETE;

        /* function row */
        case VK_F1:  return KEYBOARD_F1;
        case VK_F2:  return KEYBOARD_F2;
        case VK_F3:  return KEYBOARD_F3;
        case VK_F4:  return KEYBOARD_F4;
        case VK_F5:  return KEYBOARD_F5;
        case VK_F6:  return KEYBOARD_F6;
        case VK_F7:  return KEYBOARD_F7;
        case VK_F8:  return KEYBOARD_F8;
        case VK_F9:  return KEYBOARD_F9;
        case VK_F10: return KEYBOARD_F10;
        case VK_F11: return KEYBOARD_F11;
        case VK_F12: return KEYBOARD_F12;

        /* modifiers (map both sides to the same logical buttons, like Wayland path) */
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:       return KEYBOARD_SHIFT;
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:     return KEYBOARD_CTRL;
        case VK_MENU:         /* Alt */
        case VK_LMENU:
        case VK_RMENU:        return KEYBOARD_ALT;
        case VK_LWIN:
        case VK_RWIN:         return KEYBOARD_SUPER;

        /* digits (top row) */
        case '0': return KEYBOARD_0;
        case '1': return KEYBOARD_1;
        case '2': return KEYBOARD_2;
        case '3': return KEYBOARD_3;
        case '4': return KEYBOARD_4;
        case '5': return KEYBOARD_5;
        case '6': return KEYBOARD_6;
        case '7': return KEYBOARD_7;
        case '8': return KEYBOARD_8;
        case '9': return KEYBOARD_9;

        /* letters */
        case 'A': return KEYBOARD_A;
        case 'B': return KEYBOARD_B;
        case 'C': return KEYBOARD_C;
        case 'D': return KEYBOARD_D;
        case 'E': return KEYBOARD_E;
        case 'F': return KEYBOARD_F;
        case 'G': return KEYBOARD_G;
        case 'H': return KEYBOARD_H;
        case 'I': return KEYBOARD_I;
        case 'J': return KEYBOARD_J;
        case 'K': return KEYBOARD_K;
        case 'L': return KEYBOARD_L;
        case 'M': return KEYBOARD_M;
        case 'N': return KEYBOARD_N;
        case 'O': return KEYBOARD_O;
        case 'P': return KEYBOARD_P;
        case 'Q': return KEYBOARD_Q;
        case 'R': return KEYBOARD_R;
        case 'S': return KEYBOARD_S;
        case 'T': return KEYBOARD_T;
        case 'U': return KEYBOARD_U;
        case 'V': return KEYBOARD_V;
        case 'W': return KEYBOARD_W;
        case 'X': return KEYBOARD_X;
        case 'Y': return KEYBOARD_Y;
        case 'Z': return KEYBOARD_Z;

        default:              return BUTTON_UNKNOWN;
    }
}

static void emit_key(struct win32_window* w, WPARAM vk, int pressed){
    if (!w || !w->on_key) return;
    w->on_key(w->cb_ud, vk_to_button(vk), pressed ? PRESSED : RELEASED);
}
static void emit_mouse_button(struct win32_window* w, enum BUTTON b, int pressed){
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

        /* mouse */
            /* scrolling (vertical + horizontal) */
        case WM_MOUSEWHEEL:
            if (w){
                // Position in WM_MOUSEWHEEL is in screen coords; convert to client
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hwnd, &pt);
                w->mouse_x = pt.x; w->mouse_y = pt.y;

                // Convert WHEEL_DELTA units to step count, with remainder for smooth wheels
                static int v_remainder = 0;
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);  // multiples of 120
                v_remainder += delta;
                int steps = v_remainder / WHEEL_DELTA;       // signed
                v_remainder -= steps * WHEEL_DELTA;

                if (steps != 0 && w->on_mouse){
                    // NOTE: for scroll, we pass 'steps' in the 'pressed' parameter
                    w->on_mouse(w->cb_ud, w->mouse_x, w->mouse_y, MOUSE_SCROLL, -steps * 30);
                }
                return 0; // prevent default scrolling (like scrolling inactive window behind)
            }
            break;

        case WM_MOUSEHWHEEL:
            if (w){
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hwnd, &pt);
                w->mouse_x = pt.x; w->mouse_y = pt.y;

                static int h_remainder = 0;
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                h_remainder += delta;
                int steps = h_remainder / WHEEL_DELTA;       // signed
                h_remainder -= steps * WHEEL_DELTA;

                if (steps != 0 && w->on_mouse){
                    w->on_mouse(w->cb_ud, w->mouse_x, w->mouse_y, MOUSE_SCROLL_SIDE, -steps * 30);
                }
                return 0;
            }
            break;

        case WM_MOUSEMOVE:
            if (w){
                w->mouse_x = GET_X_LPARAM(lParam);
                w->mouse_y = GET_Y_LPARAM(lParam);
                emit_mouse_move(w, w->mouse_x, w->mouse_y);
            } break;
        case WM_LBUTTONDOWN: if (w) emit_mouse_button(w, MOUSE_LEFT,   1); break;
        case WM_LBUTTONUP:   if (w) emit_mouse_button(w, MOUSE_LEFT,   0); break;
        case WM_RBUTTONDOWN: if (w) emit_mouse_button(w, MOUSE_RIGHT,  1); break;
        case WM_RBUTTONUP:   if (w) emit_mouse_button(w, MOUSE_RIGHT,  0); break;
        case WM_MBUTTONDOWN: if (w) emit_mouse_button(w, MOUSE_MIDDLE, 1); break;
        case WM_MBUTTONUP:   if (w) emit_mouse_button(w, MOUSE_MIDDLE, 0); break;

        /* keyboard – include SYSKEY to catch Alt/F10 and avoid system menu */
        case WM_KEYDOWN:      if (w) { emit_key(w, wParam, 1); return 0; } break;
        case WM_KEYUP:        if (w) { emit_key(w, wParam, 0); return 0; } break;
        case WM_SYSKEYDOWN:   if (w) { emit_key(w, wParam, 1); return 0; } break;
        case WM_SYSKEYUP:     if (w) { emit_key(w, wParam, 0); return 0; } break;

        case WM_CLOSE:        DestroyWindow(hwnd);                          return 0;
        case WM_DESTROY:      PostQuitMessage(0);                           return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
#endif
