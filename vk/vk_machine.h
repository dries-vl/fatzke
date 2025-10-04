#pragma once

#include <vulkan/vulkan_xlib.h>

struct Machine
{
    VkInstance                instance;
    VkSurfaceKHR              surface;
    VkPhysicalDevice          physical_device;
    VkDevice                  device;
    uint32_t                  queue_family_graphics;
    uint32_t                  queue_family_compute;
    uint32_t                  queue_family_present;
    VkQueue                   queue_graphics;
    VkQueue                   queue_compute;
    VkQueue                   queue_present;
};

struct Machine create_machine(WINDOW x11_window)
{
    // Create instance (with optional debug mode enabled)
    struct Machine machine = {0};
    VkApplicationInfo app_info = {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = APP_NAME,
        .apiVersion         = VK_API_VERSION_1_3
    };
    static const char* extensions[8]; int extension_count = 0;
    extensions[0] = VK_KHR_SURFACE_EXTENSION_NAME; extension_count++;
    extensions[1] = VK_KHR_XLIB_SURFACE_EXTENSION_NAME; extension_count++;
#if DEBUG == 1
    extensions[2] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME; extension_count++;
    const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
    const uint32_t layer_count = 1;
    VkDebugUtilsMessengerCreateInfoEXT *debugCreateInfo = &(VkDebugUtilsMessengerCreateInfoEXT){
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
#else
    const char** layers = NULL; uint32_t layer_count = 0; void *debugCreateInfo = NULL;
#endif
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount   = extension_count,
        .ppEnabledExtensionNames = extensions,
        .enabledLayerCount = layer_count,
        .ppEnabledLayerNames = layers,
        .pNext = debugCreateInfo
    };
    VK_CHECK(vkCreateInstance(&instance_info, NULL, &machine.instance));
#if DEBUG == 1
    VkDebugUtilsMessengerEXT debug_msgr = VK_NULL_HANDLE;
    VK_CHECK(CreateDebugUtilsMessengerEXT(machine.instance, debugCreateInfo, NULL, &debug_msgr));
#endif
    pf_timestamp("Vulkan instance created");

    // Create surface
    VkXlibSurfaceCreateInfoKHR surface_info = {
        .sType  = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
        .dpy    = (Display*)pf_display_or_instance(x11_window),
        .window = (Window)pf_surface_or_hwnd(x11_window)
    };
    VK_CHECK(vkCreateXlibSurfaceKHR(machine.instance, &surface_info, NULL, &machine.surface));
    pf_timestamp("Xlib surface created");

    /* -------- Physical Device & Logical Device -------- */
    uint32_t device_count = 0;
    VkResult res = vkEnumeratePhysicalDevices(machine.instance, &device_count, NULL);
    if (res != VK_SUCCESS || device_count == 0) {
        printf("No Vulkan physical devices found (res=%d, count=%u)\n", res, device_count);
        _exit(0);
    }
    VkPhysicalDevice devices[16];
    if (device_count > 16) device_count = 16;
    VK_CHECK(vkEnumeratePhysicalDevices(machine.instance, &device_count, devices));
    for (uint32_t i = 0; i < device_count; i++) {
        VkPhysicalDevice dev = devices[i];

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);
        printf("Found GPU: %s\n", props.deviceName);

        // Check queue families
        uint32_t qf_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qf_count, NULL);
        VkQueueFamilyProperties qprops[32];
        if (qf_count > 32) qf_count = 32;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qf_count, qprops);

        uint32_t q_graphics = UINT32_MAX, q_present = UINT32_MAX;
        for (uint32_t q = 0; q < qf_count; q++) {
            if (qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) q_graphics = q;
            VkBool32 supports_present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, q, machine.surface, &supports_present);
            if (supports_present) q_present = q;
        }
        if (q_graphics == UINT32_MAX || q_present == UINT32_MAX) continue;

        // Check swapchain extension
        uint32_t extc = 0;
        vkEnumerateDeviceExtensionProperties(dev, NULL, &extc, NULL);
        VkExtensionProperties exts[64];
        if (extc > 64) extc = 64;
        vkEnumerateDeviceExtensionProperties(dev, NULL, &extc, exts);

        // Accept this device
        machine.physical_device       = dev;
        machine.queue_family_graphics = q_graphics;
        machine.queue_family_compute  = q_graphics; // on Intel, graphics does compute
        machine.queue_family_present  = q_present;
        printf("Selected GPU: %s\n", props.deviceName);
    }
    if (!machine.physical_device) {
        printf("No suitable GPU found after checking %u devices.\n", device_count);
        _exit(0);
    }

    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_infos[3];
    uint32_t queue_infos_count = 0;

    uint32_t families[3] = {
        machine.queue_family_graphics,
        machine.queue_family_compute,
        machine.queue_family_present
    };

    // De-duplicate queue families while preserving order
    for (int i = 0; i < 3; ++i) {
        int already_added = 0;
        for (uint32_t j = 0; j < queue_infos_count; ++j)
            if (queue_infos[j].queueFamilyIndex == families[i]) { already_added = 1; break; }
        if (already_added) continue;
        queue_infos[queue_infos_count++] = (VkDeviceQueueCreateInfo) {
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = families[i],
            .queueCount       = 1,
            .pQueuePriorities = &queue_priority
        };
    }

    // enable a bunch of features on the device
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
    vkGetPhysicalDeviceFeatures2(machine.physical_device, &features);

    static const char* kRequiredDeviceExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_PRESENT_ID_EXTENSION_NAME,
        VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
        VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME
    };
    VkDeviceCreateInfo device_info = {
        .pNext = &features,
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount    = queue_infos_count,
        .pQueueCreateInfos       = queue_infos,
        .enabledExtensionCount   = (uint32_t)(sizeof(kRequiredDeviceExtensions) / sizeof(kRequiredDeviceExtensions[0])),
        .ppEnabledExtensionNames = kRequiredDeviceExtensions
    };
    VK_CHECK(vkCreateDevice(machine.physical_device, &device_info, NULL, &machine.device));

    vkGetDeviceQueue(machine.device, machine.queue_family_graphics, 0, &machine.queue_graphics);
    vkGetDeviceQueue(machine.device, machine.queue_family_compute,  0, &machine.queue_compute);
    vkGetDeviceQueue(machine.device, machine.queue_family_present,  0, &machine.queue_present);
    pf_timestamp("Logical device and queues created");
    return machine;
}
