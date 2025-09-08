#pragma once
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned u32;
typedef unsigned long long u64;
typedef signed char i8;
typedef short i16;
typedef int i32;
typedef long long i64;
typedef float f32;
typedef double f64;
typedef long long isize;
typedef unsigned long long usize;

#if !defined(_WIN32)
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#endif

extern void _exit(int);

struct ScanoutImage{ int dma_fd; u32 fb_id; u32 width, height; u32 stride; u64 modifier; };
struct KmsMode{ u32 width, height, vrefresh; };

/* Embedded assets (provide via objcopy/xxd) */
extern const unsigned char font_atlas[], font_atlas_end[]; /* KTX2 ASTC */
extern const unsigned char shaders[], shaders_end[];       /* SPIR-V blob with VS/FS */

void vk_init_instance(void);
void vk_pick_phys_and_queue(void);
void vk_create_device(void);
void vk_adopt_scanout_images(const struct ScanoutImage* imgs, u32 count); /* also builds pipeline */
int  vk_draw_and_export_sync(u32 image_index);  /* records textured draw, returns sync_fd >= 0 */
void vk_wait_idle(void);
void vk_shutdown(void);

/* Linux side */
int  linux_open_drm_pick(int prefer_conn, int* out_fd, u32* out_conn, u32* out_crtc, struct KmsMode* out_mode);
int  linux_alloc_scanout_images(int drm_fd, u32 w, u32 h, u32 count, struct ScanoutImage* out_imgs);
int  linux_queue_flip_nonblock(int drm_fd, u32 crtc, u32 fb_id, int in_fence_fd); /* returns 1 queued */
int  linux_pump_events(int drm_fd, int timeout_ms); /* drives page-flip completion */
void linux_free_scanout_images(int drm_fd, struct ScanoutImage* imgs, u32 count);
void linux_drop_master_if_owner(int drm_fd);
