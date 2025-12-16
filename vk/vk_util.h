#pragma once

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

#define VK_CHECK(call) do{ VkResult _r=(call); if(_r!=VK_SUCCESS){ printf("vulkan error: %s on line @%s:%d\n",vk_result_str(_r),__FILE__,__LINE__); _exit(1);} }while(0)

static uint32_t find_memory_type_index(VkPhysicalDevice physical_device,uint32_t memory_type_bits,VkMemoryPropertyFlags required_properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        const int supported = (memory_type_bits & (1u << i)) != 0;
        const int has_props = (mem_props.memoryTypes[i].propertyFlags & required_properties) == required_properties;
        if (supported && has_props) return i;
    }
    printf("Failed to find suitable memory type.\n");
    _exit(0); return 0;
}

#if DEBUG_VULKAN == 1
VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT messageTypes, const VkDebugUtilsMessengerCallbackDataEXT* data, void* pUserData) {
    u32 error = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    u32 warning = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    if (error || warning) {
        const char* sev =
            (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR" :
            (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "WARN" :
            (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) ? "INFO" : "VERB";
        printf("[Vulkan][%s] %s\n", sev, data->pMessage);
    }
    return VK_FALSE;
}
static VkResult CreateDebugUtilsMessengerEXT(VkInstance inst, const VkDebugUtilsMessengerCreateInfoEXT* ci, const VkAllocationCallbacks* alloc, VkDebugUtilsMessengerEXT* out) {
    PFN_vkCreateDebugUtilsMessengerEXT fp =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(inst, "vkCreateDebugUtilsMessengerEXT");
    return fp ? fp(inst, ci, alloc, out) : VK_ERROR_EXTENSION_NOT_PRESENT;
}
static void DestroyDebugUtilsMessengerEXT(VkInstance inst, VkDebugUtilsMessengerEXT msgr, const VkAllocationCallbacks* alloc) {
    PFN_vkDestroyDebugUtilsMessengerEXT fp =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(inst, "vkDestroyDebugUtilsMessengerEXT");
    if (fp) fp(inst, msgr, alloc);
}
#endif


// stages
#define ST_HOST   VK_PIPELINE_STAGE_2_HOST_BIT
#define ST_XFER   VK_PIPELINE_STAGE_2_TRANSFER_BIT
#define ST_CS     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
#define ST_VS     VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
#define ST_FS     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
#define ST_GFX    VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT
#define ST_CA     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
#define ST_EFT    (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT)

// accesses
#define AC_HWR    VK_ACCESS_2_HOST_WRITE_BIT
#define AC_SRD    VK_ACCESS_2_SHADER_READ_BIT
#define AC_SWR    VK_ACCESS_2_SHADER_WRITE_BIT
#define AC_TWR    VK_ACCESS_2_TRANSFER_WRITE_BIT
#define AC_IND    VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT
#define AC_VA     VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT
#define AC_DSRD   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
#define AC_DSWR   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
#define AC_CWR    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT

static inline VkMemoryBarrier2 mem_barrier2(
    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 b = {0};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    b.srcStageMask  = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask  = dstStage;
    b.dstAccessMask = dstAccess;
    return b;
}

static inline VkBufferMemoryBarrier2 buf_barrier2(
    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
    VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size)
{
    VkBufferMemoryBarrier2 b = {0};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    b.srcStageMask  = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask  = dstStage;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buffer;
    b.offset = offset;
    b.size   = size;
    return b;
}

static inline VkImageMemoryBarrier2 img_barrier2(
    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
    VkImageLayout oldLayout, VkImageLayout newLayout,
    VkImage image, VkImageAspectFlags aspect,
    uint32_t baseMip, uint32_t levelCount,
    uint32_t baseLayer, uint32_t layerCount)
{
    VkImageMemoryBarrier2 b = {0};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask  = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask  = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.image = image;
    b.subresourceRange.aspectMask     = aspect;
    b.subresourceRange.baseMipLevel   = baseMip;
    b.subresourceRange.levelCount     = levelCount;
    b.subresourceRange.baseArrayLayer = baseLayer;
    b.subresourceRange.layerCount     = layerCount;
    return b;
}

static inline void cmd_barrier2(
    VkCommandBuffer cmd,
    const VkMemoryBarrier2* mem, uint32_t memCount,
    const VkBufferMemoryBarrier2* buf, uint32_t bufCount,
    const VkImageMemoryBarrier2* img, uint32_t imgCount)
{
    VkDependencyInfo dep = {0};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount       = memCount;
    dep.pMemoryBarriers          = mem;
    dep.bufferMemoryBarrierCount = bufCount;
    dep.pBufferMemoryBarriers    = buf;
    dep.imageMemoryBarrierCount  = imgCount;
    dep.pImageMemoryBarriers     = img;
    vkCmdPipelineBarrier2(cmd, &dep);
}
