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

#pragma region VULKAN
/* scanout image descriptor (OS-agnostic payload) */
struct ScanoutImage{ int dma_fd; u32 fb_id; u32 width, height; u32 stride; u64 modifier; };

/* minimal KMS mode info we care about */
struct KmsMode{ u32 width, height, vrefresh; };

/* ---- Vulkan (OS-agnostic) ---- */
void vk_init_instance(void);
void vk_pick_phys_and_queue(void);
void vk_create_device(void);
void vk_adopt_scanout_images(const struct ScanoutImage* imgs, u32 count);
int  vk_draw_and_export_sync(u32 image_index);         /* returns sync_fd (>=0) */
void vk_wait_idle(void);
void vk_shutdown(void);

/* ---- Linux KMS/GBM (kept separate; no Linux headers leak) ---- */
int  linux_open_drm_pick(int prefer_conn, int* out_fd, u32* out_conn, u32* out_crtc, struct KmsMode* out_mode);
int  linux_alloc_scanout_images(int drm_fd, u32 w, u32 h, u32 count, struct ScanoutImage* out_imgs);
int  linux_atomic_present(int drm_fd, u32 conn, u32 crtc, u32 fb_id, int in_fence_fd, int* out_fence_fd, int allow_modeset);
void linux_free_scanout_images(int drm_fd, struct ScanoutImage* imgs, u32 count);
#pragma endregion
