#include "header.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// sudo apt install libwayland-dev (for getting the wayland-client header and lib)
#include <wayland-client.h>
// sudo apt install wayland-protocols wayland-scanner (for generating the xdg-shell files)
// sudo wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml /usr/include/xdg-shell-client-protocol.h
// sudo wayland-scanner private-code /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml /usr/include/xdg-shell-client-protocol.c
#include <xdg-shell-client-protocol.h>
#include <xdg-shell-client-protocol.c>

// todo: enum for keys (to sync windows and wayland keyboard presses)
enum MOUSE_BUTTON { MOUSE_MOVED, MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE, MOUSE_BUTTON_UNKNOWN };
enum INPUT_STATE { RELEASED, PRESSED };
typedef void (*keyboard_cb)(void* ud, unsigned key, enum INPUT_STATE state);
typedef void (*mouse_cb)(void* ud, int x, unsigned y, enum MOUSE_BUTTON, enum INPUT_STATE state);

// todo: find a good place to put these
inline u64 now_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (u64)ts.tv_sec*1000000000ull+ts.tv_nsec; }
u64 T0;
#define TINIT() do{T0=now_ns();}while(0)
#define TSTAMP(msg) do{u64 _t=now_ns(); fprintf(stderr,"[+%7.3f ms] %s\n",(_t-T0)/1e6,(msg));}while(0)

struct Window {
    /* Wayland core */
    struct wl_display* dpy;
    struct wl_surface* surf;
    struct wl_compositor* comp;
    struct xdg_wm_base* xdg;
    struct xdg_surface* xsurf;
    struct xdg_toplevel* xtop;

    /* Input */
    struct wl_seat* seat;
    struct wl_keyboard* kbd;
    struct wl_pointer* ptr;

    /* State */
    i32 win_w, win_h;
    bool running;
    bool vsync_ready;
    i32 mouse_x, mouse_y;

    /* App callbacks */
    keyboard_cb on_key;
    mouse_cb on_mouse;
};

/* ---------------- wl_keyboard callbacks ---------------- */
void kb_keymap(void* data, struct wl_keyboard* k, u32 format, i32 fd, u32 size) { (void)data;(void)k;(void)format;(void)fd;(void)size; }
void kb_enter(void* data, struct wl_keyboard* k, u32 serial, struct wl_surface* surface, struct wl_array* keys) { (void)data;(void)k;(void)serial;(void)surface;(void)keys; }
void kb_leave(void* data, struct wl_keyboard* k, u32 serial, struct wl_surface* surface) { (void)data;(void)k;(void)serial;(void)surface; }
void kb_key(void* data, struct wl_keyboard* k, u32 serial, u32 time, u32 key, u32 state){
    (void)k;(void)serial;(void)time;
    struct Window* w = (struct Window*)data;
    if (w && w->on_key) w->on_key(NULL, key, state);
}
void kb_modifiers(void* data, struct wl_keyboard* k, u32 serial, u32 dep, u32 lat, u32 lock, u32 group){ (void)data;(void)k;(void)serial;(void)dep;(void)lat;(void)lock;(void)group; }
void kb_repeat_info(void* data, struct wl_keyboard* k, i32 rate, i32 delay){ (void)data;(void)k;(void)rate;(void)delay; }

const struct wl_keyboard_listener* get_kb_listener(void){
    static const struct wl_keyboard_listener kb_l = {
        .keymap = kb_keymap,
        .enter = kb_enter,
        .leave = kb_leave,
        .key = kb_key,
        .modifiers = kb_modifiers,
        .repeat_info = kb_repeat_info
    };
    return &kb_l;
}

/* ---------------- wl_pointer callbacks ---------------- */
void ptr_enter(void* d, struct wl_pointer* p, u32 serial, struct wl_surface* s, wl_fixed_t sx, wl_fixed_t sy){
    (void)p;(void)serial;(void)s;
    struct Window* w = (struct Window*)d;
    if (!w) return;
    w->mouse_x = wl_fixed_to_int(sx);
    w->mouse_y = wl_fixed_to_int(sy);
    if (w->on_mouse) w->on_mouse(NULL, w->mouse_x, w->mouse_y, MOUSE_MOVED, 0u);
}
void ptr_leave(void* d, struct wl_pointer* p, u32 serial, struct wl_surface* s){ (void)d;(void)p;(void)serial;(void)s; }
void ptr_motion(void* d, struct wl_pointer* p, u32 time, wl_fixed_t sx, wl_fixed_t sy){
    (void)p;(void)time;
    struct Window* w = (struct Window*)d;
    if (!w) return;
    w->mouse_x = wl_fixed_to_int(sx);
    w->mouse_y = wl_fixed_to_int(sy);
    if (w->on_mouse) w->on_mouse(NULL, w->mouse_x, w->mouse_y, MOUSE_MOVED, 0u);
}
void ptr_button(void* d, struct wl_pointer* p, u32 serial, u32 time, u32 button, u32 state){
    (void)p;(void)serial;(void)time;
    struct Window* w = (struct Window*)d;
    if (!w) return;
    u32 mb = MOUSE_BUTTON_UNKNOWN;
    if (button == 272) mb = MOUSE_BUTTON_LEFT;
    if (button == 273) mb = MOUSE_BUTTON_RIGHT;
    if (button == 274) mb = MOUSE_BUTTON_MIDDLE;
    if (w->on_mouse) w->on_mouse(NULL, w->mouse_x, w->mouse_y, mb, state);
}
void ptr_axis(void* d, struct wl_pointer* p, u32 time, u32 axis, wl_fixed_t value){ (void)d;(void)p;(void)time;(void)axis;(void)value; }
void ptr_frame(void* d, struct wl_pointer* p){ (void)d;(void)p; }
void ptr_axis_source(void* d, struct wl_pointer* p, u32 source){ (void)d;(void)p;(void)source; }
void ptr_axis_stop(void* d, struct wl_pointer* p, u32 time, u32 axis){ (void)d;(void)p;(void)time;(void)axis; }
void ptr_axis_discrete(void* d, struct wl_pointer* p, u32 axis, i32 discrete){ (void)d;(void)p;(void)axis;(void)discrete; }
void ptr_axis_value120(void* d, struct wl_pointer* p, u32 axis, i32 value120){ (void)d;(void)p;(void)axis;(void)value120; }
void ptr_axis_relative_direction(void* d, struct wl_pointer* p, u32 axis, u32 direction){ (void)d;(void)p;(void)axis;(void)direction; }

const struct wl_pointer_listener* get_ptr_listener(void){
    static const struct wl_pointer_listener ptr_l = {
        .enter = ptr_enter,
        .leave = ptr_leave,
        .motion = ptr_motion,
        .button = ptr_button,
        .axis = ptr_axis,
        .frame = ptr_frame,
        .axis_source = ptr_axis_source,
        .axis_stop = ptr_axis_stop,
        .axis_discrete = ptr_axis_discrete,
        .axis_value120 = ptr_axis_value120,
        .axis_relative_direction = ptr_axis_relative_direction
    };
    return &ptr_l;
}

/* ---------------- wl_seat callbacks ---------------- */
void seat_capabilities(void* d, struct wl_seat* s, u32 caps){
    struct Window* w = (struct Window*)d;
    if (!w) return;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !w->kbd){
        w->kbd = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(w->kbd, get_kb_listener(), w);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !w->ptr){
        w->ptr = wl_seat_get_pointer(s);
        wl_pointer_add_listener(w->ptr, get_ptr_listener(), w);
    }
}
void seat_name(void* d, struct wl_seat* s, const char* name){ (void)d;(void)s;(void)name; }

const struct wl_seat_listener* get_seat_listener(void){
    static const struct wl_seat_listener seat_l = { .capabilities = seat_capabilities, .name = seat_name };
    return &seat_l;
}

/* ---------------- xdg wm + toplevel ---------------- */
void xdg_ping(void* d, struct xdg_wm_base* b, u32 s){ (void)d; xdg_wm_base_pong(b, s); }
const struct xdg_wm_base_listener* get_xdg_wm_listener(void){
    static const struct xdg_wm_base_listener l = { .ping = xdg_ping };
    return &l;
}
void top_config(void* d, struct xdg_toplevel* t, i32 w, i32 h, struct wl_array* st){
    (void)t;(void)st;
    struct Window* wdw = (struct Window*)d;
    if (wdw && w > 0 && h > 0){ wdw->win_w = w; wdw->win_h = h; }
}
void top_close(void* d, struct xdg_toplevel* t){ (void)t; struct Window* w = (struct Window*)d; if (w) w->running = false; }
void top_bounds(void* d, struct xdg_toplevel* t, i32 w, i32 h){ (void)d;(void)t;(void)w;(void)h; }
void top_caps(void* d, struct xdg_toplevel* t, struct wl_array* c){ (void)d;(void)t;(void)c; }

const struct xdg_toplevel_listener* get_top_listener(void){
    static const struct xdg_toplevel_listener top_l = {
        .configure = top_config, .close = top_close, .configure_bounds = top_bounds, .wm_capabilities = top_caps
    };
    return &top_l;
}

void xsurf_conf(void* d, struct xdg_surface* s, u32 serial){
    struct Window* w = (struct Window*)d;
    xdg_surface_ack_configure(s, serial);
    if (w){ w->vsync_ready = true; TSTAMP("xdg_surface configure"); }
}
const struct xdg_surface_listener* get_xsurf_listener(void){
    static const struct xdg_surface_listener xsurf_l = { .configure = xsurf_conf };
    return &xsurf_l;
}

/* ---------------- registry ---------------- */
void reg_add(void* d, struct wl_registry* r, uint32_t name, const char* iface, uint32_t ver){
    struct Window* w = (struct Window*)d;
    if (!w) return;
    if (!strcmp(iface, wl_compositor_interface.name)){
        u32 v = ver < 4 ? ver : 4; w->comp = wl_registry_bind(r, name, &wl_compositor_interface, v);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)){
        u32 v = ver < 6 ? ver : 6; w->xdg = wl_registry_bind(r, name, &xdg_wm_base_interface, v);
        xdg_wm_base_add_listener(w->xdg, get_xdg_wm_listener(), w);
    } else if (!strcmp(iface, wl_seat_interface.name)){
        u32 v = ver < 5 ? ver : 5; w->seat = wl_registry_bind(r, name, &wl_seat_interface, v);
        wl_seat_add_listener(w->seat, get_seat_listener(), w);
    }
}
void reg_rem(void* d, struct wl_registry* r, u32 name){ (void)d;(void)r;(void)name; }

const struct wl_registry_listener* get_registry_listener(void){
    static const struct wl_registry_listener reg_l = { .global = reg_add, .global_remove = reg_rem };
    return &reg_l;
}

struct Window* pf_create_window(keyboard_cb key_cb, mouse_cb mouse_cb){
    TINIT();
    struct Window* w = (struct Window*)calloc(1, sizeof(*w));
    w->win_w = 0; w->win_h = 0; w->running = true; w->vsync_ready = false;
    w->on_key = key_cb; w->on_mouse = mouse_cb;

    w->dpy = wl_display_connect(NULL);
    if (!w->dpy){ fprintf(stderr, "wl connect failed\n"); exit(1); }
    TSTAMP("wl_display_connect");

    struct wl_registry* reg = wl_display_get_registry(w->dpy);
    wl_registry_add_listener(reg, get_registry_listener(), w);
    wl_display_roundtrip(w->dpy);
    TSTAMP("wl globals ready");
    if (!w->comp || !w->xdg){ fprintf(stderr, "missing compositor/xdg\n"); exit(1); }

    w->surf  = wl_compositor_create_surface(w->comp);
    w->xsurf = xdg_wm_base_get_xdg_surface(w->xdg, w->surf);
    xdg_surface_add_listener(w->xsurf, get_xsurf_listener(), w);
    w->xtop  = xdg_surface_get_toplevel(w->xsurf);
    xdg_toplevel_add_listener(w->xtop, get_top_listener(), w);
    xdg_toplevel_set_title(w->xtop, "tri2");
    xdg_toplevel_set_app_id(w->xtop, "tri2");
    xdg_toplevel_set_fullscreen(w->xtop, NULL);
    wl_surface_commit(w->surf);
    bool *flag = &w->vsync_ready; // making sure here we've actually gone fullscreen before moving on
    while (!*flag) {
        wl_display_flush(w->dpy);
        if (wl_display_dispatch(w->dpy) < 0) exit(1);
    }
    TSTAMP("wl first commit"); // should be done with all this in ~10ms or less
    return w;
}

int pf_poll_events(struct Window* w){
    if (!w || !w->running) return 0;
    wl_display_dispatch(w->dpy);
    return 1;
}
