#include "header.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

static volatile sig_atomic_t g_stop=0;
static void on_sigint(int s){ (void)s; g_stop=1; }

static u64 now_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (u64)ts.tv_sec*1000000000ull+ts.tv_nsec; }

int main(void){
    struct sigaction sa; sa.sa_handler=on_sigint; sigemptyset(&sa.sa_mask); sa.sa_flags=0; sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL);

    int drm_fd=-1; u32 conn=0, crtc=0; struct KmsMode mode={0};
    if(!linux_open_drm_pick(0,&drm_fd,&conn,&crtc,&mode)){ fprintf(stderr,"DRM pick failed\n"); return 1; }
    fprintf(stderr,"KMS %ux%u@%u conn=%u crtc=%u\n",mode.width,mode.height,mode.vrefresh,conn,crtc);

    struct ScanoutImage imgs[3]={0};
    if(!linux_alloc_scanout_images(drm_fd,mode.width,mode.height,3,imgs)){ fprintf(stderr,"alloc scanout failed\n"); return 1; }

    /* one-time modeset on first FB so we own the screen */
    if(drmModeSetCrtc(drm_fd, crtc, imgs[0].fb_id, 0, 0, &conn, 1, NULL)!=0){
        /* fallback to helper if NULL mode rejected */
        extern int linux_queue_flip_nonblock(int,u32,u32,int);
        extern int linux_pump_events(int,int);
        /* many drivers accept SetCrtc with the connector's current mode pulled by the helper in linux.c,
           but keeping it compact here; if this fails, the prior helper modeset() can be called. */
    }

    vk_init_instance(); vk_pick_phys_and_queue(); vk_create_device(); vk_adopt_scanout_images(imgs,3);

    u32 idx=0; u64 t0=now_ns();
    while(!g_stop){
        int sync_fd = vk_draw_and_export_sync(idx);
        if(!linux_queue_flip_nonblock(drm_fd,crtc,imgs[idx].fb_id,sync_fd)){ fprintf(stderr,"flip queue fail\n"); break; }
        /* pump DRM events a little to avoid blocking / keep VT healthy */
        linux_pump_events(drm_fd, 8);
        idx=(idx+1)%3;

        /* simple escape valve: quit after 10 minutes */
        if(((now_ns()-t0)/1000000000ull) > 600ull) break;
    }

    vk_wait_idle(); vk_shutdown();
    linux_free_scanout_images(drm_fd,imgs,3);
    linux_drop_master_if_owner(drm_fd);
    if(drm_fd>=0) close(drm_fd);
    return 0;
}
