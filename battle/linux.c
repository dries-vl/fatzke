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

static volatile sig_atomic_t g_stop=0;
static void on_sigint(int s){ (void)s; g_stop=1; }

static int pick_mode(const drmModeConnector* c, drmModeModeInfo* out){
    if(!c||c->count_modes==0) return 0;
    for(int i=0;i<c->count_modes;i++) if(c->modes[i].type & DRM_MODE_TYPE_PREFERRED){ *out=c->modes[i]; return 1; }
    *out=c->modes[0]; return 1;
}
static int choose_crtc(int fd, const drmModeRes* res, const drmModeConnector* conn, u32* out_crtc){
    if(conn->encoder_id){
        drmModeEncoder* e=drmModeGetEncoder(fd,conn->encoder_id);
        if(e){
            if(e->crtc_id){ *out_crtc=e->crtc_id; drmModeFreeEncoder(e); return 1; }
            for(int i=0;i<res->count_crtcs;i++) if(e->possible_crtcs & (1<<i)){ *out_crtc=res->crtcs[i]; drmModeFreeEncoder(e); return 1; }
            drmModeFreeEncoder(e);
        }
    }
    for(int i=0;i<conn->count_encoders;i++){
        drmModeEncoder* e=drmModeGetEncoder(fd,conn->encoders[i]);
        if(!e) continue;
        for(int j=0;j<res->count_crtcs;j++) if(e->possible_crtcs & (1<<j)){ *out_crtc=res->crtcs[j]; drmModeFreeEncoder(e); return 1; }
        drmModeFreeEncoder(e);
    }
    return 0;
}
static int open_card_any(int prefer_conn, int* out_fd, u32* out_conn, u32* out_crtc, drmModeModeInfo* out_mode){
    char path[64];
    for(int idx=0; idx<16; idx++){
        snprintf(path,sizeof(path),"/dev/dri/card%d",idx);
        int fd=open(path,O_RDWR|O_CLOEXEC|O_NONBLOCK); if(fd<0) continue;
        if(drmSetMaster(fd)!=0) LOGE("drmSetMaster failed on %s (errno=%d %s)",path,errno,strerror(errno));
        drmModeRes* res=drmModeGetResources(fd); if(!res){ close(fd); continue; }

        u32 conn=0, crtc=0; drmModeModeInfo mode={0};
        for(int i=0;i<res->count_connectors;i++){
            drmModeConnector* c=drmModeGetConnector(fd,res->connectors[i]); if(!c) continue;
            if(c->connection==DRM_MODE_CONNECTED && c->count_modes>0){
                if(prefer_conn && c->connector_id!=(u32)prefer_conn){ drmModeFreeConnector(c); continue; }
                if(!pick_mode(c,&mode)){ drmModeFreeConnector(c); continue; }
                if(!choose_crtc(fd,res,c,&crtc)){ drmModeFreeConnector(c); continue; }
                conn=c->connector_id; drmModeFreeConnector(c); break;
            }
            drmModeFreeConnector(c);
        }
        drmModeFreeResources(res);
        if(conn && crtc){ *out_fd=fd; *out_conn=conn; *out_crtc=crtc; *out_mode=mode; LOGE("picked %s conn=%u crtc=%u %ux%u@%d",path,conn,crtc,mode.hdisplay,mode.vdisplay,mode.vrefresh); return 1; }
        close(fd);
    }
    return 0;
}
int linux_open_drm_pick(int prefer_conn, int* out_fd, u32* out_conn, u32* out_crtc, struct KmsMode* out_mode){
    struct sigaction sa; memset(&sa,0,sizeof(sa)); sa.sa_handler=on_sigint; sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL);

    drmModeModeInfo m={0}; u32 conn=0, crtc=0;
    if(!open_card_any(prefer_conn,out_fd,&conn,&crtc,&m)){ LOGE("No DRM card/connector found (run on a free VT as root)"); return 0; }
    out_mode->width=m.hdisplay; out_mode->height=m.vdisplay; out_mode->vrefresh=m.vrefresh; *out_conn=conn; *out_crtc=crtc; return 1;
}

/* allocate GBM BOs + FBs, export dma-fd */
int linux_alloc_scanout_images(int drm_fd, u32 w, u32 h, u32 count, struct ScanoutImage* out){
    if(count>4) count=4;
    struct gbm_device* gbm=gbm_create_device(drm_fd); if(!gbm){ LOGE("gbm_create_device failed"); return 0; }
    for(u32 i=0;i<count;i++){
        struct gbm_bo* bo=gbm_bo_create(gbm,w,h,GBM_BO_FORMAT_XRGB8888,GBM_BO_USE_SCANOUT|GBM_BO_USE_RENDERING);
        if(!bo){ LOGE("gbm_bo_create failed"); gbm_device_destroy(gbm); return 0; }
        out[i].width=w; out[i].height=h; out[i].stride=gbm_bo_get_stride(bo);
        out[i].modifier=gbm_bo_get_modifier? gbm_bo_get_modifier(bo):DRM_FORMAT_MOD_LINEAR;
        out[i].dma_fd=gbm_bo_get_fd(bo); if(out[i].dma_fd<0){ LOGE("gbm_bo_get_fd failed"); gbm_bo_destroy(bo); gbm_device_destroy(gbm); return 0; }
        u32 handles[4]={ gbm_bo_get_handle(bo).u32,0,0,0 }, pitches[4]={ out[i].stride,0,0,0 }, offsets[4]={0,0,0,0};
#ifdef DRM_MODE_FB_MODIFIERS
        u64 mods[4]={ out[i].modifier,0,0,0 };
        if(drmModeAddFB2WithModifiers(drm_fd,w,h,DRM_FORMAT_XRGB8888,handles,pitches,offsets,mods,&out[i].fb_id,DRM_MODE_FB_MODIFIERS)!=0)
            if(drmModeAddFB2(drm_fd,w,h,DRM_FORMAT_XRGB8888,handles,pitches,offsets,&out[i].fb_id,0)!=0){ LOGE("drmModeAddFB2 failed"); close(out[i].dma_fd); gbm_bo_destroy(bo); gbm_device_destroy(gbm); return 0; }
#else
        if(drmModeAddFB2(drm_fd,w,h,DRM_FORMAT_XRGB8888,handles,pitches,offsets,&out[i].fb_id,0)!=0){ LOGE("drmModeAddFB2 failed"); close(out[i].dma_fd); gbm_bo_destroy(bo); gbm_device_destroy(gbm); return 0; }
#endif
        /* Note: We drop the bo ref for brevity; in production keep and destroy explicitly. */
    }
    gbm_device_destroy(gbm);
    return 1;
}

/* do one-time legacy modeset */
static int modeset_once(int fd, u32 crtc, u32 conn, u32 fb){
    drmModeRes* res=drmModeGetResources(fd); if(!res) return -1;
    drmModeConnector* c=NULL;
    for(int i=0;i<res->count_connectors;i++){ drmModeConnector* tmp=drmModeGetConnector(fd,res->connectors[i]); if(tmp && tmp->connector_id==conn){ c=tmp; break; } if(tmp) drmModeFreeConnector(tmp); }
    int r=-1; if(c) r=drmModeSetCrtc(fd,crtc,fb,0,0,&conn,1,&c->modes[0]);
    if(c) drmModeFreeConnector(c); drmModeFreeResources(res); return r;
}

/* queue non-blocking pageflip; return 1 if queued */
int linux_queue_flip_nonblock(int drm_fd, u32 crtc, u32 fb_id, int in_fence_fd){
    if(in_fence_fd>=0){ struct pollfd pfd={ .fd=in_fence_fd, .events=POLLIN }; (void)poll(&pfd,1,-1); close(in_fence_fd); }
    if(drmModePageFlip(drm_fd,crtc,fb_id,DRM_MODE_PAGE_FLIP_EVENT,NULL)!=0){ LOGE("pageflip failed: %s", strerror(errno)); return 0; }
    return 1;
}

/* pump drm events for up to timeout_ms */
int linux_pump_events(int drm_fd, int timeout_ms){
    struct pollfd pfd={ .fd=drm_fd, .events=POLLIN };
    int r=poll(&pfd,1,timeout_ms);
    if(r>0 && (pfd.revents&POLLIN)){
        drmEventContext ev; memset(&ev,0,sizeof(ev)); ev.version=DRM_EVENT_CONTEXT_VERSION;
        return drmHandleEvent(drm_fd,&ev)==0 ? 1 : 0;
    }
    return 0;
}

void linux_free_scanout_images(int drm_fd, struct ScanoutImage* imgs, u32 count){
    for(u32 i=0;i<count;i++){ if(imgs[i].fb_id) drmModeRmFB(drm_fd,imgs[i].fb_id); if(imgs[i].dma_fd>=0) close(imgs[i].dma_fd); }
}

void linux_drop_master_if_owner(int drm_fd){ if(drm_fd>=0) drmDropMaster(drm_fd); }
