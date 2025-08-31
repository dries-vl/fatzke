// cl /O2 win32.c /DUNICODE /D_UNICODE /I"%VULKAN_SDK%\Include" /link /LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib user32.lib gdi32.lib
// or: x86_64-w64-mingw32-gcc win32.c -O2 -lvulkan-1 -luser32 -lgdi32 -o tri2.exe
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

typedef uint32_t u32; typedef int32_t i32; typedef unsigned long long u64;

/* timing: QPC */
static LARGE_INTEGER g_freq; static inline u64 now_ns(void){ LARGE_INTEGER t; QueryPerformanceCounter(&t); return (u64)((t.QuadPart*1000000000ull)/g_freq.QuadPart); }
static u64 T0;
#define TINIT() do{ QueryPerformanceFrequency(&g_freq); T0=now_ns(); }while(0)
#define TSTAMP(msg) \
do { \
    u64 _t = now_ns(); \
    char buf[256]; \
    snprintf(buf, sizeof(buf), "[+%7.3f ms] %s\n", ((_t - T0) / 1e6), (msg)); \
    OutputDebugStringA(buf); \
} while (0)

/* Pull in platform-agnostic Vulkan implementation (creates Win32 surface via helper) */
#include "vulkan.inc"

/* Vulkan entry points (from vulkan.c) */
void vk_init_instance(void); void vk_choose_phys_and_queue(void); void vk_make_device(void);
void vk_graph_initial_build(u32 w,u32 h); void vk_recreate_all(u32 w,u32 h); int vk_present_frame(void); void vk_shutdown_all(void);
void vk_create_surface_win32(HINSTANCE hinst, HWND hwnd);

/* window state */
static i32 win_w=640, win_h=480; static bool running=true, need_swapchain=false; static HINSTANCE ghInst; static HWND gHwnd;

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam){
    switch(msg){
        case WM_SIZE:{
            win_w = (i32)LOWORD(lParam); win_h = (i32)HIWORD(lParam);
            if(win_w>0 && win_h>0) need_swapchain = true;
        } break;
        case WM_CLOSE: running=false; DestroyWindow(hWnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: break;
    }
    return DefWindowProcW(hWnd,msg,wParam,lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR lpCmdLine, int nShowCmd){
    (void)hPrev; (void)lpCmdLine; (void)nShowCmd;
    TINIT(); ghInst=hInst;

    WNDCLASSW wc={0}; wc.lpfnWndProc=WndProc; wc.hInstance=hInst; wc.lpszClassName=L"tri2_cls"; wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    if(!RegisterClassW(&wc)){ MessageBoxW(NULL,L"RegisterClassW failed",L"tri2",MB_OK|MB_ICONERROR); return 1; }

    DWORD style=WS_OVERLAPPEDWINDOW; RECT r={0,0,win_w,win_h}; AdjustWindowRect(&r,style,FALSE);
    gHwnd=CreateWindowExW(0,wc.lpszClassName,L"tri2",style,CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,NULL,NULL,hInst,NULL);
    if(!gHwnd){ MessageBoxW(NULL,L"CreateWindowExW failed",L"tri2",MB_OK|MB_ICONERROR); return 1; }
    ShowWindow(gHwnd,SW_SHOW); UpdateWindow(gHwnd); TSTAMP("Create+ShowWindow");

    /* init vulkan */
    vk_init_instance(); vk_create_surface_win32(hInst,gHwnd);

    /* get initial client size */
    RECT cr; GetClientRect(gHwnd,&cr); win_w=(i32)(cr.right-cr.left); win_h=(i32)(cr.bottom-cr.top); need_swapchain=true;

    /* wait until WM_SIZE processed once (already true), then build graph */
    vk_choose_phys_and_queue(); vk_make_device(); vk_graph_initial_build((u32)win_w,(u32)win_h);
    int pr=vk_present_frame(); TSTAMP("first present"); if(pr) vk_recreate_all((u32)win_w,(u32)win_h);

    MSG msg;
    while(running){
        while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)){ if(msg.message==WM_QUIT) running=false; TranslateMessage(&msg); DispatchMessageW(&msg); }
        if(!running) break;
        if(need_swapchain){ need_swapchain=false; vk_recreate_all((u32)win_w,(u32)win_h); }
        if(vk_present_frame()) vk_recreate_all((u32)win_w,(u32)win_h);
        Sleep(0);
    }

    vk_shutdown_all(); return 0;
}
