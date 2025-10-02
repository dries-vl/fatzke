#include "header.h"

#include <X11/Xlib.h>
#include <vulkan/vulkan.h>

#include <stdlib.h> // todo: get rid of this and all calloc/malloc/free
#include <stdio.h> // todo: get rid of this and link with compiled spv as object

// todo: create vk_shader (to have one big shader with multiple entrypoints, have shader hot reloading, ...)
// todo: create vk_texture (loading textures, maybe hot reloading textures, ...)

#define MAX_FRAMES_IN_FLIGHT 2

struct Renderer {
    // Render pass & graphics pipeline
    VkRenderPass              render_pass;
    VkPipelineLayout          graphics_pipeline_layout;
    VkPipeline                graphics_pipeline;
    // Compute pipelines / descriptors
    VkDescriptorSetLayout     compute_set_layout;
    VkPipelineLayout          compute_pipeline_layout;
    VkPipeline                pipeline_cs_instance;
    VkPipeline                pipeline_cs_prepare;
    VkPipeline                pipeline_cs_meshlet;
    VkDescriptorPool          descriptor_pool;
    VkDescriptorSet           descriptor_set;

    // Buffers and memory
    VkBuffer                  buffer_vertices;   VkDeviceMemory memory_vertices;
    VkBuffer                  buffer_instances;  VkDeviceMemory memory_instances;
    VkBuffer                  buffer_visible;    VkDeviceMemory memory_visible;
    VkBuffer                  buffer_counters;   VkDeviceMemory memory_counters;
    VkBuffer                  buffer_varyings;   VkDeviceMemory memory_varyings;
    VkBuffer                  buffer_indices;    VkDeviceMemory memory_indices;
    VkBuffer                  buffer_dispatch;   VkDeviceMemory memory_dispatch;
    // Commands & synchronization
    VkCommandPool             command_pool_graphics;
    VkCommandBuffer*          command_buffers_per_image;

    VkSemaphore               image_available[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore               render_finished[MAX_FRAMES_IN_FLIGHT];
    VkFence                   in_flight[MAX_FRAMES_IN_FLIGHT];
    uint32_t                  current_frame;

};

#include "vk_util.h"
#include "vk_machine.h"
#include "vk_swapchain.h"


/* ----------------------------- Utilities ----------------------------- */

void* map_entire_allocation(VkDevice device, VkDeviceMemory memory, VkDeviceSize size_bytes) {
    void* data = NULL;
    VK_CHECK(vkMapMemory(device, memory, 0, size_bytes, 0, &data));
    return data;
}

static VkShaderModule
create_shader_module_from_spirv(VkDevice device, const char* file_path)
{
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

static uint32_t
find_memory_type_index(VkPhysicalDevice physical_device,
                       uint32_t memory_type_bits,
                       VkMemoryPropertyFlags required_properties)
{
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
                              VkBuffer* out_buf, VkDeviceMemory* out_mem)
{
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

void key_input_callback(void* ud, enum KEYBOARD_BUTTON key, enum INPUT_STATE state)
{
    if (key == KEYBOARD_ESCAPE) {_exit(0);}
}
void mouse_input_callback(void* ud, i32 x, i32 y, enum MOUSE_BUTTON button, enum INPUT_STATE state)
{
}
int main(void)
{
    // setup with X11
    pf_time_reset();
    WINDOW window = pf_create_window(NULL, key_input_callback,mouse_input_callback);
    pf_timestamp("Create X window");

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

    // create render pass with some obvious settings
    {
        VkAttachmentDescription color_attachment = { // -> can be moved to swapchain as constant
            .format         = swapchain.swapchain_format,
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
        };
        VkAttachmentReference color_ref = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass = {
            .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &color_ref
        };
        VkRenderPassCreateInfo render_pass_info = {
            .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments    = &color_attachment,
            .subpassCount    = 1,
            .pSubpasses      = &subpass
        };
        VK_CHECK(vkCreateRenderPass(machine.device, &render_pass_info, NULL, &renderer.render_pass));
    }
    
    swapchain.framebuffers = (VkFramebuffer*)malloc(sizeof(VkFramebuffer) * swapchain.swapchain_image_count);
    for (uint32_t i = 0; i < swapchain.swapchain_image_count; ++i) {
        VkImageView attachments[1] = { swapchain.swapchain_views[i] };
        VkFramebufferCreateInfo fb_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = renderer.render_pass,
            .attachmentCount = 1,
            .pAttachments = attachments,
            .width  = swapchain.swapchain_extent.width,
            .height = swapchain.swapchain_extent.height,
            .layers = 1
        };
        VK_CHECK(vkCreateFramebuffer(machine.device, &fb_info, NULL, &swapchain.framebuffers[i]));
    }

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
    VkShaderModule sm_cs_instance = create_shader_module_from_spirv(machine.device, "static/cs_instance.spv");
    VkShaderModule sm_cs_prepare  = create_shader_module_from_spirv(machine.device, "static/cs_prepare.spv");
    VkShaderModule sm_cs_meshlet  = create_shader_module_from_spirv(machine.device, "static/cs_meshlet.spv");

    VkComputePipelineCreateInfo compute_infos[3] = {
        {
            .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage  = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm_cs_instance, .pName = "main" },
            .layout = renderer.compute_pipeline_layout
        },
        {
            .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage  = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm_cs_prepare,  .pName = "main"  },
            .layout = renderer.compute_pipeline_layout
        },
        {
            .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage  = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm_cs_meshlet,  .pName = "main"  },
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
    vkDestroyShaderModule(machine.device, sm_cs_instance, NULL);
    vkDestroyShaderModule(machine.device, sm_cs_prepare,  NULL);
    vkDestroyShaderModule(machine.device, sm_cs_meshlet,  NULL);

    /* -------- Graphics Pipeline -------- */
    VkShaderModule sm_vs = create_shader_module_from_spirv(machine.device, "static/tri.vert.spv");
    VkShaderModule sm_fs = create_shader_module_from_spirv(machine.device, "static/tri.frag.spv");

    VkPipelineShaderStageCreateInfo shader_stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,   .module = sm_vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = sm_fs, .pName = "main" }
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
        .renderPass          = renderer.render_pass,
        .subpass             = 0
    };
    VK_CHECK(vkCreateGraphicsPipelines(machine.device, VK_NULL_HANDLE, 1, &graphics_info, NULL, &renderer.graphics_pipeline));
    vkDestroyShaderModule(machine.device, sm_vs, NULL);
    vkDestroyShaderModule(machine.device, sm_fs, NULL);

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
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, host_visible_coherent,
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

    /* -------- Command Pool/Buffers & Sync -------- */
    VkCommandPoolCreateInfo cmd_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = machine.queue_family_graphics,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
    };
    VK_CHECK(vkCreateCommandPool(machine.device, &cmd_pool_info, NULL, &renderer.command_pool_graphics));

    renderer.command_buffers_per_image = (VkCommandBuffer*)calloc(swapchain.swapchain_image_count, sizeof(VkCommandBuffer));
    VkCommandBufferAllocateInfo cmd_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = renderer.command_pool_graphics,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = swapchain.swapchain_image_count
    };
    VK_CHECK(vkAllocateCommandBuffers(machine.device, &cmd_alloc_info, renderer.command_buffers_per_image));

    // create semaphores etc.
    VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VK_CHECK(vkCreateSemaphore(machine.device, &semaphore_info, NULL, &renderer.image_available[i]));
        VK_CHECK(vkCreateSemaphore(machine.device, &semaphore_info, NULL, &renderer.render_finished[i]));
        VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        VK_CHECK(vkCreateFence(machine.device, &fence_info, NULL, &renderer.in_flight[i]));
    }
    renderer.current_frame = 0;

    /* ----------------------------- Frame Loop ----------------------------- */
    while (pf_poll_events(window)) {
        // Zero out the VkDrawIndexedIndirectCommand (5 u32 fields) at the start of the frame
        uint32_t zeroed_counters[5] = {0,0,0,0,0};
        static void* counters_mapping = NULL;
        if (!counters_mapping) counters_mapping = map_entire_allocation(machine.device, renderer.memory_counters, size_counters);
        memcpy(counters_mapping, zeroed_counters, sizeof(zeroed_counters));

        // (A) throttle CPU to <= 1 frame ahead
        VK_CHECK(vkWaitForFences(machine.device, 1, &renderer.in_flight[renderer.current_frame], VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(machine.device, 1, &renderer.in_flight[renderer.current_frame]));

        // (B) Acquire swapchain image, signaling per-frame semaphore
        uint32_t swap_image_index = 0;
        VkResult acquire_res = vkAcquireNextImageKHR(
            machine.device, swapchain.swapchain, UINT64_MAX,
            renderer.image_available[renderer.current_frame], VK_NULL_HANDLE,
            &swap_image_index);

        if (acquire_res == VK_ERROR_OUT_OF_DATE_KHR) { // window resized, etc.
            recreate_swapchain(&machine, &renderer, &swapchain, window);
            continue;
        }
        if (acquire_res != VK_SUCCESS && acquire_res != VK_SUBOPTIMAL_KHR) {
            printf("vkAcquireNextImageKHR failed: %d\n", acquire_res);
            break;
        }

        // (C) Record command buffer for this swapchain image
        VK_CHECK(vkResetCommandBuffer(renderer.command_buffers_per_image[swap_image_index], 0));
        VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VK_CHECK(vkBeginCommandBuffer(renderer.command_buffers_per_image[swap_image_index], &begin_info));

        // --- Compute passes ---
        vkCmdBindDescriptorSets(renderer.command_buffers_per_image[swap_image_index],
                                VK_PIPELINE_BIND_POINT_COMPUTE,
                                renderer.compute_pipeline_layout,
                                0, 1, &renderer.descriptor_set, 0, NULL);

        vkCmdBindPipeline(renderer.command_buffers_per_image[swap_image_index],
                          VK_PIPELINE_BIND_POINT_COMPUTE, renderer.pipeline_cs_instance);
        vkCmdDispatch(renderer.command_buffers_per_image[swap_image_index], 1, 1, 1);

        vkCmdBindPipeline(renderer.command_buffers_per_image[swap_image_index],
                          VK_PIPELINE_BIND_POINT_COMPUTE, renderer.pipeline_cs_prepare);
        vkCmdDispatch(renderer.command_buffers_per_image[swap_image_index], 1, 1, 1);

        vkCmdBindPipeline(renderer.command_buffers_per_image[swap_image_index],
                          VK_PIPELINE_BIND_POINT_COMPUTE, renderer.pipeline_cs_meshlet);
        vkCmdDispatchIndirect(renderer.command_buffers_per_image[swap_image_index], renderer.buffer_dispatch, 0);

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
        vkCmdPipelineBarrier2(renderer.command_buffers_per_image[swap_image_index], &dep_info);

        // --- render pass (use persistent framebuffer) ---
        VkClearValue clear_color = { .color = {{0, 0, 0, 1}} };
        VkRenderPassBeginInfo render_begin = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = renderer.render_pass,
            .framebuffer = swapchain.framebuffers[swap_image_index], // <-- persistent
            .renderArea = { .offset = {0,0}, .extent = swapchain.swapchain_extent },
            .clearValueCount = 1, .pClearValues = &clear_color
        };
        vkCmdBeginRenderPass(renderer.command_buffers_per_image[swap_image_index], &render_begin, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(renderer.command_buffers_per_image[swap_image_index], VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.graphics_pipeline);
        VkDeviceSize vertex_offset = 0;
        vkCmdBindVertexBuffers(renderer.command_buffers_per_image[swap_image_index], 0, 1, &renderer.buffer_varyings, &vertex_offset);
        vkCmdBindIndexBuffer(renderer.command_buffers_per_image[swap_image_index], renderer.buffer_indices, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexedIndirect(renderer.command_buffers_per_image[swap_image_index], renderer.buffer_counters, 0, 1, sizeof(VkDrawIndexedIndirectCommand));

        vkCmdEndRenderPass(renderer.command_buffers_per_image[swap_image_index]);

        VK_CHECK(vkEndCommandBuffer(renderer.command_buffers_per_image[swap_image_index]));

        // (D) Submit: wait on acquire, signal render-finished, fence per-frame
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit_info = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &renderer.image_available[renderer.current_frame],
            .pWaitDstStageMask  = &wait_stage,
            .commandBufferCount = 1, .pCommandBuffers = &renderer.command_buffers_per_image[swap_image_index],
            .signalSemaphoreCount = 1, .pSignalSemaphores = &renderer.render_finished[renderer.current_frame]
        };
        VK_CHECK(vkQueueSubmit(machine.queue_graphics, 1, &submit_info, renderer.in_flight[renderer.current_frame]));

        // (E) Present: wait on render-finished
        VkPresentInfoKHR present_info = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &renderer.render_finished[renderer.current_frame],
            .swapchainCount = 1, .pSwapchains = &swapchain.swapchain, .pImageIndices = &swap_image_index
        };
        VkResult present_res = vkQueuePresentKHR(machine.queue_present, &present_info);
        if (present_res == VK_ERROR_OUT_OF_DATE_KHR || present_res == VK_SUBOPTIMAL_KHR) {
            recreate_swapchain(&machine, &renderer, &swapchain, window);
        } else if (present_res != VK_SUCCESS) {
            printf("vkQueuePresentKHR failed: %d\n", present_res);
            break;
        }

        renderer.current_frame = (renderer.current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
        pf_timestamp("Frame submitted");
    }

    return 0;
}
