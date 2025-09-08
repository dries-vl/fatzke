#include "header.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s){ (void)s; g_stop = 1; }

static u64 now_ns(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec*1000000000ull + ts.tv_nsec;
}

int main(void){
    struct sigaction sa; sa.sa_handler = on_sigint; sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);

    int drm_fd = -1; u32 conn = 0, crtc = 0; struct KmsMode mode = {0};
    if(!linux_open_drm_pick(0, &drm_fd, &conn, &crtc, &mode)){
        fprintf(stderr, "DRM pick failed\n");
        return 1;
    }
    fprintf(stderr, "KMS %ux%u@%u conn=%u crtc=%u\n", mode.width, mode.height, mode.vrefresh, conn, crtc);

    struct ScanoutImage imgs[3] = {0};
    if(!linux_alloc_scanout_images(drm_fd, mode.width, mode.height, 3, imgs)){
        fprintf(stderr, "alloc scanout failed\n");
        return 1;
    }

    /* Proper initial modeset to OWN the display */
    extern int linux_initial_modeset(int, u32, u32, u32, u32, u32, u32);
    if(!linux_initial_modeset(drm_fd, crtc, conn, imgs[0].fb_id, mode.width, mode.height, mode.vrefresh)){
        fprintf(stderr, "initial modeset failed (need DRM master on this VT?)\n");
        goto out;
    }

    /* Vulkan setup */
    vk_init_instance();
    vk_pick_phys_and_queue();
    vk_create_device();
    vk_adopt_scanout_images(imgs, 3);  /* also builds ASTC texture + pipeline */

    /* Main loop: draw → export syncfd → queue flip → wait flip → next */
    u32 idx = 0; u64 t0 = now_ns();
    while(!g_stop){
        int sync_fd = vk_draw_and_export_sync(idx);   /* GPU signals this when frame done */
        if(!linux_queue_flip_nonblock(drm_fd, crtc, imgs[idx].fb_id, sync_fd)){
            /* Busy (previous flip pending) or other error. Wait for flip, then try again once. */
            linux_pump_events(drm_fd, -1); /* wait indefinitely for current flip to finish */
            int sync_fd2 = vk_draw_and_export_sync(idx); /* redraw to be safe (very cheap in this demo) */
            if(!linux_queue_flip_nonblock(drm_fd, crtc, imgs[idx].fb_id, sync_fd2)){
                fprintf(stderr, "queue flip still failing — aborting.\n");
                break;
            }
        }

        /* Wait until this flip completes before reusing the same FB index (avoids EBUSY) */
        linux_pump_events(drm_fd, -1);

        idx = (idx + 1) % 3;

        /* safety exit after ~10 minutes */
        if(((now_ns()-t0)/1000000000ull) > 600ull) break;
    }

out:
    vk_wait_idle();
    vk_shutdown();
    linux_free_scanout_images(drm_fd, imgs, 3);
    linux_drop_master_if_owner(drm_fd);
    if(drm_fd >= 0) close(drm_fd);
    return 0;
}
