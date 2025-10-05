#pragma once

struct Swapchain {
    // swapchain
    VkSwapchainKHR            swapchain;
    VkFormat                  swapchain_format;
    VkColorSpaceKHR           swapchain_colorspace;
    VkExtent2D                swapchain_extent;
    uint32_t                  swapchain_image_count;
    // for each image in the swapchain
    VkImage*                  swapchain_images; // nr. of swapchain images
    VkImageView*              swapchain_views; // nr. of swapchain images
    VkImageLayout*            image_layouts; // nr. of swapchain images
    VkCommandPool             command_pool_graphics;
    VkCommandBuffer*          command_buffers_per_image; // nr. of swapchain images
    VkSemaphore*              present_ready_per_image; // nr. of swapchain images
    // remember which swapchain image the previous frame used
    uint32_t                  previous_frame_image_index[MAX_FRAMES_IN_FLIGHT];
    #if DEBUG == 1
    #define QUERIES_PER_IMAGE 2
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
    swapchain.swapchain_image_count = min_image_count;
    swapchain.swapchain_images = (VkImage*)malloc(sizeof(VkImage) * min_image_count);
    VK_CHECK(vkGetSwapchainImagesKHR(machine->device, swapchain.swapchain, &min_image_count, swapchain.swapchain_images));
    swapchain.image_layouts = (VkImageLayout*)malloc(sizeof(VkImageLayout) * min_image_count);
    for (uint32_t i = 0; i < min_image_count; ++i) {
        swapchain.image_layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    swapchain.swapchain_views = (VkImageView*)malloc(sizeof(VkImageView) * min_image_count);
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
    #if DEBUG == 1
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        swapchain.previous_frame_image_index[i] = UINT32_MAX;
    #endif

    // create per-image present semaphores
    swapchain.present_ready_per_image = (VkSemaphore*) calloc(swapchain.swapchain_image_count, sizeof(VkSemaphore));
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
    swapchain.command_buffers_per_image = (VkCommandBuffer*)calloc(swapchain.swapchain_image_count, sizeof(VkCommandBuffer));
    VkCommandBufferAllocateInfo cmd_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = swapchain.command_pool_graphics,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = swapchain.swapchain_image_count
    };
    VK_CHECK(vkAllocateCommandBuffers(machine->device, &cmd_alloc_info, swapchain.command_buffers_per_image));

    #if DEBUG == 1
    VkQueryPoolCreateInfo qpci = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = swapchain.swapchain_image_count * QUERIES_PER_IMAGE
    };
    VK_CHECK(vkCreateQueryPool(machine->device, &qpci, NULL, &swapchain.query_pool));
    uint32_t total_queries = swapchain.swapchain_image_count * QUERIES_PER_IMAGE;
    vkResetQueryPool(machine->device, swapchain.query_pool, 0, total_queries);
    #endif

    pf_timestamp("Swapchain created");
    return swapchain;
}

void destroy_swapchain(struct Machine* machine, struct Renderer* renderer, struct Swapchain* swapchain) {
    // Present semaphores depend on image count — free them here
    if (swapchain->present_ready_per_image) {
        for (uint32_t i = 0; i < swapchain->swapchain_image_count; ++i)
            vkDestroySemaphore(machine->device, swapchain->present_ready_per_image[i], NULL);
        free(swapchain->present_ready_per_image);
        swapchain->present_ready_per_image = NULL;
    }
    // Command buffers depend on image count — free them here
    if (swapchain->command_buffers_per_image) {
        vkFreeCommandBuffers(machine->device, swapchain->command_pool_graphics,
                             swapchain->swapchain_image_count, swapchain->command_buffers_per_image);
        free(swapchain->command_buffers_per_image);
        swapchain->command_buffers_per_image = NULL;
    }
    // Destroy the command pool itself
    if (swapchain->command_pool_graphics) {
        vkDestroyCommandPool(machine->device, swapchain->command_pool_graphics, NULL);
        swapchain->command_pool_graphics = VK_NULL_HANDLE;
    }
    #if DEBUG == 1
    if (swapchain->query_pool) {
        vkDestroyQueryPool(machine->device, swapchain->query_pool, NULL);
        swapchain->query_pool = VK_NULL_HANDLE;
    }
    #endif
    if (swapchain->image_layouts) { free(swapchain->image_layouts); swapchain->image_layouts = NULL; }
    if (swapchain->swapchain_views) {
        for (uint32_t i = 0; i < swapchain->swapchain_image_count; ++i)
            vkDestroyImageView(machine->device, swapchain->swapchain_views[i], NULL);
        free(swapchain->swapchain_views);
        swapchain->swapchain_views = NULL;
    }
    // Free the heap array of VkImage handles (the images are owned by the swapchain; no vkDestroyImage)
    if (swapchain->swapchain_images) {
        free(swapchain->swapchain_images);
        swapchain->swapchain_images = NULL;
    }
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
