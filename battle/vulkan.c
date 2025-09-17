#include "header.h"

#if defined(_WIN32)
  #define VK_USE_PLATFORM_WIN32_KHR
  #include <vulkan/vulkan_win32.h>
#else
  #define VK_USE_PLATFORM_XLIB_KHR
  extern int putenv(char*);
#endif
#include <vulkan/vulkan.h>

extern const unsigned char font_atlas[];
extern const unsigned char font_atlas_end[];
#define font_atlas_len ((size_t)(font_atlas_end - font_atlas))

extern const unsigned char shaders[];
extern const unsigned char shaders_end[];
#define shaders_len ((size_t)(shaders_end - shaders))

#define TARGET_FORMAT VK_FORMAT_B8G8R8A8_SRGB
#define TARGET_COLOR_SPACE VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
#define OFF_W 1920u
#define OFF_H 1080u
#define FB_COUNT 2                /* “double-buffered” behavior => 1 frame in flight */
#define MAX_SC_IMG 8              /* upper bound we’ll handle from the runtime */

struct VulkanState {
    // Fundamental vulkan objects
    VkInstance instance;
    VkSurfaceKHR surface;
    VkDevice device;
    VkPhysicalDevice physical_device;
    VkPhysicalDeviceMemoryProperties device_memory_properties;
    VkQueue queue;
    u32 queue_family_index;

    // Texture
    VkImage texture_image;
    VkDeviceMemory texture_memory;
    VkImageView texture_view;
    u32 number_of_mip_levels;
    VkFormat texture_format;

    /* 'offscreen' render target before blitting to fullscreen resolution */
    VkImage off_img;
    VkDeviceMemory off_mem;
    VkImageView off_view;
    VkSampler off_sampler;
    VkFramebuffer off_fb;

    /* descriptors */
    VkDescriptorSetLayout dsl;
    VkDescriptorPool dpool;
    VkDescriptorSet dsetA;
    VkDescriptorSet dsetB;

    /* passes/pipes */
    VkPipelineLayout plA;
    VkPipeline gpA;
    VkRenderPass rpA;
} vk;

static PFN_vkGetCalibratedTimestampsEXT  pfnGetCalibratedTimestampsEXT  = NULL;
static PFN_vkWaitForPresentKHR           pfnWaitForPresentKHR           = NULL;

/* -------- timing additions (global/static) ------------------------------ */
static VkQueryPool g_qp = VK_NULL_HANDLE;
static double g_timestamp_period_ns = 1.0;   /* ns per GPU tick */
static uint64_t g_gpu_calib = 0;             /* device ticks at calibration */
static uint64_t g_cpu_calib_ns = 0;          /* CLOCK_MONOTONIC ns at calibration */
static uint32_t g_present_counter = 0;       /* monotonically increasing present IDs */

/* Convert GPU ticks -> CLOCK_MONOTONIC ns using the calibration pair */
static inline double gpu_ticks_to_cpu_ns(uint64_t gpu_ticks){
    double gpu_calib_ns = (double)g_gpu_calib * g_timestamp_period_ns;
    double gpu_ts_ns    = (double)gpu_ticks   * g_timestamp_period_ns;
    return (double)g_cpu_calib_ns + (gpu_ts_ns - gpu_calib_ns);
}

static const char* vk_result_str(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    default: return "VK_RESULT_UNKNOWN";
    }
}
#define VULKAN_CALL(x) do{ VkResult _r=(x); if(_r!=VK_SUCCESS){ printf("vulkan error: %s on line @%s:%d\n",vk_result_str(_r),__FILE__,__LINE__); _exit(1);} }while(0)

static void create_timestamps_and_calibrate()
{
    /* Query timestampPeriod for tick->ns conversion */
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk.physical_device, &props);
    g_timestamp_period_ns = (double)props.limits.timestampPeriod;

    /* Create a pool large enough for begin/end per swapchain image */
    VkQueryPoolCreateInfo qpci = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = MAX_SC_IMG * 2
    };
    VULKAN_CALL(vkCreateQueryPool(vk.device, &qpci, NULL, &g_qp));

    /* Calibrate GPU (device) vs CLOCK_MONOTONIC */
    VkCalibratedTimestampInfoEXT infos[2] = {
        { .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT,
          .timeDomain = VK_TIME_DOMAIN_DEVICE_EXT },
        { .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT,
          .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT },
    };
    uint64_t ts[2]; uint64_t maxDev = 0;
    VULKAN_CALL(pfnGetCalibratedTimestampsEXT(vk.device, 2, infos, ts, &maxDev));
    g_gpu_calib     = ts[0];
    g_cpu_calib_ns  = ts[1];
}

#pragma region DEBUG
static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT types,const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data) {
    u32 error = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    u32 warning = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    if (error || warning) {
        const char* sev =
            (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR" :
            (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "WARN " :
            (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) ? "INFO " : "VERB ";
        printf("[Vulkan][%s] %s\n", sev, data->pMessage);
    }
    return VK_FALSE;
}

static VkResult CreateDebugUtilsMessengerEXT(
    VkInstance inst, const VkDebugUtilsMessengerCreateInfoEXT* ci,
    const VkAllocationCallbacks* alloc, VkDebugUtilsMessengerEXT* out)
{
    PFN_vkCreateDebugUtilsMessengerEXT fp =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(inst, "vkCreateDebugUtilsMessengerEXT");
    return fp ? fp(inst, ci, alloc, out) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

static void DestroyDebugUtilsMessengerEXT(
    VkInstance inst, VkDebugUtilsMessengerEXT msgr, const VkAllocationCallbacks* alloc)
{
    PFN_vkDestroyDebugUtilsMessengerEXT fp =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(inst, "vkDestroyDebugUtilsMessengerEXT");
    if (fp) fp(inst, msgr, alloc);
}
#pragma endregion

#pragma region SETUP
void vk_init(void* display_or_hinst, void* surface_or_hwnd) {
#if (DEBUG == 1 && defined(__linux__))
    putenv("VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json");
    putenv("VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json");
#endif

#if DEBUG == 1
    const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
    const uint32_t layer_count = 1;
#else
    const char** layers = NULL; uint32_t layer_count = 0;
#endif

    VkApplicationInfo ai = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = APP_NAME,
        .applicationVersion = VK_MAKE_VERSION(0,0,0),
        .pEngineName = APP_NAME,
        .engineVersion = VK_MAKE_VERSION(0,0,0),
        .apiVersion = VK_API_VERSION_1_3,  // fall back to 1.2 if needed
    };
#if defined(_WIN32)
    const char* exts[] = {"VK_KHR_surface","VK_KHR_win32_surface","VK_EXT_debug_utils"};
#else
    const char* exts[] = {"VK_KHR_surface","VK_KHR_xlib_surface","VK_EXT_debug_utils"};
#endif
    const uint32_t ext_count = sizeof(exts)/sizeof(exts[0]);
    VkDebugUtilsMessengerCreateInfoEXT dbg_ci = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
        .messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = vk_debug_cb,
        .pUserData = NULL
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &ai,
        .enabledExtensionCount = ext_count,
        .ppEnabledExtensionNames = exts,
        .enabledLayerCount = layer_count,
        .ppEnabledLayerNames = layers,
        .pNext = &dbg_ci
    };
    VULKAN_CALL(vkCreateInstance(&ici, NULL, &vk.instance));
    pf_timestamp("vkCreateInstance");
#ifdef DEBUG
    VkDebugUtilsMessengerEXT debug_msgr = VK_NULL_HANDLE;
    VULKAN_CALL(CreateDebugUtilsMessengerEXT(vk.instance, &dbg_ci, NULL, &debug_msgr));
#endif

#ifdef __linux__
    Display* dpy = (Display*)display_or_hinst;     // from pf_display_or_instance()
    Window   win = (Window)(uintptr_t)surface_or_hwnd; // from pf_surface_or_hwnd()
    VkXlibSurfaceCreateInfoKHR sci = {
        .sType   = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
        .dpy     = dpy,
        .window  = win
    };
    VULKAN_CALL(vkCreateXlibSurfaceKHR(vk.instance,&sci,NULL,&vk.surface));
#else
    VkWin32SurfaceCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .hinstance = display_or_hinst, .hwnd = surface_or_hwnd
    };
    VULKAN_CALL(vkCreateWin32SurfaceKHR(vk.instance, &sci, NULL, &vk.surface));
#endif
    pf_timestamp("created vk surface");

    // Pick physical device (simple: first available)
    uint32_t n=0; VULKAN_CALL(vkEnumeratePhysicalDevices(vk.instance,&n,NULL));
    if(!n){printf("no phys\n"); _exit(1);}
    if (n>8) n=8; VkPhysicalDevice d[8];
    VULKAN_CALL(vkEnumeratePhysicalDevices(vk.instance,&n,d));
    vk.physical_device=d[0]; vk.queue_family_index=0;  /* assumes family 0 works */
    vkGetPhysicalDeviceMemoryProperties(vk.physical_device, &vk.device_memory_properties);
    pf_timestamp("pick phys+queue");

    float pr = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = vk.queue_family_index, .queueCount = 1,
        .pQueuePriorities = &pr
    };

    /* ---- enable required device extensions ---- */
    const char* device_exts[] = {
        "VK_KHR_swapchain",
        "VK_KHR_present_id",
        "VK_KHR_present_wait",
        "VK_EXT_calibrated_timestamps",
    };

    // enable a bunch of features
    VkPhysicalDeviceFeatures2 features = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan11Features v11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    v11.shaderDrawParameters = 1;
    VkPhysicalDeviceVulkan12Features v12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    v12.timelineSemaphore = 1;v12.bufferDeviceAddress = 1;v12.scalarBlockLayout = 1;v12.hostQueryReset = 1;
    VkPhysicalDeviceVulkan13Features v13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    v13.synchronization2 = 1; v13.dynamicRendering = 1;
    VkPhysicalDevicePresentIdFeaturesKHR presentIdFeat = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
        .presentId = 1
    };
    VkPhysicalDevicePresentWaitFeaturesKHR presentWaitFeat = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
        .presentWait = 1
    };
    features.pNext = &v11; v11.pNext = &v12; v12.pNext = &v13;
    v13.pNext = &presentIdFeat;
    presentIdFeat.pNext = &presentWaitFeat;
    vkGetPhysicalDeviceFeatures2(vk.physical_device, &features);

    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &features,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = (uint32_t)(sizeof(device_exts)/sizeof(device_exts[0])),
        .ppEnabledExtensionNames = device_exts
    };
    VULKAN_CALL(vkCreateDevice(vk.physical_device,&dci,NULL,&vk.device));
    vkGetDeviceQueue(vk.device, vk.queue_family_index, 0, &vk.queue);
    pf_timestamp("vkCreateDevice");

    /* fetch extension entry points (required for tcc / dynamic loaders) */
    pfnGetCalibratedTimestampsEXT =
        (PFN_vkGetCalibratedTimestampsEXT)vkGetDeviceProcAddr(vk.device, "vkGetCalibratedTimestampsEXT");
    pfnWaitForPresentKHR =
        (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(vk.device, "vkWaitForPresentKHR");

    if (!pfnGetCalibratedTimestampsEXT) {
        printf("Fatal: VK_EXT_calibrated_timestamps not available at runtime.\n");
        _exit(1);
    }
    if (!pfnWaitForPresentKHR) {
        printf("Fatal: VK_KHR_present_wait not available at runtime.\n");
        _exit(1);
    }

    /* Timestamp query pool + GPU/CPU calibration (EXT_calibrated_timestamps) */
    create_timestamps_and_calibrate();
}
#pragma endregion

/* ------------------------------- (rest of your resource setup unchanged) */
struct KTX2Header{
    u8  identifier[12];                 /* «KTX 20»\r\n\x1A\n */
    u32 vkFormat;
    u32 typeSize;
    u32 pixelWidth, pixelHeight, pixelDepth;
    u32 layerCount, faceCount, levelCount, supercompressionScheme;
    u32 dfdByteOffset, dfdByteLength;
    u32 kvdByteOffset, kvdByteLength;
    u64 sgdByteOffset, sgdByteLength;
};
struct KTX2LevelIndex{ u64 byteOffset, byteLength, uncompressedByteLength; };

void vk_create_resources(void) {
    // Parse the texture memory header
    const u8* file = font_atlas; const size_t file_len = font_atlas_len;
    struct KTX2Header hdr; memcpy(&hdr, file, sizeof hdr);
    static const u8 magic[12]={0xAB,'K','T','X',' ','2','0',0xBB,0x0D,0x0A,0x1A,0x0A};
    if (memcmp(hdr.identifier, magic, 12) != 0) { printf("not a KTX2\n"); _exit(1); }
    VkFormat fmt = (VkFormat)hdr.vkFormat;
    if (!(fmt==VK_FORMAT_ASTC_12x12_UNORM_BLOCK || fmt==VK_FORMAT_ASTC_12x12_SRGB_BLOCK)) { printf("need ASTC 12x12\n"); _exit(1); }

    // **Probe support before creating the image**
    VkImageFormatProperties ifp;
    VkResult fr = vkGetPhysicalDeviceImageFormatProperties(
        vk.physical_device, fmt, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &ifp);
    if (fr != VK_SUCCESS) {
        printf("ASTC 12x12 not supported for sampled images on this device (format %d). Cannot proceed without transcoding.\n", fmt);
        _exit(1);
    }

    const u32 mips = hdr.levelCount;
    const struct KTX2LevelIndex* lv = (const struct KTX2LevelIndex*)(file + sizeof(struct KTX2Header));
    size_t total = 0;
    for (u32 i=0;i<mips;i++){
        total += (size_t)lv[i].byteLength;
        if (file_len < (size_t)lv[i].byteOffset + (size_t)lv[i].byteLength) { printf("ktx2 trunc\n"); _exit(1); }
    }

    // Texture image + memory
    VkImageCreateInfo ici={ .sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType=VK_IMAGE_TYPE_2D, .format=fmt,
        .extent={(u32)hdr.pixelWidth,(u32)hdr.pixelHeight,1}, .mipLevels=mips, .arrayLayers=1, .samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL, .usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT };
    VULKAN_CALL(vkCreateImage(vk.device,&ici,NULL,&vk.texture_image));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(vk.device,vk.texture_image,&mr);
    if (mr.size == 0) { printf("image memory size is zero\n"); _exit(1); }
    VkMemoryPropertyFlags req = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    u32 mem_type = 0; int found=0;
    for (u32 i=0;i<vk.device_memory_properties.memoryTypeCount;i++)
        if (mr.memoryTypeBits & 1u<<i && (vk.device_memory_properties.memoryTypes[i].propertyFlags & req)==req) { mem_type=i; found=1; break; }
    if (!found) { printf("no device local memory type\n"); _exit(1); }
    VkMemoryAllocateInfo mai={ .sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize=mr.size, .memoryTypeIndex=mem_type };
    VULKAN_CALL(vkAllocateMemory(vk.device,&mai,NULL,&vk.texture_memory));
    VULKAN_CALL(vkBindImageMemory(vk.device,vk.texture_image,vk.texture_memory,0));

    // Staging buffer
    VkBuffer stg=NULL; VkDeviceMemory stg_mem=NULL;
    VkBufferCreateInfo bci={ .sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size=(VkDeviceSize)total, .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode=VK_SHARING_MODE_EXCLUSIVE };
    VULKAN_CALL(vkCreateBuffer(vk.device,&bci,NULL,&stg));
    VkMemoryRequirements bmr; vkGetBufferMemoryRequirements(vk.device,stg,&bmr);
    u32 type=0; VkMemoryPropertyFlags flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT; found=0;
    for (u32 i=0;i<vk.device_memory_properties.memoryTypeCount;i++)
        if (bmr.memoryTypeBits & 1u<<i && (vk.device_memory_properties.memoryTypes[i].propertyFlags & flags)==flags) { type=i; found=1; break; }
    if (!found) { printf("no host visible memory type\n"); _exit(1); }
    VkMemoryAllocateInfo bmai={ .sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize=bmr.size, .memoryTypeIndex=type };
    VULKAN_CALL(vkAllocateMemory(vk.device,&bmai,NULL,&stg_mem));
    VULKAN_CALL(vkBindBufferMemory(vk.device,stg,stg_mem,0));

    static const int MAX_MIPS=16;
    size_t offs[MAX_MIPS]; size_t off=0;
    void* map=0; VULKAN_CALL(vkMapMemory(vk.device,stg_mem,0,(VkDeviceSize)total,0,&map));
    for (u32 i=0;i<mips;i++){
        off = (off + 15) & ~((size_t)15);
        offs[i]=off;
        memcpy((u8*)map+off, file+(size_t)lv[i].byteOffset, (size_t)lv[i].byteLength);
        off += (size_t)lv[i].byteLength;
    }
    vkUnmapMemory(vk.device,stg_mem);

    // Immediate copy + layout
    VkCommandPool ipool; VkCommandBuffer icb;
    VkCommandPoolCreateInfo cpci={ .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex=vk.queue_family_index, .flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT|VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    VULKAN_CALL(vkCreateCommandPool(vk.device,&cpci,NULL,&ipool));
    VkCommandBufferAllocateInfo ai={ .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool=ipool, .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount=1 };
    VULKAN_CALL(vkAllocateCommandBuffers(vk.device,&ai,&icb));
    VkCommandBufferBeginInfo bi={ .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VULKAN_CALL(vkBeginCommandBuffer(icb,&bi));

    VkBufferMemoryBarrier bmb={ .sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask=VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT, .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .buffer=stg, .offset=0, .size=off };
    vkCmdPipelineBarrier(icb, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &bmb, 0, NULL);

    VkImageMemoryBarrier to_dst={ .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask=0, .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED, .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=vk.texture_image, .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,mips,0,1}};
    vkCmdPipelineBarrier(icb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);

    u32 pw=(u32)hdr.pixelWidth, ph=(u32)hdr.pixelHeight;
    for (u32 i=0;i<mips;i++){
        VkBufferImageCopy bic={ .bufferOffset=offs[i], .bufferRowLength=0, .bufferImageHeight=0,
            .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,i,0,1}, .imageOffset={0,0,0}, .imageExtent={ pw?pw:1, ph?ph:1, 1 } };
        vkCmdCopyBufferToImage(icb, stg, vk.texture_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        if (pw>1) pw>>=1; if (ph>1) ph>>=1;
    }

    VkImageMemoryBarrier to_ro={ .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask=VK_ACCESS_SHADER_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=vk.texture_image, .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,mips,0,1}};
    vkCmdPipelineBarrier(icb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &to_ro);

    VULKAN_CALL(vkEndCommandBuffer(icb));
    VkSubmitInfo si={ .sType=VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount=1, .pCommandBuffers=&icb };
    VULKAN_CALL(vkQueueSubmit(vk.queue,1,&si,NULL));
    VULKAN_CALL(vkQueueWaitIdle(vk.queue));
    vkFreeCommandBuffers(vk.device, ipool, 1, &icb);
    vkDestroyCommandPool(vk.device, ipool, NULL);
    vkDestroyBuffer(vk.device, stg, NULL); vkFreeMemory(vk.device, stg_mem, NULL);

    VkImageViewCreateInfo vci={ .sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image=vk.texture_image, .viewType=VK_IMAGE_VIEW_TYPE_2D, .format=fmt, .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,mips,0,1}};
    VULKAN_CALL(vkCreateImageView(vk.device,&vci,NULL,&vk.texture_view));

    vk.number_of_mip_levels = mips; vk.texture_format = fmt;

    // --- Offscreen target (fixed OFF_W/H) ---
    VkImageCreateInfo create_info={ .sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType=VK_IMAGE_TYPE_2D, .format=TARGET_FORMAT,
        .extent={OFF_W,OFF_H,1}, .mipLevels=1, .arrayLayers=1, .samples=VK_SAMPLE_COUNT_1_BIT, .tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT, .sharingMode=VK_SHARING_MODE_EXCLUSIVE, .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED };
    VULKAN_CALL(vkCreateImage(vk.device,&create_info,NULL,&vk.off_img));
    VkMemoryRequirements mem_reqs; vkGetImageMemoryRequirements(vk.device,vk.off_img,&mem_reqs);
    u32 m_type=0; VkMemoryPropertyFlags m_flags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT; int m_found=0;
    for (u32 i=0;i<vk.device_memory_properties.memoryTypeCount;i++)
        if (mem_reqs.memoryTypeBits & 1u<<i && (vk.device_memory_properties.memoryTypes[i].propertyFlags & m_flags)==m_flags) { m_type=i; m_found=1; break; }
    if (!m_found) { printf("no device local memory type for offscreen\n"); _exit(1); }
    VkMemoryAllocateInfo allocate_info={ .sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize=mem_reqs.size, .memoryTypeIndex=m_type };
    VULKAN_CALL(vkAllocateMemory(vk.device,&allocate_info,NULL,&vk.off_mem));
    VULKAN_CALL(vkBindImageMemory(vk.device,vk.off_img,vk.off_mem,0));
    VkImageViewCreateInfo view_create_info={ .sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image=vk.off_img, .viewType=VK_IMAGE_VIEW_TYPE_2D, .format=TARGET_FORMAT, .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VULKAN_CALL(vkCreateImageView(vk.device,&view_create_info,NULL,&vk.off_view));
    VkSamplerCreateInfo sci={ .sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter=VK_FILTER_LINEAR, .minFilter=VK_FILTER_LINEAR, .mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU=VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeV=VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT, .minLod=0.0f, .maxLod=(f32)(vk.number_of_mip_levels-1) };
    VULKAN_CALL(vkCreateSampler(vk.device,&sci,NULL,&vk.off_sampler));

    // --- Descriptors ---
    VkDescriptorSetLayoutBinding b[2] = {
        {.binding=0, .descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT },
        {.binding=1, .descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER,      .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT }
    };
    VkDescriptorSetLayoutCreateInfo dlci={ .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount=2, .pBindings=b };
    VULKAN_CALL(vkCreateDescriptorSetLayout(vk.device,&dlci,NULL,&vk.dsl));
    VkDescriptorPoolSize ps[2]={{.type=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,.descriptorCount=2},{.type=VK_DESCRIPTOR_TYPE_SAMPLER,.descriptorCount=2}};
    VkDescriptorPoolCreateInfo dpci={ .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets=2, .poolSizeCount=2, .pPoolSizes=ps };
    VULKAN_CALL(vkCreateDescriptorPool(vk.device,&dpci,NULL,&vk.dpool));
    VkDescriptorSetLayout layouts[2]={ vk.dsl, vk.dsl }; VkDescriptorSetAllocateInfo dsai={ .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool=vk.dpool, .descriptorSetCount=2, .pSetLayouts=layouts };
    VkDescriptorSet sets[2]; VULKAN_CALL(vkAllocateDescriptorSets(vk.device,&dsai,sets)); vk.dsetA=sets[0]; vk.dsetB=sets[1];

    VkDescriptorImageInfo imgA={ .imageView=vk.texture_view, .imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo smpA={ .sampler=vk.off_sampler };
    VkWriteDescriptorSet W_A[2]={
        { .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet=vk.dsetA, .dstBinding=0, .descriptorCount=1, .descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo=&imgA },
        { .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet=vk.dsetA, .dstBinding=1, .descriptorCount=1, .descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER,      .pImageInfo=&smpA }
    };
    vkUpdateDescriptorSets(vk.device,2,W_A,0,NULL);

    VkDescriptorImageInfo imgB={ .imageView=vk.off_view, .imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo smpB={ .sampler=vk.off_sampler };
    VkWriteDescriptorSet W_B[2]={
        { .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet=vk.dsetB, .dstBinding=0, .descriptorCount=1, .descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo=&imgB },
        { .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet=vk.dsetB, .dstBinding=1, .descriptorCount=1, .descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER,      .pImageInfo=&smpB }
    };
    vkUpdateDescriptorSets(vk.device,2,W_B,0,NULL);

    // --- Render pass A + pipeline A (offscreen) ---
    VkAttachmentDescription aA={ .format=TARGET_FORMAT, .samples=VK_SAMPLE_COUNT_1_BIT, .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE, .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkAttachmentReference arA={ .attachment=0, .layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription spA={ .pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount=1, .pColorAttachments=&arA };
    VkRenderPassCreateInfo rpciA={ .sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount=1, .pAttachments=&aA, .subpassCount=1, .pSubpasses=&spA };
    VULKAN_CALL(vkCreateRenderPass(vk.device,&rpciA,NULL,&vk.rpA));

    VkImageView off_att[] = { vk.off_view };
    VkFramebufferCreateInfo fciA={ .sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass=vk.rpA, .attachmentCount=1, .pAttachments=off_att, .width=OFF_W, .height=OFF_H, .layers=1 };
    VULKAN_CALL(vkCreateFramebuffer(vk.device,&fciA,NULL,&vk.off_fb));

    VkShaderModuleCreateInfo smci={ .sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize=shaders_len, .pCode=(const u32*)shaders };
    VkShaderModule mod; VULKAN_CALL(vkCreateShaderModule(vk.device,&smci,NULL,&mod));
    VkPipelineShaderStageCreateInfo stA[2]={
        { .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage=VK_SHADER_STAGE_VERTEX_BIT,   .module=mod, .pName="VS_Tri" },
        { .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage=VK_SHADER_STAGE_FRAGMENT_BIT, .module=mod, .pName="PS_Tri" }
    };
    VkPipelineVertexInputStateCreateInfo vin={ .sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia={ .sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vpst={ .sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount=1, .scissorCount=1 };
    VkDynamicState dyns[2]={ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR }; VkPipelineDynamicStateCreateInfo dyn={ .sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount=2, .pDynamicStates=dyns };
    VkPipelineRasterizationStateCreateInfo rs={ .sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode=VK_POLYGON_MODE_FILL, .cullMode=VK_CULL_MODE_NONE, .frontFace=VK_FRONT_FACE_CLOCKWISE, .lineWidth=1.0f };
    VkPipelineMultisampleStateCreateInfo ms={ .sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba={ .colorWriteMask=0xF, .blendEnable=VK_FALSE }; VkPipelineColorBlendStateCreateInfo cb={ .sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount=1, .pAttachments=&cba };
    VkPipelineLayoutCreateInfo plciA={ .sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount=1, .pSetLayouts=&vk.dsl };
    VULKAN_CALL(vkCreatePipelineLayout(vk.device,&plciA,NULL,&vk.plA));
    VkGraphicsPipelineCreateInfo pciA={ .sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount=2, .pStages=stA, .pVertexInputState=&vin, .pInputAssemblyState=&ia, .pViewportState=&vpst, .pDynamicState=&dyn, .pRasterizationState=&rs, .pMultisampleState=&ms, .pColorBlendState=&cb, .layout=vk.plA, .renderPass=vk.rpA, .subpass=0 };
    VULKAN_CALL(vkCreateGraphicsPipelines(vk.device,NULL,1,&pciA,NULL,&vk.gpA));
    vkDestroyShaderModule(vk.device,mod,NULL);
    pf_timestamp("created vulkan resources");
}

/* ---------------------------- per-frame rendering with accurate timing --- */
void vk_render_frame(u32 win_w, u32 win_h) {
    // swapchain
    static VkSwapchainKHR swapchain;
    static VkFormat swap_fmt;
    static VkExtent2D swap_extent;

    static u32 sc_count = 0;
    static VkImage     sc_img[MAX_SC_IMG];
    static VkImageView sc_view[MAX_SC_IMG];
    static VkFramebuffer fb[MAX_SC_IMG];

    // post-process pipeline for scaling
    static VkPipelineLayout plB;
    static VkPipeline gpB;
    static VkRenderPass rpB;

    // cmd pool + per-image command buffers (kept as-is)
    static VkCommandPool frame_pool;
    static VkCommandBuffer frame_cb[MAX_SC_IMG];

    // ---- sync (NUM_FRAMES CPU pacing; per-image renderFinished) ----
    enum { NUM_FRAMES = 2 };
    static VkSemaphore imageAvailable[NUM_FRAMES];     // per-frame acquire
    static VkFence     inFlight[NUM_FRAMES];           // per-frame CPU pacing
    static VkSemaphore renderFinished_img[MAX_SC_IMG]; // per-image present wait

    static u32 curFrame = 0;

    static int built = 0;
    static int built_sync = 0;

    if (!built)
    {
        // caps / extent
        VkSurfaceCapabilitiesKHR caps;
        VULKAN_CALL(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical_device, vk.surface, &caps));

        // formats
        uint32_t fcount = 0;
        VULKAN_CALL(vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical_device, vk.surface, &fcount, NULL));
        if (fcount == 0) { printf("no surface formats\n"); _exit(1); }
        VkSurfaceFormatKHR fmts[128];
        VkResult rf = vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical_device, vk.surface, &fcount, fmts);
        if (rf != VK_SUCCESS && rf != VK_INCOMPLETE) { printf("surface formats: %s\n", vk_result_str(rf)); _exit(1); }
        VkSurfaceFormatKHR chosen = fmts[0];
        swap_fmt = chosen.format;
        VkColorSpaceKHR swap_cs = chosen.colorSpace;

        // present mode (FIFO is always available; prefer MAILBOX if you later probe it)
        VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;

        // extent
        if (caps.currentExtent.width != UINT32_MAX) swap_extent = caps.currentExtent;
        else swap_extent = (VkExtent2D){win_w, win_h};
        if (swap_extent.width==0 || swap_extent.height==0) return;

        // composite alpha
        const VkCompositeAlphaFlagBitsKHR order[] = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
        };
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for (u32 i=0;i<sizeof(order)/sizeof(order[0]);i++) if (caps.supportedCompositeAlpha & order[i]) { alpha=order[i]; break; }

        // request 2, clamp to device
        uint32_t desired = 2;
        if (desired < caps.minImageCount) desired = caps.minImageCount;
        if (caps.maxImageCount && desired > caps.maxImageCount) desired = caps.maxImageCount;
        printf("Got a swapchain with minimum %u images\n", desired);

        VkSwapchainCreateInfoKHR sc_ci = {
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = vk.surface,
            .minImageCount = desired,
            .imageFormat = swap_fmt,
            .imageColorSpace = swap_cs,
            .imageExtent = swap_extent,
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform = caps.currentTransform,
            .compositeAlpha = alpha,
            .presentMode = pm,
            .clipped = 1,
            .oldSwapchain = VK_NULL_HANDLE
        };
        VULKAN_CALL(vkCreateSwapchainKHR(vk.device,&sc_ci,NULL,&swapchain));

        // enumerate images
        VULKAN_CALL(vkGetSwapchainImagesKHR(vk.device, swapchain, &sc_count, NULL));
        if (sc_count > MAX_SC_IMG) sc_count = MAX_SC_IMG;
        VULKAN_CALL(vkGetSwapchainImagesKHR(vk.device, swapchain, &sc_count, sc_img));

        // views
        for (u32 i=0;i<sc_count;i++){
            VkImageViewCreateInfo vci={ .sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image=sc_img[i], .viewType=VK_IMAGE_VIEW_TYPE_2D, .format=swap_fmt, .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
            VULKAN_CALL(vkCreateImageView(vk.device,&vci,NULL,&sc_view[i]));
        }

        // Render pass B
        VkAttachmentDescription aB = {
            .format = swap_fmt,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
        };
        VkAttachmentReference arB={ .attachment=0, .layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription spB={ .pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount=1, .pColorAttachments=&arB };
        VkSubpassDependency dep = {
            .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
            .srcStageMask =
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        };
        VkRenderPassCreateInfo rpciB={ .sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount=1, .pAttachments=&aB, .subpassCount=1, .pSubpasses=&spB, .dependencyCount=1, .pDependencies=&dep };
        VULKAN_CALL(vkCreateRenderPass(vk.device,&rpciB,NULL,&rpB));

        // pipeline B
        VkShaderModuleCreateInfo smci={ .sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize=shaders_len, .pCode=(const u32*)shaders };
        VkShaderModule mod; VULKAN_CALL(vkCreateShaderModule(vk.device,&smci,NULL,&mod));
        VkPipelineShaderStageCreateInfo stB[2]={
            { .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage=VK_SHADER_STAGE_VERTEX_BIT,   .module=mod, .pName="VS_Blit" },
            { .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage=VK_SHADER_STAGE_FRAGMENT_BIT, .module=mod, .pName="PS_Blit" }
        };
        VkPipelineVertexInputStateCreateInfo vin={ .sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia={ .sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
        VkPipelineViewportStateCreateInfo vpst={ .sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount=1, .scissorCount=1 };
        VkDynamicState dyns[2]={ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR }; VkPipelineDynamicStateCreateInfo dyn={ .sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount=2, .pDynamicStates=dyns };
        VkPipelineRasterizationStateCreateInfo rs={ .sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode=VK_POLYGON_MODE_FILL, .cullMode=VK_CULL_MODE_NONE, .frontFace=VK_FRONT_FACE_CLOCKWISE, .lineWidth=1.0f };
        VkPipelineMultisampleStateCreateInfo ms={ .sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT };
        VkPipelineColorBlendAttachmentState cba={ .colorWriteMask=0xF, .blendEnable=VK_FALSE }; VkPipelineColorBlendStateCreateInfo cb={ .sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount=1, .pAttachments=&cba };
        VkPipelineLayoutCreateInfo plciB={ .sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount=1, .pSetLayouts=&vk.dsl };
        VULKAN_CALL(vkCreatePipelineLayout(vk.device,&plciB,NULL,&plB));
        VkGraphicsPipelineCreateInfo pciB={ .sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount=2, .pStages=stB, .pVertexInputState=&vin, .pInputAssemblyState=&ia, .pViewportState=&vpst, .pDynamicState=&dyn, .pRasterizationState=&rs, .pMultisampleState=&ms, .pColorBlendState=&cb, .layout=plB, .renderPass=rpB, .subpass=0 };
        VULKAN_CALL(vkCreateGraphicsPipelines(vk.device,NULL,1,&pciB,NULL,&gpB));
        vkDestroyShaderModule(vk.device,mod,NULL);

        // FBOs
        for (u32 i=0;i<sc_count;i++){
            VkImageView atts[] = { sc_view[i] };
            VkFramebufferCreateInfo fciB={ .sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass=rpB, .attachmentCount=1, .pAttachments=atts, .width=swap_extent.width, .height=swap_extent.height, .layers=1 };
            VULKAN_CALL(vkCreateFramebuffer(vk.device,&fciB,NULL,&fb[i]));
        }

        // cmd pool + per-image CBs
        VkCommandPoolCreateInfo cp_create_info={ .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex=vk.queue_family_index, .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
        VULKAN_CALL(vkCreateCommandPool(vk.device,&cp_create_info,NULL,&frame_pool));
        VkCommandBufferAllocateInfo cai={ .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool=frame_pool, .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount=sc_count };
        VULKAN_CALL(vkAllocateCommandBuffers(vk.device,&cai,frame_cb));

        for (u32 i=0;i<sc_count;i++){
            VkCommandBufferBeginInfo bi={ .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            VULKAN_CALL(vkBeginCommandBuffer(frame_cb[i],&bi));

            /* GPU query indices for this image */
            uint32_t qBegin = 2*i;
            uint32_t qEnd   = 2*i+1;
            vkCmdResetQueryPool(frame_cb[i], g_qp, qBegin, 2);
            vkCmdWriteTimestamp(frame_cb[i],
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_qp, qBegin);

            // Pass A -> offscreen
            VkClearValue clrA={ .color={{0.02f,0.02f,0.05f,1.0f}}};
            VkRenderPassBeginInfo rbiA={ .sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass=vk.rpA, .framebuffer=vk.off_fb, .renderArea={{0,0},{OFF_W,OFF_H}}, .clearValueCount=1, .pClearValues=&clrA };
            vkCmdBeginRenderPass(frame_cb[i],&rbiA,VK_SUBPASS_CONTENTS_INLINE);
            VkViewport vpA={0,0,(float)OFF_W,(float)OFF_H,0,1}; VkRect2D scA={{0,0},{OFF_W,OFF_H}};
            vkCmdSetViewport(frame_cb[i],0,1,&vpA); vkCmdSetScissor(frame_cb[i],0,1,&scA);
            vkCmdBindPipeline(frame_cb[i],VK_PIPELINE_BIND_POINT_GRAPHICS,vk.gpA);
            vkCmdBindDescriptorSets(frame_cb[i],VK_PIPELINE_BIND_POINT_GRAPHICS,vk.plA,0,1,&vk.dsetA,0,NULL);
            vkCmdDraw(frame_cb[i],3,1,0,0);
            vkCmdEndRenderPass(frame_cb[i]);

            // Barrier to sample off_img
            VkImageMemoryBarrier toSample={ .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask=VK_ACCESS_SHADER_READ_BIT,
                .oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                .image=vk.off_img, .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
            vkCmdPipelineBarrier(frame_cb[i], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,NULL,0,NULL,1,&toSample);

            // Pass B -> swapchain image i
            VkClearValue clrB={ .color={{0,0,0,1}}};
            VkRenderPassBeginInfo rbiB={ .sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass=rpB, .framebuffer=fb[i], .renderArea={{0,0},{swap_extent.width,swap_extent.height}}, .clearValueCount=1, .pClearValues=&clrB };
            vkCmdBeginRenderPass(frame_cb[i],&rbiB,VK_SUBPASS_CONTENTS_INLINE);
            VkViewport vpB={0,0,(float)swap_extent.width,(float)swap_extent.height,0,1}; VkRect2D scB={{0,0},{swap_extent.width,swap_extent.height}};
            vkCmdSetViewport(frame_cb[i],0,1,&vpB); vkCmdSetScissor(frame_cb[i],0,1,&scB);
            vkCmdBindPipeline(frame_cb[i],VK_PIPELINE_BIND_POINT_GRAPHICS,gpB);
            vkCmdBindDescriptorSets(frame_cb[i],VK_PIPELINE_BIND_POINT_GRAPHICS,plB,0,1,&vk.dsetB,0,NULL);
            vkCmdDraw(frame_cb[i],3,1,0,0);
            vkCmdEndRenderPass(frame_cb[i]);

            vkCmdWriteTimestamp(frame_cb[i],
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_qp, qEnd);

            VULKAN_CALL(vkEndCommandBuffer(frame_cb[i]));
        }

        built = 1;
        pf_timestamp("CREATE SWAPCHAIN");
    }

    // build sync objects once we know sc_count
    if (!built_sync) {
        VkSemaphoreCreateInfo s_ci={ .sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo f_ci={ .sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags=VK_FENCE_CREATE_SIGNALED_BIT };

        for (u32 i=0;i<NUM_FRAMES;i++){
            VULKAN_CALL(vkCreateSemaphore(vk.device,&s_ci,NULL,&imageAvailable[i])); // per-frame acquire sem
            VULKAN_CALL(vkCreateFence    (vk.device,&f_ci,NULL,&inFlight[i]));       // per-frame fence
        }
        for (u32 i=0;i<sc_count;i++){
            VULKAN_CALL(vkCreateSemaphore(vk.device,&s_ci,NULL,&renderFinished_img[i])); // per-image present sem
        }
        built_sync = 1;
    }

    // ---- frame (NUM_FRAMES in flight) ----
    VULKAN_CALL(vkWaitForFences(vk.device,1,&inFlight[curFrame],1,UINT64_MAX));
    VULKAN_CALL(vkResetFences(vk.device,1,&inFlight[curFrame]));

    u32 image_index = 0;
    VkResult ar = vkAcquireNextImageKHR(vk.device, swapchain, UINT64_MAX,
                                        imageAvailable[curFrame], VK_NULL_HANDLE, &image_index);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) { /* TODO: recreate swapchain */ return; }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { printf("Acquire failed: %s\n", vk_result_str(ar)); _exit(1); }

    if (image_index >= sc_count) { printf("bad image index\n"); _exit(1); }

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &imageAvailable[curFrame],
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 1, .pCommandBuffers = &frame_cb[image_index],
        .signalSemaphoreCount = 1, .pSignalSemaphores = &renderFinished_img[image_index] // <-- per-image!
    };
    VULKAN_CALL(vkQueueSubmit(vk.queue,1,&si,inFlight[curFrame]));

    /* ---- present with ID + wait for visibility ---- */
    uint64_t present_id = ++g_present_counter;

    VkPresentIdKHR pid = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .swapchainCount = 1,
        .pPresentIds = &present_id
    };

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = &pid,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &renderFinished_img[image_index],
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image_index
    };

    /* CPU timestamp immediately before present call (CLOCK_MONOTONIC domain) */
    uint64_t cpu_present_call_ns = pf_ns_now();

    VkResult pr = vkQueuePresentKHR(vk.queue, &pi);
    // if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
    //     /* TODO: recreate swapchain */
    //     return;
    // } else if (pr != VK_SUCCESS) {
    //     printf("Present failed: %s\n", vk_result_str(pr)); _exit(1);
    // }

    /* Wait for THIS present ID to become visible (close to first-pixel) */
    VkResult wr = pfnWaitForPresentKHR(vk.device, swapchain, present_id, UINT64_MAX);
    if (wr != VK_SUCCESS && wr != VK_ERROR_OUT_OF_DATE_KHR) {
        printf("vkWaitForPresentKHR failed: %s\n", vk_result_str(wr)); _exit(1);
    }

    uint64_t cpu_present_visible_ns = pf_ns_now();

    /* ---- pull GPU timestamps for this image (convert to CLOCK_MONOTONIC ns) ---- */
    uint64_t ticks[2] = {0,0};
    VkResult qr = vkGetQueryPoolResults(
        vk.device, g_qp, 2*image_index, 2, sizeof(ticks), ticks,
        sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

    double gpu_begin_ns = 0.0, gpu_end_ns = 0.0, gpu_time_ms = 0.0;
    if (qr == VK_SUCCESS) {
        gpu_begin_ns = gpu_ticks_to_cpu_ns(ticks[0]);
        gpu_end_ns   = gpu_ticks_to_cpu_ns(ticks[1]);
        gpu_time_ms  = (gpu_end_ns - gpu_begin_ns) / 1e6;
    }

    double submit_to_visible_ms = (cpu_present_visible_ns - cpu_present_call_ns) / 1e6;
    double gpuend_to_visible_ms = (qr == VK_SUCCESS) ? (cpu_present_visible_ns - gpu_end_ns)/1e6 : -1.0;

    printf("Frame %llu | gpu=%.3f ms | submit→visible=%.3f ms | gpuend→visible=%.3f ms | visible @ %.3f ms\n",
           (unsigned long long)present_id,
           gpu_time_ms, submit_to_visible_ms, gpuend_to_visible_ms,
           (cpu_present_visible_ns - pf_ns_start())/1e6);

    curFrame = (curFrame + 1) % NUM_FRAMES;
}
