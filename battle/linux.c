#include "header.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>

#define LOGE(...) do{ fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); }while(0)

/* --------- global state for simple single-CRTC demo --------- */
static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s){ (void)s; g_stop = 1; }

/* Track flip pending per process (single CRTC) */
static volatile int g_flip_pending = 0;
static void handle_page_flip(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data){
    (void)fd; (void)frame; (void)sec; (void)usec; (void)data;
    g_flip_pending = 0;
}

/* Pick preferred mode if available */
static int pick_mode(const drmModeConnector* c, drmModeModeInfo* out){
    if(!c || c->count_modes == 0) return 0;
    for(int i=0;i<c->count_modes;i++){
        if(c->modes[i].type & DRM_MODE_TYPE_PREFERRED){ *out = c->modes[i]; return 1; }
    }
    *out = c->modes[0];
    return 1;
}

/* Choose a compatible CRTC for this connector */
static int choose_crtc(int fd, const drmModeRes* res, const drmModeConnector* conn, u32* out_crtc){
    if(conn->encoder_id){
        drmModeEncoder* e = drmModeGetEncoder(fd, conn->encoder_id);
        if(e){
            if(e->crtc_id){ *out_crtc = e->crtc_id; drmModeFreeEncoder(e); return 1; }
            for(int i=0;i<res->count_crtcs;i++){
                if(e->possible_crtcs & (1<<i)){ *out_crtc = res->crtcs[i]; drmModeFreeEncoder(e); return 1; }
            }
            drmModeFreeEncoder(e);
        }
    }
    for(int i=0;i<conn->count_encoders;i++){
        drmModeEncoder* e = drmModeGetEncoder(fd, conn->encoders[i]);
        if(!e) continue;
        for(int j=0;j<res->count_crtcs;j++){
            if(e->possible_crtcs & (1<<j)){ *out_crtc = res->crtcs[j]; drmModeFreeEncoder(e); return 1; }
        }
        drmModeFreeEncoder(e);
    }
    return 0;
}

/* Open /dev/dri/cardX (scan), become master (best effort), choose connected connector + crtc + mode */
static int open_card_any(int prefer_conn, int* out_fd, u32* out_conn, u32* out_crtc, drmModeModeInfo* out_mode){
    char path[64];
    for(int idx=0; idx<16; idx++){
        snprintf(path, sizeof(path), "/dev/dri/card%d", idx);
        int fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
        if(fd < 0) continue;

        if(drmSetMaster(fd) != 0){
            LOGE("drmSetMaster failed on %s (errno=%d %s) — continuing; modeset may fail",
                 path, errno, strerror(errno));
        }

        drmModeRes* res = drmModeGetResources(fd);
        if(!res){ close(fd); continue; }

        u32 picked_conn = 0, picked_crtc = 0; drmModeModeInfo picked_mode = {0};
        for(int i=0;i<res->count_connectors;i++){
            drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
            if(!c) continue;
            if(c->connection == DRM_MODE_CONNECTED && c->count_modes > 0){
                if(prefer_conn && c->connector_id != (u32)prefer_conn){ drmModeFreeConnector(c); continue; }
                if(!pick_mode(c, &picked_mode)){ drmModeFreeConnector(c); continue; }
                if(!choose_crtc(fd, res, c, &picked_crtc)){ drmModeFreeConnector(c); continue; }
                picked_conn = c->connector_id;
                drmModeFreeConnector(c);
                break;
            }
            drmModeFreeConnector(c);
        }

        drmModeFreeResources(res);

        if(picked_conn && picked_crtc){
            *out_fd = fd; *out_conn = picked_conn; *out_crtc = picked_crtc; *out_mode = picked_mode;
            LOGE("picked %s conn=%u crtc=%u mode=%ux%u@%d", path, picked_conn, picked_crtc,
                 picked_mode.hdisplay, picked_mode.vdisplay, picked_mode.vrefresh);
            return 1;
        }

        close(fd);
    }
    return 0;
}

int linux_open_drm_pick(int prefer_conn, int* out_fd, u32* out_conn, u32* out_crtc, struct KmsMode* out_mode){
    struct sigaction sa; memset(&sa,0,sizeof(sa)); sa.sa_handler = on_sigint;
    sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL);

    drmModeModeInfo m = {0}; u32 conn = 0, crtc = 0;
    if(!open_card_any(prefer_conn, out_fd, &conn, &crtc, &m)){
        LOGE("No suitable DRM card/connector found. Use a free VT (Ctrl+Alt+F2) and run as root.");
        return 0;
    }
    out_mode->width = m.hdisplay; out_mode->height = m.vdisplay; out_mode->vrefresh = m.vrefresh;
    *out_conn = conn; *out_crtc = crtc;
    return 1;
}

/* Allocate GBM scanout BOs + FBs; export dma-fd for Vulkan import */
int linux_alloc_scanout_images(int drm_fd, u32 w, u32 h, u32 count, struct ScanoutImage* out){
    if(count > 4) count = 4;
    struct gbm_device* gbm = gbm_create_device(drm_fd);
    if(!gbm){ LOGE("gbm_create_device failed"); return 0; }

    for(u32 i=0;i<count;i++){
        struct gbm_bo* bo = gbm_bo_create(gbm, w, h, GBM_BO_FORMAT_XRGB8888,
                                          GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if(!bo){ LOGE("gbm_bo_create failed"); gbm_device_destroy(gbm); return 0; }

        out[i].width = w; out[i].height = h;
        out[i].stride = gbm_bo_get_stride(bo);
        out[i].modifier = gbm_bo_get_modifier ? gbm_bo_get_modifier(bo) : DRM_FORMAT_MOD_LINEAR;
        out[i].dma_fd = gbm_bo_get_fd(bo);
        if(out[i].dma_fd < 0){ LOGE("gbm_bo_get_fd failed"); gbm_bo_destroy(bo); gbm_device_destroy(gbm); return 0; }

        u32 handles[4] = { gbm_bo_get_handle(bo).u32, 0,0,0 };
        u32 pitches[4] = { out[i].stride, 0,0,0 };
        u32 offsets[4] = { 0,0,0,0 };

#ifdef DRM_MODE_FB_MODIFIERS
        u64 mods[4] = { out[i].modifier, 0,0,0 };
        if(drmModeAddFB2WithModifiers(drm_fd, w, h, DRM_FORMAT_XRGB8888,
                                      handles, pitches, offsets, mods,
                                      &out[i].fb_id, DRM_MODE_FB_MODIFIERS) != 0)
        {
            if(drmModeAddFB2(drm_fd, w, h, DRM_FORMAT_XRGB8888,
                             handles, pitches, offsets, &out[i].fb_id, 0) != 0)
            { LOGE("drmModeAddFB2 failed"); close(out[i].dma_fd); gbm_bo_destroy(bo); gbm_device_destroy(gbm); return 0; }
        }
#else
        if(drmModeAddFB2(drm_fd, w, h, DRM_FORMAT_XRGB8888,
                         handles, pitches, offsets, &out[i].fb_id, 0) != 0)
        { LOGE("drmModeAddFB2 failed"); close(out[i].dma_fd); gbm_bo_destroy(bo); gbm_device_destroy(gbm); return 0; }
#endif
        /* For brevity we don't keep bo*; process exit reclaims. */
    }

    gbm_device_destroy(gbm);
    return 1;
}

/* Proper legacy modeset with a specific mode */
static int modeset_with_mode(int fd, u32 crtc, u32 conn, const drmModeModeInfo* mode, u32 fb){
    int r = drmModeSetCrtc(fd, crtc, fb, 0, 0, &conn, 1, mode);
    if(r != 0) LOGE("drmModeSetCrtc failed: %s", strerror(errno));
    return r;
}

/* Queue a non-blocking page flip. Fails if a flip is still pending. */
int linux_queue_flip_nonblock(int drm_fd, u32 crtc, u32 fb_id, int in_fence_fd){
    if(g_flip_pending){ errno = EBUSY; return 0; }

    /* Ensure GPU finished before flip */
    if(in_fence_fd >= 0){
        struct pollfd pfd = { .fd = in_fence_fd, .events = POLLIN };
        (void)poll(&pfd, 1, -1);
        close(in_fence_fd);
    }

    if(drmModePageFlip(drm_fd, crtc, fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL) != 0){
        LOGE("pageflip failed: %s", strerror(errno));
        return 0;
    }
    g_flip_pending = 1;
    return 1;
}

/* Wait (up to timeout_ms, -1 = infinite) for the flip event to clear pending */
static int wait_flip_internal(int drm_fd, int timeout_ms){
    drmEventContext ev; memset(&ev,0,sizeof(ev));
    ev.version = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler = handle_page_flip;

    while(g_flip_pending && !g_stop){
        struct pollfd pfd = { .fd = drm_fd, .events = POLLIN };
        int r = poll(&pfd, 1, timeout_ms);
        if(r < 0){
            if(errno == EINTR) continue;
            LOGE("poll error: %s", strerror(errno));
            return 0;
        }
        if(r == 0){
            /* timeout; let caller decide to continue or not */
            return g_flip_pending ? 0 : 1;
        }
        if(pfd.revents & POLLIN){
            if(drmHandleEvent(drm_fd, &ev) != 0){
                LOGE("drmHandleEvent error");
                return 0;
            }
        }
    }
    return 1;
}

/* Expose a helper the app can call each frame */
int linux_pump_events(int drm_fd, int timeout_ms){
    return wait_flip_internal(drm_fd, timeout_ms);
}

void linux_free_scanout_images(int drm_fd, struct ScanoutImage* imgs, u32 count){
    for(u32 i=0;i<count;i++){
        if(imgs[i].fb_id) drmModeRmFB(drm_fd, imgs[i].fb_id);
        if(imgs[i].dma_fd >= 0) close(imgs[i].dma_fd);
    }
}

void linux_drop_master_if_owner(int drm_fd){
    if(drm_fd >= 0) (void)drmDropMaster(drm_fd);
}

/* Extra helper for main: perform the initial modeset with a given mode+fb */
int linux_initial_modeset(int drm_fd, u32 crtc, u32 conn, u32 fb_id, u32 w, u32 h, u32 vrefresh){
    /* We need the exact drmModeModeInfo we picked earlier. Re-query connector to get it. */
    drmModeRes* res = drmModeGetResources(drm_fd); if(!res) return 0;
    drmModeConnector* c = NULL; drmModeModeInfo mode = {0};
    for(int i=0;i<res->count_connectors;i++){
        drmModeConnector* tmp = drmModeGetConnector(drm_fd, res->connectors[i]);
        if(tmp && tmp->connection == DRM_MODE_CONNECTED){
            if(tmp->connector_id == conn){
                if(!pick_mode(tmp, &mode)){ drmModeFreeConnector(tmp); break; }
                c = tmp; break;
            }
        }
        if(tmp) drmModeFreeConnector(tmp);
    }
    int ok = 0;
    if(c){ ok = (modeset_with_mode(drm_fd, crtc, conn, &mode, fb_id) == 0); drmModeFreeConnector(c); }
    drmModeFreeResources(res);
    return ok;
}
