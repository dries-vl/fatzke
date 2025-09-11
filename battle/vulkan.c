#include "header.h"

#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#else
#define VK_USE_PLATFORM_WAYLAND_KHR
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
#define FB_COUNT 2

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
    VkFormat texture_format; // optional, also handy

    /* offscreen */
    VkImage off_img;
    VkDeviceMemory off_mem;
    VkImageView off_view;
    VkSampler off_sampler;
    VkFramebuffer off_fb; // persistent even when resizing

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

static const char* vk_result_str(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
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

#pragma region DEBUG
static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* user_data)
{
    (void)types; (void)user_data;
    const char* sev =
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR" :
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "WARN " :
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) ? "INFO " : "VERB ";
    printf("[Vulkan][%s] %s\n", sev, data->pMessage);
    return VK_FALSE; // don't abort calls
}

// Loader helpers (EXT functions are not core; fetch them)
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
    // we are picking the intel icd on a hardcoded path to avoid delaying startup time on nvidia icd json
    //putenv("VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json");
    //putenv("VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json");
#endif

#ifdef DEBUG
    const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
    const uint32_t layer_count = 1;
#else
    const char** layers = NULL; uint32_t layer_count = 0;
#endif

    VkApplicationInfo ai = { .sType=VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName="tri2", .applicationVersion=1, .pEngineName="none", .apiVersion=VK_API_VERSION_1_1 };
#if defined(_WIN32)
    const char* exts[] = {"VK_KHR_surface","VK_KHR_win32_surface","VK_EXT_debug_utils"};
#else
    const char* exts[] = {"VK_KHR_surface","VK_KHR_wayland_surface","VK_EXT_debug_utils"};
#endif
    const uint32_t ext_count = sizeof(exts)/sizeof(exts[0]);
    // Optional: turn on extra validation goodies
    VkValidationFeatureEnableEXT enables[] = {
        VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT,
        VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT,
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
        // VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT, // enable if you use debugPrintf in shaders
    };
    VkValidationFeaturesEXT vfeatures = {
        .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .enabledValidationFeatureCount = (uint32_t)(sizeof(enables)/sizeof(enables[0])),
        .pEnabledValidationFeatures = enables,
    };

    // CreateInfo for the messenger (hooked into pNext so we catch messages during instance creation)
    VkDebugUtilsMessengerCreateInfoEXT dbg_ci = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, // drop INFO/VERBOSE if too chatty
        .messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = vk_debug_cb,
    };

    VkInstanceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &ai,
        .enabledExtensionCount = ext_count,
        .ppEnabledExtensionNames = exts,
        .enabledLayerCount = layer_count,
        .ppEnabledLayerNames = layers,
        .pNext = &dbg_ci
    };
    VULKAN_CALL(vkCreateInstance(&ci, NULL, &vk.instance));  // -- ca. 20ms
    pf_timestamp("vkCreateInstance");
#ifdef DEBUG
    VkDebugUtilsMessengerEXT debug_msgr = VK_NULL_HANDLE;
    VULKAN_CALL(CreateDebugUtilsMessengerEXT(vk.instance, &dbg_ci, NULL, &debug_msgr));
#endif
#ifdef __linux__
    VkWaylandSurfaceCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, .display = display_or_hinst, .surface = surface_or_hwnd
    };
    VULKAN_CALL(vkCreateWaylandSurfaceKHR(vk.instance,&sci,NULL,&vk.surface));
#else
    VkWin32SurfaceCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, .hinstance = display_or_hinst, .hwnd = surface_or_hwnd
    };
    VULKAN_CALL(vkCreateWin32SurfaceKHR(vk.instance, &sci, NULL, &vk.surface));
#endif
    pf_timestamp("vkCreateWaylandSurfaceKHR");
    uint32_t n=1; VkPhysicalDevice d[1];
    VULKAN_CALL(vkEnumeratePhysicalDevices(vk.instance,&n,d));
    if(!n){printf("no phys\n"); _exit(1);}
    vk.physical_device=d[0]; vk.queue_family_index=0;  /* assumes family 0 works */
    vkGetPhysicalDeviceMemoryProperties(vk.physical_device, &vk.device_memory_properties);
    pf_timestamp("pick phys+queue");
    float pr = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = vk.queue_family_index, .queueCount = 1,
        .pQueuePriorities = &pr
    };
    const char* device_exts[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = device_exts
    };
    VULKAN_CALL(vkCreateDevice(vk.physical_device,&dci,NULL,&vk.device)); // -- ca. 20ms
    vkGetDeviceQueue(vk.device, vk.queue_family_index, 0, &vk.queue);
    pf_timestamp("vkCreateDevice");
}
#pragma endregion

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
struct KTX2LevelIndex{ u64 byteOffset, byteLength, uncompressedByteLength; };
//_Static_assert(sizeof(struct KTX2Header)==80, "KTX2 header must be 80 bytes");
//_Static_assert(sizeof(struct KTX2LevelIndex)==24, "LevelIndex must be 24 bytes");

void vk_create_resources(void) {
    // Parse the texture memory header
    const u8* file = font_atlas; const size_t file_len = font_atlas_len;
    struct KTX2Header hdr; memcpy(&hdr, file, sizeof hdr);
    static const u8 magic[12]={0xAB,'K','T','X',' ','2','0',0xBB,0x0D,0x0A,0x1A,0x0A};
    if (memcmp(hdr.identifier, magic, 12) != 0) { printf("not a KTX2\n"); _exit(1); }
    VkFormat fmt = (VkFormat)hdr.vkFormat;
    if (!(fmt==VK_FORMAT_ASTC_12x12_UNORM_BLOCK || fmt==VK_FORMAT_ASTC_12x12_SRGB_BLOCK)) { printf("need ASTC 12x12\n"); _exit(1); }
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
    VkMemoryPropertyFlags req = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    u32 mem_type = 0;
    for (u32 i=0;i<vk.device_memory_properties.memoryTypeCount;i++)
        if (mr.memoryTypeBits & 1u<<i && (vk.device_memory_properties.memoryTypes[i].propertyFlags & req)==req) { mem_type=i; break; }
    VkMemoryAllocateInfo mai={ .sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize=mr.size, .memoryTypeIndex=mem_type };
    VULKAN_CALL(vkAllocateMemory(vk.device,&mai,NULL,&vk.texture_memory));
    VULKAN_CALL(vkBindImageMemory(vk.device,vk.texture_image,vk.texture_memory,0));

    // Staging buffer
    VkBuffer stg=NULL; VkDeviceMemory stg_mem=NULL;
    VkBufferCreateInfo bci={ .sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size=(VkDeviceSize)total, .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode=VK_SHARING_MODE_EXCLUSIVE };
    VULKAN_CALL(vkCreateBuffer(vk.device,&bci,NULL,&stg));
    VkMemoryRequirements bmr; vkGetBufferMemoryRequirements(vk.device,stg,&bmr);
    u32 type=0; VkMemoryPropertyFlags flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (u32 i=0;i<vk.device_memory_properties.memoryTypeCount;i++)
        if (bmr.memoryTypeBits & 1u<<i && (vk.device_memory_properties.memoryTypes[i].propertyFlags & flags)==flags) { type=i; break; }
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
    u32 m_type=0; VkMemoryPropertyFlags m_flags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    for (u32 i=0;i<vk.device_memory_properties.memoryTypeCount;i++)
        if (mem_reqs.memoryTypeBits & 1u<<i && (vk.device_memory_properties.memoryTypes[i].propertyFlags & m_flags)==m_flags) { m_type=i; break; }
    VkMemoryAllocateInfo allocate_info={ .sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize=mem_reqs.size, .memoryTypeIndex=m_type };
    VULKAN_CALL(vkAllocateMemory(vk.device,&allocate_info,NULL,&vk.off_mem));
    VULKAN_CALL(vkBindImageMemory(vk.device,vk.off_img,vk.off_mem,0));
    VkImageViewCreateInfo view_create_info={ .sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image=vk.off_img, .viewType=VK_IMAGE_VIEW_TYPE_2D, .format=TARGET_FORMAT, .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VULKAN_CALL(vkCreateImageView(vk.device,&view_create_info,NULL,&vk.off_view));
    VkSamplerCreateInfo sci={ .sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter=VK_FILTER_LINEAR, .minFilter=VK_FILTER_LINEAR, .mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU=VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeV=VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT, .minLod=0.0f, .maxLod=(f32)(vk.number_of_mip_levels-1) };
    VULKAN_CALL(vkCreateSampler(vk.device,&sci,NULL,&vk.off_sampler));

    // --- Descriptors (layout + pool + sets) ---
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

    // --- Render pass A + pipeline A (static: samples offscreen, format fixed) ---
    VkAttachmentDescription aA={ .format=TARGET_FORMAT, .samples=VK_SAMPLE_COUNT_1_BIT, .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE, .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkAttachmentReference arA={ .attachment=0, .layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription spA={ .pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount=1, .pColorAttachments=&arA };
    VkRenderPassCreateInfo rpciA={ .sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount=1, .pAttachments=&aA, .subpassCount=1, .pSubpasses=&spA };
    VULKAN_CALL(vkCreateRenderPass(vk.device,&rpciA,NULL,&vk.rpA));

    // offscreen framebuffer (fixed size)
    VkImageView off_att[] = { vk.off_view };
    VkFramebufferCreateInfo fciA={ .sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass=vk.rpA, .attachmentCount=1, .pAttachments=off_att, .width=OFF_W, .height=OFF_H, .layers=1 };
    VULKAN_CALL(vkCreateFramebuffer(vk.device,&fciA,NULL,&vk.off_fb));

    // Build pipeline A (uses the shared shader module only temporarily)
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

void vk_render_frame(u32 win_w, u32 win_h) {
    // swapchain
    static VkSwapchainKHR swapchain;
    static VkFormat swap_fmt;
    static VkExtent2D swap_extent;
    static VkImage sc_img[FB_COUNT];
    static VkImageView sc_view[FB_COUNT];
    static VkFramebuffer fb[FB_COUNT];
    static u32 sc_count;
    // post-process pipeline for scaling
    static VkPipelineLayout plB;
    static VkPipeline gpB;
    static VkRenderPass rpB;

    if (swapchain == NULL)
    {
        // caps / extent
        VkSurfaceCapabilitiesKHR caps;
        VULKAN_CALL(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical_device, vk.surface, &caps));
        swap_fmt = TARGET_FORMAT;
        VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR; // vsync
        if (caps.currentExtent.width != UINT32_MAX) swap_extent = caps.currentExtent; else swap_extent = (VkExtent2D){win_w, win_h};
        if (swap_extent.width==0 || swap_extent.height==0) return;

        const VkCompositeAlphaFlagBitsKHR order[] = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
        };
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for (u32 i=0;i<sizeof(order)/sizeof(order[0]);i++) if (caps.supportedCompositeAlpha & order[i]) { alpha=order[i]; break; }

        // swapchain (request exactly FB_COUNT images)
        VkSwapchainCreateInfoKHR sc_ci={ .sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface=vk.surface,
            .minImageCount=FB_COUNT, .imageFormat=swap_fmt, .imageColorSpace=TARGET_COLOR_SPACE, .imageExtent=swap_extent,
            .imageArrayLayers=1, .imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,
            .preTransform=caps.currentTransform, .compositeAlpha=alpha, .presentMode=pm, .clipped=VK_TRUE, .oldSwapchain=swapchain };
        VULKAN_CALL(vkCreateSwapchainKHR(vk.device,&sc_ci,NULL,&swapchain));
        if (sc_ci.oldSwapchain) vkDestroySwapchainKHR(vk.device, sc_ci.oldSwapchain, NULL);

        VULKAN_CALL(vkGetSwapchainImagesKHR(vk.device, swapchain, &sc_count, NULL));
        if (sc_count > FB_COUNT) sc_count = FB_COUNT; // hard cap to your fixed arrays
        VULKAN_CALL(vkGetSwapchainImagesKHR(vk.device, swapchain, &sc_count, sc_img));

        for (u32 i=0;i<sc_count;i++){
            VkImageViewCreateInfo vci={ .sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image=sc_img[i], .viewType=VK_IMAGE_VIEW_TYPE_2D, .format=swap_fmt, .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
            VULKAN_CALL(vkCreateImageView(vk.device,&vci,NULL,&sc_view[i]));
        }

        // Render pass B (must match swapchain format)
        VkAttachmentDescription aB={ .format=swap_fmt, .samples=VK_SAMPLE_COUNT_1_BIT, .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE, .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
        VkAttachmentReference arB={ .attachment=0, .layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription spB={ .pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount=1, .pColorAttachments=&arB };
        VkRenderPassCreateInfo rpciB={ .sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount=1, .pAttachments=&aB, .subpassCount=1, .pSubpasses=&spB };
        VULKAN_CALL(vkCreateRenderPass(vk.device,&rpciB,NULL,&rpB));

        // Pipeline B (depends on rpB)
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

        // Framebuffers B (one per swapchain image)
        for (u32 i=0;i<sc_count;i++){
            VkImageView atts[] = { sc_view[i] };
            VkFramebufferCreateInfo fciB={ .sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass=rpB, .attachmentCount=1, .pAttachments=atts, .width=swap_extent.width, .height=swap_extent.height, .layers=1 };
            VULKAN_CALL(vkCreateFramebuffer(vk.device,&fciB,NULL,&fb[i]));
        }
        pf_timestamp("CREATE SWAPCHAIN");
    }

    // command buffers to record and reuse across frames
    static VkCommandPool frame_pool;
    static VkCommandBuffer frame_cb[FB_COUNT];
    static int recorded = 0;
    // redo each frame
    static VkSemaphore sem_acquire, sem_render;
    static VkFence fence;

    if (!recorded) {
        recorded = 1;
        // Per-frame: pool + CBs + sync
        VkCommandPoolCreateInfo cp_create_info={ .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex=vk.queue_family_index, .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
        VULKAN_CALL(vkCreateCommandPool(vk.device,&cp_create_info,NULL,&frame_pool));
        VkCommandBufferAllocateInfo cai={ .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool=frame_pool, .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount=sc_count };
        VULKAN_CALL(vkAllocateCommandBuffers(vk.device,&cai,frame_cb));

        for (u32 i=0;i<sc_count;i++){
            VkCommandBufferBeginInfo bi={ .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            VULKAN_CALL(vkBeginCommandBuffer(frame_cb[i],&bi));

            // Pass A: render to offscreen
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

            // Pass B: blit to swapchain
            VkClearValue clrB={ .color={{0,0,0,1}}};
            VkRenderPassBeginInfo rbiB={ .sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass=rpB, .framebuffer=fb[i], .renderArea={{0,0},{swap_extent.width,swap_extent.height}}, .clearValueCount=1, .pClearValues=&clrB };
            vkCmdBeginRenderPass(frame_cb[i],&rbiB,VK_SUBPASS_CONTENTS_INLINE);
            VkViewport vpB={0,0,(float)swap_extent.width,(float)swap_extent.height,0,1}; VkRect2D scB={{0,0},{swap_extent.width,swap_extent.height}};
            vkCmdSetViewport(frame_cb[i],0,1,&vpB); vkCmdSetScissor(frame_cb[i],0,1,&scB);
            vkCmdBindPipeline(frame_cb[i],VK_PIPELINE_BIND_POINT_GRAPHICS,gpB);
            vkCmdBindDescriptorSets(frame_cb[i],VK_PIPELINE_BIND_POINT_GRAPHICS,plB,0,1,&vk.dsetB,0,NULL);
            vkCmdDraw(frame_cb[i],3,1,0,0);
            vkCmdEndRenderPass(frame_cb[i]);

            VULKAN_CALL(vkEndCommandBuffer(frame_cb[i]));
        }

        VkSemaphoreCreateInfo s_ci={ .sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo f_ci={ .sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags=VK_FENCE_CREATE_SIGNALED_BIT };
        VULKAN_CALL(vkCreateSemaphore(vk.device,&s_ci,NULL,&sem_acquire));
        VULKAN_CALL(vkCreateSemaphore(vk.device,&s_ci,NULL,&sem_render));
        VULKAN_CALL(vkCreateFence(vk.device,&f_ci,NULL,&fence));
        pf_timestamp("CREATE + RECORD COMMAND BUFFER");
    }

    { // this part doesn't actually need the topmost swapchain variables, except the swapchain itself
        u32 next_image_index = 0;
        VULKAN_CALL(vkWaitForFences(vk.device,1,&fence,VK_TRUE,UINT64_MAX));
        VULKAN_CALL(vkResetFences(vk.device,1,&fence));
        // --- THIS CALL BLOCKS THE CPU UNTIL WE GET TO THE NEXT FRAME (VSYNC, when using FIFO)
        vkAcquireNextImageKHR(vk.device, swapchain,UINT64_MAX, sem_acquire, NULL, &next_image_index);
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem_acquire,
            .pWaitDstStageMask = &waitStage, .commandBufferCount = 1, .pCommandBuffers = &frame_cb[next_image_index],
            .signalSemaphoreCount = 1, .pSignalSemaphores = &sem_render
        };
        VULKAN_CALL(vkQueueSubmit(vk.queue,1,&si,fence));
        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem_render,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &next_image_index
        };
        vkQueuePresentKHR(vk.queue, &pi);
        pf_timestamp("FRAME");
    }
}
