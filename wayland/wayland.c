#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-shell-protocol.c"

#define FALLBACK_W 640
#define FALLBACK_H 480

/* user-supplied input callbacks */
typedef void (*keyboard_cb)(void *ud, uint32_t key, uint32_t state);         /* wl_keyboard key */
typedef void (*mouse_cb)(void *ud, int32_t x, int32_t y, uint32_t b); /* wl_pointer button */
typedef void (*resize_cb)(void *ud, int w, int h);

/* ===== internal state ===== */
struct ctx {
    struct wl_display    *dpy;
    struct wl_compositor *comp;
    struct wl_shm        *shm;
    struct xdg_wm_base   *wm;

    struct wl_surface    *surf;
    struct xdg_surface   *xs;
    struct xdg_toplevel  *top;

    struct wl_buffer     *buf;
    void   *pixels;
    int     buf_w, buf_h, stride;
    int     win_w, win_h;
    int     configured;
    
    double last_x, last_y; // keep track of mouse position
    int vsync_ready; // set by frame_done callback
    keyboard_cb     keyboard_cb; // callback for keyboard input events
    mouse_cb mouse_cb; // callback for mouse input events
    resize_cb  resize_window_cb; // callback for window resize events
    void *callback_userdata;
};

/* ========= helpers ========= */
static int memfd(size_t len)
{
    size_t pagesize = (size_t)sysconf(_SC_PAGESIZE);      /* normally 4096 */
    size_t padded   = (len + pagesize - 1) & ~(pagesize - 1);  /* round-up  */
    int fd = syscall(SYS_memfd_create, "ultrafast", 0);
    if (fd < 0 || ftruncate(fd, (off_t)padded) < 0) {
        perror("memfd");  exit(1);
    }
    return fd;
}
static void alloc_buffer(struct ctx *st, int w, int h)
{
    size_t stride = (size_t)w * 4, len = stride * h;
    int fd = memfd(len);

    st->pixels = mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (st->pixels == MAP_FAILED) { perror("mmap"); exit(1); }

    struct wl_shm_pool *pool = wl_shm_create_pool(st->shm, fd, (int)len);
    st->buf = wl_shm_pool_create_buffer(pool, 0, w, h, (int)stride,
                                        WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    st->buf_w = w; st->buf_h = h; st->stride = (int)stride;

    /* paint once */
    //for (uint32_t *p = st->pixels, *end = p + (size_t)w * h; p < end; ++p)
    //    *p = BLACK;
}

// INPUT CALLBACKS
static void kb_key(void *data, struct wl_keyboard *kbd, uint32_t serial,
                   uint32_t time, uint32_t key, uint32_t state)
{
    struct ctx *c = data;
    if (c->keyboard_cb) c->keyboard_cb(c->callback_userdata, key, state);
}

static void ptr_motion(void *data, struct wl_pointer *ptr, uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
    struct ctx *c = data;
    c->last_x = wl_fixed_to_double(sx);
    c->last_y = wl_fixed_to_double(sy);
}

static void ptr_enter(void *data, struct wl_pointer *ptr, uint32_t serial,
                      struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
    struct ctx *c = data;
    c->last_x = wl_fixed_to_double(sx);
    c->last_y = wl_fixed_to_double(sy);
}

static void ptr_button(void *data, struct wl_pointer *ptr, uint32_t serial,
                       uint32_t time, uint32_t button, uint32_t state) {
    struct ctx *c = data;
    if (c->mouse_cb) c->mouse_cb(c->callback_userdata, c->last_x, c->last_y, state);
}


/* ---------- keyboard no-ops ---------- */
static void kb_keymap(void *d, struct wl_keyboard *k,
                      uint32_t format, int32_t fd, uint32_t size) {}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial,
                     struct wl_surface *s, struct wl_array *keys) {}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial,
                     struct wl_surface *s) {}
static void kb_modifiers(void *d, struct wl_keyboard *k, uint32_t serial,
                         uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp) {}
static void kb_repeat_info(void *d, struct wl_keyboard *k,
                           int32_t rate, int32_t delay) {}

/* every slot filled → no aborts */
static const struct wl_keyboard_listener kbd_lis = {
    .keymap       = kb_keymap,
    .enter        = kb_enter,
    .leave        = kb_leave,
    .key          = kb_key,
    .modifiers    = kb_modifiers,
    .repeat_info  = kb_repeat_info
};

/* ---------- pointer no-ops ---------- */
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s) {}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t time,
                     uint32_t axis, wl_fixed_t value) {}
static void ptr_frame(void *d, struct wl_pointer *p) {}

static const struct wl_pointer_listener ptr_lis = {
    .enter  = ptr_enter,
    .leave  = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis   = ptr_axis,
    .frame  = ptr_frame
};

/* ===== Wayland listeners ===== */
static void reg_add(void *data, struct wl_registry *reg,
                    uint32_t id, const char *iface, uint32_t ver)
{
    struct ctx *st = data;
    if (!strcmp(iface, "wl_compositor"))
        st->comp = wl_registry_bind(reg, id, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "wl_shm"))
        st->shm  = wl_registry_bind(reg, id, &wl_shm_interface,       1);
    else if (!strcmp(iface, "xdg_wm_base"))
        st->wm   = wl_registry_bind(reg, id, &xdg_wm_base_interface,  1);
    else if (!strcmp(iface, "wl_seat")) {
        struct wl_seat *seat = wl_registry_bind(reg, id, &wl_seat_interface, 4);
        struct wl_keyboard *kbd = wl_seat_get_keyboard(seat);
        if (kbd) wl_keyboard_add_listener(kbd, &kbd_lis, st);
        struct wl_pointer  *ptr = wl_seat_get_pointer(seat);
        if (ptr) wl_pointer_add_listener(ptr, &ptr_lis, st);
    }
}
static const struct wl_registry_listener reg_lis = { reg_add, NULL };

static void ping_cb(void *d, struct xdg_wm_base *wm, uint32_t serial)
{ xdg_wm_base_pong(wm, serial); }
static const struct xdg_wm_base_listener wm_lis = { ping_cb };

static void surf_cfg(void *d, struct xdg_surface *s, uint32_t serial)
{
    struct ctx *st = d;
    xdg_surface_ack_configure(s, serial);
    st->configured = 1;
}
static const struct xdg_surface_listener surf_lis = { surf_cfg };

static void top_cfg(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st_) {
    struct ctx *st = d;
    int resized = 0;
    if (w > 0 && w != st->win_w) { st->win_w = w; resized = 1; }
    if (h > 0 && h != st->win_h) { st->win_h = h; resized = 1; }
    if (resized && st->resize_window_cb)
        st->resize_window_cb(st->callback_userdata, st->win_w, st->win_h);
}

static const struct xdg_toplevel_listener top_lis = { top_cfg, NULL };

/* simple helper: run until *flag set */
static void run_until(struct ctx *st, int *flag)
{
    while (!*flag) {
        wl_display_flush(st->dpy);
        if (wl_display_dispatch(st->dpy) < 0) exit(1);
    }
}

struct ctx *create_window(int w, int h, const char *title, keyboard_cb kcb, mouse_cb mcb, resize_cb rcb, void *ud)
{
    struct ctx *st = calloc(1, sizeof *st);
    st->win_w = w ? w : FALLBACK_W;
    st->win_h = h ? h : FALLBACK_H;
    
    st->keyboard_cb = kcb;
    st->mouse_cb = mcb;
    st->resize_window_cb = rcb;
    st->callback_userdata = ud;

    if (!(st->dpy = wl_display_connect(NULL))) { perror("connect"); goto fail; }

    struct wl_registry *r = wl_display_get_registry(st->dpy);
    wl_registry_add_listener(r, &reg_lis, st);

    while (!st->comp || !st->shm || !st->wm)
        wl_display_dispatch(st->dpy);
    xdg_wm_base_add_listener(st->wm, &wm_lis, NULL);

    st->surf = wl_compositor_create_surface(st->comp);
    st->xs   = xdg_wm_base_get_xdg_surface(st->wm, st->surf);
    st->top  = xdg_surface_get_toplevel(st->xs);
    xdg_surface_add_listener(st->xs,  &surf_lis, st);
    xdg_toplevel_add_listener(st->top, &top_lis, st);
    if (title) xdg_toplevel_set_title(st->top, title);

    xdg_toplevel_set_fullscreen(st->top, NULL); // set fullscreen

    wl_surface_commit(st->surf); // 1st commit: no buffer
    wl_display_flush(st->dpy);

    // ---- Wait for real fullscreen size ----
    // todo: we wait for the callback here, but shouldn't we put the code below in there instead then?
    run_until(st, &st->configured);

    // ---- Allocate buffer with correct size ----
    alloc_buffer(st, st->win_w, st->win_h);

    wl_surface_attach(st->surf, st->buf, 0, 0);
    wl_surface_damage_buffer(st->surf, 0, 0, st->win_w, st->win_h);
    wl_surface_commit(st->surf);
    wl_display_flush(st->dpy);

    return st;
fail:
    free(st);
    return NULL;
}

void *get_buffer(struct ctx *c)
{
    return c->pixels;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time);

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    wl_callback_destroy(cb);
    struct ctx *c = data;
    c->vsync_ready = 1;
}

void window_wait_vsync(struct ctx *c)
{
    c->vsync_ready = 0;
    struct wl_callback *cb = wl_surface_frame(c->surf);
    static const struct wl_callback_listener frame_listener = { .done = frame_done };
    wl_callback_add_listener(cb, &frame_listener, c);
}

void commit(struct ctx *c)
{
    wl_surface_attach(c->surf, c->buf, 0, 0);
    wl_surface_damage_buffer(c->surf, 0, 0, c->win_w, c->win_h);
    wl_surface_commit(c->surf);
    wl_display_flush(c->dpy);

    // Block in dispatch until vsync_ready is set by callback
    while (!c->vsync_ready)
        wl_display_dispatch(c->dpy);
}

int window_poll(struct ctx *c) {
    wl_display_flush(c->dpy); // send any pending requests

    // Try to read new events (non-blocking)
    if (wl_display_prepare_read(c->dpy) == 0) {
        // No events waiting: poll fd for input
        struct pollfd pfd = {
            .fd = wl_display_get_fd(c->dpy),
            .events = POLLIN
        };
        // poll with zero timeout: don't block
        poll(&pfd, 1, 0);

        // This is always non-blocking
        wl_display_read_events(c->dpy);
    } else {
        wl_display_cancel_read(c->dpy); // always cancel if can't read
    }

    // Process any new events already in queue
    return wl_display_dispatch_pending(c->dpy) >= 0;
}

void destroy(struct ctx *c)
{
    if (!c) return;
    wl_buffer_destroy(c->buf);
    munmap(c->pixels, (size_t)c->buf_h * c->stride);
    wl_surface_destroy(c->surf);
    wl_display_disconnect(c->dpy);
    free(c);
}
