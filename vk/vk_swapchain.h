#pragma once

struct Swapchain {
    // swapchain
    VkSwapchainKHR            swapchain;
    VkFormat                  swapchain_format;
    VkColorSpaceKHR           swapchain_colorspace;
    VkExtent2D                swapchain_extent;
    uint32_t                  swapchain_image_count;
    // for each image in the swapchain
    VkImage                   swapchain_images[MAX_SWAPCHAIN_IMAGES];
    VkImageView               swapchain_views[MAX_SWAPCHAIN_IMAGES];
    VkImageLayout             image_layouts[MAX_SWAPCHAIN_IMAGES];
    VkCommandPool             command_pool_graphics;
    VkCommandBuffer           command_buffers_per_image[MAX_SWAPCHAIN_IMAGES];
    int                       command_buffers_recorded[MAX_SWAPCHAIN_IMAGES];
    VkSemaphore               present_ready_per_image[MAX_SWAPCHAIN_IMAGES];
    VkFramebuffer             framebuffers[MAX_SWAPCHAIN_IMAGES];
    // remember which swapchain image the previous frame used
    uint32_t                  previous_frame_image_index[MAX_FRAMES_IN_FLIGHT];
    // depth
    VkFormat                  depth_format;
    VkImage                   depth_image;
    VkDeviceMemory            depth_memory;
    VkImageView               depth_view;
    // pick object ids
    VkImage         pick_image;
    VkDeviceMemory  pick_image_memory;
    VkImageView     pick_image_view;
    VkBuffer        pick_readback_buffer;
    VkDeviceMemory  pick_readback_memory;
    VkBuffer        pick_region_buffer;
    VkDeviceMemory  pick_region_memory;
    // debug
    #if DEBUG_APP == 1
    VkQueryPool query_pool; // GPU timestamps
    #endif
};

struct Swapchain create_swapchain(const struct Machine *machine, WINDOW w) {
    struct Swapchain swapchain = {0};

    // Surface capabilities and formats
    VkSurfaceCapabilitiesKHR capabilities;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(machine->physical_device, machine->surface, &capabilities));

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(machine->physical_device, machine->surface, &format_count, NULL);
    VkSurfaceFormatKHR formats[32];
    if (format_count > 32) format_count = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(machine->physical_device, machine->surface, &format_count, formats);

    VkSurfaceFormatKHR chosen_format = formats[0];
    for (uint32_t i = 0; i < format_count; ++i) {
        if (ENABLE_HDR) {
            if (formats[i].format == VK_FORMAT_R16G16B16A16_SFLOAT) { chosen_format = formats[i]; break; }
        } else {
            if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB) { chosen_format = formats[i]; break; }
        }
    }
    swapchain.swapchain_format     = chosen_format.format;
    swapchain.swapchain_colorspace = chosen_format.colorSpace;

    VkExtent2D desired_extent = capabilities.currentExtent;
    if (desired_extent.width == UINT32_MAX) {
        desired_extent.width  = pf_window_width(w);
        desired_extent.height = pf_window_height(w);
    }
    swapchain.swapchain_extent = desired_extent;

    uint32_t min_image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && min_image_count > capabilities.maxImageCount)
        min_image_count = capabilities.maxImageCount;

    VkSwapchainCreateInfoKHR swapchain_info = {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = machine->surface,
        .minImageCount    = min_image_count,
        .imageFormat      = swapchain.swapchain_format,
        .imageColorSpace  = swapchain.swapchain_colorspace,
        .imageExtent      = swapchain.swapchain_extent,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .preTransform     = capabilities.currentTransform,
        .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode      = VK_PRESENT_MODE_FIFO_KHR,
        .clipped          = VK_TRUE
    };

    uint32_t queue_indices[3] = { machine->queue_family_graphics, machine->queue_family_present, machine->queue_family_compute };
    if (machine->queue_family_graphics != machine->queue_family_present) {
        swapchain_info.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        swapchain_info.queueFamilyIndexCount = 2;
        swapchain_info.pQueueFamilyIndices   = queue_indices;
    } else {
        swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VK_CHECK(vkCreateSwapchainKHR(machine->device, &swapchain_info, NULL, &swapchain.swapchain));

    VK_CHECK(vkGetSwapchainImagesKHR(machine->device, swapchain.swapchain, &min_image_count, NULL));
    if(min_image_count > MAX_SWAPCHAIN_IMAGES) {printf("Too many swapchain images; can handle %d but got %d\n", MAX_SWAPCHAIN_IMAGES, min_image_count); _exit(0);};
    swapchain.swapchain_image_count = min_image_count;
    VK_CHECK(vkGetSwapchainImagesKHR(machine->device, swapchain.swapchain, &min_image_count, swapchain.swapchain_images));
    for (uint32_t i = 0; i < min_image_count; ++i) {
        swapchain.image_layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    for (uint32_t i = 0; i < min_image_count; ++i) {
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = swapchain.swapchain_images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = swapchain.swapchain_format,
            .components = {
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY
            },
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 }
        };
        VK_CHECK(vkCreateImageView(machine->device, &view_info, NULL, &swapchain.swapchain_views[i]));
    }
    #if DEBUG_APP == 1
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        swapchain.previous_frame_image_index[i] = UINT32_MAX;
    #endif

    // create per-image present semaphores
    VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < swapchain.swapchain_image_count; ++i) {
        VK_CHECK(vkCreateSemaphore(machine->device, &si, NULL, &swapchain.present_ready_per_image[i]));
    }

    // create command pool
    VkCommandPoolCreateInfo cmd_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = machine->queue_family_graphics,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
    };
    VK_CHECK(vkCreateCommandPool(machine->device, &cmd_pool_info, NULL, &swapchain.command_pool_graphics));

    // create per-image command buffers (needs the command pool)
    VkCommandBufferAllocateInfo cmd_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = swapchain.command_pool_graphics,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = swapchain.swapchain_image_count
    };
    VK_CHECK(vkAllocateCommandBuffers(machine->device, &cmd_alloc_info, swapchain.command_buffers_per_image));

    #if DEBUG_APP == 1
    VkQueryPoolCreateInfo qpci = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = swapchain.swapchain_image_count * QUERIES_PER_IMAGE
    };
    VK_CHECK(vkCreateQueryPool(machine->device, &qpci, NULL, &swapchain.query_pool));
    uint32_t total_queries = swapchain.swapchain_image_count * QUERIES_PER_IMAGE;
    vkResetQueryPool(machine->device, swapchain.query_pool, 0, total_queries);
    #endif
    
    // depth
    const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_X8_D24_UNORM_PACK32,  // optional on some platforms
        VK_FORMAT_D16_UNORM
    };
    for (uint32_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); ++i) {
        VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(machine->physical_device, candidates[i], &p);
        if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            swapchain.depth_format = candidates[i];
            break;
        }
    }
    // Fallback — should not happen on conformant HW
    if (!swapchain.depth_format) swapchain.depth_format = VK_FORMAT_D16_UNORM;

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format    = swapchain.depth_format,
        .extent    = { swapchain.swapchain_extent.width, swapchain.swapchain_extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples   = VK_SAMPLE_COUNT_1_BIT,
        .tiling    = VK_IMAGE_TILING_OPTIMAL,
        .usage     = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    VK_CHECK(vkCreateImage(machine->device, &ici, NULL, &swapchain.depth_image));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(machine->device, swapchain.depth_image, &req);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = find_memory_type_index(machine->physical_device, req.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    VK_CHECK(vkAllocateMemory(machine->device, &mai, NULL, &swapchain.depth_memory));
    VK_CHECK(vkBindImageMemory(machine->device, swapchain.depth_image, swapchain.depth_memory, 0));

    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = swapchain.depth_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = swapchain.depth_format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1
        }
    };
    VK_CHECK(vkCreateImageView(machine->device, &ivci, NULL, &swapchain.depth_view));
    
    // create object pick image
    VkImageCreateInfo img = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent = {
            .width  = swapchain.swapchain_extent.width,
            .height = swapchain.swapchain_extent.height,
            .depth  = 1
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_R32_UINT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VK_CHECK(vkCreateImage(machine->device, &img, NULL, &swapchain.pick_image));

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(machine->device, swapchain.pick_image, &memReq);

    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReq.size,
        .memoryTypeIndex = find_memory_type_index(
            machine->physical_device,
            memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        )
    };
    VK_CHECK(vkAllocateMemory(machine->device, &alloc, NULL, &swapchain.pick_image_memory));
    VK_CHECK(vkBindImageMemory(machine->device, swapchain.pick_image, swapchain.pick_image_memory, 0));

    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = swapchain.pick_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R32_UINT,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };
    VK_CHECK(vkCreateImageView(machine->device, &view, NULL, &swapchain.pick_image_view));

    pf_timestamp("Swapchain created");
    return swapchain;
}

void destroy_swapchain(struct Machine* machine, struct Renderer* renderer, struct Swapchain* swapchain) {
    // Present semaphores depend on image count — free them here
    if (swapchain->present_ready_per_image) {
        for (uint32_t i = 0; i < swapchain->swapchain_image_count; ++i)
            vkDestroySemaphore(machine->device, swapchain->present_ready_per_image[i], NULL);
        memset(swapchain->present_ready_per_image, 0, sizeof swapchain->present_ready_per_image);
    }
    // Command buffers depend on image count — free them here
    if (swapchain->command_buffers_per_image) {
        vkFreeCommandBuffers(machine->device, swapchain->command_pool_graphics, swapchain->swapchain_image_count, swapchain->command_buffers_per_image);
        memset(swapchain->command_buffers_per_image, 0, sizeof swapchain->command_buffers_per_image);
    }
    // Destroy the command pool itself
    if (swapchain->command_pool_graphics) {
        vkDestroyCommandPool(machine->device, swapchain->command_pool_graphics, NULL);
        swapchain->command_pool_graphics = VK_NULL_HANDLE;
    }
    #if DEBUG_APP == 1
    if (swapchain->query_pool) {
        vkDestroyQueryPool(machine->device, swapchain->query_pool, NULL);
        swapchain->query_pool = VK_NULL_HANDLE;
    }
    #endif
    if (swapchain->image_layouts) { memset(swapchain->image_layouts, 0, sizeof swapchain->image_layouts); }
    if (swapchain->swapchain_views) {
        for (uint32_t i = 0; i < swapchain->swapchain_image_count; ++i)
            vkDestroyImageView(machine->device, swapchain->swapchain_views[i], NULL);
        memset(swapchain->swapchain_views, 0, sizeof swapchain->swapchain_views);
    }
    // Free the heap array of VkImage handles (the images are owned by the swapchain; no vkDestroyImage)
    if (swapchain->swapchain_images) {
        memset(swapchain->swapchain_images, 0, sizeof swapchain->swapchain_images);
    }
    // depth
    if (swapchain->depth_view)   { vkDestroyImageView(machine->device, swapchain->depth_view, NULL);   swapchain->depth_view = VK_NULL_HANDLE; }
    if (swapchain->depth_image)  { vkDestroyImage(machine->device, swapchain->depth_image, NULL);      swapchain->depth_image = VK_NULL_HANDLE; }
    if (swapchain->depth_memory) { vkFreeMemory(machine->device, swapchain->depth_memory, NULL);       swapchain->depth_memory = VK_NULL_HANDLE; }
    // swapchain
    if (swapchain->swapchain) {
        vkDestroySwapchainKHR(machine->device, swapchain->swapchain, NULL);
        swapchain->swapchain = VK_NULL_HANDLE;
    }
    swapchain->swapchain_image_count = 0;
}

static void recreate_swapchain(struct Machine* machine, struct Renderer* renderer,struct Swapchain* swapchain,WINDOW window) {
    // Wait for GPU to finish anything that might use old swapchain resources
    vkDeviceWaitIdle(machine->device);
    destroy_swapchain(machine, renderer, swapchain);
    *swapchain = create_swapchain(machine, window);
}
