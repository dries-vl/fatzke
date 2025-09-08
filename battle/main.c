#include <errno.h>

#include "header.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <string.h>

/* ---------- small helpers ---------- */
static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s){ (void)s; g_stop = 1; }

static u64 now_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (u64)ts.tv_sec*1000000000ull+ts.tv_nsec; }

/* nonblocking stdin so you can press 'q' or ESC to exit */
static struct termios g_oldt;
static void stdin_raw_begin(void){
    tcgetattr(STDIN_FILENO,&g_oldt);
    struct termios t = g_oldt; t.c_lflag &= ~(ICANON|ECHO); t.c_cc[VMIN]=0; t.c_cc[VTIME]=0;
    tcsetattr(STDIN_FILENO,TCSANOW,&t);
    int fl = fcntl(STDIN_FILENO,F_GETFL,0); fcntl(STDIN_FILENO,F_SETFL,fl|O_NONBLOCK);
}
static void stdin_raw_end(void){ tcsetattr(STDIN_FILENO,TCSANOW,&g_oldt); }
static int key_quit_pressed(void){
    char ch;
    while (read(STDIN_FILENO,&ch,1)==1){
        if (ch=='q' || ch==27) return 1; /* 'q' or ESC */
    }
    return 0;
}

int main(void){
    struct sigaction sa; sa.sa_handler=on_sigint; sigemptyset(&sa.sa_mask); sa.sa_flags=0;
    sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL);

    int drm_fd=-1; u32 conn=0, crtc=0; struct KmsMode mode={0};
    if(!linux_open_drm_pick(0,&drm_fd,&conn,&crtc,&mode)){
        fprintf(stderr,"DRM pick failed\n"); return 1;
    }
    fprintf(stderr,"KMS %ux%u@%u conn=%u crtc=%u\n",mode.width,mode.height,mode.vrefresh,conn,crtc);

    struct ScanoutImage imgs[3]={0};
    if(!linux_alloc_scanout_images(drm_fd,mode.width,mode.height,3,imgs)){
        fprintf(stderr,"alloc scanout failed\n"); return 1;
    }

    /* First atomic modeset (ALLOW_MODESET) onto FB0 */
    extern int linux_initial_modeset_atomic(int,int,u32,u32,u32,u32);
    if(!linux_initial_modeset_atomic(drm_fd,conn,crtc,imgs[0].fb_id,mode.width,mode.height)){
        fprintf(stderr,"initial atomic modeset failed (need DRM master on this VT?)\n");
        goto out;
    }

    /* Vulkan setup */
    vk_init_instance();
    vk_pick_phys_and_queue();
    vk_create_device();
    vk_adopt_scanout_images(imgs,3);   /* builds ASTC texture + pipeline */

    stdin_raw_begin();
    u32 idx=0; u64 t0=now_ns();
    while(!g_stop){
        /* 1) Render into scanout image idx and export SYNC_FD */
        int sync_fd = vk_draw_and_export_sync(idx);

        /* 2) Atomic commit with IN_FENCE_FD, get OUT_FENCE_FD back */
        int out_fd=-1;
        if(!linux_atomic_present(drm_fd,conn,crtc,imgs[idx].fb_id,sync_fd,&out_fd,0)){
            /* If EBUSY (flip pending), wait for previous OUT_FENCE and retry once */
            if(errno==EBUSY && out_fd<0){
                /* The previous out fence was the pacing source; if you track it externally, wait it here */
                /* In this sample the atomic helper tracks pending; fall through to a redraw+retry path */
            }
            /* redraw a new frame (cheap) and try once more */
            sync_fd = vk_draw_and_export_sync(idx);
            if(!linux_atomic_present(drm_fd,conn,crtc,imgs[idx].fb_id,sync_fd,&out_fd,0)){
                fprintf(stderr,"atomic present failed: %s\n", strerror(errno));
                break;
            }
        }

        /* 3) Wait for OUT_FENCE_FD (the moment the flip actually takes effect) */
        extern int linux_wait_out_fence_fd(int,int);
        linux_wait_out_fence_fd(out_fd, -1); /* block until latched; lowest jitter */

        idx = (idx+1)%3;

        /* graceful exits */
        if (key_quit_pressed()) break;
        if (((now_ns()-t0)/1000000000ull) > 600ull) break; /* 10 min safety */
    }

out:
    stdin_raw_end();
    vk_wait_idle();
    vk_shutdown();

    linux_free_scanout_images(drm_fd,imgs,3);
    linux_drop_master_if_owner(drm_fd);
    if(drm_fd>=0) close(drm_fd);
    return 0;
}
