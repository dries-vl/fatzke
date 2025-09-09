#ifdef _WIN32
#include "header.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h> // todo: get rid of calloc

struct win32_window {
    HWND hwnd;
    HINSTANCE hinst;
    int win_w;
    int win_h;
    int running;
    int mouse_x;
    int mouse_y;
    keyboard_cb on_key;
    mouse_cb on_mouse;
    void* ud;
};

static LARGE_INTEGER pf_qpc_freq = {0};
static LARGE_INTEGER pf_t0 = {0};
void pf_time_reset(void){ if(!pf_qpc_freq.QuadPart) QueryPerformanceFrequency(&pf_qpc_freq); QueryPerformanceCounter(&pf_t0); }
void pf_timestamp(char *msg){ LARGE_INTEGER t; QueryPerformanceCounter(&t); double ms = (double)(t.QuadPart - pf_t0.QuadPart) * 1000.0 / (double)pf_qpc_freq.QuadPart; printf("[+%7.3f ms] %s\n", ms, msg?msg:""); }

static LRESULT CALLBACK pf_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    struct win32_window* ww = (struct win32_window*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m)
    {
    case WM_DESTROY:
        {
            if (ww)
            {
                ww->running = 0;
                SetWindowLongPtrW(h, GWLP_USERDATA, 0);
                free(ww);
            }
            PostQuitMessage(0);
            return 0;
        }
    case WM_CLOSE:
        {
            DestroyWindow(h);
            return 0;
        }
    case WM_KEYDOWN:
        {
            if (!ww || !ww->on_key) return 0;
            if (w == VK_ESCAPE) ww->on_key(ww->ud, KEYBOARD_ESCAPE, PRESSED);
            return 0;
        }
    case WM_KEYUP:
        {
            if (!ww || !ww->on_key) return 0;
            if (w == VK_ESCAPE) ww->on_key(ww->ud, KEYBOARD_ESCAPE, RELEASED);
            return 0;
        }
    case WM_MOUSEMOVE:
        {
            if (!ww || !ww->on_mouse) return 0;
            int x = (short)LOWORD(l);
            int y = (short)HIWORD(l);
            ww->mouse_x = x;
            ww->mouse_y = y;
            ww->on_mouse(ww->ud, x, (unsigned)y, MOUSE_MOVED, RELEASED);
            return 0;
        }
    case WM_LBUTTONDOWN:
        {
            if (ww && ww->on_mouse) ww->on_mouse(ww->ud, ww->mouse_x, (unsigned)ww->mouse_y, MOUSE_LEFT, PRESSED);
            return 0;
        }
    case WM_LBUTTONUP:
        {
            if (ww && ww->on_mouse) ww->on_mouse(ww->ud, ww->mouse_x, (unsigned)ww->mouse_y, MOUSE_LEFT, RELEASED);
            return 0;
        }
    case WM_RBUTTONDOWN:
        {
            if (ww && ww->on_mouse) ww->on_mouse(ww->ud, ww->mouse_x, (unsigned)ww->mouse_y, MOUSE_RIGHT, PRESSED);
            return 0;
        }
    case WM_RBUTTONUP:
        {
            if (ww && ww->on_mouse) ww->on_mouse(ww->ud, ww->mouse_x, (unsigned)ww->mouse_y, MOUSE_RIGHT, RELEASED);
            return 0;
        }
    case WM_MBUTTONDOWN:
        {
            if (ww && ww->on_mouse) ww->on_mouse(ww->ud, ww->mouse_x, (unsigned)ww->mouse_y, MOUSE_MIDDLE, PRESSED);
            return 0;
        }
    case WM_MBUTTONUP:
        {
            if (ww && ww->on_mouse) ww->on_mouse(ww->ud, ww->mouse_x, (unsigned)ww->mouse_y, MOUSE_MIDDLE, RELEASED);
            return 0;
        }
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

WINDOW pf_create_window(void* ud, keyboard_cb key_cb, mouse_cb mouse_cb) {
    pf_time_reset();
    HINSTANCE hinst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = pf_wndproc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"tri2_cls";
    RegisterClassW(&wc);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    struct win32_window* ww = calloc(1,sizeof *ww); if(!ww) exit(1);
    ww->hinst=hinst; ww->win_w=sw; ww->win_h=sh; ww->running=1; ww->ud=ud; ww->on_key=key_cb; ww->on_mouse=mouse_cb;
    pf_timestamp("Register class etc.");
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"tri2_cls", L"tri2", WS_POPUP, 0, 0, sw, sh, NULL, NULL, hinst, NULL);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)ww);
    pf_timestamp("Win32 window created");
    ShowWindow(hwnd, SW_SHOW);
    pf_timestamp("Win32 window shown");
    return ww;
}

int pf_poll_events(WINDOW w) {
    if (!w) return 0;
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 1;
}

int pf_window_width(void *w) {
    struct win32_window* win = w; return win->win_w;
}

int pf_window_height(void *w) {
    struct win32_window* win = w; return win->win_h;
}

void *pf_surface_or_hwnd(void *w) {
    struct win32_window* win = w; return win->hwnd;
}

void *pf_display_or_instance(void *w) {
    struct win32_window* win = w; return win->hinst;
}

int pf_window_visible(void *w) {
    struct win32_window* win = w; return 1;
}
#endif
