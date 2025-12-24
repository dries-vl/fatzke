#pragma once

#define MAX_TEXTURES  64

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
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
} Texture;

Texture textures[MAX_TEXTURES];
VkSampler global_sampler;
uint32_t texture_count = 0;

void create_image_2d_mipped(VkDevice dev, VkPhysicalDevice phys,
                            uint32_t w, uint32_t h,
                            VkFormat format,
                            uint32_t mipLevels,
                            VkImageUsageFlags usage,
                            VkImage* out_img, VkDeviceMemory* out_mem) {
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { w, h, 1 },
        .mipLevels = mipLevels,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(vkCreateImage(dev, &ici, NULL, out_img));

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, *out_img, &mr);

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = find_memory_type_index(phys, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    VK_CHECK(vkAllocateMemory(dev, &mai, NULL, out_mem));
    VK_CHECK(vkBindImageMemory(dev, *out_img, *out_mem, 0));
}

void create_view_2d_mipped(VkDevice dev, VkImage img,
                           VkFormat fmt, uint32_t mipLevels,
                           VkImageView* out_view) {
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount   = mipLevels,
            .baseArrayLayer = 0,
            .layerCount     = 1
        }
    };
    VK_CHECK(vkCreateImageView(dev, &vci, NULL, out_view));
}

void upload_image_2d(VkDevice dev, VkPhysicalDevice phys,
                     VkQueue q, VkCommandPool pool,
                     VkImage image, uint32_t w, uint32_t h,
                     const void* pixels, VkDeviceSize size) {
    // staging buffer
    VkBuffer staging; VkDeviceMemory staging_mem;
    create_buffer_and_memory(dev, phys, size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &staging, &staging_mem);
    upload_to_buffer(dev, staging_mem, 0, pixels, size);

    VkCommandBuffer cmd = begin_single_use_cmd(dev, pool);

    // layout undefined -> TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier2 to_copy = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = 0,
        .dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image         = image,
        .subresourceRange = { .aspectMask=VK_IMAGE_ASPECT_COLOR_BIT, .levelCount=1, .layerCount=1 },
    };
    vkCmdPipelineBarrier2(cmd, &(VkDependencyInfo){
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_copy,
    });

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .imageSubresource = { .aspectMask=VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel=0, .baseArrayLayer=0, .layerCount=1 },
        .imageExtent = { w, h, 1 },
    };
    vkCmdCopyBufferToImage(cmd, staging, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);

    // TRANSFER_DST -> SHADER_READ_ONLY
    VkImageMemoryBarrier2 to_shader = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .image         = image,
        .subresourceRange = { .aspectMask=VK_IMAGE_ASPECT_COLOR_BIT, .levelCount=1, .layerCount=1 },
    };
    vkCmdPipelineBarrier2(cmd, &(VkDependencyInfo){
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_shader,
    });

    end_single_use_cmd(dev, q, pool, cmd);
    vkDestroyBuffer(dev, staging, NULL);
    vkFreeMemory(dev, staging_mem, NULL);
}

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
static int parse_ktx2_header_and_levels(
    const uint8_t* bytes, size_t len,
    struct KTX2Header* out_hdr,
    struct KTX2LevelIndex* out_levels, // array sized for at least header.levelCount
    uint32_t* out_level_count) {
    if (len < sizeof(struct KTX2Header)) {
        printf("KTX2: too small for header\n");
        return 0;
    }

    struct KTX2Header header;
    memcpy(&header, bytes, sizeof(header));

    static const uint8_t magic[12] = {
        0xAB, 'K','T','X',' ','2','0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
    };
    if (memcmp(header.identifier, magic, 12) != 0) {
        printf("KTX2: invalid magic\n");
        return 0;
    }

    uint32_t levelCount = header.levelCount;
    if (levelCount == 0) levelCount = 1; // per spec 0 means 1 level

    size_t levelsBytes = (size_t)levelCount * sizeof(struct KTX2LevelIndex);
    size_t levelsOffset = sizeof(struct KTX2Header);
    if (len < levelsOffset + levelsBytes) {
        printf("KTX2: too small for level index\n");
        return 0;
    }

    memcpy(out_levels, bytes + levelsOffset, levelsBytes);

    *out_hdr = header;
    *out_level_count = levelCount;
    return 1;
}

int create_texture_from_ktx2_astc(
    struct Machine* m,
    struct Swapchain* swapchain, // for command_pool_graphics
    const uint8_t* bytes, size_t len,
    Texture* out_tex)
{
    struct KTX2Header header;
    struct KTX2LevelIndex levels[32];   // enough for typical mips; grow if needed
    uint32_t levelCount = 0;
    if (!parse_ktx2_header_and_levels(bytes, len, &header, levels, &levelCount)) {
        printf("Failed to parse KTX2 header\n");
        return 0;
    }

    // Expect ASTC 8x8 sRGB; KTX2 stores actual VkFormat value
    VkFormat format = (VkFormat)header.vkFormat;
    if (format != VK_FORMAT_ASTC_8x8_SRGB_BLOCK) {
        printf("Unexpected vkFormat in KTX2: %u\n", header.vkFormat);
        // you can still continue if you want
    }

    uint32_t w = header.pixelWidth;
    uint32_t h = header.pixelHeight;

    // Determine contiguous pixel-data range [first, last)
    uint64_t first = UINT64_MAX;
    uint64_t last  = 0;
    for (uint32_t i = 0; i < levelCount; ++i) {
        if (levels[i].byteLength == 0) continue;
        if (levels[i].byteOffset < first) first = levels[i].byteOffset;
        uint64_t end = levels[i].byteOffset + levels[i].byteLength;
        if (end > last) last = end;
    }
    if (first == UINT64_MAX || last > len) {
        printf("Invalid KTX2 level data offsets\n");
        return 0;
    }

    VkDevice dev = m->device;
    VkPhysicalDevice phys = m->physical_device;
    VkQueue queue = m->queue_graphics;
    VkCommandPool pool = swapchain->command_pool_graphics;

    // 1) Create compressed image with all mips
    create_image_2d_mipped(dev, phys, w, h, format,
                            levelCount,
                            VK_IMAGE_USAGE_SAMPLED_BIT,
                            &out_tex->image, &out_tex->memory);

    // 2) Create staging buffer containing only pixel range [first,last)
    VkDeviceSize pixelSize = (VkDeviceSize)(last - first);
    VkBuffer staging;
    VkDeviceMemory staging_mem;
    create_buffer_and_memory(dev, phys, pixelSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &staging, &staging_mem);

    upload_to_buffer(dev, staging_mem, 0, bytes + first, (size_t)pixelSize);

    // 3) Record copy commands
    VkCommandBuffer cmd = begin_single_use_cmd(dev, pool);

    // UNDEFINED -> TRANSFER_DST_OPTIMAL for all mips
    VkImageMemoryBarrier2 to_copy = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = 0,
        .dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image         = out_tex->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount   = levelCount,
            .baseArrayLayer = 0,
            .layerCount     = 1
        },
    };
    vkCmdPipelineBarrier2(cmd, &(VkDependencyInfo){
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &to_copy,
    });

    // 4) Setup copy regions for each mip
    VkBufferImageCopy regions[32];
    uint32_t regionCount = 0;
    for (uint32_t level = 0; level < levelCount; ++level) {
        if (levels[level].byteLength == 0) continue;

        uint32_t mipW = w >> level;
        uint32_t mipH = h >> level;
        if (mipW == 0) mipW = 1;
        if (mipH == 0) mipH = 1;

        VkBufferImageCopy r = {
            .bufferOffset = (VkDeviceSize)(levels[level].byteOffset - first),
            .bufferRowLength   = 0, // tightly packed
            .bufferImageHeight = 0,
            .imageSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel   = level,
                .baseArrayLayer = 0,
                .layerCount     = 1
            },
            .imageOffset = { 0, 0, 0 },
            .imageExtent = { mipW, mipH, 1 },
        };
        regions[regionCount++] = r;
    }

    vkCmdCopyBufferToImage(cmd, staging, out_tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           regionCount, regions);

    // 5) TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
    VkImageMemoryBarrier2 to_shader = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .image         = out_tex->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount   = levelCount,
            .baseArrayLayer = 0,
            .layerCount     = 1
        },
    };
    vkCmdPipelineBarrier2(cmd, &(VkDependencyInfo){
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &to_shader,
    });

    end_single_use_cmd(dev, queue, pool, cmd);

    vkDestroyBuffer(dev, staging, NULL);
    vkFreeMemory(dev, staging_mem, NULL);

    // 6) Create view
    create_view_2d_mipped(dev, out_tex->image, format, levelCount, &out_tex->view);

    return 1;
}

void create_textures(struct Machine *machine, struct Swapchain *swapchain) {
    // sampler
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 3.40282346638528859811704183484516925e+38F, // float max
    };
    VK_CHECK(vkCreateSampler(machine->device, &sci, NULL, &global_sampler));

    // font atlas from KTX2 ASTC
    if (!create_texture_from_ktx2_astc(machine, swapchain, font_atlas, font_atlas_len, &textures[0]))
        printf("Failed to create font atlas texture from KTX2\n");
    texture_count++;
    if (!create_texture_from_ktx2_astc(machine, swapchain, map, map_len, &textures[1]))
        printf("Failed to create map texture from KTX2\n");
    texture_count++;
    if (!create_texture_from_ktx2_astc(machine, swapchain, height, height_len, &textures[2]))
        printf("Failed to create height texture from KTX2\n");
    texture_count++;
    if (!create_texture_from_ktx2_astc(machine, swapchain, normals, normals_len, &textures[3]))
        printf("Failed to create normals texture from KTX2\n");
    texture_count++;
    if (!create_texture_from_ktx2_astc(machine, swapchain, noise, noise_len, &textures[4]))
        printf("Failed to create noise texture from KTX2\n");
    texture_count++;
    if (!create_texture_from_ktx2_astc(machine, swapchain, height_detail, height_detail_len, &textures[5]))
        printf("Failed to create height detail texture from KTX2\n");
    texture_count++;

    // later: map/map, world map, per-mesh textures, etc.
}
