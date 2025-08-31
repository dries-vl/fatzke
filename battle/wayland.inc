// cc wayland.c -lwayland-client -lvulkan -ldl -lpthread -O2 -o tri2 && ./tri2
#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// todo: header inc or something
typedef uint32_t u32; typedef int32_t i32; typedef unsigned long long u64;
static inline u64 now_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (u64)ts.tv_sec*1000000000ull+ts.tv_nsec; }
static u64 T0;
#define TINIT() do{T0=now_ns();}while(0)
#define TSTAMP(msg) do{u64 _t=now_ns(); fprintf(stderr,"[+%7.3f ms] %s\n",(_t-T0)/1e6,(msg));}while(0)

/* Pull in the platform-agnostic Vulkan implementation */
#include "vulkan.inc"

#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
#include <xdg-shell-client-protocol.c>

/* Vulkan entry points (from vulkan.c) */
struct wl_display; struct wl_surface;
void vk_init_instance(void); void vk_create_surface_wayland(struct wl_display*, struct wl_surface*);
void vk_choose_phys_and_queue(void); void vk_make_device(void);
void vk_graph_initial_build(u32 w,u32 h); void vk_recreate_all(u32 w,u32 h);
int vk_present_frame(void); void vk_shutdown_all(void);

/* Wayland state */
static struct wl_display* dpy; static struct wl_compositor* comp; static struct xdg_wm_base* xdg;
static struct wl_surface* surf; static struct xdg_surface* xsurf; static struct xdg_toplevel* xtop;
static i32 win_w=640, win_h=480; static bool running=true, need_swapchain=false;

static void xdg_ping(void* d, struct xdg_wm_base* b, u32 s){ (void)d; xdg_wm_base_pong(b,s); }
static const struct xdg_wm_base_listener xdg_wm = {.ping=xdg_ping};
static void top_config(void* d, struct xdg_toplevel* t, i32 w,i32 h, struct wl_array* st){ (void)d;(void)t;(void)st; if(w>0&&h>0){win_w=w;win_h=h;} }
static void top_close(void* d, struct xdg_toplevel* t){ (void)d;(void)t; running=false; }
static void top_bounds(void* d, struct xdg_toplevel* t, i32 w,i32 h){ (void)d;(void)t;(void)w;(void)h; }
static void top_caps(void* d, struct xdg_toplevel* t, struct wl_array* c){ (void)d;(void)t;(void)c; }
static const struct xdg_toplevel_listener top_l={.configure=top_config,.close=top_close,.configure_bounds=top_bounds,.wm_capabilities=top_caps};
static void xsurf_conf(void* d, struct xdg_surface* s, u32 serial){ (void)d; xdg_surface_ack_configure(s,serial); need_swapchain=true; TSTAMP("xdg_surface configure"); }
static const struct xdg_surface_listener xsurf_l={.configure=xsurf_conf};
static void reg_add(void* d, struct wl_registry* r, uint32_t name, const char* iface, uint32_t ver){
    (void)d;
    if(!strcmp(iface,wl_compositor_interface.name)){ u32 v=ver<4?ver:4; comp=wl_registry_bind(r,name,&wl_compositor_interface,v); }
    else if(!strcmp(iface,xdg_wm_base_interface.name)){ u32 v=ver<6?ver:6; xdg=wl_registry_bind(r,name,&xdg_wm_base_interface,v); xdg_wm_base_add_listener(xdg,&xdg_wm,NULL); }
}
static void reg_rem(void* d, struct wl_registry* r, u32 name){ (void)d;(void)r;(void)name; }
static const struct wl_registry_listener reg_l={.global=reg_add,.global_remove=reg_rem};

int main(void){
    TINIT();
    dpy = wl_display_connect(NULL); if(!dpy){ fprintf(stderr,"wl connect failed\n"); return 1; }
    TSTAMP("wl_display_connect");
    struct wl_registry* reg = wl_display_get_registry(dpy); wl_registry_add_listener(reg,&reg_l,NULL); wl_display_roundtrip(dpy);
    TSTAMP("wl globals ready"); if(!comp||!xdg){ fprintf(stderr,"missing compositor/xdg\n"); return 1; }

    surf=wl_compositor_create_surface(comp); xsurf=xdg_wm_base_get_xdg_surface(xdg,surf);
    xdg_surface_add_listener(xsurf,&xsurf_l,NULL); xtop=xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(xtop,&top_l,NULL); xdg_toplevel_set_title(xtop,"tri2"); xdg_toplevel_set_app_id(xtop,"tri2");
    xdg_toplevel_set_fullscreen(xtop,NULL); wl_surface_commit(surf); TSTAMP("wl first commit");

    vk_init_instance(); vk_create_surface_wayland(dpy,surf);
    while(running && !need_swapchain) wl_display_dispatch(dpy);
    vk_choose_phys_and_queue(); vk_make_device(); vk_graph_initial_build((u32)win_w,(u32)win_h);

    int pr = vk_present_frame(); TSTAMP("first present"); if(pr) vk_recreate_all((u32)win_w,(u32)win_h);
    while(running){
        wl_display_dispatch(dpy);
        if(need_swapchain){ need_swapchain=false; vk_recreate_all((u32)win_w,(u32)win_h); }
        if(vk_present_frame()) vk_recreate_all((u32)win_w,(u32)win_h);
    }
    vk_shutdown_all(); return 0;
}
