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
#define LOGI(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)

/* ==== global stop flag ==== */
static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s){ (void)s; g_stop = 1; }

/* ==== helpers ==== */
static int pick_mode(const drmModeConnector* c, drmModeModeInfo* out) {
    if (!c || c->count_modes == 0) return 0;
    for (int i=0;i<c->count_modes;i++)
        if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED) { *out = c->modes[i]; return 1; }
    *out = c->modes[0]; return 1;
}

static int choose_crtc_for_conn(int fd, const drmModeRes* res, const drmModeConnector* conn, uint32_t* out_crtc, int* out_crtc_idx){
    if (conn->encoder_id) {
        drmModeEncoder* e = drmModeGetEncoder(fd, conn->encoder_id);
        if (e) {
            if (e->crtc_id) {
                *out_crtc = e->crtc_id;
                for (int i=0;i<res->count_crtcs;i++) if (res->crtcs[i]==(int)*out_crtc){ *out_crtc_idx=i; break; }
                drmModeFreeEncoder(e);
                return 1;
            }
            for (int i=0;i<res->count_crtcs;i++) {
                if (e->possible_crtcs & (1<<i)) { *out_crtc = res->crtcs[i]; *out_crtc_idx = i; drmModeFreeEncoder(e); return 1; }
            }
            drmModeFreeEncoder(e);
        }
    }
    for (int i=0;i<conn->count_encoders;i++) {
        drmModeEncoder* e = drmModeGetEncoder(fd, conn->encoders[i]); if (!e) continue;
        for (int j=0;j<res->count_crtcs;j++) if (e->possible_crtcs & (1<<j)) { *out_crtc = res->crtcs[j]; *out_crtc_idx=j; drmModeFreeEncoder(e); return 1; }
        drmModeFreeEncoder(e);
    }
    return 0;
}

static int open_card_any(int prefer_conn, int* out_fd, uint32_t* out_conn, uint32_t* out_crtc, int* out_crtc_idx, drmModeModeInfo* out_mode){
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

        uint32_t conn=0, crtc=0; int crtc_idx=-1; drmModeModeInfo mode={0};

        for (int i=0;i<res->count_connectors;i++) {
            drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
            if (!c) continue;
            if (c->connection==DRM_MODE_CONNECTED && c->count_modes>0){
                if (prefer_conn && c->connector_id != (uint32_t)prefer_conn) { drmModeFreeConnector(c); continue; }
                if (!pick_mode(c,&mode)) { drmModeFreeConnector(c); continue; }
                if (!choose_crtc_for_conn(fd,res,c,&crtc,&crtc_idx)) { drmModeFreeConnector(c); continue; }
                conn=c->connector_id; drmModeFreeConnector(c); break;
            }
            drmModeFreeConnector(c);
        }

        drmModeFreeResources(res);

        if (conn && crtc) {
            *out_fd=fd; *out_conn=conn; *out_crtc=crtc; *out_crtc_idx=crtc_idx; *out_mode=mode;
            LOGI("DRM: picked %s conn=%u crtc=%u (%d) %ux%u@%d", path, conn, crtc, crtc_idx, mode.hdisplay, mode.vdisplay, mode.vrefresh);
            return 1;
        }
        close(fd);
    }
    return 0;
}

/* ==== public: open & pick ==== */
int linux_open_drm_pick(int prefer_conn, int* out_fd, uint32_t* out_conn, uint32_t* out_crtc, struct KmsMode* out_mode){
    struct sigaction sa; memset(&sa,0,sizeof(sa)); sa.sa_handler=on_sigint;
    sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL);

    drmModeModeInfo m={0}; uint32_t conn=0, crtc=0; int crtc_idx=-1;
    if (!open_card_any(prefer_conn,out_fd,&conn,&crtc,&crtc_idx,&m)) {
        LOGE("No suitable DRM card/connector found. Use a free VT and run as root.");
        return 0;
    }
    out_mode->width=m.hdisplay; out_mode->height=m.vdisplay; out_mode->vrefresh=m.vrefresh;
    *out_conn=conn; *out_crtc=crtc;
    return 1;
}

/* ==== GBM alloc ==== */
int linux_alloc_scanout_images(int drm_fd, uint32_t w, uint32_t h, uint32_t count, struct ScanoutImage* out){
    if (count>4) count=4;
    struct gbm_device* gbm = gbm_create_device(drm_fd);
    if (!gbm) { LOGE("gbm_create_device failed"); return 0; }
    for (uint32_t i=0;i<count;i++){
        struct gbm_bo* bo = gbm_bo_create(gbm, w, h, GBM_BO_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT|GBM_BO_USE_RENDERING);
        if (!bo){ LOGE("gbm_bo_create failed"); gbm_device_destroy(gbm); return 0; }
        out[i].width=w; out[i].height=h; out[i].stride=gbm_bo_get_stride(bo);
        out[i].modifier = gbm_bo_get_modifier? gbm_bo_get_modifier(bo):DRM_FORMAT_MOD_LINEAR;
        out[i].dma_fd = gbm_bo_get_fd(bo);
        if (out[i].dma_fd<0){ LOGE("gbm_bo_get_fd failed"); gbm_bo_destroy(bo); gbm_device_destroy(gbm); return 0; }
        uint32_t handles[4]={ gbm_bo_get_handle(bo).u32,0,0,0 }, pitches[4]={ out[i].stride,0,0,0 }, offsets[4]={0,0,0,0};
#ifdef DRM_MODE_FB_MODIFIERS
        uint64_t mods[4]={ out[i].modifier,0,0,0 };
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
        /* (We don't store the bo in this demo; process exit frees it) */
    }
    gbm_device_destroy(gbm);
    return 1;
}

/* ==== Atomic property cache ==== */
struct KmsProps {
    /* flags */
    int atomic_ok;

    /* resource selection */
    uint32_t conn, crtc, primary_plane;
    int      crtc_idx;

    /* prop ids */
    uint32_t CRTC_ACTIVE, CRTC_MODE_ID, CRTC_OUT_FENCE_PTR;
    uint32_t CONN_CRTC_ID;
    uint32_t PL_FB_ID, PL_CRTC_ID, PL_SRC_X, PL_SRC_Y, PL_SRC_W, PL_SRC_H, PL_CRTC_X, PL_CRTC_Y, PL_CRTC_W, PL_CRTC_H, PL_IN_FENCE_FD;

    uint32_t mode_blob_id;
    int      flip_pending;
};
static struct KmsProps g_kms = {0};

static uint32_t get_prop_id(int fd, uint32_t obj_id, uint32_t obj_type, const char* name){
    drmModeObjectProperties* ops = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!ops) return 0;
    uint32_t id = 0;
    for (uint32_t i=0;i<ops->count_props;i++){
        drmModePropertyRes* p = drmModeGetProperty(fd, ops->props[i]);
        if (p) {
            if (strcmp(p->name, name)==0) id = p->prop_id;
            drmModeFreeProperty(p);
            if (id) break;
        }
    }
    drmModeFreeObjectProperties(ops);
    return id;
}

static int enable_client_caps(int fd){
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0){
        LOGE("DRM_CLIENT_CAP_UNIVERSAL_PLANES not supported");
        return 0;
    }
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0){
        LOGE("DRM_CLIENT_CAP_ATOMIC not supported");
        return 0;
    }
    return 1;
}

static int pick_primary_plane(int fd, int crtc_idx, uint32_t* out_plane){
    drmModePlaneRes* pres = drmModeGetPlaneResources(fd); if (!pres){ LOGE("GetPlaneResources failed"); return 0; }
    int ok=0;
    for (uint32_t i=0;i<pres->count_planes;i++){
        drmModePlane* pl = drmModeGetPlane(fd, pres->planes[i]); if (!pl) continue;
        if (!(pl->possible_crtcs & (1u<<crtc_idx))){ drmModeFreePlane(pl); continue; }
        /* find "type" property == PRIMARY */
        drmModeObjectProperties* ops = drmModeObjectGetProperties(fd, pl->plane_id, DRM_MODE_OBJECT_PLANE);
        if (!ops){ drmModeFreePlane(pl); continue; }
        int is_primary=0;
        for (uint32_t j=0;j<ops->count_props;j++){
            drmModePropertyRes* p = drmModeGetProperty(fd, ops->props[j]);
            if (p && strcmp(p->name,"type")==0){
                if (ops->prop_values[j] == DRM_PLANE_TYPE_PRIMARY) is_primary=1;
                drmModeFreeProperty(p); break;
            }
            if (p) drmModeFreeProperty(p);
        }
        drmModeFreeObjectProperties(ops);
        if (is_primary){ *out_plane = pl->plane_id; ok=1; drmModeFreePlane(pl); break; }
        drmModeFreePlane(pl);
    }
    drmModeFreePlaneResources(pres);
    if (!ok) LOGE("No PRIMARY plane matches CRTC idx=%d", crtc_idx);
    return ok;
}

/* Build cache + MODE_ID; returns 1 if atomic supported and prepared */
static int prepare_atomic_pipeline(int fd, uint32_t conn, uint32_t crtc, int crtc_idx, const drmModeModeInfo* mode){
    memset(&g_kms,0,sizeof(g_kms));
    g_kms.conn=conn; g_kms.crtc=crtc; g_kms.crtc_idx=crtc_idx;

    if (!enable_client_caps(fd)){
        g_kms.atomic_ok = 0; return 0;
    }
    if (!pick_primary_plane(fd, crtc_idx, &g_kms.primary_plane)){
        g_kms.atomic_ok = 0; return 0;
    }

    /* prop IDs */
    g_kms.CRTC_ACTIVE        = get_prop_id(fd, crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    g_kms.CRTC_MODE_ID       = get_prop_id(fd, crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    g_kms.CRTC_OUT_FENCE_PTR = get_prop_id(fd, crtc, DRM_MODE_OBJECT_CRTC, "OUT_FENCE_PTR");
    g_kms.CONN_CRTC_ID       = get_prop_id(fd, conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");

    g_kms.PL_FB_ID    = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "FB_ID");
    g_kms.PL_CRTC_ID  = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    g_kms.PL_SRC_X    = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "SRC_X");
    g_kms.PL_SRC_Y    = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    g_kms.PL_SRC_W    = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "SRC_W");
    g_kms.PL_SRC_H    = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "SRC_H");
    g_kms.PL_CRTC_X   = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    g_kms.PL_CRTC_Y   = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    g_kms.PL_CRTC_W   = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    g_kms.PL_CRTC_H   = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    g_kms.PL_IN_FENCE_FD = get_prop_id(fd, g_kms.primary_plane, DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD");

    if (!g_kms.CRTC_ACTIVE || !g_kms.CRTC_MODE_ID || !g_kms.CONN_CRTC_ID || !g_kms.PL_FB_ID || !g_kms.PL_CRTC_ID){
        LOGE("Required atomic properties missing on this device");
        g_kms.atomic_ok = 0; return 0;
    }

    if (drmModeCreatePropertyBlob(fd, mode, sizeof(*mode), &g_kms.mode_blob_id) != 0){
        LOGE("CreatePropertyBlob failed: %s", strerror(errno));
        g_kms.atomic_ok = 0; return 0;
    }
    g_kms.atomic_ok = 1; g_kms.flip_pending = 0;
    return 1;
}

/* First atomic commit: set CRTC+MODE, bind connector, set plane */
static int atomic_modeset_commit(int fd, uint32_t fb_id, uint32_t w, uint32_t h){
    drmModeAtomicReq* req = drmModeAtomicAlloc(); if (!req) return 0;

    drmModeAtomicAddProperty(req, g_kms.conn, g_kms.CONN_CRTC_ID, g_kms.crtc);

    drmModeAtomicAddProperty(req, g_kms.crtc, g_kms.CRTC_ACTIVE, 1);
    drmModeAtomicAddProperty(req, g_kms.crtc, g_kms.CRTC_MODE_ID, g_kms.mode_blob_id);

    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_FB_ID, fb_id);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_CRTC_ID, g_kms.crtc);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_CRTC_X, 0);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_CRTC_Y, 0);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_CRTC_W, w);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_CRTC_H, h);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_SRC_X, 0);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_SRC_Y, 0);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_SRC_W, (uint64_t)w<<16);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_SRC_H, (uint64_t)h<<16);

    int flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    int r = drmModeAtomicCommit(fd, req, flags, NULL);
    drmModeAtomicFree(req);

    if (r != 0) LOGE("Atomic modeset commit failed: %s", strerror(errno));
    return r==0;
}

/* Public: initial atomic modeset (with cap enabling, plane pick, blob create) */
int linux_initial_modeset_atomic(int drm_fd, uint32_t conn, uint32_t crtc, uint32_t fb_id, uint32_t w, uint32_t h){
    /* Re-query connector to get the actual mode blob content */
    drmModeRes* res = drmModeGetResources(drm_fd); if (!res){ LOGE("GetResources failed"); return 0; }
    drmModeConnector* c = NULL; drmModeModeInfo mode={0}; int crtc_idx=-1;
    for (int i=0;i<res->count_connectors;i++){
        drmModeConnector* tmp = drmModeGetConnector(drm_fd, res->connectors[i]);
        if (tmp && tmp->connector_id==conn){
            if (!pick_mode(tmp,&mode)){ drmModeFreeConnector(tmp); break; }
            c = tmp; break;
        }
        if (tmp) drmModeFreeConnector(tmp);
    }
    for (int i=0;i<res->count_crtcs;i++) if (res->crtcs[i]==(int)crtc){ crtc_idx=i; break; }
    drmModeFreeResources(res);
    if (!c){ LOGE("Connector %u not found on re-query", conn); return 0; }
    drmModeFreeConnector(c);
    if (crtc_idx<0){ LOGE("CRTC idx not found"); return 0; }

    if (!prepare_atomic_pipeline(drm_fd, conn, crtc, crtc_idx, &mode)) {
        LOGE("Atomic pipeline not prepared (caps or props missing)");
        return 0;
    }
    if (!atomic_modeset_commit(drm_fd, fb_id, w, h)) return 0;
    return 1;
}

/* Atomic present with IN_FENCE_FD and optional OUT_FENCE_FD (lowest latency) */
int linux_atomic_present(int drm_fd, uint32_t conn, uint32_t crtc, uint32_t fb_id, int in_fence_fd, int* out_fence_fd, int allow_modeset){
    (void)conn; (void)allow_modeset;
    if (!g_kms.atomic_ok){ errno = ENOTSUP; return 0; }
    if (g_kms.flip_pending){ errno = EBUSY; return 0; }

    drmModeAtomicReq* req = drmModeAtomicAlloc(); if (!req) return 0;

    int out_fd = -1;
    if (g_kms.CRTC_OUT_FENCE_PTR) {
        drmModeAtomicAddProperty(req, g_kms.crtc, g_kms.CRTC_OUT_FENCE_PTR, (uint64_t)(uintptr_t)&out_fd);
    }

    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_FB_ID, fb_id);
    drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_CRTC_ID, g_kms.crtc);

    if (g_kms.PL_IN_FENCE_FD && in_fence_fd>=0) {
        drmModeAtomicAddProperty(req, g_kms.primary_plane, g_kms.PL_IN_FENCE_FD, in_fence_fd);
        /* kernel takes ownership of the fd on success */
    }

    int flags = DRM_MODE_ATOMIC_NONBLOCK;
    int r = drmModeAtomicCommit(drm_fd, req, flags, NULL);
    drmModeAtomicFree(req);

    if (r != 0){
        LOGE("atomic commit failed: %s", strerror(errno));
        if (in_fence_fd>=0) close(in_fence_fd);
        return 0;
    }

    if (out_fence_fd) *out_fence_fd = out_fd; else if (out_fd>=0) close(out_fd);
    g_kms.flip_pending = 1;
    return 1;
}

/* Wait for OUT_FENCE_FD (preferred for precise pacing) */
int linux_wait_out_fence_fd(int out_fd, int timeout_ms){
    if (out_fd < 0){ g_kms.flip_pending = 0; return 1; }
    struct pollfd p = { .fd=out_fd, .events=POLLIN };
    int r = poll(&p,1,timeout_ms);
    if (r > 0) { close(out_fd); g_kms.flip_pending = 0; return 1; }
    if (r == 0) return 0;
    if (errno==EINTR) return 0;
    return 0;
}

/* Legacy fallback (only used if atomic totally unavailable) */
static int legacy_modeset(int fd, uint32_t crtc, uint32_t conn, uint32_t fb){
    drmModeRes* res=drmModeGetResources(fd); if(!res) return -1;
    drmModeConnector* c=NULL;
    for (int i=0;i<res->count_connectors;i++){
        drmModeConnector* tmp=drmModeGetConnector(fd,res->connectors[i]);
        if (tmp && tmp->connector_id==conn){ c=tmp; break; }
        if (tmp) drmModeFreeConnector(tmp);
    }
    int r=-1; if (c) r=drmModeSetCrtc(fd,crtc,fb,0,0,&conn,1,&c->modes[0]);
    if (c) drmModeFreeConnector(c); drmModeFreeResources(res); return r;
}

/* compatibility pump — not used with OUT_FENCE pacing, but kept for API */
int linux_pump_events(int drm_fd, int timeout_ms){ (void)drm_fd; (void)timeout_ms; return 1; }

/* cleanup */
void linux_free_scanout_images(int drm_fd, struct ScanoutImage* imgs, uint32_t count){
    for (uint32_t i=0;i<count;i++){
        if (imgs[i].fb_id) drmModeRmFB(drm_fd, imgs[i].fb_id);
        if (imgs[i].dma_fd >= 0) close(imgs[i].dma_fd);
    }
    if (g_kms.mode_blob_id) { drmModeDestroyPropertyBlob(drm_fd, g_kms.mode_blob_id); g_kms.mode_blob_id=0; }
}
void linux_drop_master_if_owner(int drm_fd){ if (drm_fd>=0) (void)drmDropMaster(drm_fd); }

/* expose legacy for main fallback */
int linux_initial_modeset(int drm_fd, uint32_t crtc, uint32_t conn, uint32_t fb){
    return legacy_modeset(drm_fd,crtc,conn,fb)==0;
}
