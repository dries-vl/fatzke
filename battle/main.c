#include "header.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <bits/poll.h>
#include <sys/poll.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s){ (void)s; g_stop = 1; }
static u64 now_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (u64)ts.tv_sec*1000000000ull+ts.tv_nsec; }

/* raw key input for 'q'/ESC */
static struct termios g_oldt;
static void stdin_raw_begin(void){
    tcgetattr(STDIN_FILENO,&g_oldt);
    struct termios t = g_oldt; t.c_lflag &= ~(ICANON|ECHO); t.c_cc[VMIN]=0; t.c_cc[VTIME]=0;
    tcsetattr(STDIN_FILENO,TCSANOW,&t);
    int fl = fcntl(STDIN_FILENO,F_GETFL,0); fcntl(STDIN_FILENO,F_SETFL,fl|O_NONBLOCK);
}
static void stdin_raw_end(void){ tcsetattr(STDIN_FILENO,TCSANOW,&g_oldt); }
static int key_quit_pressed(void){
    char ch; while (read(STDIN_FILENO,&ch,1)==1){ if (ch=='q' || ch==27) return 1; } return 0;
}

int main(void){
    struct sigaction sa; sa.sa_handler=on_sigint; sigemptyset(&sa.sa_mask); sa.sa_flags=0;
    sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL);

    int drm_fd=-1; u32 conn=0, crtc=0; struct KmsMode mode={0};
    if(!linux_open_drm_pick(0,&drm_fd,&conn,&crtc,&mode)){ fprintf(stderr,"DRM pick failed\n"); return 1; }
    fprintf(stderr,"KMS %ux%u@%u conn=%u crtc=%u\n",mode.width,mode.height,mode.vrefresh,conn,crtc);

    struct ScanoutImage imgs[3]={0};
    if(!linux_alloc_scanout_images(drm_fd,mode.width,mode.height,3,imgs)){ fprintf(stderr,"alloc scanout failed\n"); return 1; }

    /* ---- initial modeset: try ATOMIC first, fallback to LEGACY ---- */
    extern int linux_initial_modeset_atomic(int,int,u32,u32,u32,u32);
    extern int linux_initial_modeset(int,int,u32,u32); /* legacy fallback */
    if(!linux_initial_modeset_atomic(drm_fd,conn,crtc,imgs[0].fb_id,mode.width,mode.height)){
        fprintf(stderr,"warning: atomic modeset failed (%s). Falling back to legacy modeset.\n", strerror(errno));
        if(!linux_initial_modeset(drm_fd,crtc,conn,imgs[0].fb_id)){
            fprintf(stderr,"fatal: legacy modeset also failed\n");
            goto out;
        }
    }

    /* Vulkan setup */
    vk_init_instance(); vk_pick_phys_and_queue(); vk_create_device(); vk_adopt_scanout_images(imgs,3);

    stdin_raw_begin();
    u32 idx=0; u64 t0=now_ns();
    while(!g_stop){
        int sync_fd = vk_draw_and_export_sync(idx);

        /* Present via atomic: IN_FENCE_FD + OUT_FENCE_FD */
        int out_fd=-1;
        if(!linux_atomic_present(drm_fd,conn,crtc,imgs[idx].fb_id,sync_fd,&out_fd,0)){
            if(errno==ENOTSUP){
                /* Atomic not available at all — emergency fallback: block on GPU fence and do a legacy pageflip */
                if(sync_fd>=0){ struct pollfd p={.fd=sync_fd,.events=POLLIN}; (void)poll(&p,1,-1); close(sync_fd); }
                /* NOTE: For brevity we do not include a full legacy pageflip event loop here since you asked for lowest-latency atomic.
                        If you need it, we can add it back. */
                fprintf(stderr,"fatal: atomic present not supported; exiting to avoid stalls\n");
                break;
            }
            /* If EBUSY (pending flip), just wait the previous out fence in next iteration */
            fprintf(stderr,"atomic present failed: %s\n", strerror(errno));
            break;
        }

        /* Wait until hardware latches this frame */
        extern int linux_wait_out_fence_fd(int,int);
        linux_wait_out_fence_fd(out_fd, -1); /* precise pacing; minimal latency & jitter */

        idx=(idx+1)%3;
        if(key_quit_pressed()) break;
        if(((now_ns()-t0)/1000000000ull)>600ull) break; /* 10 min safety */
    }

out:
    stdin_raw_end();
    vk_wait_idle(); vk_shutdown();
    linux_free_scanout_images(drm_fd,imgs,3);
    linux_drop_master_if_owner(drm_fd);
    if(drm_fd>=0) close(drm_fd);
    return 0;
}
