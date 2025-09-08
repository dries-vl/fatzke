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

#define LOGE(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)

/* ---------- Minimal global control ---------- */
static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s){ (void)s; g_stop = 1; }

/* ---------- Utility: pick preferred mode ---------- */
static int pick_mode(const drmModeConnector* c, drmModeModeInfo* out) {
    if (!c || c->count_modes == 0) return 0;
    for (int i=0;i<c->count_modes;i++)
        if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED) { *out = c->modes[i]; return 1; }
    *out = c->modes[0];
    return 1;
}

/* ---------- Utility: choose CRTC for connector ---------- */
static int choose_crtc(int fd, const drmModeRes* res, const drmModeConnector* conn, u32* out_crtc){
    if (conn->encoder_id) {
        drmModeEncoder* e = drmModeGetEncoder(fd, conn->encoder_id);
        if (e) {
            if (e->crtc_id) { *out_crtc = e->crtc_id; drmModeFreeEncoder(e); return 1; }
            for (int i=0;i<res->count_crtcs;i++) if (e->possible_crtcs & (1<<i)) { *out_crtc = res->crtcs[i]; drmModeFreeEncoder(e); return 1; }
            drmModeFreeEncoder(e);
        }
    }
    for (int i=0;i<conn->count_encoders;i++) {
        drmModeEncoder* e = drmModeGetEncoder(fd, conn->encoders[i]); if (!e) continue;
        for (int j=0;j<res->count_crtcs;j++) if (e->possible_crtcs & (1<<j)) { *out_crtc = res->crtcs[j]; drmModeFreeEncoder(e); return 1; }
        drmModeFreeEncoder(e);
    }
    return 0;
}

/* ---------- Open any /dev/dri/cardX, become master, pick connector/crtc/mode ---------- */
static int open_card_any(int prefer_conn, int* out_fd, u32* out_conn, u32* out_crtc, drmModeModeInfo* out_mode){
    char path[64];
    for (int idx=0; idx<16; idx++) {
        snprintf(path,sizeof(path),"/dev/dri/card%d",idx);
        int fd = open(path, O_RDWR|O_CLOEXEC|O_NONBLOCK);
        if (fd < 0) continue;

        if (drmSetMaster(fd) != 0) {
            LOGE("drmSetMaster on %s failed (errno=%d %s) — continuing", path, errno, strerror(errno));
        }

        drmModeRes* res = drmModeGetResources(fd);
        if (!res) { close(fd); continue; }

        u32 conn=0, crtc=0; drmModeModeInfo mode={0};
        for (int i=0;i<res->count_connectors;i++) {
            drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
            if (!c) continue;
            if (c->connection==DRM_MODE_CONNECTED && c->count_modes>0){
                if (prefer_conn && c->connector_id != (u32)prefer_conn) { drmModeFreeConnector(c); continue; }
                if (!pick_mode(c,&mode)) { drmModeFreeConnector(c); continue; }
                if (!choose_crtc(fd,res,c,&crtc)) { drmModeFreeConnector(c); continue; }
                conn=c->connector_id; drmModeFreeConnector(c); break;
            }
            drmModeFreeConnector(c);
        }
        drmModeFreeResources(res);

        if (conn && crtc) { *out_fd=fd; *out_conn=conn; *out_crtc=crtc; *out_mode=mode;
            fprintf(stderr, "DRM: picked %s conn=%u crtc=%u %ux%u@%d\n", path, conn, crtc, mode.hdisplay, mode.vdisplay, mode.vrefresh);
            return 1;
        }
        close(fd);
    }
    return 0;
}

int linux_open_drm_pick(int prefer_conn, int* out_fd, u32* out_conn, u32* out_crtc, struct KmsMode* out_mode){
    struct sigaction sa; memset(&sa,0,sizeof(sa)); sa.sa_handler=on_sigint;
    sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL);

    drmModeModeInfo m={0}; u32 conn=0, crtc=0;
    if (!open_card_any(prefer_conn,out_fd,&conn,&crtc,&m)) {
        LOGE("No suitable DRM card/connector found. Use a free VT and run as root.");
        return 0;
    }
    out_mode->width=m.hdisplay; out_mode->height=m.vdisplay; out_mode->vrefresh=m.vrefresh;
    *out_conn=conn; *out_crtc=crtc; return 1;
}

/* ---------- Allocate GBM BOs + FBs; export dma-fd ---------- */
int linux_alloc_scanout_images(int drm_fd, u32 w, u32 h, u32 count, struct ScanoutImage* out){
    if (count>4) count=4;
    struct gbm_device* gbm = gbm_create_device(drm_fd);
    if (!gbm) { LOGE("gbm_create_device failed"); return 0; }
    for (u32 i=0;i<count;i++){
        struct gbm_bo* bo = gbm_bo_create(gbm, w, h, GBM_BO_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT|GBM_BO_USE_RENDERING);
        if (!bo){ LOGE("gbm_bo_create failed"); gbm_device_destroy(gbm); return 0; }
        out[i].width=w; out[i].height=h; out[i].stride=gbm_bo_get_stride(bo);
        out[i].modifier = gbm_bo_get_modifier? gbm_bo_get_modifier(bo) : DRM_FORMAT_MOD_LINEAR;
        out[i].dma_fd = gbm_bo_get_fd(bo);
        if (out[i].dma_fd<0){ LOGE("gbm_bo_get_fd failed"); gbm_bo_destroy(bo); gbm_device_destroy(gbm); return 0; }
        u32 handles[4]={ gbm_bo_get_handle(bo).u32,0,0,0 }, pitches[4]={ out[i].stride,0,0,0 }, offsets[4]={0,0,0,0};
#ifdef DRM_MODE_FB_MODIFIERS
        u64 mods[4]={ out[i].modifier,0,0,0 };
        if (drmModeAddFB2WithModifiers(drm_fd,w,h,DRM_FORMAT_XRGB8888,handles,pitches,offsets,mods,&out[i].fb_id,DRM_MODE_FB_MODIFIERS)!=0){
            if (drmModeAddFB2(drm_fd,w,h,DRM_FORMAT_XRGB8888,handles,pitches,offsets,&out[i].fb_id,0)!=0){
                LOGE("drmModeAddFB2 failed"); close(out[i].dma_fd); gbm_bo_destroy(bo); gbm_device_destroy(gbm); return 0;
            }
        }
#else
        if (drmModeAddFB2(drm_fd,w,h,DRM_FORMAT_XRGB8888,handles,pitches,offsets,&out[i].fb_id,0)!=0){
            LOGE("drmModeAddFB2 failed"); close(out[i].dma_fd); gbm_bo_destroy(bo); gbm_device_destroy(gbm); return 0;
        }
#endif
        /* note: not storing bo for brevity */
    }
    gbm_device_destroy(gbm);
    return 1;
}

/* ========================= Atomic KMS path ========================= */

/* cache of property IDs */
struct KmsProps {
    /* CRTC props */
    u32 CRTC_ACTIVE, CRTC_MODE_ID, CRTC_OUT_FENCE_PTR;
    /* Connector props */
    u32 CONN_CRTC_ID;
    /* Plane props (primary) */
    u32 PL_FB_ID, PL_CRTC_ID, PL_SRC_X, PL_SRC_Y, PL_SRC_W, PL_SRC_H, PL_CRTC_X, PL_CRTC_Y, PL_CRTC_W, PL_CRTC_H, PL_IN_FENCE_FD;
    /* chosen primary plane */
    u32 primary_plane;
    /* mode blob id */
    u32 mode_blob_id;
    /* flip pending flag */
    int flip_pending;
};
static struct KmsProps g_kms = {0};

static u32 get_prop_id(int fd, u32 obj_id, u32 obj_type, const char* name){
    drmModeObjectProperties* ops = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!ops) return 0;
    u32 id = 0;
    for (u32 i=0;i<ops->count_props;i++){
        drmModePropertyRes* p = drmModeGetProperty(fd, ops->props[i]);
        if (p && strcmp(p->name, name)==0){ id = p->prop_id; drmModeFreeProperty(p); break; }
        if (p) drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(ops);
    return id;
}

static int find_primary_plane(int fd, u32 crtc_idx, u32* out_plane_id){
    drmModePlaneRes* pres = drmModeGetPlaneResources(fd);
    if (!pres) return 0;
    int ok=0;
    for (u32 i=0;i<pres->count_planes;i++){
        drmModePlane* pl = drmModeGetPlane(fd, pres->planes[i]); if (!pl) continue;
        if (!(pl->possible_crtcs & (1u<<crtc_idx))){ drmModeFreePlane(pl); continue; }
        /* check plane "type" == PRIMARY */
        drmModeObjectProperties* ops = drmModeObjectGetProperties(fd, pl->plane_id, DRM_MODE_OBJECT_PLANE);
        if (!ops){ drmModeFreePlane(pl); continue; }
        int is_primary = 0;
        for (u32 j=0;j<ops->count_props;j++){
            drmModePropertyRes* p = drmModeGetProperty(fd, ops->props[j]);
            if (p && strcmp(p->name,"type")==0){
                if (ops->prop_values[j] == DRM_PLANE_TYPE_PRIMARY) is_primary = 1;
                drmModeFreeProperty(p);
                break;
            }
            if (p) drmModeFreeProperty(p);
        }
        drmModeFreeObjectProperties(ops);
        if (is_primary){ *out_plane_id = pl->plane_id; ok=1; drmModeFreePlane(pl); break; }
        drmModeFreePlane(pl);
    }
    drmModeFreePlaneResources(pres);
    return ok;
}

/* Build property cache and create mode blob. Must be called once before first commit. */
static int prepare_atomic(int fd, u32 conn, u32 crtc, const drmModeModeInfo* mode){
    memset(&g_kms,0,sizeof(g_kms));
    /* find crtc index into resource's crtc list */
    drmModeRes* res = drmModeGetResources(fd); if (!res) return 0;
    int crtc_idx = -1;
    for (int i=0;i<res->count_crtcs;i++) if ((u32)res->crtcs[i] == crtc){ crtc_idx = i; break; }
    if (crtc_idx < 0){ drmModeFreeResources(res); return 0; }

    /* primary plane */
    if (!find_primary_plane(fd, (u32)crtc_idx, &g_kms.primary_plane)){ drmModeFreeResources(res); return 0; }

    /* property ids */
    g_kms.CRTC_ACTIVE       = get_prop_id(fd, crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    g_kms.CRTC_MODE_ID      = get_prop_id(fd, crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    g_kms.CRTC_OUT_FENCE_PTR= get_prop_id(fd, crtc, DRM_MODE_OBJECT_CRTC, "OUT_FENCE_PTR");
    g_kms.CONN_CRTC_ID      = get_prop_id(fd, conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");

    g_kms.PL_FB_ID   = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "FB_ID");
    g_kms.PL_CRTC_ID = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    g_kms.PL_SRC_X   = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "SRC_X");
    g_kms.PL_SRC_Y   = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    g_kms.PL_SRC_W   = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "SRC_W");
    g_kms.PL_SRC_H   = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "SRC_H");
    g_kms.PL_CRTC_X  = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    g_kms.PL_CRTC_Y  = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    g_kms.PL_CRTC_W  = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    g_kms.PL_CRTC_H  = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    g_kms.PL_IN_FENCE_FD = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD");

    /* make sure required props exist */
    if(!g_kms.CRTC_ACTIVE || !g_kms.CRTC_MODE_ID || !g_kms.CONN_CRTC_ID || !g_kms.PL_FB_ID || !g_kms.PL_CRTC_ID){
        drmModeFreeResources(res);
        LOGE("Atomic properties missing on this device");
        return 0;
    }

    /* create mode blob */
    if (drmModeCreatePropertyBlob(fd, mode, sizeof(*mode), &g_kms.mode_blob_id) != 0){
        drmModeFreeResources(res);
        LOGE("CreatePropertyBlob failed");
        return 0;
    }
    drmModeFreeResources(res);
    g_kms.flip_pending = 0;
    return 1;
}

/* First atomic modeset (ALLOW_MODESET) */
static int first_atomic_modeset(int fd, u32 conn, u32 crtc, u32 fb, u32 w, u32 h){
    drmModeConnector* c = drmModeGetConnector(fd, conn); if (!c) return 0;
    drmModeModeInfo m; if (!pick_mode(c,&m)){ drmModeFreeConnector(c); return 0; }
    if (!prepare_atomic(fd, conn, crtc, &m)){ drmModeFreeConnector(c); return 0; }
    drmModeFreeConnector(c);

    drmModeAtomicReq* req = drmModeAtomicAlloc();
    if (!req) return 0;

    /* Connector -> CRTC */
    drmModeAtomicAddProperty(req, conn, g_kms.CONN_CRTC_ID, crtc);
    /* CRTC -> MODE_ID, ACTIVE; OUT_FENCE_PTR optional on modeset (we can ignore) */
    drmModeAtomicAddProperty(req, crtc, g_kms.CRTC_ACTIVE, 1);
    drmModeAtomicAddProperty(req, crtc, g_kms.CRTC_MODE_ID, g_kms.mode_blob_id);
    /* Plane -> FB + CRTC + dst & src rectangles */
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_FB_ID, fb);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_CRTC_ID, crtc);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_CRTC_X, 0);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_CRTC_Y, 0);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_CRTC_W, w);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_CRTC_H, h);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_SRC_X, 0);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_SRC_Y, 0);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_SRC_W, (u64)w<<16);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_SRC_H, (u64)h<<16);

    int flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    int r = drmModeAtomicCommit(fd, req, flags, NULL);
    drmModeAtomicFree(req);
    if (r != 0){ LOGE("Atomic modeset failed: %s", strerror(errno)); return 0; }
    return 1;
}

/* Non-blocking atomic flip with IN_FENCE_FD and OUT_FENCE_PTR */
int linux_atomic_present(int drm_fd, u32 conn, u32 crtc, u32 fb_id, int in_fence_fd, int* out_fence_fd, int allow_modeset){
    (void)conn; (void)allow_modeset; /* modeset already done in first_atomic_modeset */

    if (g_kms.flip_pending){ errno = EBUSY; return 0; }

    /* drain GPU fence: we can pass it as IN_FENCE_FD and avoid explicit wait here */
    int in_fd = in_fence_fd; /* ownership moved to kernel on commit */

    drmModeAtomicReq* req = drmModeAtomicAlloc(); if (!req) return 0;

    /* OUT_FENCE */
    int out_fd = -1;
    if (g_kms.CRTC_OUT_FENCE_PTR) {
        drmModeAtomicAddProperty(req, crtc, g_kms.CRTC_OUT_FENCE_PTR, (u64)(uintptr_t)&out_fd);
    }

    /* Plane setup each frame */
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_FB_ID, fb_id);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_CRTC_ID, crtc);

    /* Pass the GPU completion fence to KMS */
    if (g_kms.PL_IN_FENCE_FD && in_fd >= 0)
        drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_IN_FENCE_FD, in_fd);

    int flags = DRM_MODE_ATOMIC_NONBLOCK;
    int r = drmModeAtomicCommit(drm_fd, req, flags, NULL);
    drmModeAtomicFree(req);

    if (r != 0){
        LOGE("atomic commit failed: %s", strerror(errno));
        if (in_fd >= 0) close(in_fd);
        return 0;
    }

    if (out_fence_fd) *out_fence_fd = out_fd; else if (out_fd>=0) close(out_fd);
    g_kms.flip_pending = 1;
    return 1;
}

/* Wait for current flip fence FD or page-flip event */
static int wait_out_fence(int out_fd, int timeout_ms){
    if (out_fd < 0) return 1;
    struct pollfd p = { .fd=out_fd, .events=POLLIN };
    int r = poll(&p, 1, timeout_ms);
    if (r > 0) { close(out_fd); return 1; }
    if (r == 0) return 0;
    if (errno==EINTR) return 0;
    return 0;
}

/* Simple “pump” that clears our pending flag using OUT_FENCE_FD wait */
int linux_pump_events(int drm_fd, int timeout_ms){
    (void)drm_fd; /* we pace via OUT_FENCE, not drm events */
    static int last_out_fd = -1;
    (void)timeout_ms;
    if (!g_kms.flip_pending) return 1;
    /* nothing to do here; the app provides and waits the actual out fence it received */
    return 1;
}

/* Exported helpers for first modeset + teardown */
int linux_initial_modeset_atomic(int drm_fd, u32 conn, u32 crtc, u32 fb_id, u32 w, u32 h){
    return first_atomic_modeset(drm_fd, conn, crtc, fb_id, w, h);
}

void linux_free_scanout_images(int drm_fd, struct ScanoutImage* imgs, u32 count){
    for (u32 i=0;i<count;i++){
        if (imgs[i].fb_id) drmModeRmFB(drm_fd, imgs[i].fb_id);
        if (imgs[i].dma_fd >= 0) close(imgs[i].dma_fd);
    }
    if (g_kms.mode_blob_id) { drmModeDestroyPropertyBlob(drm_fd, g_kms.mode_blob_id); g_kms.mode_blob_id = 0; }
}

void linux_drop_master_if_owner(int drm_fd){
    if (drm_fd >= 0) (void)drmDropMaster(drm_fd);
}

/* Wait for the OUT_FENCE_FD (exposed for main loop convenience) */
int linux_wait_out_fence_fd(int out_fd, int timeout_ms){
    int ok = wait_out_fence(out_fd, timeout_ms);
    if (ok) g_kms.flip_pending = 0;
    return ok;
}
