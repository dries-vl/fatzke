#include "header.h"

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

#define MAX_FRAMES_IN_FLIGHT 1
VkExtent2D OFFSCREEN_EXTENT = { 128, 72 };

struct Renderer {
    // render pipeline
    VkPipelineLayout          graphics_pipeline_layout;
    VkPipeline                graphics_pipeline;
    // compute pipelines
    VkDescriptorSetLayout     compute_set_layout;
    VkPipelineLayout          compute_pipeline_layout;
    VkPipeline                pipeline_cs_instance;
    VkPipeline                pipeline_cs_prepare;
    VkPipeline                pipeline_cs_meshlet;
    VkDescriptorPool          descriptor_pool;
    VkDescriptorSet           descriptor_set;
    // buffers
    VkBuffer                  buffer_vertices;   VkDeviceMemory memory_vertices;
    VkBuffer                  buffer_instances;  VkDeviceMemory memory_instances;
    VkBuffer                  buffer_visible;    VkDeviceMemory memory_visible;
    VkBuffer                  buffer_counters;   VkDeviceMemory memory_counters;
    VkBuffer                  buffer_varyings;   VkDeviceMemory memory_varyings;
    VkBuffer                  buffer_indices;    VkDeviceMemory memory_indices;
    VkBuffer                  buffer_dispatch;   VkDeviceMemory memory_dispatch;
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

#pragma region MAIN
void key_input_callback(void* ud, enum KEYBOARD_BUTTON key, enum INPUT_STATE state) {
    if (key == KEYBOARD_ESCAPE) {_exit(0);}
}
void mouse_input_callback(void* ud, i32 x, i32 y, enum MOUSE_BUTTON button, enum INPUT_STATE state) {}
int main(void)
{
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
    /* -------- Descriptor Set Layout (compute) --------
       Bindings:
        0 INSTANCES (readonly), 1 VISIBLE (read_write),
        2 COUNTERS (read_write), 3 VERTICES (readonly),
        4 VARYINGS (read_write), 5 INDICES (read_write),
        6 DISPATCH (read_write)
    */
    VkDescriptorSetLayoutBinding bindings[7];
    for (uint32_t i = 0; i < 7; ++i) {
        bindings[i] = (VkDescriptorSetLayoutBinding) {
            .binding         = i,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT
        };
    }
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 7,
        .pBindings    = bindings
    };
    VK_CHECK(vkCreateDescriptorSetLayout(machine.device, &set_layout_info, NULL, &renderer.compute_set_layout));

    /* -------- Pipeline Layouts -------- */
    VkPipelineLayoutCreateInfo compute_pl_info = {
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts    = &renderer.compute_set_layout
    };
    VK_CHECK(vkCreatePipelineLayout(machine.device, &compute_pl_info, NULL, &renderer.compute_pipeline_layout));

    VkPipelineLayoutCreateInfo graphics_pl_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VK_CHECK(vkCreatePipelineLayout(machine.device, &graphics_pl_info, NULL, &renderer.graphics_pipeline_layout));

    /* -------- Compute Pipelines -------- */
    VkComputePipelineCreateInfo compute_infos[3] = {
        {
            .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage  = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader_module, .pName = "cs_instance" },
            .layout = renderer.compute_pipeline_layout
        },
        {
            .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage  = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader_module,  .pName = "cs_prepare"  },
            .layout = renderer.compute_pipeline_layout
        },
        {
            .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage  = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader_module,  .pName = "cs_meshlet"  },
            .layout = renderer.compute_pipeline_layout
        }
    };
    VkPipeline pipelines[3];
    VK_CHECK(vkCreateComputePipelines(
        machine.device,
        VK_NULL_HANDLE,
        3,
        compute_infos,
        NULL,
        pipelines
    ));
    renderer.pipeline_cs_instance = pipelines[0];
    renderer.pipeline_cs_prepare  = pipelines[1];
    renderer.pipeline_cs_meshlet  = pipelines[2];
    #pragma endregion

    #pragma region GRAPHICS PIPELINE
    VkPipelineShaderStageCreateInfo shader_stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,   .module = shader_module, .pName = "vs_main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = shader_module, .pName = "fs_main" }
    };

    // Vertex: vec2 position at location=0
    VkVertexInputBindingDescription vertex_binding = { .binding = 0, .stride = sizeof(float) * 2, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription vertex_attr  = { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = 1, .pVertexBindingDescriptions   = &vertex_binding,
        .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &vertex_attr
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
        .layout              = renderer.graphics_pipeline_layout,
        .renderPass          = renderer.offscreen_render_pass,
        .pDynamicState       = &dyn,
        .subpass             = 0
    };
    VK_CHECK(vkCreateGraphicsPipelines(machine.device, VK_NULL_HANDLE, 1, &graphics_info, NULL, &renderer.graphics_pipeline));
    #pragma endregion

    // destroy the shader module after using it
    vkDestroyShaderModule(machine.device, shader_module,  NULL);

    #pragma region BUFFERS
    /* -------- Buffers (host-visible for simplicity) -------- */
    const VkDeviceSize size_vertices = sizeof(float) * 6;             // 3 * vec2
    const VkDeviceSize size_instances = sizeof(float) * 2 * 4;        // 4 instances (vec2 offsets)
    const VkDeviceSize size_visible   = 256 * sizeof(uint32_t);
    const VkDeviceSize size_counters  = 5 * sizeof(uint32_t);         // matches VkDrawIndexedIndirectCommand fields
    const VkDeviceSize size_varyings  = MAX_VERTICES * sizeof(float) * 2;
    const VkDeviceSize size_indices   = MAX_INSTANCES * sizeof(uint32_t);
    const VkDeviceSize size_dispatch  = 3 * sizeof(uint32_t);         // x,y,z for vkCmdDispatchIndirect

    const VkMemoryPropertyFlags host_visible_coherent =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    create_buffer_and_memory(machine.device, machine.physical_device, size_vertices,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, host_visible_coherent,
        &renderer.buffer_vertices, &renderer.memory_vertices);

    create_buffer_and_memory(machine.device, machine.physical_device, size_instances,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, host_visible_coherent,
        &renderer.buffer_instances, &renderer.memory_instances);

    create_buffer_and_memory(machine.device, machine.physical_device, size_visible,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_visible_coherent,
        &renderer.buffer_visible, &renderer.memory_visible);

    create_buffer_and_memory(machine.device, machine.physical_device, size_counters,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        host_visible_coherent, &renderer.buffer_counters, &renderer.memory_counters);

    create_buffer_and_memory(machine.device, machine.physical_device, size_varyings,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, host_visible_coherent,
        &renderer.buffer_varyings, &renderer.memory_varyings);

    create_buffer_and_memory(machine.device, machine.physical_device, size_indices,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, host_visible_coherent,
        &renderer.buffer_indices, &renderer.memory_indices);

    create_buffer_and_memory(machine.device, machine.physical_device, size_dispatch,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, host_visible_coherent,
        &renderer.buffer_dispatch, &renderer.memory_dispatch);

    pf_timestamp("Buffers created");

    // Initialize vertex & instance data (same as your WGSL path)
    {
        float initial_vertices[6] = { 0.0f, 0.5f,  -0.5f,-0.5f,   0.5f,-0.5f };
        struct { float x, y; } initial_instances[] = {
            { -0.5f, -0.5f }, { 0.5f, -0.5f }, { -0.5f, 0.5f }, { 0.5f, 0.5f }
        };
        void* vertices_mapping = NULL;
        VK_CHECK(vkMapMemory(machine.device, renderer.memory_vertices, 0, size_vertices, 0, &vertices_mapping));
        memcpy(vertices_mapping, initial_vertices, sizeof(initial_vertices));
        void* instances_mapping = NULL;
        VK_CHECK(vkMapMemory(machine.device, renderer.memory_instances, 0, size_instances, 0, &instances_mapping));
        memcpy(instances_mapping, initial_instances, sizeof(initial_instances));
    }

    /* -------- Descriptor Pool & Set -------- */
    VkDescriptorPoolSize pool_size = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 7 };
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

    VkDescriptorBufferInfo buffer_infos[7] = {
        { .buffer = renderer.buffer_instances, .offset = 0, .range = size_instances }, // binding 0
        { .buffer = renderer.buffer_visible,   .offset = 0, .range = size_visible   }, // binding 1
        { .buffer = renderer.buffer_counters,  .offset = 0, .range = size_counters  }, // binding 2
        { .buffer = renderer.buffer_vertices,  .offset = 0, .range = size_vertices  }, // binding 3
        { .buffer = renderer.buffer_varyings,  .offset = 0, .range = size_varyings  }, // binding 4
        { .buffer = renderer.buffer_indices,   .offset = 0, .range = size_indices   }, // binding 5
        { .buffer = renderer.buffer_dispatch,  .offset = 0, .range = size_dispatch  }  // binding 6
    };
    VkWriteDescriptorSet writes[7];
    for (uint32_t i = 0; i < 7; ++i) {
        writes[i] = (VkWriteDescriptorSet) {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = renderer.descriptor_set,
            .dstBinding      = i,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo     = &buffer_infos[i]
        };
    }
    vkUpdateDescriptorSets(machine.device, 7, writes, 0, NULL);
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

    renderer.frame_slot = 0;
    while (pf_poll_events(window)) {
        // block on the fence here to avoid having to many frames queued up (latency)
        VK_CHECK(vkWaitForFences(machine.device, 1, &renderer.fe_in_flight[renderer.frame_slot], VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(machine.device, 1, &renderer.fe_in_flight[renderer.frame_slot])); // set unsignaled again

        #if DEBUG == 1
        f32 frame_start_time = (f32) (pf_ns_now() - pf_ns_start()) / 1e6;
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            uint64_t presented_frame_id = presented_frame_ids[i];
            if (!presented_frame_id) continue;
            VkResult r = vkWaitForPresentKHR(machine.device, swapchain.swapchain, presented_frame_id, 0); // timeout zero for non-blocking
            if (r == VK_SUCCESS || r == VK_ERROR_DEVICE_LOST) { // -4 device lost instead of success somehow...
                f32 present_time = (f32) (pf_ns_now() - pf_ns_start()) / 1e6;
                printf("[%llu] presented at %.3f ms\n",presented_frame_id, present_time);
                presented_frame_ids[i] = 0; // clear slot
                f32 latency = present_time - renderer.start_time_per_slot[i];
                printf("[%llu] latency %.3f ms\n", presented_frame_id, latency);
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
        VkResult acquire_result = vkAcquireNextImageKHR(machine.device, swapchain.swapchain, UINT64_MAX, renderer.sem_image_available[renderer.frame_slot], VK_NULL_HANDLE, &swap_image_index);
        // recreate the swapchain if the window resized
        if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(&machine, &renderer, &swapchain, window); continue; }
        if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) { printf("vkAcquireNextImageKHR failed: %d\n", acquire_result); break; }

        // start recording for this frame's command buffer
        VK_CHECK(vkResetCommandBuffer(swapchain.command_buffers_per_image[swap_image_index], 0));
        VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VK_CHECK(vkBeginCommandBuffer(swapchain.command_buffers_per_image[swap_image_index], &begin_info));
        #if DEBUG == 1
        uint32_t query0 = swap_image_index * QUERIES_PER_IMAGE + 0; // begin
        uint32_t query1 = query0 + 1; // end
        // Reset just the two queries for this image before writing them
        vkCmdResetQueryPool(swapchain.command_buffers_per_image[swap_image_index], swapchain.query_pool, query0, QUERIES_PER_IMAGE);
        vkCmdWriteTimestamp(swapchain.command_buffers_per_image[swap_image_index], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, swapchain.query_pool, query0);
        #endif
        
        // Zero the counters on GPU
        vkCmdFillBuffer(swapchain.command_buffers_per_image[swap_image_index], renderer.buffer_counters, 0, size_counters, 0);
        vkCmdFillBuffer(swapchain.command_buffers_per_image[swap_image_index], renderer.buffer_dispatch, 0, size_dispatch, 0);
        
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
        vkCmdPipelineBarrier2(swapchain.command_buffers_per_image[swap_image_index], &dep0);

        // --- Compute passes ---
        vkCmdBindDescriptorSets(swapchain.command_buffers_per_image[swap_image_index],
                                VK_PIPELINE_BIND_POINT_COMPUTE,
                                renderer.compute_pipeline_layout,
                                0, 1, &renderer.descriptor_set, 0, NULL);

        vkCmdBindPipeline(swapchain.command_buffers_per_image[swap_image_index],
                          VK_PIPELINE_BIND_POINT_COMPUTE, renderer.pipeline_cs_instance);
        vkCmdDispatch(swapchain.command_buffers_per_image[swap_image_index], 1, 1, 1);

        vkCmdBindPipeline(swapchain.command_buffers_per_image[swap_image_index],
                          VK_PIPELINE_BIND_POINT_COMPUTE, renderer.pipeline_cs_prepare);
        vkCmdDispatch(swapchain.command_buffers_per_image[swap_image_index], 1, 1, 1);

        vkCmdBindPipeline(swapchain.command_buffers_per_image[swap_image_index],
                          VK_PIPELINE_BIND_POINT_COMPUTE, renderer.pipeline_cs_meshlet);
        vkCmdDispatchIndirect(swapchain.command_buffers_per_image[swap_image_index], renderer.buffer_dispatch, 0);

        // Barrier: make compute writes visible to graphics/indirect stages
        VkMemoryBarrier2 mem_barrier = {
            .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
                             VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT |
                             VK_ACCESS_2_INDEX_READ_BIT |
                             VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
        };
        VkDependencyInfo dep_info = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mem_barrier };
        vkCmdPipelineBarrier2(swapchain.command_buffers_per_image[swap_image_index], &dep_info);

        // --- render pass (use persistent framebuffer) ---
        VkClearValue clear_color = { .color = {{0, 0, 0, 1}} };
        VkRenderPassBeginInfo render_begin = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = renderer.offscreen_render_pass,
            .framebuffer = renderer.offscreen_fb,
            .renderArea = { .offset = {0,0}, .extent = OFFSCREEN_EXTENT },
            .clearValueCount = 1, .pClearValues = &clear_color
        };
        vkCmdBeginRenderPass(swapchain.command_buffers_per_image[swap_image_index], &render_begin, VK_SUBPASS_CONTENTS_INLINE);

        // set the resolution of the intermediary pass
        VkViewport vp = {
            .x = 0, .y = 0,
            .width  = (float)OFFSCREEN_EXTENT.width,
            .height = (float)OFFSCREEN_EXTENT.height,
            .minDepth = 0.f, .maxDepth = 1.f
        };
        VkRect2D sc = { .offset = {0,0}, .extent = OFFSCREEN_EXTENT };
        vkCmdSetViewport(swapchain.command_buffers_per_image[swap_image_index], 0, 1, &vp);
        vkCmdSetScissor (swapchain.command_buffers_per_image[swap_image_index], 0, 1, &sc);

        vkCmdBindPipeline(swapchain.command_buffers_per_image[swap_image_index], VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.graphics_pipeline);
        VkDeviceSize vertex_offset = 0;
        vkCmdBindVertexBuffers(swapchain.command_buffers_per_image[swap_image_index], 0, 1, &renderer.buffer_varyings, &vertex_offset);
        vkCmdBindIndexBuffer(swapchain.command_buffers_per_image[swap_image_index], renderer.buffer_indices, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexedIndirect(swapchain.command_buffers_per_image[swap_image_index], renderer.buffer_counters, 0, 1, sizeof(VkDrawIndexedIndirectCommand));

        vkCmdEndRenderPass(swapchain.command_buffers_per_image[swap_image_index]);
        
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
        vkCmdPipelineBarrier2(swapchain.command_buffers_per_image[swap_image_index], &depA);
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
        vkCmdBlitImage2(swapchain.command_buffers_per_image[swap_image_index], &blit);
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
        vkCmdPipelineBarrier2(swapchain.command_buffers_per_image[swap_image_index], &depB);

        // Record the new layout for this image
        swapchain.image_layouts[swap_image_index] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        #if DEBUG == 1
        vkCmdWriteTimestamp(swapchain.command_buffers_per_image[swap_image_index],VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, swapchain.query_pool, query1);
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
        f32 submit_time = (f32) (pf_ns_now() - pf_ns_start()) / 1e6;
        printf("[%llu] cpu time %.3fms - %.3fms [%.3fms]\n",renderer.frame_id_counter, frame_start_time, submit_time,
            submit_time - frame_start_time);
        #endif
        VK_CHECK(vkEndCommandBuffer(swapchain.command_buffers_per_image[swap_image_index]));

        // (D) Submit: wait on acquire, signal render-finished, fence per-frame
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore present_ready = swapchain.present_ready_per_image[swap_image_index];
        VkSubmitInfo submit_info = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &renderer.sem_image_available[renderer.frame_slot],
            .pWaitDstStageMask  = &wait_stage,
            .commandBufferCount = 1, .pCommandBuffers = &swapchain.command_buffers_per_image[swap_image_index],
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

        swapchain.previous_frame_image_index[renderer.frame_slot] = swap_image_index;
        renderer.frame_slot = (renderer.frame_slot + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    return 0;
}
