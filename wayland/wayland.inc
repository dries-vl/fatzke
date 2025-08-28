#include "../header/header.inc"
// sudo apt install libwayland-dev (for getting the wayland-client header and lib)
#include <wayland-client.h>
// sudo apt install wayland-protocols wayland-scanner (for generating the xdg-shell files)
// sudo wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml /usr/include/xdg-shell-client-protocol.h
// sudo wayland-scanner private-code /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml /usr/include/xdg-shell-client-protocol.c
#include <xdg-shell-client-protocol.h>
#include <xdg-shell-client-protocol.c>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

// todo: this is interlinked badly
#include "../fatzke/data/static/icon.h"

typedef void (*keyboard_cb)(void *ud, u32 key, u32 state);
typedef void (*mouse_cb)(void *ud, i32 x, i32 y, u32 b);
typedef void (*resize_cb)(void *ud, u32 w, u32 h);

struct ctx {
    struct wl_display *dpy;
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct xdg_wm_base *wm;

    struct wl_surface *surf;
    struct xdg_surface *xs;
    struct xdg_toplevel *top;

    struct wl_buffer *buf;
    u32 *pixels;
    int buf_w, buf_h, stride;
    int win_w, win_h;
    int configured;
    
    double last_x, last_y; // keep track of mouse position
    int vsync_ready; // set by frame_done callback
    keyboard_cb keyboard_cb; // callback for keyboard input events
    mouse_cb mouse_cb; // callback for mouse input events
    resize_cb  resize_window_cb; // callback for window resize events
    void *callback_userdata;
};

static int memfd(size_t len) {
    size_t pagesize = (size_t)sysconf(_SC_PAGESIZE);      /* normally 4096 */
    size_t padded   = (len + pagesize - 1) & ~(pagesize - 1);  /* round-up  */
    int fd = syscall(SYS_memfd_create, "ultrafast", 0);
    if (fd < 0 || ftruncate(fd, (off_t)padded) < 0) {
        perror("memfd");  exit(1);
    }
    return fd;
}

static void alloc_buffer(struct ctx *st, int w, int h) {
    size_t stride = (size_t)w * 4, len = stride * h;
    int fd = memfd(len);

    st->pixels = mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (st->pixels == MAP_FAILED) { perror("mmap"); exit(1); }

    struct wl_shm_pool *pool = wl_shm_create_pool(st->shm, fd, (int)len);
    st->buf = wl_shm_pool_create_buffer(pool, 0, w, h, (int)stride,
                                        WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    st->buf_w = w; st->buf_h = h; st->stride = (int)stride;
}

// INPUT CALLBACKS
static void kb_key(void *data, struct wl_keyboard *kbd, u32 serial, u32 time, u32 key, u32 state) {
    struct ctx *c = data;
    if (c->keyboard_cb) c->keyboard_cb(c->callback_userdata, key, state);
}

static void ptr_motion(void *data, struct wl_pointer *ptr, u32 time, wl_fixed_t sx, wl_fixed_t sy) {
    struct ctx *c = data;
    c->last_x = wl_fixed_to_double(sx);
    c->last_y = wl_fixed_to_double(sy);
}

static void ptr_enter(void *data, struct wl_pointer *ptr, u32 serial, struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
    struct ctx *c = data;
    c->last_x = wl_fixed_to_double(sx);
    c->last_y = wl_fixed_to_double(sy);
}

static void ptr_button(void *data, struct wl_pointer *ptr, u32 serial, u32 time, u32 button, u32 state) {
    struct ctx *c = data;
    if (c->mouse_cb) c->mouse_cb(c->callback_userdata, c->last_x, c->last_y, state);
}

static void kb_keymap(void *d, struct wl_keyboard *k, u32 format, i32 fd, u32 size) {}
static void kb_enter(void *d, struct wl_keyboard *k, u32 serial, struct wl_surface *s, struct wl_array *keys) {}
static void kb_leave(void *d, struct wl_keyboard *k, u32 serial, struct wl_surface *s) {}
static void kb_modifiers(void *d, struct wl_keyboard *k, u32 serial, u32 dep, u32 lat, u32 lock, u32 grp) {}
static void kb_repeat_info(void *d, struct wl_keyboard *k, i32 rate, i32 delay) {}

static const struct wl_keyboard_listener kbd_lis = {
    .keymap       = kb_keymap,
    .enter        = kb_enter,
    .leave        = kb_leave,
    .key          = kb_key,
    .modifiers    = kb_modifiers,
    .repeat_info  = kb_repeat_info
};

static void ptr_leave(void *d, struct wl_pointer *p, u32 serial, struct wl_surface *s) {}
static void ptr_axis(void *d, struct wl_pointer *p, u32 time, u32 axis, wl_fixed_t value) {}
static void ptr_frame(void *d, struct wl_pointer *p) {}

static const struct wl_pointer_listener ptr_lis = {
    .enter  = ptr_enter,
    .leave  = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis   = ptr_axis,
    .frame  = ptr_frame
};

struct output_info {
    int width;
    int height;
};

static void output_geometry(void *data, struct wl_output *output, i32 x, i32 y, i32 phys_width, i32 phys_height, i32 subpixel, const char *make, const char *model, i32 transform) {}
static void output_done(void *data, struct wl_output *output) {}
static void output_scale(void *data, struct wl_output *output, i32 factor) {}
static void output_mode(void *data, struct wl_output *output, u32 flags, i32 width, i32 height, i32 refresh) {}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode     = output_mode,
    .done     = output_done,
    .scale    = output_scale
};

static void reg_add(void *data, struct wl_registry *reg, u32 id, const char *iface, u32 ver) {
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
    } else if (!strcmp(iface, "wl_output")) {
        struct wl_output *output = wl_registry_bind(reg, id, &wl_output_interface, 2);
        wl_output_add_listener(output, &output_listener, st);
    }
}
static const struct wl_registry_listener reg_lis = {
    reg_add, NULL
};

static void ping_cb(void *d, struct xdg_wm_base *wm, u32 serial) {
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_lis = {
    ping_cb
};

static void surf_cfg(void *d, struct xdg_surface *s, u32 serial) {
    struct ctx *st = d;
    xdg_surface_ack_configure(s, serial);
    st->configured = 1;
}
static const struct xdg_surface_listener surf_lis = {
    surf_cfg
};

void top_cfg(void *d, struct xdg_toplevel *t, i32 w, i32 h, struct wl_array *st_) {
    struct ctx *st = d;

    int resized = (w != st->win_w) || (h != st->win_h);
    if (!resized) return;

    st->win_w = w;
    st->win_h = h;

    if (st->resize_window_cb) st->resize_window_cb(st->callback_userdata, st->win_w, st->win_h);
}

static const struct xdg_toplevel_listener top_lis = {
    top_cfg, NULL
};

static void run_until(struct ctx *st, int *flag) {
    while (!*flag) {
        wl_display_flush(st->dpy);
        if (wl_display_dispatch(st->dpy) < 0) exit(1);
    }
}

u32 *get_buffer(struct ctx *c) {
    return c->pixels;
}

static void frame_done(void *data, struct wl_callback *cb, u32 time);

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void frame_done(void *data, struct wl_callback *cb, u32 time) {
    wl_callback_destroy(cb);
    struct ctx *c = data;
    c->vsync_ready = 1;
}

void commit(struct ctx *c)
{
    c->vsync_ready = 0;

    wl_surface_attach(c->surf, c->buf, 0, 0);
    wl_surface_damage_buffer(c->surf, 0, 0, c->win_w, c->win_h);
    
    struct wl_callback *cb = wl_surface_frame(c->surf);
    static const struct wl_callback_listener frame_listener = { .done = frame_done };
    wl_callback_add_listener(cb, &frame_listener, c);

    wl_surface_commit(c->surf);
    wl_display_flush(c->dpy);
}

int poll_events(struct ctx *c) {
    // blocks until we receive an event
    return wl_display_dispatch(c->dpy) >= 0; // returns -1 if connection to compositor dead
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

static int mkpath(const char *p) { char t[PATH_MAX]; size_t n=strlen(p); if (n>=sizeof t) return -1; strcpy(t,p); for (char *q=t+1; *q; q++) if (*q=='/') { *q=0; if (mkdir(t,0755)&&errno!=EEXIST) return -1; *q='/'; } return mkdir(t,0755)&&errno!=EEXIST?-1:0; }
static int writen(const char *p, const void *buf, size_t len) { FILE *f=fopen(p,"wb"); if(!f) return -1; size_t w=fwrite(buf,1,len,f); fclose(f); return w==len?0:-1; }

static void install_desktop_and_icon_once(const char *executable_name) {
    const char *home = getenv("HOME"); if (!home || !*home) return;
    char icon_dir[PATH_MAX]; snprintf(icon_dir, sizeof icon_dir, "%s/.local/share/icons/hicolor/256x256/apps", home);
    char icon_path[PATH_MAX]; snprintf(icon_path, sizeof icon_path, "%s/%s.png", icon_dir, executable_name);
    char desk_dir[PATH_MAX]; snprintf(desk_dir, sizeof desk_dir, "%s/.local/share/applications", home);
    char desk_path[PATH_MAX]; snprintf(desk_path, sizeof desk_path, "%s/%s.desktop", desk_dir, executable_name);

    if (access(icon_path, F_OK) != 0) { if (!mkpath(icon_dir)) writen(icon_path, icon_png, icon_png_len); }
    if (access(desk_path, F_OK) != 0) {
        if (!mkpath(desk_dir)) {
            char buf[1024];
            int n = snprintf(buf, sizeof buf,
                "[Desktop Entry]\n"
                "Type=Application\n"
                "Version=1.5\n"
                "Name=%s\n"
                "Exec=%s\n"
                "Icon=%s\n"
                "Terminal=false\n"
                "Categories=Utility;\n"
                "StartupWMClass=%s\n",
                executable_name, executable_name, icon_path, executable_name);
            if (n>0 && n<(int)sizeof buf) { writen(desk_path,buf,strlen(buf)); chmod(desk_path,0755); }
        }
    }
}

struct ctx *create_window(keyboard_cb kcb, mouse_cb mcb, resize_cb rcb, void *ud) {
    // Stupid hack needed on wayland: put exe icon in executable, and then copy to the right dir to have an icon
    char exe_path[PATH_MAX]; ssize_t path_len = readlink("/proc/self/exe", exe_path, sizeof exe_path - 1); if (path_len <= 0) _exit(1); exe_path[path_len]=0;
    const char *exe_name = strrchr(exe_path, '/');
    exe_name = exe_name ? exe_name + 1 : exe_path;
    int running_from_tcc = strcmp(exe_name, "tcc") == 0;
    if (!running_from_tcc) // install the .desktop and .png files to have an icon (if not already done previously)
        install_desktop_and_icon_once(exe_name);

    struct ctx *st = calloc(1, sizeof *st);
    st->keyboard_cb = kcb; st->mouse_cb = mcb; st->resize_window_cb = rcb; st->callback_userdata = ud;

    if (!((st->dpy = wl_display_connect(NULL)))) { perror("connect"); goto fail; }

    struct wl_registry *r = wl_display_get_registry(st->dpy);
    wl_registry_add_listener(r, &reg_lis, st);

    while (!st->comp || !st->shm || !st->wm) wl_display_dispatch(st->dpy);
    xdg_wm_base_add_listener(st->wm, &wm_lis, NULL);

    st->surf = wl_compositor_create_surface(st->comp);
    st->xs   = xdg_wm_base_get_xdg_surface(st->wm, st->surf);
    st->top  = xdg_surface_get_toplevel(st->xs);
    xdg_surface_add_listener(st->xs,  &surf_lis, st);
    xdg_toplevel_add_listener(st->top, &top_lis, st);

    xdg_toplevel_set_title(st->top, exe_name);
    xdg_toplevel_set_app_id(st->top, exe_name);

    xdg_toplevel_set_fullscreen(st->top, NULL);
    wl_surface_commit(st->surf);
    wl_display_flush(st->dpy);

    run_until(st, &st->configured);
    alloc_buffer(st, st->win_w, st->win_h);

    wl_surface_attach(st->surf, st->buf, 0, 0);
    wl_surface_damage_buffer(st->surf, 0, 0, st->win_w, st->win_h);
    wl_surface_commit(st->surf);

    struct wl_callback *cb = wl_surface_frame(st->surf);
    wl_callback_add_listener(cb, &frame_listener, st);
    st->vsync_ready = 1;

    wl_display_flush(st->dpy);
    return st;
    fail:
        free(st);
    return NULL;
}
