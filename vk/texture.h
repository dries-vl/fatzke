#pragma once

#define MAX_TEXTURES 64

extern const unsigned char font_atlas[];
extern const unsigned char font_atlas_end[];
#define font_atlas_len ((size_t)(font_atlas_end - font_atlas))

extern const unsigned char map[];
extern const unsigned char map_end[];
#define map_len ((size_t)(map_end - map))

extern const unsigned char height[];
extern const unsigned char height_end[];
#define height_len ((size_t)(height_end - height))

extern const unsigned char height_detail[];
extern const unsigned char height_detail_end[];
#define height_detail_len ((size_t)(height_detail_end - height_detail))

extern const unsigned char normals[];
extern const unsigned char normals_end[];
#define normals_len ((size_t)(normals_end - normals))

extern const unsigned char noise[];
extern const unsigned char noise_end[];
#define noise_len ((size_t)(noise_end - noise))

typedef struct Texture {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
} Texture;

static Texture textures[MAX_TEXTURES];
static Texture dummy_texture;
static VkSampler global_sampler;
static uint32_t texture_count = 0;

static void create_image_2d_mipped(VkDevice dev, VkPhysicalDevice phys, uint32_t w, uint32_t h, VkFormat format, uint32_t mipLevels, VkImageUsageFlags usage, VkImage* out_img, VkDeviceMemory* out_mem) {
    VkImageCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent.width = w;
    ici.extent.height = h;
    ici.extent.depth = 1;
    ici.mipLevels = mipLevels;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(dev, &ici, NULL, out_img));

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, *out_img, &mr);

    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_memory_type_index(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(dev, &mai, NULL, out_mem));
    VK_CHECK(vkBindImageMemory(dev, *out_img, *out_mem, 0));
}

static void create_view_2d_mipped(VkDevice dev, VkImage img, VkFormat fmt, uint32_t mipLevels, VkImageView* out_view) {
    VkImageViewCreateInfo vci;
    memset(&vci, 0, sizeof(vci));
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.baseMipLevel = 0;
    vci.subresourceRange.levelCount = mipLevels;
    vci.subresourceRange.baseArrayLayer = 0;
    vci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(dev, &vci, NULL, out_view));
}

static void upload_image_2d(VkDevice dev, VkPhysicalDevice phys, VkQueue q, VkCommandPool pool, VkImage image, uint32_t w, uint32_t h, const void* pixels, VkDeviceSize size) {
    VkBuffer staging;
    VkDeviceMemory staging_mem;
    create_buffer_and_memory(dev, phys, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging, &staging_mem);
    upload_to_buffer(dev, staging_mem, 0, pixels, size);

    VkCommandBuffer cmd;
    cmd = begin_single_use_cmd(dev, pool);

    VkImageMemoryBarrier2 to_copy;
    memset(&to_copy, 0, sizeof(to_copy));
    to_copy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_copy.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    to_copy.srcAccessMask = 0;
    to_copy.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_copy.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_copy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_copy.image = image;
    to_copy.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_copy.subresourceRange.baseMipLevel = 0;
    to_copy.subresourceRange.levelCount = 1;
    to_copy.subresourceRange.baseArrayLayer = 0;
    to_copy.subresourceRange.layerCount = 1;

    VkDependencyInfo dep0;
    memset(&dep0, 0, sizeof(dep0));
    dep0.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep0.imageMemoryBarrierCount = 1;
    dep0.pImageMemoryBarriers = &to_copy;
    vkCmdPipelineBarrier2(cmd, &dep0);

    VkBufferImageCopy region;
    memset(&region, 0, sizeof(region));
    region.bufferOffset = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = w;
    region.imageExtent.height = h;
    region.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier2 to_shader;
    memset(&to_shader, 0, sizeof(to_shader));
    to_shader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_shader.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_shader.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_shader.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    to_shader.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.image = image;
    to_shader.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_shader.subresourceRange.baseMipLevel = 0;
    to_shader.subresourceRange.levelCount = 1;
    to_shader.subresourceRange.baseArrayLayer = 0;
    to_shader.subresourceRange.layerCount = 1;

    VkDependencyInfo dep1;
    memset(&dep1, 0, sizeof(dep1));
    dep1.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep1.imageMemoryBarrierCount = 1;
    dep1.pImageMemoryBarriers = &to_shader;
    vkCmdPipelineBarrier2(cmd, &dep1);

    end_single_use_cmd(dev, q, pool, cmd);
    vkDestroyBuffer(dev, staging, NULL);
    vkFreeMemory(dev, staging_mem, NULL);
}

struct KTX2Header {
    uint8_t identifier[12];
    uint32_t vkFormat;
    uint32_t typeSize;
    uint32_t pixelWidth;
    uint32_t pixelHeight;
    uint32_t pixelDepth;
    uint32_t layerCount;
    uint32_t faceCount;
    uint32_t levelCount;
    uint32_t supercompressionScheme;
    uint32_t dfdByteOffset;
    uint32_t dfdByteLength;
    uint32_t kvdByteOffset;
    uint32_t kvdByteLength;
    uint64_t sgdByteOffset;
    uint64_t sgdByteLength;
};

struct KTX2LevelIndex {
    uint64_t byteOffset;
    uint64_t byteLength;
    uint64_t uncompressedByteLength;
};

static int parse_ktx2_header_and_levels(const uint8_t* bytes, size_t len, struct KTX2Header* out_hdr, struct KTX2LevelIndex* out_levels, uint32_t* out_level_count) {
    if (len < sizeof(struct KTX2Header)) {
        printf("KTX2: too small for header\n");
        return 0;
    }

    struct KTX2Header header;
    memcpy(&header, bytes, sizeof(header));

    static const uint8_t magic[12] = { 0xAB, 'K','T','X',' ','2','0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
    if (memcmp(header.identifier, magic, 12) != 0) {
        printf("KTX2: invalid magic\n");
        return 0;
    }

    uint32_t levelCount;
    levelCount = header.levelCount;
    if (levelCount == 0) {
        levelCount = 1;
    }

    size_t levelsBytes;
    size_t levelsOffset;
    levelsBytes = (size_t)levelCount * sizeof(struct KTX2LevelIndex);
    levelsOffset = sizeof(struct KTX2Header);
    if (len < levelsOffset + levelsBytes) {
        printf("KTX2: too small for level index\n");
        return 0;
    }

    memcpy(out_levels, bytes + levelsOffset, levelsBytes);

    *out_hdr = header;
    *out_level_count = levelCount;
    return 1;
}

static int create_texture_from_ktx2_astc(struct Machine* m, struct Swapchain* swapchain, const uint8_t* bytes, size_t len, Texture* out_tex) {
    struct KTX2Header header;
    struct KTX2LevelIndex levels[32];
    uint32_t levelCount;
    memset(levels, 0, sizeof(levels));
    levelCount = 0;

    if (!parse_ktx2_header_and_levels(bytes, len, &header, levels, &levelCount)) {
        printf("Failed to parse KTX2 header\n");
        return 0;
    }

    VkFormat format;
    format = (VkFormat)header.vkFormat;
    if (format != VK_FORMAT_ASTC_8x8_SRGB_BLOCK) {
        printf("Unexpected vkFormat in KTX2: %u\n", header.vkFormat);
    }

    uint32_t w;
    uint32_t h;
    w = header.pixelWidth;
    h = header.pixelHeight;

    uint64_t first;
    uint64_t last;
    first = UINT64_MAX;
    last = 0;

    for (uint32_t i = 0; i < levelCount; ++i) {
        if (levels[i].byteLength == 0) {
            continue;
        }
        if (levels[i].byteOffset < first) {
            first = levels[i].byteOffset;
        }
        uint64_t end;
        end = levels[i].byteOffset + levels[i].byteLength;
        if (end > last) {
            last = end;
        }
    }

    if (first == UINT64_MAX || last > (uint64_t)len) {
        printf("Invalid KTX2 level data offsets\n");
        return 0;
    }

    VkDevice dev;
    VkPhysicalDevice phys;
    VkQueue queue;
    VkCommandPool pool;
    dev = m->device;
    phys = m->physical_device;
    queue = m->queue_graphics;
    pool = swapchain->command_pool_graphics;

    create_image_2d_mipped(dev, phys, w, h, format, levelCount, VK_IMAGE_USAGE_SAMPLED_BIT, &out_tex->image, &out_tex->memory);

    VkDeviceSize pixelSize;
    pixelSize = (VkDeviceSize)(last - first);

    VkBuffer staging;
    VkDeviceMemory staging_mem;
    create_buffer_and_memory(dev, phys, pixelSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging, &staging_mem);
    upload_to_buffer(dev, staging_mem, 0, bytes + first, (size_t)pixelSize);

    VkCommandBuffer cmd;
    cmd = begin_single_use_cmd(dev, pool);

    VkImageMemoryBarrier2 to_copy;
    memset(&to_copy, 0, sizeof(to_copy));
    to_copy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_copy.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    to_copy.srcAccessMask = 0;
    to_copy.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_copy.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_copy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_copy.image = out_tex->image;
    to_copy.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_copy.subresourceRange.baseMipLevel = 0;
    to_copy.subresourceRange.levelCount = levelCount;
    to_copy.subresourceRange.baseArrayLayer = 0;
    to_copy.subresourceRange.layerCount = 1;

    VkDependencyInfo dep0;
    memset(&dep0, 0, sizeof(dep0));
    dep0.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep0.imageMemoryBarrierCount = 1;
    dep0.pImageMemoryBarriers = &to_copy;
    vkCmdPipelineBarrier2(cmd, &dep0);

    VkBufferImageCopy regions[32];
    uint32_t regionCount;
    memset(regions, 0, sizeof(regions));
    regionCount = 0;

    for (uint32_t level = 0; level < levelCount; ++level) {
        if (levels[level].byteLength == 0) {
            continue;
        }

        uint32_t mipW;
        uint32_t mipH;
        mipW = w >> level;
        mipH = h >> level;
        if (mipW == 0) {
            mipW = 1;
        }
        if (mipH == 0) {
            mipH = 1;
        }

        VkBufferImageCopy r;
        memset(&r, 0, sizeof(r));
        r.bufferOffset = (VkDeviceSize)(levels[level].byteOffset - first);
        r.bufferRowLength = 0;
        r.bufferImageHeight = 0;
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.mipLevel = level;
        r.imageSubresource.baseArrayLayer = 0;
        r.imageSubresource.layerCount = 1;
        r.imageOffset.x = 0;
        r.imageOffset.y = 0;
        r.imageOffset.z = 0;
        r.imageExtent.width = mipW;
        r.imageExtent.height = mipH;
        r.imageExtent.depth = 1;

        regions[regionCount] = r;
        regionCount++;
    }

    vkCmdCopyBufferToImage(cmd, staging, out_tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, regionCount, regions);

    VkImageMemoryBarrier2 to_shader;
    memset(&to_shader, 0, sizeof(to_shader));
    to_shader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_shader.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_shader.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_shader.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    to_shader.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.image = out_tex->image;
    to_shader.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_shader.subresourceRange.baseMipLevel = 0;
    to_shader.subresourceRange.levelCount = levelCount;
    to_shader.subresourceRange.baseArrayLayer = 0;
    to_shader.subresourceRange.layerCount = 1;

    VkDependencyInfo dep1;
    memset(&dep1, 0, sizeof(dep1));
    dep1.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep1.imageMemoryBarrierCount = 1;
    dep1.pImageMemoryBarriers = &to_shader;
    vkCmdPipelineBarrier2(cmd, &dep1);

    end_single_use_cmd(dev, queue, pool, cmd);

    vkDestroyBuffer(dev, staging, NULL);
    vkFreeMemory(dev, staging_mem, NULL);

    create_view_2d_mipped(dev, out_tex->image, format, levelCount, &out_tex->view);

    return 1;
}

static void create_dummy_texture(struct Machine* machine, struct Swapchain* swapchain) {
    uint32_t pixel;
    pixel = 0xFFFFFFFFu;

    dummy_texture.image = VK_NULL_HANDLE;
    dummy_texture.memory = VK_NULL_HANDLE;
    dummy_texture.view = VK_NULL_HANDLE;

    create_image_2d_mipped(machine->device, machine->physical_device, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_USAGE_SAMPLED_BIT, &dummy_texture.image, &dummy_texture.memory);
    upload_image_2d(machine->device, machine->physical_device, machine->queue_graphics, swapchain->command_pool_graphics, dummy_texture.image, 1, 1, &pixel, sizeof(pixel));
    create_view_2d_mipped(machine->device, dummy_texture.image, VK_FORMAT_R8G8B8A8_UNORM, 1, &dummy_texture.view);

    for (uint32_t i = 0; i < MAX_TEXTURES; ++i) {
        textures[i] = dummy_texture;
    }

    texture_count = 0;
}

static void create_textures(struct Machine* machine, struct Swapchain* swapchain) {
    VkSamplerCreateInfo sci;
    memset(&sci, 0, sizeof(sci));
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 3.40282346638528859811704183484516925e+38F;
    VK_CHECK(vkCreateSampler(machine->device, &sci, NULL, &global_sampler));

    create_dummy_texture(machine, swapchain);

    if (create_texture_from_ktx2_astc(machine, swapchain, font_atlas, font_atlas_len, &textures[texture_count])) {
        texture_count++;
    } else {
        printf("Failed to create font atlas texture from KTX2\n");
    }

    if (create_texture_from_ktx2_astc(machine, swapchain, map, map_len, &textures[texture_count])) {
        texture_count++;
    } else {
        printf("Failed to create map texture from KTX2\n");
    }

    if (create_texture_from_ktx2_astc(machine, swapchain, height, height_len, &textures[texture_count])) {
        texture_count++;
    } else {
        printf("Failed to create height texture from KTX2\n");
    }

    if (create_texture_from_ktx2_astc(machine, swapchain, normals, normals_len, &textures[texture_count])) {
        texture_count++;
    } else {
        printf("Failed to create normals texture from KTX2\n");
    }

    if (create_texture_from_ktx2_astc(machine, swapchain, noise, noise_len, &textures[texture_count])) {
        texture_count++;
    } else {
        printf("Failed to create noise texture from KTX2\n");
    }

    if (create_texture_from_ktx2_astc(machine, swapchain, height_detail, height_detail_len, &textures[texture_count])) {
        texture_count++;
    } else {
        printf("Failed to create height detail texture from KTX2\n");
    }

    if (texture_count == 0) {
        printf("WARNING: no textures loaded; dummy will be used.\n");
    }
}

static void fill_texture_descriptor_infos(VkDescriptorImageInfo* out_infos, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        VkImageView view = textures[i].view;
        if (view == VK_NULL_HANDLE) view = dummy_texture.view; // hard safety
        out_infos[i].sampler = global_sampler;
        out_infos[i].imageView = view;
        out_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
}
