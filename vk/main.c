#include "header.h"

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#else
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <time.h>
#endif
#include <vulkan/vulkan.h>
#undef VkResult
#define VkResult int
#undef VkBool32
#define VkBool32 unsigned

extern const unsigned char shaders[];
extern const unsigned char shaders_end[];
#define shaders_len ((size_t)(shaders_end - shaders))

// todo: create vk_shader (to have one big shader with multiple entrypoints, have shader hot reloading, ...)
// todo: create vk_texture (loading textures, maybe hot reloading textures, ...)

// 1 for double buffering (one of ours, two of present engine) (technically triple buffered, but vulkan doesn't seem to do actual double buffering)
// one is being scanned out, one is done and waiting (this one is also considered 'presented' but not scanned out yet)
// and one of ours being rendered (this is this 1 frame that is 'in flight')
#define MAX_FRAMES_IN_FLIGHT 1
#define MAX_SWAPCHAIN_IMAGES 5

struct Renderer {
    // compute pipelines
    VkDescriptorSetLayout     common_set_layout;
    VkPipelineLayout          common_pipeline_layout;
    VkPipeline                compute_pipeline;
    VkDescriptorPool          descriptor_pool;
    VkDescriptorSet           descriptor_set;
    // buffers
    VkBuffer                  buffer_instances;  VkDeviceMemory memory_instances;
    VkBuffer                  buffer_counts;     VkDeviceMemory memory_counts;
    VkBuffer                  buffer_offsets;    VkDeviceMemory memory_offsets;
    VkBuffer                  buffer_visible_ids;VkDeviceMemory memory_visible_ids;
    VkBuffer                  buffer_visible;    VkDeviceMemory memory_visible;
    VkBuffer                  buffer_mesh_info;  VkDeviceMemory memory_mesh_info;
    VkBuffer                  buffer_counters;   VkDeviceMemory memory_counters;
    VkBuffer                  buffer_positions;  VkDeviceMemory memory_positions;
    VkBuffer                  buffer_normals;    VkDeviceMemory memory_normals;
    VkBuffer                  buffer_uvs;        VkDeviceMemory memory_uvs;
    VkBuffer                  buffer_index_ib;   VkDeviceMemory memory_indices;
    VkBuffer                  buffer_uniforms;   VkDeviceMemory memory_uniforms;
    // main rendering
    VkPipeline                main_pipeline;
    // sync
    uint32_t                  frame_slot;
    VkSemaphore               sem_image_available[MAX_FRAMES_IN_FLIGHT];
    // debug
    #if DEBUG_APP == 1
    double      gpu_ticks_to_ns;
    uint64_t    frame_id_counter;
    uint64_t    frame_id_per_slot[MAX_FRAMES_IN_FLIGHT];
    f32         start_time_per_slot[MAX_FRAMES_IN_FLIGHT];
    #endif
};

struct Uniforms { uint32_t uCam[4]; };  // 16 bytes

#if DEBUG_APP == 1
enum {
    Q_BEGIN = 0,             // start of frame (already have)
    Q_AFTER_ZERO,            // after vkCmdFillBuffer
    Q_AFTER_COMP_BUILD,      // after compute build visible
    Q_AFTER_COMP_INDIRECT,   // after prepare indirect
    Q_AFTER_COMP_TO_GFX,     // after compute->graphics barrier
    Q_BEFORE_RENDERPASS,     // just before vkCmdBeginRenderPass
    Q_AFTER_RENDERPASS,      // just after vkCmdEndRenderPass
    Q_AFTER_BLIT,            // after vkCmdBlitImage2 + transition to PRESENT
    Q_END,                   // end of cmdbuf (already have)
    QUERIES_PER_IMAGE       // nr of queries
};
#endif

#include "vk_util.h"
#include "vk_machine.h"
#include "vk_swapchain.h"
#include "helper.h"

#pragma region HELPER
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
static void upload_to_buffer(VkDevice dev, VkDeviceMemory mem, size_t offset, const void *src, size_t bytes) {
    void* dst = NULL;
    VK_CHECK(vkMapMemory(dev, mem, offset, bytes, 0, &dst));
    memcpy(dst, src, bytes);
    vkUnmapMemory(dev, mem);
}

// --- Single-use command helpers (record/submit/wait) ---
VkCommandBuffer begin_single_use_cmd(VkDevice device, VkCommandPool pool) {
    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
    VK_CHECK(vkBeginCommandBuffer(cmd, &(VkCommandBufferBeginInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    }));
    return cmd;
}
void end_single_use_cmd(VkDevice device, VkQueue queue, VkCommandPool pool, VkCommandBuffer cmd) {
    VK_CHECK(vkEndCommandBuffer(cmd));
    VK_CHECK(vkQueueSubmit(queue, 1, &(VkSubmitInfo){
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cmd
    }, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(device, pool, 1, &cmd);
}

// Create a DEVICE_LOCAL buffer and upload data via a temporary HOST_VISIBLE staging buffer.
static void create_and_upload_device_local_buffer(
    VkDevice device, VkPhysicalDevice phys, VkQueue queue, VkCommandPool pool,
    VkDeviceSize size, VkBufferUsageFlags usage,
    const void* src, VkBuffer* out_buf, VkDeviceMemory* out_mem,
    VkDeviceSize dst_offset /*usually 0*/
){
    // 1) Create destination (DEVICE_LOCAL)
    create_buffer_and_memory(device, phys, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out_buf, out_mem);

    if (size == 0 || src == NULL) return; // nothing to upload

    // 2) Create staging (HOST_VISIBLE|COHERENT, TRANSFER_SRC)
    VkBuffer staging; VkDeviceMemory staging_mem;
    create_buffer_and_memory(device, phys, size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &staging, &staging_mem);

    // 3) Map+copy to staging
    upload_to_buffer(device, staging_mem, 0, src, (size_t)size);

    // 4) Record copy
    VkCommandBuffer cmd = begin_single_use_cmd(device, pool);
    VkBufferCopy copy = { .srcOffset = 0, .dstOffset = dst_offset, .size = size };
    vkCmdCopyBuffer(cmd, staging, *out_buf, 1, &copy);
    end_single_use_cmd(device, queue, pool, cmd);

    // 5) Destroy staging
    vkDestroyBuffer(device, staging, NULL);
    vkFreeMemory(device, staging_mem, NULL);
}

#pragma region MAIN
// todo: avoid globals
WINDOW window;
i32 buttons[BUTTON_COUNT];
i16 cam_x = 0, cam_y = 2, cam_z = -5, cam_yaw = 0, cam_pitch = 0;
void move_forward(int amount) {
    double rad = (double)(cam_yaw) * 3.14159265f / 32767.0f;
    cam_x += (i16)lroundf(sinf(rad) * amount);
    cam_z += (i16)lroundf(cosf(rad) * amount);
}
void move_sideways(int amount) {
    double rad = (double)(cam_yaw) * 3.14159265f / 32767.0f;
    cam_x += (i16)lroundf(cosf(rad) * amount);
    cam_z -= (i16)lroundf(sinf(rad) * amount);
}
int scaled(int value) {
    static const int min_in = 0;
    static const int max_in = 32767;
    static const int min_out = 1;
    static const int max_out = 1024;
    float slope = (float)(max_out - min_out) / (max_in - min_in);
    float scale = (min_out + (cam_y - min_in) * slope);
    return (int)((float)value * scale);
}
void key_input_callback(void* ud, enum BUTTON button, enum BUTTON_STATE state) {
    if (state == PRESSED) buttons[button] = 1;
    else buttons[button] = 0;
}
void mouse_input_callback(void* ud, i32 x, i32 y, enum BUTTON button, int state) {
    if (button == MOUSE_MOVED) {
        if (x < 50) buttons[MOUSE_MARGIN_LEFT] += (50 - x) / 3;
        else buttons[MOUSE_MARGIN_LEFT] = 0;
        if (x > pf_window_width(window) - 50) buttons[MOUSE_MARGIN_RIGHT] += (x - (pf_window_width(window) - 50)) / 3;
        else buttons[MOUSE_MARGIN_RIGHT] = 0;
        if (y < 50) buttons[MOUSE_MARGIN_TOP] += (50 - y) / 5;
        else buttons[MOUSE_MARGIN_TOP] = 0;
        if (y > pf_window_height(window) - 50) buttons[MOUSE_MARGIN_BOTTOM] += (y - (pf_window_height(window) - 50)) / 5;
        else buttons[MOUSE_MARGIN_BOTTOM] = 0;
    }
    if (button == MOUSE_SCROLL) buttons[MOUSE_SCROLL] += scaled(state / 5);
    if (button == MOUSE_SCROLL_SIDE) buttons[MOUSE_SCROLL_SIDE] -= scaled(state / 5);
    if (button == MOUSE_LEFT || button == MOUSE_RIGHT || button == MOUSE_MIDDLE) {
        if (state == PRESSED) buttons[button] = 1;
        else buttons[button] = 0;
    }
}
void process_inputs() {
    if (buttons[KEYBOARD_ESCAPE]) {_exit(0);}
    if (buttons[KEYBOARD_W]) { move_forward(scaled(1)); }
    if (buttons[KEYBOARD_R]) { move_forward(-scaled(1)); }
    if (buttons[KEYBOARD_A]) { move_sideways(-scaled(1)); }
    if (buttons[KEYBOARD_S]) { move_sideways(scaled(1)); }
    if (buttons[MOUSE_MARGIN_LEFT]) { cam_yaw -= buttons[MOUSE_MARGIN_LEFT]; } 
    if (buttons[MOUSE_MARGIN_RIGHT]) { cam_yaw += buttons[MOUSE_MARGIN_RIGHT]; }
    if (buttons[MOUSE_MARGIN_TOP]) { cam_pitch -= buttons[MOUSE_MARGIN_TOP]; }
    if (buttons[MOUSE_MARGIN_BOTTOM]) { cam_pitch += buttons[MOUSE_MARGIN_BOTTOM]; }
    if (buttons[MOUSE_SCROLL]) { cam_y += buttons[MOUSE_SCROLL]; buttons[MOUSE_SCROLL] = 0; }
    if (buttons[MOUSE_SCROLL_SIDE]) { cam_x -= buttons[MOUSE_SCROLL_SIDE]; buttons[MOUSE_SCROLL_SIDE] = 0; }
}
int main(void) {
    // setup with X11
    pf_time_reset();
    window = pf_create_window(NULL, key_input_callback,mouse_input_callback);
    pf_timestamp("Created platform window");

#if defined(_WIN32) && DEBUG_VULKAN == 1
    extern int putenv(const char*);
    extern char* getenv(const char*);
    // set env to point to vk layer path to allow finding
    const char* sdk = getenv("VULKAN_SDK");
    char buf[1024];
    snprintf(buf, sizeof buf, "VK_LAYER_PATH=%s/Bin", sdk);
    putenv(strdup(buf));
#endif
#if USE_DISCRETE_GPU == 0 && !defined(_WIN32)
    extern int putenv(const char*);
    extern char* getenv(const char*);
    // set env to avoid loading nvidia icd (1000ms)
    putenv((char*)"VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json");
    putenv((char*)"VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json");
    pf_timestamp("Setup environment");
#endif

    // setup vulkan on the machine
    struct Machine machine = create_machine(window);
    struct Swapchain swapchain = create_swapchain(&machine,window);
    struct Renderer renderer = {0};
    
    printf("SCREEN SIZE: %d, %d\n", swapchain.swapchain_extent.width, swapchain.swapchain_extent.height);
    printf("WINDOW SIZE: %d, %d\n", pf_window_width(window), pf_window_height(window));

    // create shader module
    VkShaderModuleCreateInfo smci={ .sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize=shaders_len, .pCode=(const u32*)shaders };
    VkShaderModule shader_module; VK_CHECK(vkCreateShaderModule(machine.device,&smci,NULL,&shader_module));

    #pragma region COMPUTE PIPELINE
    #define BINDINGS 11
    #define UNIFORM_BINDING (BINDINGS-1)
    VkDescriptorSetLayoutBinding bindings[BINDINGS];
    for (uint32_t i = 0; i < BINDINGS; ++i) {
        bindings[i] = (VkDescriptorSetLayoutBinding) {
            .binding         = i,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT
        };
        // use uniform for the last one
        bindings[i].descriptorType = (i != UNIFORM_BINDING) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    }
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = BINDINGS,
        .pBindings    = bindings
    };
    VK_CHECK(vkCreateDescriptorSetLayout(machine.device, &set_layout_info, NULL, &renderer.common_set_layout));

    /* -------- Pipeline Layouts -------- */
    VkPushConstantRange range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size   = sizeof(uint32_t)
    };
    VkPipelineLayoutCreateInfo common_pipeline_info = {
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts    = &renderer.common_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &range
    };
    VK_CHECK(vkCreatePipelineLayout(machine.device, &common_pipeline_info, NULL, &renderer.common_pipeline_layout));

    VkComputePipelineCreateInfo compute_info = {
            .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage  = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader_module, .pName = "cs_main" },
            .layout = renderer.common_pipeline_layout
        };
    VK_CHECK(vkCreateComputePipelines(machine.device,VK_NULL_HANDLE,1,&compute_info,NULL,&renderer.compute_pipeline));
    #pragma endregion

    #pragma region GRAPHICS PIPELINE
    VkPipelineShaderStageCreateInfo shader_stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,   .module = shader_module, .pName = "vs_main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = shader_module, .pName = "fs_main" }
    };
    VkVertexInputBindingDescription bindings_vi[1] = {
        { .binding = 0, .stride = sizeof(uint32_t)*4,    .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE }
    };
    VkVertexInputAttributeDescription attrs_vi[1] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_UINT,   .offset = 0 }
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = 1, .pVertexBindingDescriptions   = bindings_vi,
        .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = attrs_vi
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };
    VkViewport viewport = {
        .x = 0,
        .y = (float)swapchain.swapchain_extent.height,   // flip Y
        .width  = (float)swapchain.swapchain_extent.width,
        .height = -(float)swapchain.swapchain_extent.height, // negative to flip
        .minDepth = 0.f, .maxDepth = 1.f
    };
    VkRect2D scissor = { .offset = {0,0}, .extent = swapchain.swapchain_extent };
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport,
        .scissorCount  = 1, .pScissors  = &scissor
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_BACK_BIT,
        .frontFace   = VK_FRONT_FACE_CLOCKWISE,
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
        .layout              = renderer.common_pipeline_layout
    };
    VkPipelineRenderingCreateInfo pr = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = (VkFormat[]){ swapchain.swapchain_format },
        .depthAttachmentFormat = VK_FORMAT_D32_SFLOAT,
        // .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
    };
    graphics_info.pNext = &pr;
    VkPipelineDepthStencilStateCreateInfo depth = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE,
      .depthWriteEnable = VK_TRUE,          // OK to keep TRUE even for sky
      .depthCompareOp = VK_COMPARE_OP_GREATER,
    };
    graphics_info.pDepthStencilState = &depth;
    VK_CHECK(vkCreateGraphicsPipelines(machine.device, VK_NULL_HANDLE, 1, &graphics_info, NULL, &renderer.main_pipeline));
    #pragma endregion

    // destroy the shader module after using it
    vkDestroyShaderModule(machine.device, shader_module,  NULL);

    #pragma region BUFFERS
    #include "mesh.h"
    #include "plane_lod0.h"
    #include "plane_lod1.h"
    #include "plane_lod2.h"
    #include "plane_lod3.h"
    #include "plane_lod4.h"
    #include "plane_lod5.h"
    #include "plane_lod6.h"
    #define MESH_COUNT 14

    struct gpu_instance { uint32_t xy_dm; uint32_t z_cs; };
    struct mesh_info {
        u32 vertex_count, index_count, instance_count;
        const struct gpu_instance *instances;
        const u16 *indices;
        const u32 *positions, *normals, *uvs;
    };
    #include "plane_instances.h"

    struct gpu_instance mesh_inst;
    mesh_inst.xy_dm = ((uint16_t)0) << 16 | ((uint16_t)0);
    {
        float yaw = 0.0f;
        mesh_inst.z_cs = ((uint16_t)0)
            | (((uint8_t)lrintf(cosf(yaw) * 127.0f)) << 16)
            | (((uint8_t)lrintf(sinf(yaw) * 127.0f)) << 24);
    }
    const struct mesh_info meshes[MESH_COUNT] = {
        {g_vertex_count_plane_lod6, g_index_count_plane_lod6, 2601, g_plane_instances, g_indices_plane_lod6, NULL,NULL,NULL},
        {g_vertex_count_plane_lod5, g_index_count_plane_lod5, 0,    g_plane_instances, g_indices_plane_lod5, NULL,NULL,NULL},
        {g_vertex_count_plane_lod4, g_index_count_plane_lod4, 0,    g_plane_instances, g_indices_plane_lod4, NULL,NULL,NULL},
        {g_vertex_count_plane_lod3, g_index_count_plane_lod3, 0,    g_plane_instances, g_indices_plane_lod3, NULL,NULL,NULL},
        {g_vertex_count_plane_lod2, g_index_count_plane_lod2, 0,    g_plane_instances, g_indices_plane_lod2, NULL,NULL,NULL},
        {g_vertex_count_plane_lod1, g_index_count_plane_lod1, 0,    g_plane_instances, g_indices_plane_lod1, NULL,NULL,NULL},
        {g_vertex_count_plane_lod0, g_index_count_plane_lod0, 0,    g_plane_instances, g_indices_plane_lod0, NULL,NULL,NULL},
        {g_vertex_count_mesh,       g_index_count_mesh,       1,    &mesh_inst,        g_indices_mesh,       g_positions_mesh, g_normals_mesh, g_uvs_mesh},
        {g_vertex_count_mesh,       g_index_count_mesh,       0,    &mesh_inst,        g_indices_mesh,       g_positions_mesh, g_normals_mesh, g_uvs_mesh},
        {g_vertex_count_mesh,       g_index_count_mesh,       0,    &mesh_inst,        g_indices_mesh,       g_positions_mesh, g_normals_mesh, g_uvs_mesh},
        {g_vertex_count_mesh,       g_index_count_mesh,       0,    &mesh_inst,        g_indices_mesh,       g_positions_mesh, g_normals_mesh, g_uvs_mesh},
        {g_vertex_count_mesh,       g_index_count_mesh,       0,    &mesh_inst,        g_indices_mesh,       g_positions_mesh, g_normals_mesh, g_uvs_mesh},
        {g_vertex_count_mesh,       g_index_count_mesh,       0,    &mesh_inst,        g_indices_mesh,       g_positions_mesh, g_normals_mesh, g_uvs_mesh},
        {g_vertex_count_mesh,       g_index_count_mesh,       0,    &mesh_inst,        g_indices_mesh,       g_positions_mesh, g_normals_mesh, g_uvs_mesh}
    };

    u32 total_vertex_count = 0;
    u32 total_index_count = 0;
    u32 total_instance_count = 0;
    struct VkDrawIndexedIndirectCommand mesh_info[MESH_COUNT];
    for (u32 i = 0; i < MESH_COUNT; ++i) {
        mesh_info[i].firstIndex    = total_index_count;
        mesh_info[i].vertexOffset  = total_vertex_count;
        mesh_info[i].indexCount    = meshes[i].index_count;
        mesh_info[i].firstInstance = total_instance_count;
        mesh_info[i].instanceCount = meshes[i].instance_count;
        total_vertex_count   += meshes[i].vertex_count;
        total_index_count    += meshes[i].index_count;
        total_instance_count += meshes[i].instance_count;
    }

    VkDeviceSize size_instances = total_instance_count * sizeof(uint64_t);
    VkDeviceSize size_counts = MESH_COUNT * sizeof(uint32_t);
    VkDeviceSize size_offsets = MESH_COUNT * sizeof(uint32_t);
    VkDeviceSize size_visible_ids = total_instance_count * sizeof(uint32_t);
    VkDeviceSize size_visible   = total_instance_count * sizeof(uint32_t) * 4;
    VkDeviceSize size_mesh_info = MESH_COUNT * sizeof(VkDrawIndexedIndirectCommand);
    VkDeviceSize size_counters  = MESH_COUNT * sizeof(VkDrawIndexedIndirectCommand);
    VkDeviceSize size_positions = total_vertex_count * sizeof(uint32_t);
    VkDeviceSize size_normals   = total_vertex_count * sizeof(uint32_t);
    VkDeviceSize size_uvs       = total_vertex_count * sizeof(uint32_t);
    VkDeviceSize size_indices   = total_index_count  * sizeof(uint16_t);

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = machine.queue_family_graphics, // use your graphics family index
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT
    };
    VkCommandPool upload_pool; VK_CHECK(vkCreateCommandPool(machine.device, &pool_info, NULL, &upload_pool));

    // INSTANCES (SSBO, read by compute)  -> DEVICE_LOCAL + upload
    create_and_upload_device_local_buffer(
        machine.device, machine.physical_device, machine.queue_graphics, upload_pool,
        size_instances, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        NULL,
        &renderer.buffer_instances, &renderer.memory_instances, 0);

    // COUNTS
    create_buffer_and_memory(machine.device, machine.physical_device, size_counts,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &renderer.buffer_counts, &renderer.memory_counts);

    // OFFSETS
    create_buffer_and_memory(machine.device, machine.physical_device, size_offsets,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &renderer.buffer_offsets, &renderer.memory_offsets);

    // VISIBLE IDS
    create_buffer_and_memory(machine.device, machine.physical_device, size_visible_ids,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &renderer.buffer_visible_ids, &renderer.memory_visible_ids);

    // VISIBLE
    create_buffer_and_memory(machine.device, machine.physical_device, size_visible,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &renderer.buffer_visible, &renderer.memory_visible);

    // DRAW_CALLS
    create_buffer_and_memory(machine.device, machine.physical_device, size_counters,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &renderer.buffer_counters, &renderer.memory_counters);

    // MESH INFO (INDIRECT + sometimes read in compute) -> DEVICE_LOCAL + upload once
    create_and_upload_device_local_buffer(
        machine.device, machine.physical_device, machine.queue_graphics, upload_pool,
        size_mesh_info, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        mesh_info, &renderer.buffer_mesh_info, &renderer.memory_mesh_info, 0);

    // POSITIONS / NORMALS / UVS (SSBOs) -> DEVICE_LOCAL; upload per-mesh ranges below
    create_and_upload_device_local_buffer(
        machine.device, machine.physical_device, machine.queue_graphics, upload_pool,
        size_positions, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        /*src*/ NULL, &renderer.buffer_positions, &renderer.memory_positions, 0);
    create_and_upload_device_local_buffer(
        machine.device, machine.physical_device, machine.queue_graphics, upload_pool,
        size_normals, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        /*src*/ NULL, &renderer.buffer_normals, &renderer.memory_normals, 0);
    create_and_upload_device_local_buffer(
        machine.device, machine.physical_device, machine.queue_graphics, upload_pool,
        size_uvs, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        /*src*/ NULL, &renderer.buffer_uvs, &renderer.memory_uvs, 0);

    // INDEX BUFFER -> DEVICE_LOCAL + upload once
    create_and_upload_device_local_buffer(
        machine.device, machine.physical_device, machine.queue_graphics, upload_pool,
        size_indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        /*src*/ NULL, &renderer.buffer_index_ib, &renderer.memory_indices, 0);

    // UNIFORMS: keep tiny UBO host-visible (or convert to push constants later)
    create_buffer_and_memory(machine.device, machine.physical_device, sizeof(struct Uniforms),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &renderer.buffer_uniforms, &renderer.memory_uniforms);

    pf_timestamp("Buffers created (DEVICE_LOCAL + staging)");

    // ---- Upload per-mesh data into the big DEVICE_LOCAL buffers ----
    // We batch copies by repeatedly calling create_and_upload_device_local_buffer with dstOffset.
    // (For simplicity, we create a staging buffer per upload. For large games, batch copies into one cmd.)

    for (u32 i = 0; i < MESH_COUNT; ++i) {
        // Vertex attributes
        if (meshes[i].positions) {
            VkDeviceSize bytes = meshes[i].vertex_count * sizeof(uint32_t);
            VkDeviceSize dstOfs = (VkDeviceSize)mesh_info[i].vertexOffset * sizeof(uint32_t);
            // Create a temp staging and copy into the already-created DEVICE_LOCAL buffer at dstOfs
            // Reuse the helper by creating a temp device-local “alias” of same buffer & memory? Simpler: a small inline staging+copy:
            {
                VkBuffer staging; VkDeviceMemory staging_mem;
                create_buffer_and_memory(machine.device, machine.physical_device, bytes,
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    &staging, &staging_mem);
                upload_to_buffer(machine.device, staging_mem, 0, meshes[i].positions, (size_t)bytes);
                VkCommandBuffer cmd = begin_single_use_cmd(machine.device, upload_pool);
                VkBufferCopy c = { .srcOffset = 0, .dstOffset = dstOfs, .size = bytes };
                vkCmdCopyBuffer(cmd, staging, renderer.buffer_positions, 1, &c);
                end_single_use_cmd(machine.device, machine.queue_graphics, upload_pool, cmd);
                vkDestroyBuffer(machine.device, staging, NULL);
                vkFreeMemory(machine.device, staging_mem, NULL);
            }
        }
        if (meshes[i].normals) {
            VkDeviceSize bytes = meshes[i].vertex_count * sizeof(uint32_t);
            VkDeviceSize dstOfs = (VkDeviceSize)mesh_info[i].vertexOffset * sizeof(uint32_t);
            VkBuffer staging; VkDeviceMemory staging_mem;
            create_buffer_and_memory(machine.device, machine.physical_device, bytes,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &staging, &staging_mem);
            upload_to_buffer(machine.device, staging_mem, 0, meshes[i].normals, (size_t)bytes);
            VkCommandBuffer cmd = begin_single_use_cmd(machine.device, upload_pool);
            VkBufferCopy c = { .srcOffset = 0, .dstOffset = dstOfs, .size = bytes };
            vkCmdCopyBuffer(cmd, staging, renderer.buffer_normals, 1, &c);
            end_single_use_cmd(machine.device, machine.queue_graphics, upload_pool, cmd);
            vkDestroyBuffer(machine.device, staging, NULL);
            vkFreeMemory(machine.device, staging_mem, NULL);
        }
        if (meshes[i].uvs) {
            VkDeviceSize bytes = meshes[i].vertex_count * sizeof(uint32_t);
            VkDeviceSize dstOfs = (VkDeviceSize)mesh_info[i].vertexOffset * sizeof(uint32_t);
            VkBuffer staging; VkDeviceMemory staging_mem;
            create_buffer_and_memory(machine.device, machine.physical_device, bytes,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &staging, &staging_mem);
            upload_to_buffer(machine.device, staging_mem, 0, meshes[i].uvs, (size_t)bytes);
            VkCommandBuffer cmd = begin_single_use_cmd(machine.device, upload_pool);
            VkBufferCopy c = { .srcOffset = 0, .dstOffset = dstOfs, .size = bytes };
            vkCmdCopyBuffer(cmd, staging, renderer.buffer_uvs, 1, &c);
            end_single_use_cmd(machine.device, machine.queue_graphics, upload_pool, cmd);
            vkDestroyBuffer(machine.device, staging, NULL);
            vkFreeMemory(machine.device, staging_mem, NULL);
        }

        // Indices
        {
            VkDeviceSize bytes = meshes[i].index_count * sizeof(uint16_t);
            VkDeviceSize dstOfs = (VkDeviceSize)mesh_info[i].firstIndex * sizeof(uint16_t);
            VkBuffer staging; VkDeviceMemory staging_mem;
            create_buffer_and_memory(machine.device, machine.physical_device, bytes,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &staging, &staging_mem);
            upload_to_buffer(machine.device, staging_mem, 0, meshes[i].indices, (size_t)bytes);
            VkCommandBuffer cmd = begin_single_use_cmd(machine.device, upload_pool);
            VkBufferCopy c = { .srcOffset = 0, .dstOffset = dstOfs, .size = bytes };
            vkCmdCopyBuffer(cmd, staging, renderer.buffer_index_ib, 1, &c);
            end_single_use_cmd(machine.device, machine.queue_graphics, upload_pool, cmd);
            vkDestroyBuffer(machine.device, staging, NULL);
            vkFreeMemory(machine.device, staging_mem, NULL);
        }

        // Instances
        if (meshes[i].instance_count > 0) {
            VkDeviceSize bytes = meshes[i].instance_count * sizeof(struct gpu_instance);
            VkDeviceSize dstOfs = (VkDeviceSize)mesh_info[i].firstInstance * sizeof(struct gpu_instance);
            VkBuffer staging; VkDeviceMemory staging_mem;
            create_buffer_and_memory(machine.device, machine.physical_device, bytes,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &staging, &staging_mem);
            upload_to_buffer(machine.device, staging_mem, 0, meshes[i].instances, (size_t)bytes);
            VkCommandBuffer cmd = begin_single_use_cmd(machine.device, upload_pool);
            VkBufferCopy c = { .srcOffset = 0, .dstOffset = dstOfs, .size = bytes };
            vkCmdCopyBuffer(cmd, staging, renderer.buffer_instances, 1, &c);
            end_single_use_cmd(machine.device, machine.queue_graphics, upload_pool, cmd);
            vkDestroyBuffer(machine.device, staging, NULL);
            vkFreeMemory(machine.device, staging_mem, NULL);
        }
    }

    vkDestroyCommandPool(machine.device, upload_pool, NULL);
    pf_timestamp("Uploads complete");

    // --- Descriptor pool & set (unchanged, but note buffers are now DEVICE_LOCAL) ---
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  BINDINGS-1 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1 },
    };
    VkDescriptorPoolCreateInfo pool_info_desc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2,
        .poolSizeCount = (uint32_t)(sizeof(pool_sizes)/sizeof(pool_sizes[0])),
        .pPoolSizes = pool_sizes,
    };
    VK_CHECK(vkCreateDescriptorPool(machine.device, &pool_info_desc, NULL, &renderer.descriptor_pool));

    VkDescriptorSetAllocateInfo set_alloc_info = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = renderer.descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &renderer.common_set_layout
    };
    VK_CHECK(vkAllocateDescriptorSets(machine.device, &set_alloc_info, &renderer.descriptor_set));

    VkDescriptorBufferInfo buffer_infos[BINDINGS] = {
        { renderer.buffer_instances, 0, size_instances },
        { renderer.buffer_counts,0, size_counts   },
        { renderer.buffer_offsets,0, size_offsets   },
        { renderer.buffer_visible_ids,0, size_visible_ids   },
        { renderer.buffer_visible,   0, size_visible   },
        { renderer.buffer_mesh_info, 0, size_mesh_info },
        { renderer.buffer_counters,  0, size_counters  },
        { renderer.buffer_positions, 0, size_positions },
        { renderer.buffer_normals,   0, size_normals   },
        { renderer.buffer_uvs,       0, size_uvs       },
        { renderer.buffer_uniforms,  0, sizeof(struct Uniforms) }
    };
    VkWriteDescriptorSet writes[BINDINGS];
    for (uint32_t i = 0; i < BINDINGS; ++i) {
        writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = renderer.descriptor_set,
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = (i != UNIFORM_BINDING) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &buffer_infos[i]
        };
    }
    vkUpdateDescriptorSets(machine.device, BINDINGS, writes, 0, NULL);
    pf_timestamp("Descriptors created (DEVICE_LOCAL)");

#pragma endregion

    VkSemaphoreTypeCreateInfo type_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue  = 0
    };
    VkSemaphoreCreateInfo tsci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type_info };
    VkSemaphore render_timeline;
    VK_CHECK(vkCreateSemaphore(machine.device, &tsci, NULL, &render_timeline));
    uint64_t timeline_value = 0;

    #pragma region FRAME LOOP
    #if DEBUG_APP == 1
    renderer.frame_id_counter = 1;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        renderer.frame_id_per_slot[i]   = 0;
    }
    PFN_vkGetCalibratedTimestampsEXT vkGetCalibratedTimestampsEXT =
    (PFN_vkGetCalibratedTimestampsEXT)vkGetDeviceProcAddr(machine.device, "vkGetCalibratedTimestampsEXT");
    if (!vkGetCalibratedTimestampsEXT) {
        printf("VK_KHR_calibrated_timestamps not supported.\n");
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(machine.physical_device, &props);
    renderer.gpu_ticks_to_ns = props.limits.timestampPeriod; // 1 tick == this many ns

    PFN_vkWaitForPresentKHR vkWaitForPresentKHR =
    (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(machine.device, "vkWaitForPresentKHR");
    if (!vkWaitForPresentKHR) {
        printf("VK_KHR_present_wait not supported.\n");
    }
    uint64_t presented_frame_ids[MAX_FRAMES_IN_FLIGHT] = {0};
    #endif

    // create semaphore linked to swapchain image (for gpu to wait for this swapchain image to be released) 
    // create fence (for cpu to wait for one of the command buffers to finish on the gpu)
    VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VK_CHECK(vkCreateSemaphore(machine.device, &semaphore_info, NULL, &renderer.sem_image_available[i]));
    }
    
    // take out two of the four images to have only two images in use
    // this way we only ever render a third frame when we're sure the first has been scanned out
    #if DEBUG_APP == 1
    printf("SWAPCHAIN IMAGE COUNT: %d\n", swapchain.swapchain_image_count);
    #endif
    // VkSemaphore parking_semaphores[3]; uint32_t parked_images[3];
    // for (uint32_t i = 0; i < swapchain.swapchain_image_count - MAX_FRAMES_IN_FLIGHT - 1; ++i) {
    //     printf("parking an image\n");
    //     VK_CHECK(vkCreateSemaphore(machine.device, &semaphore_info, NULL, &parking_semaphores[i]));
    //     vkAcquireNextImageKHR(machine.device, swapchain.swapchain, UINT64_MAX, parking_semaphores[i], VK_NULL_HANDLE, &parked_images[i]);
    // }

    renderer.frame_slot = 0;
    while (pf_poll_events(window)) {
        uint64_t wait_value = timeline_value; // for MAX_FRAMES_IN_FLIGHT == 1
        // If you ever increase MAX_FRAMES_IN_FLIGHT to N, use:
        // uint64_t wait_value = renderer.timeline_value - (MAX_FRAMES_IN_FLIGHT - 1);
        VkSemaphoreWaitInfo wi = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &render_timeline,
            .pValues     = &wait_value
        };
        VK_CHECK(vkWaitSemaphores(machine.device, &wi, UINT64_MAX));

        #if DEBUG_APP == 1
        if (vkWaitForPresentKHR) {
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
                }
            }
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
#ifdef _WIN32
                    { .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, .timeDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT }
#else
                    { .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT }
#endif
                };
                // Take the sample as close as possible to the read
                vkGetCalibratedTimestampsEXT(machine.device, 2, infos, ts, &maxDev);
                // Map GPU ticks -> CPU ns using your device period
                double gpu_now_ns = (double)ts[0] * renderer.gpu_ticks_to_ns;
                double cpu_now_ns = (double)ts[1];
                #ifdef _WIN32
                cpu_now_ns = (double) pf_ticks_to_ns(cpu_now_ns);
                #endif
                offset_ns = cpu_now_ns - gpu_now_ns;  // add this to any GPU ns to place on CPU timeline
            }
            // Now read the two GPU timestamps for that image (they’re done because we waited the fence)
            uint64_t t[QUERIES_PER_IMAGE] = {0};
            VkResult qr = vkGetQueryPoolResults(machine.device, swapchain.query_pool,
                                    q_first, QUERIES_PER_IMAGE, sizeof(t), t,
                                    sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            if (qr == VK_SUCCESS) {
                double ns[QUERIES_PER_IMAGE];
                for (int i=0;i<QUERIES_PER_IMAGE;i++) ns[i] = (double)t[i] * renderer.gpu_ticks_to_ns;
                // Stage durations (ms)
                double ms_zero          = (ns[Q_AFTER_ZERO]          - ns[Q_BEGIN])             * 1e-6;
                double ms_comp_build    = (ns[Q_AFTER_COMP_BUILD]    - ns[Q_AFTER_ZERO])        * 1e-6;
                double ms_comp_indirect = (ns[Q_AFTER_COMP_INDIRECT] - ns[Q_AFTER_COMP_BUILD])  * 1e-6;
                double ms_barrier_gfx   = (ns[Q_AFTER_COMP_TO_GFX]   - ns[Q_AFTER_COMP_INDIRECT])*1e-6;
                double ms_renderpass    = (ns[Q_AFTER_RENDERPASS]    - ns[Q_BEFORE_RENDERPASS]) * 1e-6;
                double ms_blit          = (ns[Q_AFTER_BLIT]          - ns[Q_AFTER_RENDERPASS])  * 1e-6;
                double ms_tail          = (ns[Q_END]                 - ns[Q_AFTER_BLIT])        * 1e-6; // usually tiny
                // (Optional) map to CPU timeline using your calibrated offset
                double cpu_ms[QUERIES_PER_IMAGE];
                for (int i=0;i<QUERIES_PER_IMAGE;i++) cpu_ms[i] = (offset_ns + ns[i] - pf_ns_start()) * 1e-6;
                printf("[%llu] gpu time %.3fms - %.3fms : zero=%.3f, comp_build=%.3f, comp_indirect=%.3f, barrier=%.3f, render=%.3f, blit=%.3f, tail=%.3f, [%.3f]\n",
                       (unsigned long long)last_frame_id, cpu_ms[Q_BEGIN], cpu_ms[Q_END],
                       ms_zero, ms_comp_build, ms_comp_indirect, ms_barrier_gfx, ms_renderpass, ms_blit, ms_tail,
                       (ns[Q_END]-ns[Q_BEGIN]) * 1e-6);
            } else {printf("ERROR: %s\n", vk_result_str(qr));}
            // Clear slot so we don’t read twice
            swapchain.previous_frame_image_index[renderer.frame_slot] = UINT32_MAX;
        }
        #endif

        // get the next swapchain image (block until one is available)
        uint32_t swap_image_index = 0;
        VkResult acquire_result = vkAcquireNextImageKHR(machine.device, swapchain.swapchain, UINT64_MAX, renderer.sem_image_available[renderer.frame_slot], VK_NULL_HANDLE, &swap_image_index);
        // recreate the swapchain if the window resized
        if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(&machine, &renderer, &swapchain, window); continue; }
        if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) { printf("vkAcquireNextImageKHR failed: %s\n", vk_result_str(acquire_result)); break; }
        
        #pragma region update uniforms
        process_inputs();
        struct Uniforms u = {0};
        encode_uniforms(&u, cam_x, cam_y, cam_z, cam_yaw, cam_pitch);
        void*dst=NULL;
        VK_CHECK(vkMapMemory(machine.device, renderer.memory_uniforms, 0, sizeof u, 0, &dst));
        memcpy(dst, &u, sizeof u);
        vkUnmapMemory(machine.device, renderer.memory_uniforms);

        // start recording for this frame's command buffer
        f32 frame_start_time = (f32) (pf_ns_now() - pf_ns_start()) / 1e6;
        VkCommandBuffer cmd = swapchain.command_buffers_per_image[swap_image_index];
        if (!swapchain.command_buffers_recorded[swap_image_index]) {
            // START RECORDING
            VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            // VK_CHECK(vkResetCommandBuffer(cmd, 0)); // don't need to reset if recorded once and reused
            VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));
            #if DEBUG_APP == 1
            uint32_t q0 = swap_image_index * QUERIES_PER_IMAGE + 0; // index of the first query of this frame
            vkCmdResetQueryPool(cmd, swapchain.query_pool, q0, QUERIES_PER_IMAGE);
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, swapchain.query_pool, q0 + Q_BEGIN);
            #endif
            
            // BLOCK UNTIL UNIFORM WRITES ARE VISIBLE
            VkMemoryBarrier2 cam_host_to_shader = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask  = VK_PIPELINE_STAGE_2_HOST_BIT,
                .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT,
                .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT
            };
            vkCmdPipelineBarrier2(cmd, &(VkDependencyInfo){
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1,
                .pMemoryBarriers    = &cam_host_to_shader
            });
            
            // ZERO THE DRAW CALL BUFFER (todo: will not be needed anymore after adding counts pass setup)
            vkCmdFillBuffer(cmd, renderer.buffer_counters, 0, size_counters, 0);
            vkCmdFillBuffer(cmd, renderer.buffer_counts, 0, size_counts, 0);
            VkMemoryBarrier2 xfer_to_compute = {
                .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT
            };
            VkDependencyInfo dep0 = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &xfer_to_compute };
            vkCmdPipelineBarrier2(cmd, &dep0);
            #if DEBUG_APP == 1
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, swapchain.query_pool, q0 + Q_AFTER_ZERO);
            #endif

            // VISIBLE COUNT PASS
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,renderer.common_pipeline_layout, 0, 1, &renderer.descriptor_set, 0, NULL);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.compute_pipeline);
            uint32_t mode_counts = 0;
            vkCmdPushConstants(cmd,
                renderer.common_pipeline_layout,
                VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(uint32_t), &mode_counts);
            vkCmdDispatch(cmd, (total_instance_count+63)/64, 1, 1);
            VkMemoryBarrier2 after_count = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = CS,
                .srcAccessMask = SSW,                  // pass 0 wrote SSBOs
                .dstStageMask = CS,
                .dstAccessMask = SSR | SSW,            // pass 1 will read & write SSBOs
            };
            VkDependencyInfo dep_after_count = {
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1,
                .pMemoryBarriers = &after_count,
            };
            vkCmdPipelineBarrier2(cmd, &dep_after_count);

            #if DEBUG_APP == 1
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, swapchain.query_pool, q0 + Q_AFTER_COMP_BUILD);
            #endif

            // PREPARE INDIRECT PASS
            uint32_t mode_prepare = 1;
            vkCmdPushConstants(cmd,
                renderer.common_pipeline_layout,
                VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(uint32_t), &mode_prepare);
            vkCmdDispatch(cmd, 1, 1, 1);
            VkMemoryBarrier2 after_prepare = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = CS,
                .srcAccessMask = SSW,
                .dstStageMask = CS,
                .dstAccessMask = SSR | SSW,            // scatter reads OFFSETS & writes VISIBLE, DRAW_CALLS.instance_count
            };
            VkDependencyInfo dep_after_prepare = {
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1,
                .pMemoryBarriers = &after_prepare,
            };
            vkCmdPipelineBarrier2(cmd, &dep_after_prepare);

            // SCATTER PASS
            uint32_t mode_scatter = 2;
            vkCmdPushConstants(cmd,
                renderer.common_pipeline_layout,
                VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(uint32_t), &mode_scatter);
            vkCmdDispatch(cmd, (total_instance_count+63)/64, 1, 1);
            
            #if DEBUG_APP == 1
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, swapchain.query_pool, q0 + Q_AFTER_COMP_INDIRECT);
            #endif
            VkBufferMemoryBarrier2 scatter_to_indirect = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask = CS,
                .srcAccessMask = SSW,                  // wrote indirect args
                .dstStageMask = DI,
                .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = renderer.buffer_counters,  // backing buffer for DRAW_CALLS
                .offset = 0,
                .size = VK_WHOLE_SIZE,
            };
            // (B) make VISIBLE[] visible to the vertex/graphics stage (SSBO reads in VS or later)
            VkBufferMemoryBarrier2 scatter_to_vs = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask = CS,
                .srcAccessMask = SSW,                  // wrote instance data
                .dstStageMask = VS,                    // or FRAGMENT if read there; include both if needed
                .dstAccessMask = SSR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = renderer.buffer_visible,     // backing buffer for VISIBLE
                .offset = 0,
                .size = VK_WHOLE_SIZE,
            };
            VkDependencyInfo dep_to_graphics = {
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .bufferMemoryBarrierCount = 2,
                .pBufferMemoryBarriers = (VkBufferMemoryBarrier2[]){ scatter_to_indirect, scatter_to_vs },
            };
            vkCmdPipelineBarrier2(cmd, &dep_to_graphics);
            #if DEBUG_APP == 1
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, swapchain.query_pool, q0 + Q_AFTER_COMP_TO_GFX);
            #endif

            // --- render pass (use persistent framebuffer) ---
            VkImageMemoryBarrier2 to_depth = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask  = VK_PIPELINE_STAGE_2_NONE,
                .srcAccessMask = 0,
                .dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                .oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED,            // or previous
                .newLayout     = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                .image         = swapchain.depth_image,                // your depth image
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                    .baseMipLevel = 0, .levelCount = 1,
                    .baseArrayLayer = 0, .layerCount = 1
                }
            };
            vkCmdPipelineBarrier2(cmd, &(VkDependencyInfo){
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_depth
            });
            VkImageMemoryBarrier2 to_color = {
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
              .srcStageMask  = VK_PIPELINE_STAGE_2_NONE,
              .srcAccessMask = 0,
              .dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
              .oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED,          // swapchain image is undefined after acquire
              .newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              .image         = swapchain.swapchain_images[swap_image_index],
              .subresourceRange = { .aspectMask=VK_IMAGE_ASPECT_COLOR_BIT, .levelCount=1, .layerCount=1 }
            };
            vkCmdPipelineBarrier2(cmd, &(VkDependencyInfo){ .sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
              .imageMemoryBarrierCount=1, .pImageMemoryBarriers=&to_color });
            VkRenderingAttachmentInfo swap_att = {
              .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
              .imageView = swapchain.swapchain_views[swap_image_index],
              .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
              .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            };
            VkRenderingAttachmentInfo depth_att = {
              .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
              .imageView = swapchain.depth_view,
              .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
              .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
              .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
              .clearValue = { .depthStencil = { 0.0f, 0 } },
            };
            VkRenderingInfo ri_swap = {
              .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
              .renderArea = { .offset = {0,0}, .extent = swapchain.swapchain_extent },
              .layerCount = 1,
              .colorAttachmentCount = 1,
              .pColorAttachments = &swap_att,
              .pDepthAttachment = &depth_att
            };
            #if DEBUG_APP == 1
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, swapchain.query_pool, q0 + Q_BEFORE_RENDERPASS);
            #endif
            vkCmdBeginRendering(cmd, &ri_swap);
            // set the resolution of the intermediary pass
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.main_pipeline);
            // descriptors for VS (POSITIONS/NORMALS etc.)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.common_pipeline_layout, 0, 1, &renderer.descriptor_set, 0, NULL);
            // vertex buffers: [0]=VISIBLE_PACKED (instance), [1]=UVs (per-vertex)
            VkBuffer vbs[1]     = { renderer.buffer_visible };
            VkDeviceSize ofs[1] = { 0 };
            vkCmdBindVertexBuffers(cmd, 0, 1, vbs, ofs);
            // index buffer: fixed 1152 indices (values 0..255 in your mesh order)
            vkCmdBindIndexBuffer(cmd, renderer.buffer_index_ib, 0, VK_INDEX_TYPE_UINT16);
            // one GPU-driven draw then a single fullscreen triangle for sky
            uint32_t mode_mesh = 0;
            vkCmdPushConstants(cmd,
                renderer.common_pipeline_layout,
                VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(uint32_t), &mode_mesh);
            vkCmdDrawIndexedIndirect(cmd, renderer.buffer_counters, 0, MESH_COUNT, sizeof(VkDrawIndexedIndirectCommand));
            #if DEBUG_APP == 1
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, swapchain.query_pool, q0 + Q_AFTER_RENDERPASS);
            #endif
            uint32_t mode_sky = 1;
            vkCmdPushConstants(cmd,
                renderer.common_pipeline_layout,
                VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(uint32_t), &mode_sky);
            vkCmdDraw(cmd, 3, 1, 0, 0); // sky fullscreen triangle
            vkCmdEndRendering(cmd);
            #if DEBUG_APP == 1
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, swapchain.query_pool, q0 + Q_AFTER_BLIT);
            #endif
            VkImageMemoryBarrier2 to_present = {
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
              .srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
              .dstStageMask  = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
              .dstAccessMask = 0,
              .oldLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              .newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
              .image         = swapchain.swapchain_images[swap_image_index],
              .subresourceRange = { .aspectMask=VK_IMAGE_ASPECT_COLOR_BIT, .levelCount=1, .layerCount=1 }
            };
            vkCmdPipelineBarrier2(cmd, &(VkDependencyInfo){ .sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
              .imageMemoryBarrierCount=1, .pImageMemoryBarriers=&to_present });
            #if DEBUG_APP == 1
            #endif
           
            #if DEBUG_APP == 1
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, swapchain.query_pool, q0 + Q_END);
            #endif
            VK_CHECK(vkEndCommandBuffer(cmd));
            swapchain.command_buffers_recorded[swap_image_index] = 1;
            printf("Recorded command buffer %d\n", swap_image_index);
        }

        #if DEBUG_APP == 1
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
                #ifdef _WIN32
                { .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, .timeDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT }
                #else
                { .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT }
                #endif
            };
            vkGetCalibratedTimestampsEXT(machine.device, 2, infos, ts, &maxDev);
            uint64_t gpu_now_ticks = ts[0];
            uint64_t cpu_now_ns    = ts[1]; // on Linux MONOTONIC is ns
        }
        #endif

        // submit
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore waits[]   = { renderer.sem_image_available[renderer.frame_slot] }; // binary from acquire
        VkSemaphore signals[] = { swapchain.present_ready_per_image[swap_image_index], render_timeline };
        uint64_t next_value = timeline_value + 1;
        // Values for each signal semaphore; binaries ignore their value (can be 0)
        uint64_t signal_values[] = { 0, next_value };
        VkTimelineSemaphoreSubmitInfo tssi = {
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .signalSemaphoreValueCount = 2,
            .pSignalSemaphoreValues    = signal_values,
            .waitSemaphoreValueCount   = 0,    // (we’re not waiting on a timeline in this submit)
            .pWaitSemaphoreValues      = NULL
        };
        VkSubmitInfo submit_info = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = &tssi,
            .waitSemaphoreCount = 1, .pWaitSemaphores = waits, .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1, .pCommandBuffers = &cmd,
            .signalSemaphoreCount = 2, .pSignalSemaphores = signals
        };
        VK_CHECK(vkQueueSubmit(machine.queue_graphics, 1, &submit_info, VK_NULL_HANDLE));
        // Bump our CPU-side notion of the timeline
        timeline_value = next_value;

        // (E) Present: wait on render-finished
        VkPresentInfoKHR present_info = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &swapchain.present_ready_per_image[swap_image_index],
            .swapchainCount = 1, .pSwapchains = &swapchain.swapchain, .pImageIndices = &swap_image_index
        };
        #if DEBUG_APP == 1
        if (vkWaitForPresentKHR) {
            VkPresentIdKHR present_id_info = {
                .sType          = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
                .pNext          = NULL,
                .swapchainCount = 1,
                .pPresentIds    = &renderer.frame_id_counter,
            };
            present_info.pNext = &present_id_info;
        }
        #endif
        VkResult present_res = vkQueuePresentKHR(machine.queue_present, &present_info);
        if (present_res == VK_ERROR_OUT_OF_DATE_KHR || present_res == VK_SUBOPTIMAL_KHR) {
            recreate_swapchain(&machine, &renderer, &swapchain, window);
            continue;
        } else if (present_res != VK_SUCCESS) {
            printf("vkQueuePresentKHR failed: %d\n", present_res);
            break;
        }

        #if DEBUG_CPU == 1
        f32 frame_end_time = (f32) (pf_ns_now() - pf_ns_start()) / 1e6;
        printf("cpu time %.3fms - %.3fms [%.3fms]\n", frame_start_time, frame_end_time, frame_end_time - frame_start_time);
        #endif
        swapchain.previous_frame_image_index[renderer.frame_slot] = swap_image_index;
        renderer.frame_slot = (renderer.frame_slot + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    return 0;
}
