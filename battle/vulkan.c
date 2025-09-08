#include <fcntl.h>

#include "header.h"

#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#else
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>
#endif

/* --- error helpers --- */
#define VK_CHECK(x) do{ VkResult _r=(x); if(_r!=VK_SUCCESS){ fprintf(stderr,"VK_ERR %d @%s:%d\n",(int)_r,__FILE__,__LINE__); exit(1);} }while(0)
#define VK_ENUM(x)  do{ VkResult _r=(x); if(_r!=VK_SUCCESS && _r!=VK_INCOMPLETE){ fprintf(stderr,"VK_ENUM %d @%s:%d\n",(int)_r,__FILE__,__LINE__); exit(1);} }while(0)

/* --- embedded assets --- */
extern const unsigned char font_atlas[];
extern const unsigned char font_atlas_end[];
#define font_atlas_len ((size_t)(font_atlas_end - font_atlas))

extern const unsigned char shaders[];
extern const unsigned char shaders_end[];
#define shaders_len ((size_t)(shaders_end - shaders))

#define TARGET_FORMAT VK_FORMAT_R8G8B8A8_SRGB

/* --- app state, no hidden globals --- */
struct VkApp{
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    u32 qfam;
    VkDevice device;
    VkQueue queue;

    /* offscreen */
    VkImage off_img; VkDeviceMemory off_mem; VkImageView off_view; VkSampler off_sampler;
    // persistent framebuffer
    VkFramebuffer off_fb;

    /* sampled ASTC texture */
    VkImage tex_img; VkDeviceMemory tex_mem; VkImageView tex_view;
    u32 tex_mips;       // <-- add this
    VkFormat tex_format; // optional, also handy

    /* descriptors */
    VkDescriptorSetLayout dsl; VkDescriptorPool dpool; VkDescriptorSet dsetA; VkDescriptorSet dsetB;

    /* passes/pipes */
    VkRenderPass rpA; VkRenderPass rpB;
    VkPipelineLayout plA; VkPipelineLayout plB;
    VkPipeline gpA; VkPipeline gpB;

    /* swapchain */
    VkSwapchainKHR swapchain; VkFormat swap_fmt; VkExtent2D swap_extent;
    VkImage sc_img[8]; VkImageView sc_view[8]; VkFramebuffer fb[8]; u32 sc_count;

    /* per-frame */
    VkCommandPool frame_pool; VkCommandBuffer frame_cb[8];
    VkSemaphore sem_acquire, sem_render; VkFence fence;
};

#define OFF_W 1920u
#define OFF_H 1080u

/* --- tiny utils --- */
static u32 find_mem_type(struct VkApp* a, u32 typeBits, VkMemoryPropertyFlags req){
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(a->phys, &mp);
    for(u32 i=0;i<mp.memoryTypeCount;i++) if((typeBits&(1u<<i)) && ((mp.memoryTypes[i].propertyFlags&req)==req)) return i;
    fprintf(stderr,"no memory type\n"); exit(1);
}

static void create_buffer(struct VkApp* a, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags, VkBuffer* out_buf, VkDeviceMemory* out_mem){
    VkBufferCreateInfo bci = { .sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size=size, .usage=usage, .sharingMode=VK_SHARING_MODE_EXCLUSIVE };
    VK_CHECK(vkCreateBuffer(a->device,&bci,NULL,out_buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(a->device,*out_buf,&mr);
    VkMemoryAllocateInfo mai = { .sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize=mr.size, .memoryTypeIndex=find_mem_type(a,mr.memoryTypeBits,flags) };
    VK_CHECK(vkAllocateMemory(a->device,&mai,NULL,out_mem)); VK_CHECK(vkBindBufferMemory(a->device,*out_buf,*out_mem,0));
}

/* --- immediate submit (self-contained, no hidden pool dependency) --- */
struct ImmediateCmd{ VkCommandPool pool; VkCommandBuffer cb; };
static struct ImmediateCmd begin_immediate(struct VkApp* a){
    struct ImmediateCmd ic = {0};
    VkCommandPoolCreateInfo cpci = { .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex=a->qfam, .flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT|VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    VK_CHECK(vkCreateCommandPool(a->device,&cpci,NULL,&ic.pool));
    VkCommandBufferAllocateInfo ai = { .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool=ic.pool, .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount=1 };
    VK_CHECK(vkAllocateCommandBuffers(a->device,&ai,&ic.cb));
    VkCommandBufferBeginInfo bi = { .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK_CHECK(vkBeginCommandBuffer(ic.cb,&bi));
    return ic;
}
static void end_immediate(struct VkApp* a, struct ImmediateCmd* ic){
    VK_CHECK(vkEndCommandBuffer(ic->cb));
    VkSubmitInfo si = { .sType=VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount=1, .pCommandBuffers=&ic->cb };
    VK_CHECK(vkQueueSubmit(a->queue,1,&si,VK_NULL_HANDLE)); VK_CHECK(vkQueueWaitIdle(a->queue));
    vkFreeCommandBuffers(a->device, ic->pool, 1, &ic->cb); vkDestroyCommandPool(a->device, ic->pool, NULL); ic->pool=VK_NULL_HANDLE; ic->cb=VK_NULL_HANDLE;
}

/* --- instance creation --- */
#if defined(_WIN32)
static void app_create_instance(struct VkApp* a){
    VkApplicationInfo ai = { .sType=VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName="tri2", .applicationVersion=1, .pEngineName="none", .apiVersion=VK_API_VERSION_1_1 };
    const char* exts[] = {"VK_KHR_surface","VK_KHR_win32_surface"};
    VkInstanceCreateInfo ci = { .sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo=&ai, .enabledExtensionCount=2, .ppEnabledExtensionNames=exts };
    VK_CHECK(vkCreateInstance(&ci,NULL,&a->instance)); pf_timestamp("vkCreateInstance");
}
#else
/* Linux direct-driver-loading for Intel ICD (kept but encapsulated) */
static int has_suffix(const char* s, const char* suf){ size_t n=strlen(s), m=strlen(suf); return n>=m && 0==strcmp(s+n-m,suf); }
static int contains_icd_intel(const char* s){ return strstr(s,"\"ICD\"") && (strstr(s,"intel")||strstr(s,"INTEL")||strstr(s,"anv")); }
static char* read_file_all(const char* p, size_t* n){ FILE* f=fopen(p,"rb"); if(!f)return NULL; fseek(f,0,SEEK_END); long m=ftell(f); fseek(f,0,SEEK_SET); char* b=(char*)malloc((size_t)m+1); if(!b){fclose(f);return NULL;} if(fread(b,1,(size_t)m,f)!=(size_t)m){fclose(f);free(b);return NULL;} fclose(f); b[m]=0; if(n)*n=(size_t)m; return b; }
static char* json_extract_library_path(const char* json){ const char* k=strstr(json,"\"library_path\""); if(!k)return NULL; k=strchr(k,':'); if(!k)return NULL; for(k++;*k==' '||*k=='\t';k++); if(*k!='\"')return NULL; k++; const char* e=strchr(k,'\"'); if(!e)return NULL; size_t len=(size_t)(e-k); char* out=(char*)malloc(len+1); if(!out)return NULL; memcpy(out,k,len); out[len]=0; return out; }
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddrLUNARG)(VkInstance,const char*);
static PFN_vkGetInstanceProcAddrLUNARG load_icd_gpa(void* h){ void* p=dlsym(h,"vkGetInstanceProcAddr"); if(!p)p=dlsym(h,"vk_icdGetInstanceProcAddr"); return (PFN_vkGetInstanceProcAddrLUNARG)p; }
static char* find_intel_icd_lib(void){ void* h=dlopen("libvulkan_intel.so",RTLD_NOW|RTLD_LOCAL); if(h){ dlclose(h); return "libvulkan_intel.so"; } const char* dirs[]={"/usr/share/vulkan/icd.d","/etc/vulkan/icd.d","/usr/local/share/vulkan/icd.d"}; for(size_t di=0;di<sizeof(dirs)/sizeof(dirs[0]);di++){ DIR* d=opendir(dirs[di]); if(!d)continue; struct dirent* ent; while((ent=readdir(d))){ if(ent->d_name[0]=='.')continue; if(!has_suffix(ent->d_name,".json"))continue; if(!(strstr(ent->d_name,"intel")||strstr(ent->d_name,"anv")))continue; char path[1024]; snprintf(path,sizeof(path),"%s/%s",dirs[di],ent->d_name); size_t n=0; char* txt=read_file_all(path,&n); if(!txt)continue; if(!contains_icd_intel(txt)){ free(txt); continue; } char* lib=json_extract_library_path(txt); free(txt); if(!lib)continue; void* h2=dlopen(lib,RTLD_NOW|RTLD_LOCAL); if(h2){ dlclose(h2); closedir(d); return lib; } free(lib); } closedir(d);} return NULL; }
// helper: is instance extension available?
/* --- helper: instance ext availability --- */
static int ext_avail(const char* name){
    uint32_t n=0; vkEnumerateInstanceExtensionProperties(NULL,&n,NULL);
    VkExtensionProperties e[128]; if(n>128) n=128;
    vkEnumerateInstanceExtensionProperties(NULL,&n,e);
    for(uint32_t i=0;i<n;i++) if(strcmp(e[i].extensionName,name)==0) return 1;
    return 0;
}
static void app_create_instance(struct VkApp* a){
    /* Try direct-driver-loading if present, but only chain pNext when enabled */
    char* intel_icd=find_intel_icd_lib(); if(!intel_icd){ fprintf(stderr,"Intel ICD not found\n"); exit(1); }
    void* h=dlopen(intel_icd,RTLD_NOW|RTLD_LOCAL); if(!h){ fprintf(stderr,"dlopen failed for %s\n", intel_icd); exit(1); }
    PFN_vkGetInstanceProcAddrLUNARG gpa=load_icd_gpa(h); if(!gpa){ fprintf(stderr,"ICD missing GetInstanceProcAddr\n"); exit(1); }

    VkDirectDriverLoadingInfoLUNARG dinfo={ .sType=VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_INFO_LUNARG, .pfnGetInstanceProcAddr=gpa };
    VkDirectDriverLoadingListLUNARG dlist={ .sType=VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG, .mode=VK_DIRECT_DRIVER_LOADING_MODE_EXCLUSIVE_LUNARG, .driverCount=1, .pDrivers=&dinfo };

    VkApplicationInfo ai={ .sType=VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName="tri2", .applicationVersion=1, .pEngineName="none", .apiVersion=VK_API_VERSION_1_1 };

    /* Build desired list in priority order */
    const char* desired[8]; uint32_t dn=0;
    desired[dn++]="VK_KHR_surface";
    desired[dn++]="VK_KHR_wayland_surface";
    if(ext_avail("VK_LUNARG_direct_driver_loading")) desired[dn++]="VK_LUNARG_direct_driver_loading";
    if(ext_avail("VK_KHR_display"))                 desired[dn++]="VK_KHR_display";
    if(ext_avail("VK_EXT_direct_mode_display"))     desired[dn++]="VK_EXT_direct_mode_display";
    if(ext_avail("VK_EXT_acquire_drm_display"))     desired[dn++]="VK_EXT_acquire_drm_display";
    if(ext_avail("VK_KHR_get_physical_device_properties2")) desired[dn++]="VK_KHR_get_physical_device_properties2"; // <-- correct name

    /* Start with everything we think is available; if create fails, peel back extras */
    for(uint32_t keep=dn; keep>=2; keep--){ /* always keep KHR_surface + KHR_wayland_surface */
        const char* exts[8]; for(uint32_t i=0;i<keep;i++) exts[i]=desired[i];
        int have_lunarg=0; for(uint32_t i=0;i<keep;i++) if(strcmp(exts[i],"VK_LUNARG_direct_driver_loading")==0) have_lunarg=1;

        VkInstanceCreateInfo ci={ .sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo=&ai,
                                  .enabledExtensionCount=keep, .ppEnabledExtensionNames=exts,
                                  .pNext = have_lunarg? (const void*)&dlist : NULL };

        VkResult r = vkCreateInstance(&ci,NULL,&a->instance);
        if(r==VK_SUCCESS){
            fprintf(stderr,"enabled instance exts:"); for(uint32_t i=0;i<keep;i++) fprintf(stderr," %s", exts[i]); fprintf(stderr,"\n");
            pf_timestamp("vkCreateInstance");
            return;
        }
        if(r!=VK_ERROR_EXTENSION_NOT_PRESENT){
            fprintf(stderr,"vkCreateInstance failed: %d\n", (int)r); exit(1);
        }
        /* Try again with one fewer optional extension (drop the last one first) */
        if(keep==2){ fprintf(stderr,"fatal: even minimal instance failed (surface+wayland)\n"); exit(1); }
    }
    fprintf(stderr,"unreachable\n"); exit(1);
}

#endif

/* --- surface creation --- */
#ifdef __linux__
void vk_create_surface(void* display, void* surface){ extern struct VkApp* vk_app_singleton(void); struct VkApp* a=vk_app_singleton(); VkWaylandSurfaceCreateInfoKHR sci={ .sType=VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, .display=display, .surface=surface }; VK_CHECK(vkCreateWaylandSurfaceKHR(a->instance,&sci,NULL,&a->surface)); pf_timestamp("vkCreateWaylandSurfaceKHR"); }
#endif
#ifdef _WIN32
void vk_create_surface(void* hinst, void* hwnd){ extern struct VkApp* vk_app_singleton(void); struct VkApp* a=vk_app_singleton(); VkWin32SurfaceCreateInfoKHR sci={ .sType=VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, .hinstance=hinst, .hwnd=hwnd }; VK_CHECK(vkCreateWin32SurfaceKHR(a->instance,&sci,NULL,&a->surface)); pf_timestamp("vkCreateWin32SurfaceKHR"); }
#endif

/* --- pick phys + queue family --- */
static void app_pick_phys_and_qfam(struct VkApp* a){
    u32 n=0; VK_CHECK(vkEnumeratePhysicalDevices(a->instance,&n,NULL)); if(!n){ fprintf(stderr,"no physical devices\n"); exit(1); }
    VkPhysicalDevice list[8]; if(n>8)n=8; VK_CHECK(vkEnumeratePhysicalDevices(a->instance,&n,list)); a->phys=list[0];
    for(u32 i=0;i<n;i++){ VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(list[i],&p); if(p.vendorID==0x8086){ a->phys=list[i]; break; } }
    u32 qn=0; vkGetPhysicalDeviceQueueFamilyProperties(a->phys,&qn,NULL); if(qn>16)qn=16; VkQueueFamilyProperties qfp[16]; vkGetPhysicalDeviceQueueFamilyProperties(a->phys,&qn,qfp);
    a->qfam=~0u; for(u32 i=0;i<qn;i++){ VkBool32 pres=VK_FALSE; VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(a->phys,i,a->surface,&pres)); if((qfp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT) && pres){ a->qfam=i; break; } }
    if(a->qfam==~0u){ fprintf(stderr,"no gfx+present queue\n"); exit(1); }
    pf_timestamp("pick phys+queue");
}

/* --- device + queue --- */
static void app_create_device_and_queue(struct VkApp* a){
    float pr=1.0f; VkDeviceQueueCreateInfo qci={ .sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex=a->qfam, .queueCount=1, .pQueuePriorities=&pr };
    const char* exts[]={"VK_KHR_swapchain"}; VkDeviceCreateInfo dci={ .sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount=1, .pQueueCreateInfos=&qci, .enabledExtensionCount=1, .ppEnabledExtensionNames=exts };
    VK_CHECK(vkCreateDevice(a->phys,&dci,NULL,&a->device)); vkGetDeviceQueue(a->device,a->qfam,0,&a->queue); pf_timestamp("vkCreateDevice");
}

/* --- ASTC KTX2 upload (self-contained; uses immediate submit; no frame pool) --- */
struct KTX2Header{
    u8  identifier[12];                 /* «KTX 20»\r\n\x1A\n */
    u32 vkFormat;
    u32 typeSize;
    u32 pixelWidth, pixelHeight, pixelDepth;
    u32 layerCount, faceCount, levelCount, supercompressionScheme;
    u32 dfdByteOffset, dfdByteLength;   /* 32-bit */
    u32 kvdByteOffset, kvdByteLength;   /* 32-bit */
    u64 sgdByteOffset, sgdByteLength;   /* 64-bit (ONLY these are 64-bit) */
};
_Static_assert(sizeof(struct KTX2Header)==80, "KTX2 header must be 80 bytes");

struct KTX2LevelIndex{ u64 byteOffset, byteLength, uncompressedByteLength; };
_Static_assert(sizeof(struct KTX2LevelIndex)==24, "LevelIndex must be 24 bytes");

static void app_upload_astc_from_embedded(struct VkApp* a){
    const u8* file=(const u8*)font_atlas; const size_t file_len=font_atlas_len;

    struct KTX2Header hdr; memcpy(&hdr,file,sizeof hdr);
    static const u8 magic[12]={0xAB,'K','T','X',' ','2','0',0xBB,0x0D,0x0A,0x1A,0x0A};
    if(memcmp(hdr.identifier,magic,12)!=0){ fprintf(stderr,"not a KTX2\n"); exit(1); }
    VkFormat fmt=(VkFormat)hdr.vkFormat;
    if(!(fmt==VK_FORMAT_ASTC_12x12_UNORM_BLOCK||fmt==VK_FORMAT_ASTC_12x12_SRGB_BLOCK)){ fprintf(stderr,"need ASTC 12x12\n"); exit(1); }

    const u32 mips=hdr.levelCount;
    const struct KTX2LevelIndex* lv=(const struct KTX2LevelIndex*)(file+sizeof(struct KTX2Header));
    size_t total=0; for(u32 i=0;i<mips;i++){ total+= (size_t)lv[i].byteLength; if(file_len<(size_t)lv[i].byteOffset+(size_t)lv[i].byteLength){ fprintf(stderr,"ktx2 trunc\n"); exit(1); } }
    { const u32 bs=16,bw=(hdr.pixelWidth+11)/12,bh=(hdr.pixelHeight+11)/12; const size_t expect0=(size_t)bw*bh*bs; if((size_t)lv[0].byteLength!=expect0){ fprintf(stderr,"mip0 bytes %zu != %zu\n",(size_t)lv[0].byteLength,expect0); exit(1);} }

    /* create image */
    VkImageCreateInfo ici={ .sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType=VK_IMAGE_TYPE_2D, .format=fmt,
        .extent={(u32)hdr.pixelWidth,(u32)hdr.pixelHeight,1}, .mipLevels=mips, .arrayLayers=1, .samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL, .usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT };
    VK_CHECK(vkCreateImage(a->device,&ici,NULL,&a->tex_img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(a->device,a->tex_img,&mr);
    VkMemoryAllocateInfo mai={ .sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize=mr.size, .memoryTypeIndex=find_mem_type(a,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VK_CHECK(vkAllocateMemory(a->device,&mai,NULL,&a->tex_mem)); VK_CHECK(vkBindImageMemory(a->device,a->tex_img,a->tex_mem,0));

    /* one big staging buffer with per-mip offsets */
    VkBuffer stg=VK_NULL_HANDLE; VkDeviceMemory stg_mem=VK_NULL_HANDLE;
    create_buffer(a,(VkDeviceSize)total,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,&stg,&stg_mem);

    size_t* offs=(size_t*)malloc(sizeof(size_t)*mips); size_t off=0;
    void* map=0; VK_CHECK(vkMapMemory(a->device,stg_mem,0,(VkDeviceSize)total,0,&map));
    for(u32 i=0;i<mips;i++){
        off = (off + 15) & ~((size_t)15); /* align to 16 (ASTC block) */
        offs[i]=off;
        memcpy((u8*)map+off, file+(size_t)lv[i].byteOffset, (size_t)lv[i].byteLength);
        off += (size_t)lv[i].byteLength;
    }
    vkUnmapMemory(a->device,stg_mem);

    struct ImmediateCmd ic=begin_immediate(a);

    /* make host writes visible to transfer */
    VkBufferMemoryBarrier bmb={ .sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask=VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
                                .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .buffer=stg, .offset=0, .size=off };
    vkCmdPipelineBarrier(ic.cb,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,1,&bmb,0,NULL);

    /* transition whole image to TRANSFER_DST */
    VkImageMemoryBarrier to_dst={ .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask=0, .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED, .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=a->tex_img, .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,mips,0,1} };
    vkCmdPipelineBarrier(ic.cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&to_dst);

    /* copy each mip from its own offset */
    u32 w=(u32)hdr.pixelWidth, h=(u32)hdr.pixelHeight;
    for(u32 i=0;i<mips;i++){
        VkBufferImageCopy bic={ .bufferOffset=offs[i], .bufferRowLength=0, .bufferImageHeight=0,
            .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,i,0,1}, .imageOffset={0,0,0}, .imageExtent={ w?w:1, h?h:1, 1 } };
        vkCmdCopyBufferToImage(ic.cb,stg,a->tex_img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&bic);
        if(w>1) w>>=1; if(h>1) h>>=1;
    }

    /* ready for sampling */
    VkImageMemoryBarrier to_ro={ .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask=VK_ACCESS_SHADER_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=a->tex_img, .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,mips,0,1} };
    vkCmdPipelineBarrier(ic.cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,NULL,0,NULL,1,&to_ro);

    end_immediate(a,&ic);
    vkDestroyBuffer(a->device,stg,NULL); vkFreeMemory(a->device,stg_mem,NULL); free(offs);

    VkImageViewCreateInfo vci={ .sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image=a->tex_img, .viewType=VK_IMAGE_VIEW_TYPE_2D, .format=fmt, .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,mips,0,1} };
    VK_CHECK(vkCreateImageView(a->device,&vci,NULL,&a->tex_view));

    a->tex_mips = mips; a->tex_format = fmt;
}



/* --- offscreen + descriptors --- */
static void app_create_offscreen(struct VkApp* a)
{
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = TARGET_FORMAT,
        .extent = {OFF_W,OFF_H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    VK_CHECK(vkCreateImage(a->device,&ici,NULL,&a->off_img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(a->device, a->off_img, &mr);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = find_mem_type(a, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    VK_CHECK(vkAllocateMemory(a->device,&mai,NULL,&a->off_mem));
    VK_CHECK(vkBindImageMemory(a->device,a->off_img,a->off_mem,0));
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = a->off_img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = TARGET_FORMAT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
    };
    VK_CHECK(vkCreateImageView(a->device,&vci,NULL,&a->off_view));
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .minLod = 0.0f,
        .maxLod = (f32)(a->tex_mips - 1)
    };
    VK_CHECK(vkCreateSampler(a->device,&sci,NULL,&a->off_sampler));
}

static void app_create_descriptors(struct VkApp* a){
    VkDescriptorSetLayoutBinding b[2]={ { .binding=0, .descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT }, { .binding=1, .descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT } };
    VkDescriptorSetLayoutCreateInfo dlci={ .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount=2, .pBindings=b }; VK_CHECK(vkCreateDescriptorSetLayout(a->device,&dlci,NULL,&a->dsl));
    VkDescriptorPoolSize ps[2]={ { .type=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount=2 }, { .type=VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount=2 } };
    VkDescriptorPoolCreateInfo dpci={ .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets=2, .poolSizeCount=2, .pPoolSizes=ps }; VK_CHECK(vkCreateDescriptorPool(a->device,&dpci,NULL,&a->dpool));
    VkDescriptorSetLayout layouts[2]={ a->dsl,a->dsl }; VkDescriptorSetAllocateInfo dsai={ .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool=a->dpool, .descriptorSetCount=2, .pSetLayouts=layouts };
    VkDescriptorSet sets[2]; VK_CHECK(vkAllocateDescriptorSets(a->device,&dsai,sets)); a->dsetA=sets[0]; a->dsetB=sets[1];

    VkDescriptorImageInfo imgA={ .imageView=a->tex_view, .imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }; VkDescriptorImageInfo smpA={ .sampler=a->off_sampler };
    VkWriteDescriptorSet W_A[2]={ { .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet=a->dsetA, .dstBinding=0, .descriptorCount=1, .descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo=&imgA }, { .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet=a->dsetA, .dstBinding=1, .descriptorCount=1, .descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo=&smpA } };
    vkUpdateDescriptorSets(a->device,2,W_A,0,NULL);

    VkDescriptorImageInfo imgB={ .imageView=a->off_view, .imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }; VkDescriptorImageInfo smpB={ .sampler=a->off_sampler };
    VkWriteDescriptorSet W_B[2]={ { .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet=a->dsetB, .dstBinding=0, .descriptorCount=1, .descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo=&imgB }, { .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet=a->dsetB, .dstBinding=1, .descriptorCount=1, .descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo=&smpB } };
    vkUpdateDescriptorSets(a->device,2,W_B,0,NULL);
}

/* --- passes/pipelines --- */
static VkShaderModule create_shader_module_from_static(struct VkApp* a){ VkShaderModuleCreateInfo ci={ .sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize=shaders_len, .pCode=(const u32*)shaders }; VkShaderModule m; VK_CHECK(vkCreateShaderModule(a->device,&ci,NULL,&m)); return m; }

static void app_create_passes_pipes(struct VkApp* a, VkFormat swap_fmt){
    VkAttachmentDescription aA={ .format=VK_FORMAT_R8G8B8A8_UNORM, .samples=VK_SAMPLE_COUNT_1_BIT, .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE, .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkAttachmentReference arA={ .attachment=0, .layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription spA={ .pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount=1, .pColorAttachments=&arA };
    VkRenderPassCreateInfo rpciA={ .sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount=1, .pAttachments=&aA, .subpassCount=1, .pSubpasses=&spA }; VK_CHECK(vkCreateRenderPass(a->device,&rpciA,NULL,&a->rpA));

    VkAttachmentDescription aB={ .format=swap_fmt, .samples=VK_SAMPLE_COUNT_1_BIT, .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE, .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
    VkAttachmentReference arB={ .attachment=0, .layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription spB={ .pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount=1, .pColorAttachments=&arB };
    VkRenderPassCreateInfo rpciB={ .sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount=1, .pAttachments=&aB, .subpassCount=1, .pSubpasses=&spB }; VK_CHECK(vkCreateRenderPass(a->device,&rpciB,NULL,&a->rpB));

    VkShaderModule mod=create_shader_module_from_static(a);
    VkPipelineShaderStageCreateInfo stA[2]={ { .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage=VK_SHADER_STAGE_VERTEX_BIT, .module=mod, .pName="VS_Tri" }, { .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage=VK_SHADER_STAGE_FRAGMENT_BIT, .module=mod, .pName="PS_Tri" } };
    VkPipelineVertexInputStateCreateInfo vin={ .sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia={ .sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vpst={ .sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount=1, .scissorCount=1 };
    VkDynamicState dyns[2]={ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR }; VkPipelineDynamicStateCreateInfo dyn={ .sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount=2, .pDynamicStates=dyns };
    VkPipelineRasterizationStateCreateInfo rs={ .sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode=VK_POLYGON_MODE_FILL, .cullMode=VK_CULL_MODE_NONE, .frontFace=VK_FRONT_FACE_CLOCKWISE, .lineWidth=1.0f };
    VkPipelineMultisampleStateCreateInfo ms={ .sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba={ .colorWriteMask=0xF, .blendEnable=VK_FALSE }; VkPipelineColorBlendStateCreateInfo cb={ .sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount=1, .pAttachments=&cba };
    VkPipelineLayoutCreateInfo plciA={ .sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount=1, .pSetLayouts=&a->dsl }; VK_CHECK(vkCreatePipelineLayout(a->device,&plciA,NULL,&a->plA));
    VkGraphicsPipelineCreateInfo pciA={ .sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount=2, .pStages=stA, .pVertexInputState=&vin, .pInputAssemblyState=&ia, .pViewportState=&vpst, .pDynamicState=&dyn, .pRasterizationState=&rs, .pMultisampleState=&ms, .pColorBlendState=&cb, .layout=a->plA, .renderPass=a->rpA, .subpass=0 };
    VK_CHECK(vkCreateGraphicsPipelines(a->device,VK_NULL_HANDLE,1,&pciA,NULL,&a->gpA));

    VkPipelineShaderStageCreateInfo stB[2]={ { .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage=VK_SHADER_STAGE_VERTEX_BIT, .module=mod, .pName="VS_Blit" }, { .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage=VK_SHADER_STAGE_FRAGMENT_BIT, .module=mod, .pName="PS_Blit" } };
    VkPipelineLayoutCreateInfo plciB={ .sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount=1, .pSetLayouts=&a->dsl }; VK_CHECK(vkCreatePipelineLayout(a->device,&plciB,NULL,&a->plB));
    VkGraphicsPipelineCreateInfo pciB={ .sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount=2, .pStages=stB, .pVertexInputState=&vin, .pInputAssemblyState=&ia, .pViewportState=&vpst, .pDynamicState=&dyn, .pRasterizationState=&rs, .pMultisampleState=&ms, .pColorBlendState=&cb, .layout=a->plB, .renderPass=a->rpB, .subpass=0 };
    VK_CHECK(vkCreateGraphicsPipelines(a->device,VK_NULL_HANDLE,1,&pciB,NULL,&a->gpB));
    vkDestroyShaderModule(a->device,mod,NULL);
}

/* --- swapchain + framebuffers + frame command buffers --- */
static VkSurfaceFormatKHR choose_format(struct VkApp* a)
{
    u32 n = 0;
    VK_ENUM(vkGetPhysicalDeviceSurfaceFormatsKHR(a->phys,a->surface,&n,NULL));
    VkSurfaceFormatKHR pick = {0};
    VkSurfaceFormatKHR* f = (VkSurfaceFormatKHR*)malloc(n * sizeof(*f));
    VK_ENUM(vkGetPhysicalDeviceSurfaceFormatsKHR(a->phys,a->surface,&n,f));
    pick = f[0];
    for (u32 i = 0; i < n; i++) if (f[i].format == TARGET_FORMAT && f[i].colorSpace ==
        VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
    {
        pick = f[i];
        break;
    }
    free(f);
    return pick;
}

static VkCompositeAlphaFlagBitsKHR choose_alpha(const VkSurfaceCapabilitiesKHR caps)
{
    const VkCompositeAlphaFlagBitsKHR order[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
    };
    for (u32 i = 0; i < sizeof(order) / sizeof(order[0]); i++) if (caps.supportedCompositeAlpha & order[i]) return order
        [i];
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}


static void app_create_swapchain_block(struct VkApp* a, u32 w, u32 h){
    VkSurfaceCapabilitiesKHR caps; VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(a->phys,a->surface,&caps));
    VkSurfaceFormatKHR sf=choose_format(a); a->swap_fmt=sf.format; VkPresentModeKHR pm=VK_PRESENT_MODE_FIFO_KHR;
    if(caps.currentExtent.width!=UINT32_MAX) a->swap_extent=caps.currentExtent; else a->swap_extent=(VkExtent2D){w,h}; if(a->swap_extent.width==0||a->swap_extent.height==0) return;
    u32 desired=caps.minImageCount+1; if(caps.maxImageCount>0 && desired>caps.maxImageCount) desired=caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha=choose_alpha(caps);
    VkSwapchainCreateInfoKHR sci={ .sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface=a->surface, .minImageCount=desired, .imageFormat=a->swap_fmt, .imageColorSpace=sf.colorSpace, .imageExtent=a->swap_extent, .imageArrayLayers=1, .imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .imageSharingMode=VK_SHARING_MODE_EXCLUSIVE, .preTransform=caps.currentTransform, .compositeAlpha=alpha, .presentMode=pm, .clipped=VK_TRUE, .oldSwapchain=a->swapchain };
    VK_CHECK(vkCreateSwapchainKHR(a->device,&sci,NULL,&a->swapchain)); if(sci.oldSwapchain) vkDestroySwapchainKHR(a->device,sci.oldSwapchain,NULL);

    VK_CHECK(vkGetSwapchainImagesKHR(a->device,a->swapchain,&a->sc_count,NULL)); if(a->sc_count>8)a->sc_count=8; VK_CHECK(vkGetSwapchainImagesKHR(a->device,a->swapchain,&a->sc_count,a->sc_img));
    for (u32 i = 0; i < a->sc_count; i++)
    {
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = a->sc_img[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = a->swap_fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
        };
        VK_CHECK(vkCreateImageView(a->device,&vci,NULL,&a->sc_view[i]));
    }

    VkImageView off_att[] = { a->off_view };
    VkFramebufferCreateInfo fciA = { .sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass=a->rpA,
        .attachmentCount=1, .pAttachments=off_att, .width=OFF_W, .height=OFF_H, .layers=1 };
    VK_CHECK(vkCreateFramebuffer(a->device, &fciA, NULL, &a->off_fb));

    for(u32 i=0;i<a->sc_count;i++){ VkImageView atts[]={ a->sc_view[i] }; VkFramebufferCreateInfo fciB={ .sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass=a->rpB, .attachmentCount=1, .pAttachments=atts, .width=a->swap_extent.width, .height=a->swap_extent.height, .layers=1 }; VK_CHECK(vkCreateFramebuffer(a->device,&fciB,NULL,&a->fb[i])); }

    VkCommandPoolCreateInfo cpci={ .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex=a->qfam, .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT }; VK_CHECK(vkCreateCommandPool(a->device,&cpci,NULL,&a->frame_pool));
    VkCommandBufferAllocateInfo cai={ .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool=a->frame_pool, .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount=a->sc_count }; VK_CHECK(vkAllocateCommandBuffers(a->device,&cai,a->frame_cb));

    for(u32 i=0;i<a->sc_count;i++){
        VkCommandBufferBeginInfo bi={ .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; VK_CHECK(vkBeginCommandBuffer(a->frame_cb[i],&bi));
        VkClearValue clrA={ .color={{0.02f,0.02f,0.05f,1.0f}}}; VkRenderPassBeginInfo rbiA={ .sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass=a->rpA, .framebuffer=a->off_fb, .renderArea={{0,0},{OFF_W,OFF_H}}, .clearValueCount=1, .pClearValues=&clrA };
        vkCmdBeginRenderPass(a->frame_cb[i],&rbiA,VK_SUBPASS_CONTENTS_INLINE); VkViewport vpA={0,0,(float)OFF_W,(float)OFF_H,0,1}; VkRect2D scA={{0,0},{OFF_W,OFF_H}};
        vkCmdSetViewport(a->frame_cb[i],0,1,&vpA); vkCmdSetScissor(a->frame_cb[i],0,1,&scA); vkCmdBindPipeline(a->frame_cb[i],VK_PIPELINE_BIND_POINT_GRAPHICS,a->gpA); vkCmdBindDescriptorSets(a->frame_cb[i],VK_PIPELINE_BIND_POINT_GRAPHICS,a->plA,0,1,&a->dsetA,0,NULL); vkCmdDraw(a->frame_cb[i],3,1,0,0); vkCmdEndRenderPass(a->frame_cb[i]);

        // make sure write to read on texture happens in order (not 100% necessary here)
        VkImageMemoryBarrier toSample = {
            .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, // finalLayout of pass A
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = a->off_img,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        };
        vkCmdPipelineBarrier(a->frame_cb[i], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &toSample);

        VkClearValue clrB={ .color={{0,0,0,1}}}; VkRenderPassBeginInfo rbiB={ .sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass=a->rpB, .framebuffer=a->fb[i], .renderArea={{0,0},{a->swap_extent.width,a->swap_extent.height}}, .clearValueCount=1, .pClearValues=&clrB };
        vkCmdBeginRenderPass(a->frame_cb[i],&rbiB,VK_SUBPASS_CONTENTS_INLINE); VkViewport vpB={0,0,(float)a->swap_extent.width,(float)a->swap_extent.height,0,1}; VkRect2D scB={{0,0},{a->swap_extent.width,a->swap_extent.height}};
        vkCmdSetViewport(a->frame_cb[i],0,1,&vpB); vkCmdSetScissor(a->frame_cb[i],0,1,&scB); vkCmdBindPipeline(a->frame_cb[i],VK_PIPELINE_BIND_POINT_GRAPHICS,a->gpB); vkCmdBindDescriptorSets(a->frame_cb[i],VK_PIPELINE_BIND_POINT_GRAPHICS,a->plB,0,1,&a->dsetB,0,NULL); vkCmdDraw(a->frame_cb[i],3,1,0,0); vkCmdEndRenderPass(a->frame_cb[i]);

        VK_CHECK(vkEndCommandBuffer(a->frame_cb[i]));
    }

    VkSemaphoreCreateInfo si={ .sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO }; VkFenceCreateInfo fi={ .sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags=VK_FENCE_CREATE_SIGNALED_BIT };
    VK_CHECK(vkCreateSemaphore(a->device,&si,NULL,&a->sem_acquire)); VK_CHECK(vkCreateSemaphore(a->device,&si,NULL,&a->sem_render)); VK_CHECK(vkCreateFence(a->device,&fi,NULL,&a->fence));
}

/* --- public API (called by your main) --- */
static struct VkApp g_app; static struct VkApp* g_app_ptr=&g_app; struct VkApp* vk_app_singleton(void){ return g_app_ptr; }

void vk_init_instance(void){ memset(&g_app,0,sizeof(g_app)); app_create_instance(&g_app); }
void vk_choose_phys_and_queue(void){ app_pick_phys_and_qfam(&g_app); }
void vk_make_device(void){ app_create_device_and_queue(&g_app); }
#ifdef __linux__
void vk_create_surface(void* display, void* surface); /* already defined above */
#endif
#ifdef _WIN32
void vk_create_surface(void* hinst, void* hwnd);     /* already defined above */
#endif

void vk_graph_initial_build(u32 win_w, u32 win_h){
    app_upload_astc_from_embedded(&g_app); // offscreen needs the mip value so call this first
    app_create_offscreen(&g_app);
    app_create_descriptors(&g_app);
    /* choose swapchain format before pipelines so B matches swapchain */
    VkSurfaceFormatKHR sf=choose_format(&g_app); g_app.swap_fmt=sf.format;
    app_create_passes_pipes(&g_app, g_app.swap_fmt);
    app_create_swapchain_block(&g_app, win_w, win_h);
    pf_timestamp("graph_init");
}

static void destroy_swapchain_block(struct VkApp* a){
    if(a->fence){ vkDestroyFence(a->device,a->fence,NULL); a->fence=VK_NULL_HANDLE; }
    if(a->sem_render){ vkDestroySemaphore(a->device,a->sem_render,NULL); a->sem_render=VK_NULL_HANDLE; }
    if(a->sem_acquire){ vkDestroySemaphore(a->device,a->sem_acquire,NULL); a->sem_acquire=VK_NULL_HANDLE; }
    if(a->frame_pool){ vkDestroyCommandPool(a->device,a->frame_pool,NULL); a->frame_pool=VK_NULL_HANDLE; }
    for(u32 i=0;i<a->sc_count;i++){ if(a->fb[i]){ vkDestroyFramebuffer(a->device,a->fb[i],NULL); a->fb[i]=VK_NULL_HANDLE; } if(a->sc_view[i]){ vkDestroyImageView(a->device,a->sc_view[i],NULL); a->sc_view[i]=VK_NULL_HANDLE; } }
    if(a->rpB){ vkDestroyRenderPass(a->device,a->rpB,NULL); a->rpB=VK_NULL_HANDLE; }
    if(a->rpA){ vkDestroyRenderPass(a->device,a->rpA,NULL); a->rpA=VK_NULL_HANDLE; }
    if(a->swapchain){ vkDestroySwapchainKHR(a->device,a->swapchain,NULL); a->swapchain=VK_NULL_HANDLE; }
    if(a->dpool){ vkDestroyDescriptorPool(a->device,a->dpool,NULL); a->dpool=VK_NULL_HANDLE; }
    if(a->dsl){ vkDestroyDescriptorSetLayout(a->device,a->dsl,NULL); a->dsl=VK_NULL_HANDLE; }
    if(a->off_sampler){ vkDestroySampler(a->device,a->off_sampler,NULL); a->off_sampler=VK_NULL_HANDLE; }
    if(a->off_view){ vkDestroyImageView(a->device,a->off_view,NULL); a->off_view=VK_NULL_HANDLE; }
    if(a->off_img){ vkDestroyImage(a->device,a->off_img,NULL); a->off_img=VK_NULL_HANDLE; }
    if(a->off_mem){ vkFreeMemory(a->device,a->off_mem,NULL); a->off_mem=VK_NULL_HANDLE; }
    if(a->gpB){ vkDestroyPipeline(a->device,a->gpB,NULL); a->gpB=VK_NULL_HANDLE; }
    if(a->plB){ vkDestroyPipelineLayout(a->device,a->plB,NULL); a->plB=VK_NULL_HANDLE; }
    if(a->gpA){ vkDestroyPipeline(a->device,a->gpA,NULL); a->gpA=VK_NULL_HANDLE; }
    if(a->plA){ vkDestroyPipelineLayout(a->device,a->plA,NULL); a->plA=VK_NULL_HANDLE; }
    if(a->tex_view){ vkDestroyImageView(a->device,a->tex_view,NULL); a->tex_view=VK_NULL_HANDLE; }
    if(a->tex_img){ vkDestroyImage(a->device,a->tex_img,NULL); a->tex_img=VK_NULL_HANDLE; }
    if(a->tex_mem){ vkFreeMemory(a->device,a->tex_mem,NULL); a->tex_mem=VK_NULL_HANDLE; }
    if (a->off_fb) { vkDestroyFramebuffer(a->device, a->off_fb, NULL); a->off_fb = VK_NULL_HANDLE; }
}

static void recreate_all(struct VkApp* a, u32 w, u32 h){
    vkDeviceWaitIdle(a->device);
    destroy_swapchain_block(a);
    app_create_offscreen(a);
    app_upload_astc_from_embedded(a);
    app_create_descriptors(a);
    VkSurfaceFormatKHR sf=choose_format(a); a->swap_fmt=sf.format;
    app_create_passes_pipes(a,a->swap_fmt);
    app_create_swapchain_block(a,w,h);
}

void vk_recreate_all(u32 w, u32 h){ recreate_all(&g_app,w,h); }

int vk_present_frame(void)
{
    struct VkApp* a = &g_app;
    u32 idx = 0;
    VkResult r;
    VK_CHECK(vkWaitForFences(a->device,1,&a->fence,VK_TRUE,UINT64_MAX));
    VK_CHECK(vkResetFences(a->device,1,&a->fence));
    // --- THIS BLOCKS THE CPU UNTIL WE GET TO THE NEXT FRAME (VSYNC, when using FIFO)
    r = vkAcquireNextImageKHR(a->device, a->swapchain,UINT64_MAX, a->sem_acquire,VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return 1;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
    {
        fprintf(stderr, "acquire %d\n", (int)r);
        exit(1);
    }
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &a->sem_acquire,
        .pWaitDstStageMask = &waitStage, .commandBufferCount = 1, .pCommandBuffers = &a->frame_cb[idx],
        .signalSemaphoreCount = 1, .pSignalSemaphores = &a->sem_render
    };
    VK_CHECK(vkQueueSubmit(a->queue,1,&si,a->fence));
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &a->sem_render,
        .swapchainCount = 1, .pSwapchains = &a->swapchain, .pImageIndices = &idx
    };
    r = vkQueuePresentKHR(a->queue, &pi);
    return (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) ? 1 : 0;
}

void vk_shutdown_all(void){
    struct VkApp* a=&g_app;
    if(a->device){ vkDeviceWaitIdle(a->device); destroy_swapchain_block(a); vkDestroyDevice(a->device,NULL); a->device=VK_NULL_HANDLE; }
    if(a->surface){ vkDestroySurfaceKHR(a->instance,a->surface,NULL); a->surface=VK_NULL_HANDLE; }
    if(a->instance){ vkDestroyInstance(a->instance,NULL); a->instance=VK_NULL_HANDLE; }
}


