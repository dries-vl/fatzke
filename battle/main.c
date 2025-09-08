// main.c  (no Vulkan headers)
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "header.h"

#include <xf86drm.h>
#include <xf86drmMode.h>

u64 now_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (u64)ts.tv_sec*1000000000ull+ts.tv_nsec; }
u64 T0;
void pf_time_reset() {T0=now_ns();}
void pf_timestamp(char *msg) {u64 _t=now_ns(); fprintf(stderr,"[+%7.3f ms] %s\n",(_t-T0)/1e6,(msg));}

#include <signal.h>
#include <sys/ioctl.h>
#include <linux/vt.h>
#include <fcntl.h>
#include <unistd.h>

static int g_drm_fd = -1;
static int g_home_vt = -1;

static int vt_cur_active(void){
    int fd = open("/dev/tty0", O_RDONLY|O_CLOEXEC);
    if(fd < 0) return -1;
    struct vt_stat s; int ok = ioctl(fd, VT_GETSTATE, &s) == 0;
    close(fd);
    return ok ? s.v_active : -1;
}
static int vt_activate(int vt){
    int fd = open("/dev/tty0", O_RDONLY|O_CLOEXEC);
    if(fd < 0) return -1;
    int ok = (ioctl(fd, VT_ACTIVATE, vt) == 0) && (ioctl(fd, VT_WAITACTIVE, vt) == 0);
    close(fd);
    return ok ? 0 : -1;
}

// read MYGAME_HOME_VT if Steam launcher exported it, else remember the VT we came from
static void detect_home_vt(void){
    const char* v = getenv("MYGAME_HOME_VT");
    if(v && *v){ g_home_vt = (int)strtol(v, NULL, 10); }
    if(g_home_vt <= 0) g_home_vt = vt_cur_active(); // best-effort
}

static void safe_cleanup(void){
    // Drop master so the console/compositor can re-take KMS
    if(g_drm_fd >= 0){
        drmDropMaster(g_drm_fd);
        // don't close(g_drm_fd) here if your Vulkan path still needs it; otherwise:
        // close(g_drm_fd); g_drm_fd = -1;
    }
    // Tear Vulkan down (your function already guards against NULLs)
    vk_shutdown_all();
    // Switch back to desktop VT
    if(g_home_vt > 0) vt_activate(g_home_vt);
}

static void on_signal(int sig){
    safe_cleanup();
    // Re-raise default for crash signals so you still get a core/log
    signal(sig, SIG_DFL);
    raise(sig);
}

static int open_card_with_connectors(void)
{
    for (int i = 0; i < 8; i++)
    {
        char p[64];
        snprintf(p, sizeof p, "/dev/dri/card%d", i);
        int fd = open(p, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmModeRes* res = drmModeGetResources(fd);
        int ok = res && res->count_connectors > 0;
        if (res) drmModeFreeResources(res);
        if (ok) return fd;
        close(fd);
    }
    return -1;
}

static int ensure_master(int fd)
{
    if (drmIsMaster(fd)) return 1;
    return drmSetMaster(fd) == 0;
}

static unsigned env_u32(const char* name, unsigned fallback)
{
    const char* v = getenv(name);
    if (!v || !*v) return fallback;
    char* end = NULL;
    unsigned long x = strtoul(v, &end, 10);
    return (end && *end == '\0') ? (unsigned)x : fallback;
}

int main(void){
    detect_home_vt();
    atexit(safe_cleanup);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP,  on_signal);
    signal(SIGSEGV, on_signal);

    vk_init_instance();
    vk_pick_phys();

    int drm_fd = open_card_with_connectors();
    g_drm_fd = drm_fd;                // so the cleanup can drop master
    if(drm_fd < 0){ fprintf(stderr,"no DRM card with connectors\n"); return 1; }

    if(!ensure_master(drm_fd)){
        fprintf(stderr,"drmSetMaster failed (launch via openvt -sw)\n");
        return 1;
    }

    unsigned prefer = env_u32("CONNECTOR", 0);
    if(!vk_try_direct_display_takeover_with_fd(drm_fd, (int)prefer)){
        fprintf(stderr,"direct-display takeover failed\n");
        return 1;
    }

    vk_pick_qfam_for_surface();
    vk_make_device();
    vk_graph_initial_build(0,0);

    for(int i = 0; i < 1000; ++i){
        int outdated = vk_present_frame();
        if(outdated) vk_recreate_all(0,0);
        pf_timestamp("FRAME");
        // TODO: add your own quit condition if you don't want infinite loop
    }
    return 0; // atexit() will run safe_cleanup()
}
