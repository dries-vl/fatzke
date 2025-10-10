#ifdef __linux__
#include "header.h"

#include <time.h>
// sudo apt install libwayland-dev (for getting the wayland-client header and lib)
#include <wayland-client.h>
// sudo apt install wayland-protocols wayland-scanner (for generating the xdg-shell files)
// sudo wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml /usr/include/xdg-shell-client-protocol.h
// sudo wayland-scanner private-code /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml /usr/include/xdg-shell-client-protocol.c
#include <xdg-shell-client-protocol.h>
#include <xdg-shell-client-protocol.c>
// sudo wayland-scanner client-header /usr/share/wayland-protocols/stable/presentation-time/presentation-time.xml /usr/include/presentation-time-client-protocol.h
// sudo wayland-scanner private-code /usr/share/wayland-protocols/stable/presentation-time/presentation-time.xml /usr/include/presentation-time-client-protocol.c
#include <presentation-time-client-protocol.h>
#include <presentation-time-client-protocol.c>
// sudo wayland-scanner client-header /usr/share/wayland-protocols/stable/presentation-time/presentation-time.xml /usr/include/drm-lease-v1-client-protocol.h
// sudo wayland-scanner private-code /usr/share/wayland-protocols/stable/presentation-time/presentation-time.xml /usr/include/drm-lease-v1-client-protocol.c
// #include <drm-lease-v1-client-protocol.h>
// #include <drm-lease-v1-client-protocol.c>

static clockid_t clockid;
u64 pf_ns_now(void){
    struct timespec ts; clock_gettime(clockid ? clockid : CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}
u64 T0;
u64 pf_ns_start(void){ return T0; }
void pf_time_reset() {T0=pf_ns_now();}
void pf_timestamp(char *msg) {u64 _t=pf_ns_now(); printf("[+%7.3f ms] %s\n",(_t-T0)/1e6,(msg));}

struct wayland_window {
    struct wl_display* display;
    struct wl_surface* surface;
    struct wl_compositor* compositor;
    struct xdg_wm_base* xdg_wm_base;
    struct xdg_surface* xdg_surface;
    struct xdg_toplevel* xdg_toplevel;

    struct wp_presentation* presentation;
    u64 frame_id;
    uint64_t refresh_ns;          // from wp_presentation (fallback 16.666 ms)
    uint64_t last_present_ns;     // compositor-reported present time (scanout)
    uint64_t last_feedback_ns;    // CPU time when feedback callback ran
    uint64_t phase_ns;            // smoothed present phase (optional)
    double   phase_alpha;         // EMA coefficient (0..1)

    // inputs
    struct wl_seat* seat;
    struct wl_keyboard* kbd;
    struct wl_pointer* ptr;

    // keep state
    i32 win_w, win_h;
    i32 mouse_x, mouse_y;
    int frame_done;
    int fullscreen_configured;
    int visible;
    int in_flight_count;

    // callbacks
    KEYBOARD_CB on_key;
    MOUSE_CB on_mouse;
    void *callback_data;
};

void kb_keymap(void* data, struct wl_keyboard* k, u32 format, i32 fd, u32 size) { }
void kb_enter(void* data, struct wl_keyboard* k, u32 serial, struct wl_surface* surface, struct wl_array* keys) { }
void kb_leave(void* data, struct wl_keyboard* k, u32 serial, struct wl_surface* surface) { }
void kb_key(void* data, struct wl_keyboard* k, u32 serial, u32 time, u32 key, u32 state){
    struct wayland_window* w = (struct wayland_window*)data;
    enum KEYBOARD_BUTTON button = KEYBOARD_BUTTON_UNKNOWN;
    if (key == 1) button = KEYBOARD_ESCAPE;
    if (w && w->on_key) w->on_key(w->callback_data, button, state);
}
void kb_modifiers(void* data, struct wl_keyboard* k, u32 serial, u32 dep, u32 lat, u32 lock, u32 group){ }
void kb_repeat_info(void* data, struct wl_keyboard* k, i32 rate, i32 delay){ }

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

void ptr_enter(void* d, struct wl_pointer* p, u32 serial, struct wl_surface* s, wl_fixed_t sx, wl_fixed_t sy){
    struct wayland_window* w = d;
    if (!w) return;
    w->mouse_x = wl_fixed_to_int(sx);
    w->mouse_y = wl_fixed_to_int(sy);
    if (w->on_mouse) w->on_mouse(w->callback_data, w->mouse_x, w->mouse_y, MOUSE_MOVED, 0u);
}
void ptr_leave(void* d, struct wl_pointer* p, u32 serial, struct wl_surface* s){ }
void ptr_motion(void* d, struct wl_pointer* p, u32 time, wl_fixed_t sx, wl_fixed_t sy){
    struct wayland_window* w = d;
    if (!w) return;
    w->mouse_x = wl_fixed_to_int(sx);
    w->mouse_y = wl_fixed_to_int(sy);
    if (w->on_mouse) w->on_mouse(w->callback_data, w->mouse_x, w->mouse_y, MOUSE_MOVED, 0u);
}
void ptr_button(void* d, struct wl_pointer* p, u32 serial, u32 time, u32 button, u32 state){
    struct wayland_window* w = d;
    if (!w) return;
    u32 mb = MOUSE_BUTTON_UNKNOWN;
    if (button == 272) mb = MOUSE_LEFT;
    if (button == 273) mb = MOUSE_RIGHT;
    if (button == 274) mb = MOUSE_MIDDLE;
    if (w->on_mouse) w->on_mouse(w->callback_data, w->mouse_x, w->mouse_y, mb, state);
}
void ptr_axis(void* d, struct wl_pointer* p, u32 time, u32 axis, wl_fixed_t value){ }
void ptr_frame(void* d, struct wl_pointer* p){ }
void ptr_axis_source(void* d, struct wl_pointer* p, u32 source){ }
void ptr_axis_stop(void* d, struct wl_pointer* p, u32 time, u32 axis){ }
void ptr_axis_discrete(void* d, struct wl_pointer* p, u32 axis, i32 discrete){ }
void ptr_axis_value120(void* d, struct wl_pointer* p, u32 axis, i32 value120){ }
void ptr_axis_relative_direction(void* d, struct wl_pointer* p, u32 axis, u32 direction){ }

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

void seat_capabilities(void* d, struct wl_seat* s, u32 caps){
    struct wayland_window* w = d;
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
void seat_name(void* d, struct wl_seat* s, const char* name){ }

const struct wl_seat_listener* get_seat_listener(void){
    static const struct wl_seat_listener seat_l = { .capabilities = seat_capabilities, .name = seat_name };
    return &seat_l;
}

void xdg_ping(void* d, struct xdg_wm_base* b, u32 s){ xdg_wm_base_pong(b, s); }
const struct xdg_wm_base_listener* get_xdg_wm_listener(void){
    static const struct xdg_wm_base_listener l = { .ping = xdg_ping };
    return &l;
}
void top_config(void* d, struct xdg_toplevel* t, i32 w, i32 h, struct wl_array* st){
    struct wayland_window* win = d;
    if (win && w > 0 && h > 0) {
        win->win_w = w; win->win_h = h;
        if (win->fullscreen_configured) pf_timestamp("first fullscreen commit");
        if (!win->fullscreen_configured) {
            pf_timestamp("fullscreen configured");
            win->fullscreen_configured = 1;
            // set the opaque region
            struct wl_region* r = wl_compositor_create_region(win->compositor);
            wl_region_add(r, 0, 0, win->win_w, win->win_h);
            wl_surface_set_opaque_region(win->surface, r);
            wl_region_destroy(r);
            wl_surface_commit(win->surface);
        }
        int active = 0, fullscreen = 0;
        uint32_t *s;
        wl_array_for_each(s, st){
            if (*s == XDG_TOPLEVEL_STATE_ACTIVATED) active = 1;
            if (*s == XDG_TOPLEVEL_STATE_FULLSCREEN) fullscreen = 1;
        }
        win->visible = active && fullscreen;
        if (!win->visible) pf_timestamp("Window is not visible"); else pf_timestamp("Window is visible");
    }
}
void top_close(void* d, struct xdg_toplevel* t){ }
void top_bounds(void* d, struct xdg_toplevel* t, i32 w, i32 h){ }
void top_caps(void* d, struct xdg_toplevel* t, struct wl_array* c){ }

const struct xdg_toplevel_listener* get_top_listener(void){
    static const struct xdg_toplevel_listener top_l = {
        .configure = top_config, .close = top_close, .configure_bounds = top_bounds, .wm_capabilities = top_caps
    };
    return &top_l;
}

void xsurf_conf(void* d, struct xdg_surface* s, u32 serial){
    struct wayland_window* w = d;
    xdg_surface_ack_configure(s, serial);
}
const struct xdg_surface_listener* get_xsurf_listener(void){
    static const struct xdg_surface_listener xsurf_l = { .configure = xsurf_conf };
    return &xsurf_l;
}

static void surf_enter(void* d, struct wl_surface* s, struct wl_output* o){
}
static void surf_leave(void* d, struct wl_surface* s, struct wl_output* o){ // surface is minimized (alt tab is not enough)
}
static const struct wl_surface_listener* get_surface_listener(void){
    static const struct wl_surface_listener L = { .enter = surf_enter, .leave = surf_leave };
    return &L;
}

static void pres_clock_id(void* d, struct wp_presentation* p, uint32_t c) {
    struct wayland_window* w = d;
    clockid = (clockid_t)c;    /* POSIX clockid_t for clock_gettime() */
    pf_timestamp("presentation clock ready");
}
static const struct wp_presentation_listener* get_pres_listener(void){
    static const struct wp_presentation_listener L = { .clock_id = pres_clock_id };
    return &L;
}

void reg_add(void* d, struct wl_registry* r, uint32_t name, const char* iface, uint32_t ver){
    struct wayland_window* w = d;
    if (!w) return;
    if (!strcmp(iface, wl_compositor_interface.name)){
        u32 v = ver < 4 ? ver : 4; w->compositor = wl_registry_bind(r, name, &wl_compositor_interface, v);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)){
        u32 v = ver < 6 ? ver : 6; w->xdg_wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, v);
        xdg_wm_base_add_listener(w->xdg_wm_base, get_xdg_wm_listener(), w);
    } else if (!strcmp(iface, wl_seat_interface.name)){
        u32 v = ver < 5 ? ver : 5; w->seat = wl_registry_bind(r, name, &wl_seat_interface, v);
        wl_seat_add_listener(w->seat, get_seat_listener(), w);
    } else if (!strcmp(iface, wp_presentation_interface.name)) {
        w->presentation = wl_registry_bind(r, name, &wp_presentation_interface, 1);
        wp_presentation_add_listener(w->presentation, get_pres_listener(), w);
    }
}
void reg_rem(void* d, struct wl_registry* r, u32 name){}

const struct wl_registry_listener* get_registry_listener(void){
    static const struct wl_registry_listener reg_l = { .global = reg_add, .global_remove = reg_rem };
    return &reg_l;
}

struct FBUserData {
    struct wayland_window* w;
    uint64_t id;
    uint64_t queued_ns;     // when we asked for feedback (just before present)
};
static void fb_sync_output(void* data, struct wp_presentation_feedback* fb, struct wl_output* out){
}
static void fb_presented(void* data, struct wp_presentation_feedback* fb,
    uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec,
    uint32_t refresh_ns, uint32_t seq_hi, uint32_t seq_lo, uint32_t flags)
{
    struct FBUserData* ud = data;
    struct wayland_window* w = ud->w;

    uint64_t sec = ((uint64_t)tv_sec_hi<<32) | tv_sec_lo;
    uint64_t t_present = sec*1000000000ull + (uint64_t)tv_nsec;
    uint64_t t_now     = pf_ns_now();

    if (refresh_ns) w->refresh_ns = (uint64_t)refresh_ns;
    w->last_present_ns = t_present;
    w->in_flight_count > 0 ? w->in_flight_count-- : 0;

    double queued_ms = (double)(int64_t)(ud->queued_ns - T0)/1e6;
    double presented_ms = (double)(int64_t)(t_present - T0)/1e6;
    double fbqueue_to_present_ms= (double)(int64_t)(t_present - ud->queued_ns)/1e6;
    double predicted_next_ms      = (double)(int64_t)(refresh_ns + t_present - T0)/1e6;

    printf("Frame %llu sent to vulkan at %.3f ms and presented at %.3f (%.3f ms later) (next present expected at %.3f)\n",
           (unsigned long long)ud->id, queued_ms, presented_ms, fbqueue_to_present_ms, predicted_next_ms);
    wp_presentation_feedback_destroy(fb);
    free(ud);
}
static void fb_discarded(void* data, struct wp_presentation_feedback* fb){
    struct FBUserData* ud = data;
    printf("[present fb id=%llu] DISCARDED\n", (unsigned long long)ud->id);
    wp_presentation_feedback_destroy(fb);
    free(ud);
}
static const struct wp_presentation_feedback_listener* get_fb_listener(void){
    static const struct wp_presentation_feedback_listener L = {
        .sync_output = fb_sync_output,
        .presented   = fb_presented,
        .discarded   = fb_discarded
    };
    return &L;
}

void pf_request_present_feedback(void *win, u64 frame_id) {
    struct wayland_window* w = win;
    if (!w || !w->presentation || !w->surface) return;
    w->in_flight_count++;
    struct FBUserData* ud = calloc(1, sizeof *ud);
    ud->w = w;
    ud->id = frame_id;
    ud->queued_ns = pf_ns_now();
    wp_presentation_feedback_add_listener(wp_presentation_feedback(w->presentation, w->surface), get_fb_listener(), ud);
}

static void frame_cb_done(void *data, struct wl_callback *cb, uint32_t time) {
    struct wayland_window *w = data;
    if (cb) wl_callback_destroy(cb);
    w->in_flight_count > 0 ? --w->in_flight_count : 0;
    w->frame_done = 1;
}
static const struct wl_callback_listener* get_frame_cb_listener(void) {
    static const struct wl_callback_listener L = { .done = frame_cb_done };
    return &L;
}
static void request_frame_callback(struct wayland_window *w) {
    struct wl_callback *cb = wl_surface_frame(w->surface);
    wl_callback_add_listener(cb, get_frame_cb_listener(), w);
}

int pf_window_width(void *w) {
    struct wayland_window* win = w; return win->win_w;
}
int pf_window_height(void *w) {
    struct wayland_window* win = w; return win->win_h;
}
void *pf_surface_or_hwnd(void *w) {
    struct wayland_window* win = w; return win->surface;
}
void *pf_display_or_instance(void *w) {
    struct wayland_window* win = w; return win->display;
}
int pf_window_visible(void *w) {
    struct wayland_window* win = w;
    return win->visible;
}

int pf_poll_events(void* win){
    struct wayland_window* w = win;
    if(!w) return 0;
    wl_display_dispatch_pending(w->display); // dispatch all queued up events, don't block
    wl_display_flush(w->display);
    return 1;
}

void *pf_create_window(void *ud, KEYBOARD_CB key_cb, MOUSE_CB mouse_cb){
    struct wayland_window* w = calloc(1, sizeof(*w));
    w->win_w = 0; w->win_h = 0; w->fullscreen_configured = 0; w->frame_done = 1;
    // set the input callbacks
    w->on_key = key_cb; w->on_mouse = mouse_cb; w->callback_data = ud;
    // frame timing
    w->refresh_ns = 16666667ull;
    w->last_present_ns = 0;
    w->last_feedback_ns = 0;
    w->phase_ns = 0;
    w->phase_alpha = 0.10;

    w->display = wl_display_connect(NULL);
    if (!w->display){ printf("wl connect failed\n"); _exit(1); }

    struct wl_registry* reg = wl_display_get_registry(w->display);
    wl_registry_add_listener(reg, get_registry_listener(), w);
    wl_display_roundtrip(w->display);
    pf_time_reset(); // at this point the global clock used by presentation time is set, so we are synced right with it
    pf_timestamp("wl globals ready");
    if (!w->compositor || !w->xdg_wm_base){ printf("missing compositor/xdg\n"); _exit(1); }

    w->surface  = wl_compositor_create_surface(w->compositor);
    wl_surface_add_listener(w->surface, get_surface_listener(), w);
    w->xdg_surface = xdg_wm_base_get_xdg_surface(w->xdg_wm_base, w->surface);
    xdg_surface_add_listener(w->xdg_surface, get_xsurf_listener(), w);
    w->xdg_toplevel  = xdg_surface_get_toplevel(w->xdg_surface);
    xdg_toplevel_add_listener(w->xdg_toplevel, get_top_listener(), w);
    xdg_toplevel_set_title(w->xdg_toplevel, APP_NAME);
    xdg_toplevel_set_app_id(w->xdg_toplevel, APP_NAME);
    xdg_toplevel_set_fullscreen(w->xdg_toplevel, NULL);
    wl_surface_commit(w->surface);
    while (!w->fullscreen_configured) {
        wl_display_flush(w->display);
        wl_display_dispatch(w->display);
    }
    pf_timestamp("set up wayland"); // should be done with all this in ~10ms or less
    return w;
}
#endif
