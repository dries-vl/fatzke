#include "header.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <linux/vt.h>
#include <linux/input.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

// -------------------------------
// private state
// -------------------------------
static struct {
    // config & callbacks
    struct VTConfig cfg;
    vt_on_acquire_fn on_acq;
    vt_on_release_fn on_rel;
    void* user;

    // vt numbers
    int vt_home;           // desktop VT (where to Alt+Tab / return on exit)
    int vt_game;           // our VT
    int active;            // are we the active VT right now?

    // drm
    int drm_fd;
    uint32_t connector_id;

    // evdev
    int want_evdev;
    int ev_count;
    int ev_fds[32];
    uint8_t key_down[256]; // simple key bitmap (ev keycodes 0..255)

    // paths
    char state_path[256];  // "$XDG_RUNTIME_DIR/<app_id>.vt" or override

    // control
    int want_exit;
} G;

// -------------------------------
// helpers
// -------------------------------
static int vt_cur_active(void){
    int fd = open("/dev/tty0", O_RDONLY | O_CLOEXEC);
    if(fd < 0) return -1;
    struct vt_stat s;
    int ok = ioctl(fd, VT_GETSTATE, &s);
    close(fd);
    return ok == 0 ? s.v_active : -1;
}

static int vt_activate(int vt){
    int fd = open("/dev/tty0", O_RDONLY | O_CLOEXEC);
    if(fd < 0) return -1;
    int ok = (ioctl(fd, VT_ACTIVATE, vt) == 0) && (ioctl(fd, VT_WAITACTIVE, vt) == 0);
    close(fd);
    return ok ? 0 : -1;
}

static void write_state_file(void){
    if(!G.state_path[0]) return;
    FILE* f = fopen(G.state_path, "w");
    if(!f) return;
    fprintf(f, "%d %d\n", G.vt_home, G.vt_game);
    fclose(f);
}

static void read_state_file(void){
    if(!G.state_path[0]) return;
    FILE* f = fopen(G.state_path, "r");
    if(!f) return;
    int h=-1,g=-1;
    if(fscanf(f, "%d %d", &h, &g) == 2){
        if(G.vt_home <= 0) G.vt_home = h;
        if(G.vt_game <= 0) G.vt_game = g;
    }
    fclose(f);
}

static const char* xdg_runtime_dir(void){
    const char* r = getenv("XDG_RUNTIME_DIR");
    return (r && *r) ? r : "/tmp";
}

// -------------------------------
// DRM helpers
// -------------------------------
static int ensure_master(int fd){
    if (drmIsMaster(fd)) return 1;
    if (drmSetMaster(fd) == 0) return 1;
    return 0;
}

static int pick_connected_connector(int fd, uint32_t prefer){
    drmModeRes* res = drmModeGetResources(fd);
    if(!res) return 0;
    uint32_t pick = 0;
    for(int i=0;i<res->count_connectors;i++){
        drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
        if(!c) continue;
        if(c->connection == DRM_MODE_CONNECTED && c->count_modes > 0){
            if(!pick) pick = c->connector_id;
            if(prefer && c->connector_id == prefer) pick = c->connector_id;
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    return pick;
}

static int open_card_with_connectors(void){
    for(int i=0;i<8;i++){
        char p[64]; snprintf(p, sizeof p, "/dev/dri/card%d", i);
        int fd = open(p, O_RDWR | O_CLOEXEC);
        if(fd < 0) continue;
        drmModeRes* res = drmModeGetResources(fd);
        int ok = res && res->count_connectors > 0;
        if(res) drmModeFreeResources(res);
        if(ok) return fd;
        close(fd);
    }
    return -1;
}

// EVDEV
static void evdev_open_all(void){
    G.ev_count = 0;
    DIR* d = opendir("/dev/input");
    if(!d) return;
    struct dirent* ent;
    while((ent = readdir(d))){
        if(strncmp(ent->d_name, "event", 5) != 0) continue;
        if(G.ev_count >= (int)(sizeof G.ev_fds / sizeof G.ev_fds[0])) break;
        char path[128]; snprintf(path, sizeof path, "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if(fd >= 0) G.ev_fds[G.ev_count++] = fd;
    }
    closedir(d);
}

static void evdev_close_all(void){
    for(int i=0;i<G.ev_count;i++) close(G.ev_fds[i]);
    G.ev_count = 0;
}

static void key_set(int code, int down){
    if((unsigned)code < sizeof(G.key_down)){
        G.key_down[code] = down ? 1 : 0;
    }
}

static int key_is_down(int code){
    return ((unsigned)code < sizeof(G.key_down)) ? G.key_down[code] : 0;
}

static void evdev_poll_once(int timeout_ms){
    if(G.ev_count <= 0) return;
    struct pollfd pf[32];
    int n = G.ev_count;
    for(int i=0;i<n;i++){ pf[i].fd = G.ev_fds[i]; pf[i].events = POLLIN; pf[i].revents = 0; }
    int r = poll(pf, n, timeout_ms);
    if(r <= 0) return;

    for(int i=0;i<n;i++){
        if(!(pf[i].revents & POLLIN)) continue;
        struct input_event ev;
        for(;;){
            ssize_t m = read(pf[i].fd, &ev, sizeof ev);
            if(m < 0){
                if(errno == EAGAIN) break;
                else break;
            }
            if(m != sizeof ev) break;
            if(ev.type == EV_KEY){
                int code = ev.code;
                int down = (ev.value != 0);
                key_set(code, down);
                // Alt+Tab => give desktop VT
                if(code == KEY_TAB && down && (key_is_down(KEY_LEFTALT) || key_is_down(KEY_RIGHTALT))){
                    G.want_exit = 0;
                    // trigger "pause to desktop"
                    // we do it outside ev loop to avoid re-entrancy; mark via a flag:
                    vt_alt_tab();
                }
                // Esc to exit completely
                if(code == KEY_ESC && down){
                    vt_request_exit();
                }
            }
        }
    }
}

// Acquire/Release + Focus handling
static void do_release_to_desktop(void){
    if(G.on_rel) G.on_rel(G.user); // Vulkan side should release the display + swapchain
    if(G.drm_fd >= 0){
        drmDropMaster(G.drm_fd);
    }
    if(G.vt_home > 0){
        vt_activate(G.vt_home);
    }
    G.active = 0;
}

static int do_acquire_on_game_vt(void){
    // Ensure we're on our VT
    if(G.vt_game > 0){
        if(vt_cur_active() != G.vt_game){
            if(vt_activate(G.vt_game) != 0) return 0;
        }
    }
    // DRM master
    if(!ensure_master(G.drm_fd)) return 0;

    // pick connector
    G.connector_id = pick_connected_connector(G.drm_fd, G.cfg.prefer_connector);
    if(!G.connector_id) return 0;

    // Vulkan side creates display-plane surface/swapchain on (drm_fd, connector)
    if(G.on_acq){
        if(G.on_acq(G.drm_fd, G.connector_id, G.user) != 1){
            return 0;
        }
    }
    G.active = 1;
    return 1;
}

// Public API
static int parse_int_env(const char* name){
    const char* v = name ? getenv(name) : NULL;
    if(!v || !*v) return -1;
    char* end = NULL; long k = strtol(v, &end, 10);
    return (end && *end == '\0' && k > 0 && k < 1000) ? (int)k : -1;
}

static void build_state_path(const char* app_id){
    if(G.cfg.state_path && *G.cfg.state_path){
        snprintf(G.state_path, sizeof G.state_path, "%s", G.cfg.state_path);
        return;
    }
    const char* r = xdg_runtime_dir();
    if(app_id && *app_id)
        snprintf(G.state_path, sizeof G.state_path, "%s/%s.vt", r, app_id);
    else
        snprintf(G.state_path, sizeof G.state_path, "%s/%s", r, "app.vt");
}

int vt_init(const struct VTConfig* cfg,
            vt_on_acquire_fn on_acquire,
            vt_on_release_fn on_release,
            void* user)
{
    memset(&G, 0, sizeof G);
    G.on_acq = on_acquire;
    G.on_rel = on_release;
    G.user   = user;

    // copy config with sensible defaults
    if(cfg) G.cfg = *cfg;
    if(!G.cfg.app_id) G.cfg.app_id = "app";
    if(G.cfg.want_evdev == 0) G.want_evdev = 0; else G.want_evdev = 1;

    // VT ids
    build_state_path(G.cfg.app_id);
    G.vt_game = vt_cur_active();      // where we are now
    G.vt_home = parse_int_env(G.cfg.env_home_vt);
    if(G.vt_home <= 0) read_state_file(); // maybe launcher left it for us
    if(G.vt_home <= 0) {
        // Fallback: try to read previous active VT from env (some launchers set it)
        G.vt_home = parse_int_env("MYGAME_HOME_VT");
    }
    if(G.vt_home <= 0) {
        fprintf(stderr, "[vt] (warning) HOME VT unknown; Alt+Tab will be disabled\n");
    }

    // persist state file (so a "Return to Game" launcher knows GAME_VT)
    write_state_file();

    // DRM
    G.drm_fd = open_card_with_connectors();
    if(G.drm_fd < 0){
        fprintf(stderr, "[vt] no DRM card with connectors\n");
        return 0;
    }

    // evdev
    if(G.want_evdev) evdev_open_all();

    // Acquire display now (we assume we were launched on our own VT)
    if(!do_acquire_on_game_vt()){
        fprintf(stderr, "[vt] initial acquire failed (not on own VT or no master); "
                        "switch to VT %d and try again.\n", G.vt_game);
        // We keep running and will acquire once the VT becomes active.
    }

    return 1;
}

void vt_poll(void){
    // If we lost/gained focus (user switched VT), handle it
    int active_now = (vt_cur_active() == G.vt_game);
    if(active_now && !G.active){
        // we regained our VT → acquire
        (void)do_acquire_on_game_vt();
    }else if(!active_now && G.active){
        // we lost VT unexpectedly → release
        if(G.on_rel) G.on_rel(G.user);
        if(G.drm_fd >= 0) drmDropMaster(G.drm_fd);
        G.active = 0;
    }

    // evdev hotkeys
    if(G.want_evdev) evdev_poll_once(0);
}

void vt_alt_tab(void){
    if(G.active && G.vt_home > 0){
        do_release_to_desktop();
    }
}

void vt_request_exit(void){ G.want_exit = 1; }
int vt_should_exit(void){ return G.want_exit != 0; }

void vt_shutdown(void){
    // release if active
    if(G.active && G.on_rel) G.on_rel(G.user);
    if(G.drm_fd >= 0){
        drmDropMaster(G.drm_fd);
        close(G.drm_fd);
        G.drm_fd = -1;
    }
    if(G.want_evdev) evdev_close_all();

    // switch back to desktop VT
    if(G.vt_home > 0) (void)vt_activate(G.vt_home);
}

int vt_get_home_vt(void){ return G.vt_home; }
int vt_get_game_vt(void){ return G.vt_game; }
int vt_is_active(void){ return G.active; }
