#include <windows.h>

#include <stdlib.h>
#include <string.h>

// todo: separate header for all common c stuff
#include <stdint.h>
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int8_t    i8;
typedef int16_t   i16;
typedef int32_t   i32;
typedef int64_t   i64;
typedef float     f32;
typedef double    f64;
typedef intptr_t  isize;
typedef uintptr_t usize;

typedef void (*keyboard_cb)(void *ud, uint32_t key, uint32_t state);
typedef void (*mouse_cb)(void *ud, int32_t x, int32_t y, uint32_t b);
typedef void (*resize_cb)(void *ud, u32 w, u32 h);

#ifndef GET_X_LPARAM
#  define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#  define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif
#ifndef MAKEPOINTS
#  define MAKEPOINTS(l) (*((POINTS*)&(l)))
#endif

struct ctx {
    HWND hwnd;
    HDC  hdc;

    /* single CPU buffer (ARGB8888, top-down) */
    u32 *pixels;
    int  buf_w, buf_h, stride; /* stride in bytes */

    int  win_w, win_h;
    int  configured;           /* always 1 after create */
    int  alive;                /* 1 until WM_DESTROY */

    double last_x, last_y;
    int  vsync_ready;          /* set to 1 after each commit */

    keyboard_cb keyboard_cb;
    mouse_cb    mouse_cb;
    resize_cb   resize_window_cb;
    void       *callback_userdata;

    /* BITMAPINFO with BI_BITFIELDS masks for ARGB8888 */
    struct {
        BITMAPINFOHEADER hdr;
        DWORD masks[3]; /* R,G,B */
    } bmi;
};

static void alloc_buffer(struct ctx *c, int w, int h) {
    if (c->pixels) { free(c->pixels); c->pixels = NULL; }
    c->buf_w = w; c->buf_h = h; c->stride = w * 4;
    c->pixels = (u32*)malloc((size_t)c->stride * (size_t)h);
    memset(c->pixels, 0, (size_t)c->stride * (size_t)h);

    memset(&c->bmi, 0, sizeof c->bmi);
    c->bmi.hdr.biSize        = sizeof(BITMAPINFOHEADER);
    c->bmi.hdr.biWidth       = w;
    c->bmi.hdr.biHeight      = -h;                /* top-down */
    c->bmi.hdr.biPlanes      = 1;
    c->bmi.hdr.biBitCount    = 32;
    c->bmi.hdr.biCompression = BI_BITFIELDS;      /* explicit channel masks */
    c->bmi.masks[0] = 0x00FF0000;                 /* R */
    c->bmi.masks[1] = 0x0000FF00;                 /* G */
    c->bmi.masks[2] = 0x000000FF;                 /* B */
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    struct ctx *c = (struct ctx*)GetWindowLongPtr(h, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: return 0;

    case WM_SIZE: {
        if (!c) break;
        int w = LOWORD(lParam), hgt = HIWORD(lParam);
        if (w < 1 || hgt < 1) break;
        int resized = (w != c->win_w) || (hgt != c->win_h);
        if (!resized) break;
        c->win_w = w; c->win_h = hgt;
        alloc_buffer(c, w, hgt);
        if (c->resize_window_cb) c->resize_window_cb(c->callback_userdata, (u32)w, (u32)hgt);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!c) break;
        c->last_x = (double)GET_X_LPARAM(lParam);
        c->last_y = (double)GET_Y_LPARAM(lParam);
        return 0;
    }

    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
        if (c && c->mouse_cb) {
            POINTS p = MAKEPOINTS(lParam);
            c->last_x = p.x; c->last_y = p.y;
            c->mouse_cb(c->callback_userdata, (int)c->last_x, (int)c->last_y, 1);
        }
        return 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        if (c && c->mouse_cb) {
            POINTS p = MAKEPOINTS(lParam);
            c->last_x = p.x; c->last_y = p.y;
            c->mouse_cb(c->callback_userdata, (int)c->last_x, (int)c->last_y, 0);
        }
        return 0;
    }

    case WM_KEYDOWN: {
        if (c && c->keyboard_cb) c->keyboard_cb(c->callback_userdata, (u32)wParam, 1);
        return 0;
    }
    case WM_KEYUP: {
        if (c && c->keyboard_cb) c->keyboard_cb(c->callback_userdata, (u32)wParam, 0);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); /* we draw in commit() */
        return 0;
    }

    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY:
        if (c) c->alive = 0;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, wParam, lParam);
}

/* ===== public API: match your Wayland backend ===== */

u32 *get_buffer(struct ctx *c) { return c->pixels; }

void commit(struct ctx *c) {
    if (!c || !c->alive || !c->pixels) return;
    c->vsync_ready = 0;
    /* immediate blit; no WM_PAINT; single buffer; no vsync wait */
    StretchDIBits(c->hdc,
                  0, 0, c->win_w, c->win_h,
                  0, 0, c->buf_w, c->buf_h,
                  c->pixels, (BITMAPINFO*)&c->bmi,
                  DIB_RGB_COLORS, SRCCOPY);
    GdiFlush();         /* ensure it hits the compositor ASAP */
    c->vsync_ready = 1; /* consumer can gate on this like Wayland's frame_done */
}

/* blocks until at least one message is handled; returns 0 after WM_QUIT */
int poll_events(struct ctx *c) {
    MSG msg;
    int got = GetMessage(&msg, NULL, 0, 0);  /* blocks */
    if (got <= 0) return 0;
    TranslateMessage(&msg);
    DispatchMessage(&msg);
    return 1;
}

/* non-blocking pump; returns 0 after WM_QUIT */
int window_poll(struct ctx *c) {
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return c && c->alive;
}

static void fullscreen(HWND hwnd) {
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    SetWindowLongPtr(hwnd, GWL_STYLE, WS_POPUP);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

/* same signature as your Wayland create_window() */
struct ctx *create_window(const char *title, keyboard_cb kcb, mouse_cb mcb, resize_cb rcb, void *ud) {
    HINSTANCE inst = GetModuleHandle(NULL);
    static ATOM cls = 0;
    if (!cls) {
        WNDCLASS wc;
        memset(&wc, 0, sizeof wc);
        wc.style         = CS_OWNDC;
        wc.lpfnWndProc   = wndproc;
        wc.hInstance     = inst;
        wc.lpszClassName = "ultrafast_win32";
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        cls = RegisterClass(&wc);
        if (!cls) return NULL;
    }

    struct ctx *c = (struct ctx*)calloc(1, sizeof *c);
    if (!c) return NULL;

    c->keyboard_cb = kcb;
    c->mouse_cb = mcb;
    c->resize_window_cb = rcb;
    c->callback_userdata = ud;
    c->alive = 1;
    c->vsync_ready = 1;

    c->hwnd = CreateWindowEx(WS_EX_APPWINDOW, "ultrafast_win32", title ? title : "",
                             WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
                             NULL, NULL, inst, NULL);
    if (!c->hwnd) { free(c); return NULL; }

    SetWindowLongPtr(c->hwnd, GWLP_USERDATA, (LONG_PTR)c);
    ShowWindow(c->hwnd, SW_SHOW);
    fullscreen(c->hwnd); /* mirror the Wayland fullscreen */

    c->hdc = GetDC(c->hwnd);

    RECT r; GetClientRect(c->hwnd, &r);
    c->win_w = (int)(r.right - r.left);
    c->win_h = (int)(r.bottom - r.top);
    alloc_buffer(c, c->win_w, c->win_h);

    c->configured = 1; /* immediately ready on Windows */

    /* first present to seed the surface (Wayland did a first commit) */
    commit(c);
    return c;
}
