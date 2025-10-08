#include "header.h"

#include "mesh.h" // todo: remove

#ifdef _WIN32
#else
#include <X11/Xlib.h>
#include <time.h>
#endif
#include <vulkan/vulkan.h>

#include <stdlib.h> // todo: get rid of this and all calloc/malloc/free
#include <stdio.h> // todo: get rid of this and link with compiled spv as object

// todo: create vk_shader (to have one big shader with multiple entrypoints, have shader hot reloading, ...)
// todo: create vk_texture (loading textures, maybe hot reloading textures, ...)

// 1 for double buffering (one of ours, two of present engine) (technically triple buffered, but modern apis don't seem to do actual double buffering)
// one is being scanned out, one is done and waiting (this one is also considered 'presented' but not scanned out yet)
// and one of ours being rendered (this is this 1 frame that is 'in flight')
#define MAX_FRAMES_IN_FLIGHT 1
VkExtent2D OFFSCREEN_EXTENT = { 1280, 720 };

struct Renderer {
    // render pipeline
    VkPipeline                graphics_pipeline;
    // compute pipelines
    VkDescriptorSetLayout     compute_set_layout;
    VkPipelineLayout          common_pipeline_layout;
    VkPipeline                compute_pipelines[2];
    VkDescriptorPool          descriptor_pool;
    VkDescriptorSet           descriptor_set;
    // buffers
    VkBuffer                  buffer_instances;  VkDeviceMemory memory_instances;
    VkBuffer                  buffer_visible;    VkDeviceMemory memory_visible;
    VkBuffer                  buffer_counters;   VkDeviceMemory memory_counters;
    VkBuffer                  buffer_positions;  VkDeviceMemory memory_positions;
    VkBuffer                  buffer_normals;    VkDeviceMemory memory_normals;
    VkBuffer                  buffer_uvs;        VkDeviceMemory memory_uvs;
    VkBuffer                  buffer_index_ib;   VkDeviceMemory memory_index_ib;
    // render to offscreen image
    VkRenderPass              offscreen_render_pass;
    VkImage                   offscreen_image;   VkDeviceMemory offscreen_memory;
    VkImageView               offscreen_view;
    VkFramebuffer             offscreen_fb;
    // sync
    uint32_t                  frame_slot;
    VkSemaphore               sem_image_available[MAX_FRAMES_IN_FLIGHT];
    VkFence                   fe_in_flight[MAX_FRAMES_IN_FLIGHT];
    // debug
    #if DEBUG == 1
    double      gpu_ticks_to_ns;
    uint64_t    frame_id_counter;
    uint64_t    frame_id_per_slot[MAX_FRAMES_IN_FLIGHT];
    f32         start_time_per_slot[MAX_FRAMES_IN_FLIGHT];
    #endif
};

#include "vk_util.h"
#include "vk_machine.h"
#include "vk_swapchain.h"

#pragma region HELPER
void* map_entire_allocation(VkDevice device, VkDeviceMemory memory, VkDeviceSize size_bytes) {
    void* data = NULL;
    VK_CHECK(vkMapMemory(device, memory, 0, size_bytes, 0, &data));
    return data;
}
static VkShaderModule create_shader_module_from_spirv(VkDevice device, const char* file_path) {
    FILE* file = fopen(file_path, "rb");
    if (!file) {
        printf("Failed to open shader file: %s\n", file_path);
        _exit(0);
    }
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);

    uint32_t* code = (uint32_t*)malloc((size_t)file_size);
    fread(code, 1, (size_t)file_size, file);
    fclose(file);

    VkShaderModuleCreateInfo create_info = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = (size_t)file_size,
        .pCode    = code
    };
    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(device, &create_info, NULL, &module));
    free(code);
    return module;
}
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
void create_buffer_and_memory(VkDevice device, VkPhysicalDevice phys,
                              VkDeviceSize size, VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags props,
                              VkBuffer* out_buf, VkDeviceMemory* out_mem) {
    VkBufferCreateInfo buf_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VK_CHECK(vkCreateBuffer(device, &buf_info, NULL, out_buf));

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(device, *out_buf, &mem_reqs);

    VkMemoryAllocateInfo alloc_info = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = mem_reqs.size,  // <-- use driver-required size
        .memoryTypeIndex = find_memory_type_index(phys, mem_reqs.memoryTypeBits, props)
    };
    VK_CHECK(vkAllocateMemory(device, &alloc_info, NULL, out_mem));
    VK_CHECK(vkBindBufferMemory(device, *out_buf, *out_mem, 0));
}
static void upload_to_buffer(VkDevice dev, VkDeviceMemory mem, size_t bytes, const void *src) {
    void* dst = NULL;
    VK_CHECK(vkMapMemory(dev, mem, 0, bytes, 0, &dst));
    memcpy(dst, src, bytes);
    vkUnmapMemory(dev, mem);
}

#pragma region MAIN
void key_input_callback(void* ud, enum KEYBOARD_BUTTON key, enum INPUT_STATE state) {
    if (key == KEYBOARD_ESCAPE) {_exit(0);}
}
void mouse_input_callback(void* ud, i32 x, i32 y, enum MOUSE_BUTTON button, enum INPUT_STATE state) {}
int main(void) {
    // setup window
    pf_time_reset();
    WINDOW window = pf_create_window(NULL, key_input_callback,mouse_input_callback);
    pf_timestamp("Create window");

    // set env to avoid loading nvidia icd (1000ms)
    if (!USE_DISCRETE_GPU) {
        //extern int putenv(char*);
        putenv((char*)"VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json");
        putenv((char*)"VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json");
        pf_timestamp("Setup environment");
    }

    // setup vulkan on the machine
    struct Machine machine = create_machine(window);
    struct Swapchain swapchain = create_swapchain(&machine,window);
    struct Renderer renderer = {0};
    
    #pragma region OFFSCREEN
    // create intermediary target image
    VkImageCreateInfo ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = swapchain.swapchain_format,
        .extent        = { OFFSCREEN_EXTENT.width, OFFSCREEN_EXTENT.height, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    VK_CHECK(vkCreateImage(machine.device, &ci, NULL, &renderer.offscreen_image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(machine.device, renderer.offscreen_image, &req);
    VkMemoryAllocateInfo ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = find_memory_type_index(machine.physical_device, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    VK_CHECK(vkAllocateMemory(machine.device, &ai, NULL, &renderer.offscreen_memory));
    VK_CHECK(vkBindImageMemory(machine.device, renderer.offscreen_image, renderer.offscreen_memory, 0));
    VkImageViewCreateInfo view_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = renderer.offscreen_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = swapchain.swapchain_format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1
        }
    };
    VK_CHECK(vkCreateImageView(machine.device, &view_ci, NULL, &renderer.offscreen_view));
    
    // create intermediary render pass and framebuffer
    VkAttachmentDescription color = {
        .format         = swapchain.swapchain_format,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    VkAttachmentReference color_ref = {
        .attachment = 0,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    VkSubpassDescription subpass = {
        .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &color_ref
    };
    VkRenderPassCreateInfo rp = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &color,
        .subpassCount    = 1, .pSubpasses  = &subpass
    };
    VK_CHECK(vkCreateRenderPass(machine.device, &rp, NULL, &renderer.offscreen_render_pass));
    VkImageView off_attachments[1] = { renderer.offscreen_view };
    VkFramebufferCreateInfo fb_ci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass      = renderer.offscreen_render_pass,
        .attachmentCount = 1,
        .pAttachments    = off_attachments,
        .width  = OFFSCREEN_EXTENT.width,
        .height = OFFSCREEN_EXTENT.height,
        .layers = 1
    };
    VK_CHECK(vkCreateFramebuffer(machine.device, &fb_ci, NULL, &renderer.offscreen_fb));
    #pragma endregion
    
    VkShaderModule shader_module = create_shader_module_from_spirv(machine.device, "static/shaders.spv");

    #pragma region COMPUTE PIPELINE
    #define BINDINGS 5
    VkDescriptorSetLayoutBinding bindings[BINDINGS];
    for (uint32_t i = 0; i < BINDINGS; ++i) {
        bindings[i] = (VkDescriptorSetLayoutBinding) {
            .binding         = i,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT
        };
    }
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = BINDINGS,
        .pBindings    = bindings
    };
    VK_CHECK(vkCreateDescriptorSetLayout(machine.device, &set_layout_info, NULL, &renderer.compute_set_layout));

    /* -------- Pipeline Layouts -------- */
    VkPipelineLayoutCreateInfo common_pipeline_info = {
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts    = &renderer.compute_set_layout
    };
    VK_CHECK(vkCreatePipelineLayout(machine.device, &common_pipeline_info, NULL, &renderer.common_pipeline_layout));

    /* -------- Compute Pipelines -------- */
    VkComputePipelineCreateInfo compute_infos[2] = {
        {
            .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage  = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader_module, .pName = "cs_build_visible" },
            .layout = renderer.common_pipeline_layout
        },
        {
            .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage  = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader_module,  .pName = "cs_prepare_indirect"  },
            .layout = renderer.common_pipeline_layout
        }
    };
    VK_CHECK(vkCreateComputePipelines(machine.device,VK_NULL_HANDLE,2,compute_infos,NULL,renderer.compute_pipelines));
    #pragma endregion

    #pragma region GRAPHICS PIPELINE
    VkPipelineShaderStageCreateInfo shader_stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,   .module = shader_module, .pName = "vs_main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = shader_module, .pName = "fs_main" }
    };

    VkVertexInputBindingDescription bindings_vi[2] = {
        { .binding = 0, .stride = sizeof(uint32_t)*2,    .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE },
        { .binding = 1, .stride = sizeof(uint16_t)*2,    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX   }
    };
    VkVertexInputAttributeDescription attrs_vi[2] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_UINT,   .offset = 0 },
        { .location = 1, .binding = 1, .format = VK_FORMAT_R16G16_UNORM,  .offset = 0 }
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = 2, .pVertexBindingDescriptions   = bindings_vi,
        .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = attrs_vi
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };
    VkViewport viewport = { // -> can be moved to swapchain as constant
        .x = 0, .y = 0,
        .width  = (float)swapchain.swapchain_extent.width,
        .height = (float)swapchain.swapchain_extent.height,
        .minDepth = 0.f, .maxDepth = 1.f
    };
    VkRect2D scissor = { .offset = {0,0}, .extent = swapchain.swapchain_extent };
    VkPipelineViewportStateCreateInfo viewport_state = { // -> can be moved to swapchain as constant
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport,
        .scissorCount  = 1, .pScissors  = &scissor
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_NONE,
        .frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth   = 1.0f
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };
    VkPipelineColorBlendAttachmentState color_blend_attachment = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo color_blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &color_blend_attachment
    };
    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dyn_states
    };
    VkGraphicsPipelineCreateInfo graphics_info = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount          = 2,
        .pStages             = shader_stages,
        .pVertexInputState   = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState      = &viewport_state,
        .pRasterizationState = &raster,
        .pMultisampleState   = &multisample,
        .pColorBlendState    = &color_blend,
        .layout              = renderer.common_pipeline_layout,
        .renderPass          = renderer.offscreen_render_pass,
        .pDynamicState       = &dyn,
        .subpass             = 0
    };
    VK_CHECK(vkCreateGraphicsPipelines(machine.device, VK_NULL_HANDLE, 1, &graphics_info, NULL, &renderer.graphics_pipeline));
    #pragma endregion

    // destroy the shader module after using it
    vkDestroyShaderModule(machine.device, shader_module,  NULL);

    #pragma region BUFFERS
    const uint32_t kVertsPerMesh = 192;
    const uint32_t kIdxPerMesh   = 1116;
    uint32_t numMeshes    = 1;
    uint32_t numInstances = 1;
    VkDeviceSize size_instances = (VkDeviceSize)numInstances * sizeof(uint32_t) * 2; // two uints each
    VkDeviceSize size_visible   = (VkDeviceSize)numInstances * sizeof(uint32_t) * 2; // compacted visible (same layout)
    VkDeviceSize size_counters  = sizeof(VkDrawIndexedIndirectCommand);
    VkDeviceSize size_positions = (VkDeviceSize)numMeshes * kVertsPerMesh * sizeof(uint32_t);
    VkDeviceSize size_normals   = (VkDeviceSize)numMeshes * kVertsPerMesh * sizeof(uint32_t);
    VkDeviceSize size_uvs       = (VkDeviceSize)kVertsPerMesh * sizeof(uint16_t) * 2; // shared 256 UVs (R16G16_UNORM)
    VkDeviceSize size_indexIB   = (VkDeviceSize)kIdxPerMesh * sizeof(uint32_t);       // fixed index list 0..255 pattern

    const VkMemoryPropertyFlags host_visible_coherent =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    // INSTANCES
    create_buffer_and_memory(machine.device, machine.physical_device, size_instances,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        host_visible_coherent, &renderer.buffer_instances, &renderer.memory_instances);

    // VISIBLE (UAV + instance-rate VB)
    create_buffer_and_memory(machine.device, machine.physical_device, size_visible,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        host_visible_coherent, &renderer.buffer_visible, &renderer.memory_visible);

    // COUNTERS (UAV + indirect)
    create_buffer_and_memory(machine.device, machine.physical_device, size_counters,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        host_visible_coherent, &renderer.buffer_counters, &renderer.memory_counters);

    // POSITIONS SSBO (you can make this device-local with a staging upload later)
    create_buffer_and_memory(machine.device, machine.physical_device, size_positions,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        host_visible_coherent, &renderer.buffer_positions, &renderer.memory_positions);

    // NORMALS SSBO
    create_buffer_and_memory(machine.device, machine.physical_device, size_normals,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        host_visible_coherent, &renderer.buffer_normals, &renderer.memory_normals);

    // UV vertex buffer (shared 256 UVs; per-vertex VB)
    create_buffer_and_memory(machine.device, machine.physical_device, size_uvs,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        host_visible_coherent, &renderer.buffer_uvs, &renderer.memory_uvs);

    // Index buffer (fixed 1152 indices)
    create_buffer_and_memory(machine.device, machine.physical_device, size_indexIB,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        host_visible_coherent /* or DEVICE_LOCAL with staging */,
        &renderer.buffer_index_ib, &renderer.memory_index_ib);

    pf_timestamp("Buffers created");

    // upload the data for: uvs, positions, normals, indices, instances
    {
        upload_to_buffer(machine.device, renderer.memory_positions, sizeof(uint32_t)*kVertsPerMesh, g_positions_mesh);
        upload_to_buffer(machine.device, renderer.memory_normals,   sizeof(uint32_t)*kVertsPerMesh, g_normals_mesh);
        upload_to_buffer(machine.device, renderer.memory_uvs,       sizeof(uint16_t)*2*kVertsPerMesh, g_uvs_mesh);
        upload_to_buffer(machine.device, renderer.memory_index_ib,  sizeof(uint32_t)*kIdxPerMesh, g_indices_mesh);
        uint64_t instance_zeroed = 0;
        upload_to_buffer(machine.device, renderer.memory_instances, sizeof(uint64_t)*1, &instance_zeroed);
    }

    /* -------- Descriptor Pool & Set -------- */
    VkDescriptorPoolSize pool_size = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = BINDINGS };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = 1,
        .poolSizeCount = 1,
        .pPoolSizes    = &pool_size
    };
    VK_CHECK(vkCreateDescriptorPool(machine.device, &pool_info, NULL, &renderer.descriptor_pool));

    VkDescriptorSetAllocateInfo set_alloc_info = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = renderer.descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &renderer.compute_set_layout
    };
    VK_CHECK(vkAllocateDescriptorSets(machine.device, &set_alloc_info, &renderer.descriptor_set));

    VkDescriptorBufferInfo buffer_infos[BINDINGS] = {
        { renderer.buffer_instances, 0, size_instances }, // binding 0
        { renderer.buffer_visible,   0, size_visible   }, // binding 1
        { renderer.buffer_counters,  0, size_counters  }, // binding 2
        { renderer.buffer_positions, 0, size_positions }, // binding 3
        { renderer.buffer_normals,   0, size_normals   }  // binding 4
    };
    VkWriteDescriptorSet writes[BINDINGS];
    for (uint32_t i = 0; i < BINDINGS; ++i) {
        writes[i] = (VkWriteDescriptorSet) {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = renderer.descriptor_set,
            .dstBinding      = i,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo     = &buffer_infos[i]
        };
    }
    vkUpdateDescriptorSets(machine.device, BINDINGS, writes, 0, NULL);
    pf_timestamp("Descriptors created");

    #pragma region FRAME LOOP
    #if DEBUG == 1
    renderer.frame_id_counter = 1;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        renderer.frame_id_per_slot[i]   = 0;
    }
    PFN_vkGetCalibratedTimestampsEXT vkGetCalibratedTimestampsEXT =
    (PFN_vkGetCalibratedTimestampsEXT)vkGetDeviceProcAddr(machine.device, "vkGetCalibratedTimestampsEXT");
    if (!vkGetCalibratedTimestampsEXT) {
        printf("vkGetCalibratedTimestampsEXT not available — vkGetCalibratedTimestampsEXT not supported.\n");
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(machine.physical_device, &props);
    renderer.gpu_ticks_to_ns = props.limits.timestampPeriod; // 1 tick == this many ns

    PFN_vkWaitForPresentKHR vkWaitForPresentKHR =
    (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(machine.device, "vkWaitForPresentKHR");
    if (!vkWaitForPresentKHR) {
        printf("vkWaitForPresentKHR not available — VK_KHR_present_wait not supported.\n");
    }
    uint64_t presented_frame_ids[MAX_FRAMES_IN_FLIGHT] = {0};
    #endif

    // create semaphore linked to swapchain image (for gpu to wait for this swapchain image to be released) 
    // create fence (for cpu to wait for one of the command buffers to finish on the gpu)
    VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT }; // start signaled so first frame doesn't block
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VK_CHECK(vkCreateSemaphore(machine.device, &semaphore_info, NULL, &renderer.sem_image_available[i]));
        VK_CHECK(vkCreateFence(machine.device, &fence_info, NULL, &renderer.fe_in_flight[i]));
    }
    
    // take out two of the four images to have only two images in use
    // this way we only ever render a third frame when we're sure the first has been scanned out
    VkSemaphore parking_semaphores[3];
    uint32_t parked_images[3];
    for (uint32_t i = 0; i < swapchain.swapchain_image_count - MAX_FRAMES_IN_FLIGHT - 1; ++i) {
        VK_CHECK(vkCreateSemaphore(machine.device, &semaphore_info, NULL, &parking_semaphores[i]));
        VkResult parking_result = vkAcquireNextImageKHR(machine.device, swapchain.swapchain, UINT64_MAX, parking_semaphores[i], VK_NULL_HANDLE, &parked_images[i]);
    }

    // TODO: ADD MORE QUERIES TO TIME; COMPUTE, GRAPHICS, BLIT
    renderer.frame_slot = 0;
    while (pf_poll_events(window)) {
        // block on the fence here to avoid having too many frames in flight
        VK_CHECK(vkWaitForFences(machine.device, 1, &renderer.fe_in_flight[renderer.frame_slot], VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(machine.device, 1, &renderer.fe_in_flight[renderer.frame_slot])); // set unsignaled again

        #if DEBUG == 1
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            uint64_t presented_frame_id = presented_frame_ids[i];
            if (!presented_frame_id) continue;
            VkResult r = vkWaitForPresentKHR(machine.device, swapchain.swapchain, presented_frame_id, 0); // timeout zero for non-blocking
            if (r == VK_SUCCESS || r == VK_ERROR_DEVICE_LOST) { // -4 device lost instead of success somehow...
                f32 present_time = (f32) (pf_ns_now() - pf_ns_start()) / 1e6;
                printf("[%llu] presented at %.3f ms\n",presented_frame_id, present_time);
                presented_frame_ids[i] = 0; // clear slot
                f32 latency = present_time - renderer.start_time_per_slot[i];
                // presented means its ready to be scanned out, still has to wait up to 16ms for vsync for actual scanout
                printf("[%llu] latency until 'present' %.3f ms\n", presented_frame_id, latency);
            };
        }
        uint64_t last_frame_id = renderer.frame_id_per_slot[renderer.frame_slot];
        uint32_t image = swapchain.previous_frame_image_index[renderer.frame_slot];
        if (image != UINT32_MAX) {
            uint32_t q_first = image * QUERIES_PER_IMAGE;
            uint32_t q_last  = q_first + 1;
            uint64_t ticks[2] = {0,0};
            // calibrated sample
            uint64_t ts[2], maxDev = 0;
            double   offset_ns = 0.0;
            if (vkGetCalibratedTimestampsEXT) {
                VkCalibratedTimestampInfoEXT infos[2] = {
                    { .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, .timeDomain = VK_TIME_DOMAIN_DEVICE_EXT },
                    { .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT }
                };
                // Take the sample as close as possible to the read
                vkGetCalibratedTimestampsEXT(machine.device, 2, infos, ts, &maxDev);
                // Map GPU ticks -> CPU ns using your device period
                double gpu_now_ns = (double)ts[0] * renderer.gpu_ticks_to_ns;
                double cpu_now_ns = (double)ts[1];
                offset_ns = cpu_now_ns - gpu_now_ns;  // add this to any GPU ns to place on CPU timeline
            }
            // Now read the two GPU timestamps for that image (they’re done because we waited the fence)
            VkResult qr = vkGetQueryPoolResults(
                machine.device,
                swapchain.query_pool,
                q_first,  /* firstQuery */
                2,        /* queryCount */
                sizeof(ticks), ticks,
                sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT /* WAIT_BIT not needed after fence wait */);
            if (qr == VK_SUCCESS) {
                double gpu_begin_ns = (double)ticks[0] * renderer.gpu_ticks_to_ns;
                double gpu_end_ns   = (double)ticks[1] * renderer.gpu_ticks_to_ns;
                double gpu_dur_ms   = (gpu_end_ns - gpu_begin_ns) * 1e-6;
                // CPU mapping
                double cpu_mapped_begin_ms = (offset_ns + gpu_begin_ns - pf_ns_start()) * 1e-6;
                double cpu_mapped_end_ms   = (offset_ns + gpu_end_ns   - pf_ns_start()) * 1e-6;
                printf("[%llu] past fence at %.3fms\n", (unsigned long long)last_frame_id, (f32) (pf_ns_now() - pf_ns_start()) / 1e6);
                printf("[%llu] gpu time %.3fms - %.3fms [%.3fms]\n",(unsigned long long)last_frame_id, cpu_mapped_begin_ms, cpu_mapped_end_ms, gpu_dur_ms);
            }
            // Clear slot so we don’t read twice
            swapchain.previous_frame_image_index[renderer.frame_slot] = UINT32_MAX;
        }
        #endif

        // get the next swapchain image (block until one is available)
        uint32_t swap_image_index = 0;
        VkResult acquire_result = vkAcquireNextImageKHR(machine.device, swapchain.swapchain, UINT64_MAX-1, renderer.sem_image_available[renderer.frame_slot], VK_NULL_HANDLE, &swap_image_index);
        // recreate the swapchain if the window resized
        if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(&machine, &renderer, &swapchain, window); continue; }
        if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) { printf("vkAcquireNextImageKHR failed: %d\n", acquire_result); break; }

        // start recording for this frame's command buffer
        f32 frame_start_time = (f32) (pf_ns_now() - pf_ns_start()) / 1e6;
        VkCommandBuffer cmd = swapchain.command_buffers_per_image[swap_image_index];
        VK_CHECK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));
        #if DEBUG == 1
        uint32_t query0 = swap_image_index * QUERIES_PER_IMAGE + 0; // begin
        uint32_t query1 = query0 + 1; // end
        // Reset just the two queries for this image before writing them
        vkCmdResetQueryPool(cmd, swapchain.query_pool, query0, QUERIES_PER_IMAGE);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, swapchain.query_pool, query0);
        #endif
        
        // Zero the counters on GPU
        vkCmdFillBuffer(cmd, renderer.buffer_counters, 0, size_counters, 0);
        
        // Make the TRANSFER writes visible to COMPUTE (and later INDIRECT/vertex input)
        VkMemoryBarrier2 xfer_to_compute = {
            .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT
        };
        VkDependencyInfo dep0 = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                  .memoryBarrierCount = 1, .pMemoryBarriers = &xfer_to_compute };
        vkCmdPipelineBarrier2(cmd, &dep0);

        // --- Compute passes ---
        vkCmdBindDescriptorSets(cmd,
                                VK_PIPELINE_BIND_POINT_COMPUTE,
                                renderer.common_pipeline_layout,
                                0, 1, &renderer.descriptor_set, 0, NULL);

        // Build visible (no culling): instanceCount = numInstances
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.compute_pipelines[0]);
        vkCmdDispatch(cmd, (numInstances+63)/64, 1, 1);

        // Prepare indirect
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.compute_pipelines[1]);
        vkCmdDispatch(cmd, 1, 1, 1);

        // Barrier: make compute writes visible to graphics/indirect stages
        VkMemoryBarrier2 mb = {
          .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
          .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
          .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
                           VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
          .dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT |
                           VK_ACCESS_2_SHADER_READ_BIT |
                           VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT
        };
        vkCmdPipelineBarrier2(cmd, &(VkDependencyInfo){ .sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount=1, .pMemoryBarriers=&mb });

        // --- render pass (use persistent framebuffer) ---
        VkClearValue clear_color = { .color = {{0, 0, 0, 1}} };
        VkRenderPassBeginInfo render_begin = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = renderer.offscreen_render_pass,
            .framebuffer = renderer.offscreen_fb,
            .renderArea = { .offset = {0,0}, .extent = OFFSCREEN_EXTENT },
            .clearValueCount = 1, .pClearValues = &clear_color
        };
        vkCmdBeginRenderPass(cmd, &render_begin, VK_SUBPASS_CONTENTS_INLINE);

        // set the resolution of the intermediary pass
        VkViewport vp = {
            .x = 0, .y = (float)OFFSCREEN_EXTENT.height, // flip to have y origin in bottom left
            .width  = (float)OFFSCREEN_EXTENT.width,
            .height = -(float)OFFSCREEN_EXTENT.height,
            .minDepth = 0.f, .maxDepth = 1.f
        };
        VkRect2D sc = { .offset = {0,0}, .extent = OFFSCREEN_EXTENT };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor (cmd, 0, 1, &sc);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.graphics_pipeline);
        // descriptors for VS (POSITIONS/NORMALS etc.)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.common_pipeline_layout, 0, 1, &renderer.descriptor_set, 0, NULL);
        // vertex buffers: [0]=VISIBLE_PACKED (instance), [1]=UVs (per-vertex)
        VkBuffer vbs[2]     = { renderer.buffer_visible, renderer.buffer_uvs };
        VkDeviceSize ofs[2] = { 0, 0 };
        vkCmdBindVertexBuffers(cmd, 0, 2, vbs, ofs);
        // index buffer: fixed 1152 indices (values 0..255 in your mesh order)
        vkCmdBindIndexBuffer(cmd, renderer.buffer_index_ib, 0, VK_INDEX_TYPE_UINT32);
        // one GPU-driven draw
        vkCmdDrawIndexedIndirect(cmd, renderer.buffer_counters, 0, 1, sizeof(VkDrawIndexedIndirectCommand));

        vkCmdEndRenderPass(cmd);
        
        #pragma region BLIT
        // B) Transition offscreen to TRANSFER_SRC, swapchain image to TRANSFER_DST
        VkImageMemoryBarrier2 off_to_src = {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
          .dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
          .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
          .oldLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          .image         = renderer.offscreen_image,
          .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 }
        };
        VkImageLayout old = swapchain.image_layouts[swap_image_index]; // UNDEFINED the first time, PRESENT thereafter
        VkImageMemoryBarrier2 pres_to_dst = {
            .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_NONE,     // coming from UNDEFINED or PRESENT; no producer
            .srcAccessMask = 0,
            .dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout     = old,
            .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image         = swapchain.swapchain_images[swap_image_index],
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 }
        };
        VkDependencyInfo depA = {
          .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
          .imageMemoryBarrierCount = 2,
          .pImageMemoryBarriers = (VkImageMemoryBarrier2[]){ off_to_src, pres_to_dst }
        };
        vkCmdPipelineBarrier2(cmd, &depA);
        // C) Blit low-res → swapchain (nearest or linear)
        VkImageBlit2 region = {
          .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
          .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
          .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
        };
        region.srcOffsets[0] = (VkOffset3D){0, 0, 0};
        region.srcOffsets[1] = (VkOffset3D){ (int)OFFSCREEN_EXTENT.width,
                                             (int)OFFSCREEN_EXTENT.height, 1 };
        region.dstOffsets[0] = (VkOffset3D){0, 0, 0};
        region.dstOffsets[1] = (VkOffset3D){ (int)swapchain.swapchain_extent.width,
                                             (int)swapchain.swapchain_extent.height, 1 };
        VkBlitImageInfo2 blit = {
          .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
          .srcImage = renderer.offscreen_image,
          .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          .dstImage = swapchain.swapchain_images[swap_image_index],
          .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          .regionCount = 1, .pRegions = &region,
          .filter = VK_FILTER_LINEAR // or VK_FILTER_NEAREST
        };
        vkCmdBlitImage2(cmd, &blit);
        // D) Swapchain back to PRESENT
        VkImageMemoryBarrier2 dst_to_present = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_NONE,
            .dstAccessMask = 0,
            .oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .image         = swapchain.swapchain_images[swap_image_index],
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 }
        };
        VkDependencyInfo depB = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                  .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &dst_to_present };
        vkCmdPipelineBarrier2(cmd, &depB);

        // Record the new layout for this image
        swapchain.image_layouts[swap_image_index] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        #if DEBUG == 1
        vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, swapchain.query_pool, query1);
        // bump counters, then advance slot
        renderer.frame_id_counter++;
        presented_frame_ids[renderer.frame_slot] = renderer.frame_id_counter;
        renderer.start_time_per_slot[renderer.frame_slot] = frame_start_time;
        renderer.frame_id_per_slot[renderer.frame_slot] = renderer.frame_id_counter;
        // Calibrated CPU+GPU now
        uint64_t ts[2], maxDev = 0;
        if (vkGetCalibratedTimestampsEXT) {
            VkCalibratedTimestampInfoEXT infos[2] = {
                { .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, .timeDomain = VK_TIME_DOMAIN_DEVICE_EXT },
                { .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT }
            };
            vkGetCalibratedTimestampsEXT(machine.device, 2, infos, ts, &maxDev);
            uint64_t gpu_now_ticks = ts[0];
            uint64_t cpu_now_ns    = ts[1]; // on Linux MONOTONIC is ns
        }
        #endif
        VK_CHECK(vkEndCommandBuffer(cmd));

        // (D) Submit: wait on acquire, signal render-finished, fence per-frame
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore present_ready = swapchain.present_ready_per_image[swap_image_index];
        VkSubmitInfo submit_info = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &renderer.sem_image_available[renderer.frame_slot],
            .pWaitDstStageMask  = &wait_stage,
            .commandBufferCount = 1, .pCommandBuffers = &cmd,
            .signalSemaphoreCount = 1, .pSignalSemaphores = &present_ready
        };
        VK_CHECK(vkQueueSubmit(machine.queue_graphics, 1, &submit_info, renderer.fe_in_flight[renderer.frame_slot]));

        // (E) Present: wait on render-finished
        VkPresentInfoKHR present_info = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &present_ready,
            .swapchainCount = 1, .pSwapchains = &swapchain.swapchain, .pImageIndices = &swap_image_index
        };
        #if DEBUG == 1
        VkPresentIdKHR present_id_info = {
            .sType          = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
            .pNext          = NULL,
            .swapchainCount = 1,
            .pPresentIds    = &renderer.frame_id_counter,
        };
        present_info.pNext = &present_id_info;
        #endif
        VkResult present_res = vkQueuePresentKHR(machine.queue_present, &present_info);
        if (present_res == VK_ERROR_OUT_OF_DATE_KHR || present_res == VK_SUBOPTIMAL_KHR) {
            recreate_swapchain(&machine, &renderer, &swapchain, window);
            continue;
        } else if (present_res != VK_SUCCESS) {
            printf("vkQueuePresentKHR failed: %d\n", present_res);
            break;
        }

        #if DEBUG == 1
        f32 frame_end_time = (f32) (pf_ns_now() - pf_ns_start()) / 1e6;
        printf("[%llu] cpu time %.3fms - %.3fms [%.3fms]\n",renderer.frame_id_counter, frame_start_time, frame_end_time, frame_end_time - frame_start_time);
        #endif
        swapchain.previous_frame_image_index[renderer.frame_slot] = swap_image_index;
        renderer.frame_slot = (renderer.frame_slot + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    return 0;
}
