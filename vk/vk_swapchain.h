#pragma once

struct Swapchain
{
    VkSwapchainKHR            swapchain;
    VkFormat                  swapchain_format;
    VkColorSpaceKHR           swapchain_colorspace;
    VkExtent2D                swapchain_extent;
    VkImage*                  swapchain_images;
    uint32_t                  swapchain_image_count;
    VkImageView*              swapchain_views;
    VkFramebuffer*            framebuffers;
};

struct Swapchain create_swapchain(const struct Machine *machine, WINDOW w)
{
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
        .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
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
    pf_timestamp("Swapchain created");
    return swapchain;
}

void destroy_swapchain(struct Machine* machine, struct Renderer* renderer, struct Swapchain* swapchain)
{
    // DO NOT destroy per-frame semaphores/fences here (renderer owns them)
    // Command buffers depend on image count — free them here
    if (renderer->command_buffers_per_image) {
        vkFreeCommandBuffers(machine->device, renderer->command_pool_graphics,
                             swapchain->swapchain_image_count, renderer->command_buffers_per_image);
        free(renderer->command_buffers_per_image);
        renderer->command_buffers_per_image = NULL;
    }

    if (swapchain->framebuffers) {
        for (uint32_t i = 0; i < swapchain->swapchain_image_count; ++i)
            vkDestroyFramebuffer(machine->device, swapchain->framebuffers[i], NULL);
        free(swapchain->framebuffers);
        swapchain->framebuffers = NULL;
    }
    if (swapchain->swapchain_views) {
        for (uint32_t i = 0; i < swapchain->swapchain_image_count; ++i)
            vkDestroyImageView(machine->device, swapchain->swapchain_views[i], NULL);
        free(swapchain->swapchain_views);
        swapchain->swapchain_views = NULL;
    }
    if (swapchain->swapchain) {
        vkDestroySwapchainKHR(machine->device, swapchain->swapchain, NULL);
        swapchain->swapchain = VK_NULL_HANDLE;
    }
}

static void recreate_swapchain(struct Machine* machine, struct Renderer* renderer,struct Swapchain* swapchain,WINDOW window) {
    // Wait for GPU to finish anything that might use old swapchain resources
    vkDeviceWaitIdle(machine->device);

    destroy_swapchain(machine, renderer, swapchain);

    // Recreate swapchain
    *swapchain = create_swapchain(machine, window);

    // Recreate persistent framebuffers per image
    swapchain->framebuffers = (VkFramebuffer*)malloc(sizeof(VkFramebuffer) * swapchain->swapchain_image_count);
    for (uint32_t i = 0; i < swapchain->swapchain_image_count; ++i) {
        VkImageView attachments[1] = { swapchain->swapchain_views[i] };
        VkFramebufferCreateInfo fb_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = renderer->render_pass,
            .attachmentCount = 1, .pAttachments = attachments,
            .width  = swapchain->swapchain_extent.width,
            .height = swapchain->swapchain_extent.height,
            .layers = 1
        };
        VK_CHECK(vkCreateFramebuffer(machine->device, &fb_info, NULL, &swapchain->framebuffers[i]));
    }

    // Recreate per-image command buffers to match new image count
    renderer->command_buffers_per_image = (VkCommandBuffer*)calloc(swapchain->swapchain_image_count, sizeof(VkCommandBuffer));
    VkCommandBufferAllocateInfo cmd_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = renderer->command_pool_graphics,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = swapchain->swapchain_image_count
    };
    VK_CHECK(vkAllocateCommandBuffers(machine->device, &cmd_alloc_info, renderer->command_buffers_per_image));
}

