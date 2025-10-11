#include "header.h"

#include "mesh.h" // todo: remove

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

extern const unsigned char shaders[];
extern const unsigned char shaders_end[];
#define shaders_len ((size_t)(shaders_end - shaders))

// todo: create vk_shader (to have one big shader with multiple entrypoints, have shader hot reloading, ...)
// todo: create vk_texture (loading textures, maybe hot reloading textures, ...)

// 1 for double buffering (one of ours, two of present engine) (technically triple buffered, but modern apis don't seem to do actual double buffering)
// one is being scanned out, one is done and waiting (this one is also considered 'presented' but not scanned out yet)
// and one of ours being rendered (this is this 1 frame that is 'in flight')
#define MAX_FRAMES_IN_FLIGHT 1
#define MAX_SWAPCHAIN_IMAGES 5

struct Renderer {
    // compute pipelines
    VkDescriptorSetLayout     common_set_layout;
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

struct Uniforms {
    float camera_vp[4][4];
    float light_vp[4][4];
    float camera_ws[4];
    float light_ws[4];
};

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
    // setup with X11
    pf_time_reset();
    WINDOW window = pf_create_window(NULL, key_input_callback,mouse_input_callback);
    pf_timestamp("Created platform window");

#if defined(_WIN32) && DEBUG == 1
    // set env to point to vk layer path to allow finding
    const char* sdk = getenv("VULKAN_SDK");
    char buf[1024];
    snprintf(buf, sizeof buf, "VK_LAYER_PATH=%s/Bin", sdk);
    putenv(strdup(buf));
#endif
#if USE_DISCRETE_GPU == 0 && !defined(_WIN32)
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
    #define BINDINGS 6
    VkDescriptorSetLayoutBinding bindings[BINDINGS];
    for (uint32_t i = 0; i < BINDINGS; ++i) {
        bindings[i] = (VkDescriptorSetLayoutBinding) {
            .binding         = i,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT
        };
        // use uniform for the last one
        bindings[i].descriptorType = (i < 5) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    }
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = BINDINGS,
        .pBindings    = bindings
    };
    VK_CHECK(vkCreateDescriptorSetLayout(machine.device, &set_layout_info, NULL, &renderer.common_set_layout));

    /* -------- Pipeline Layouts -------- */
    VkPipelineLayoutCreateInfo common_pipeline_info = {
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts    = &renderer.common_set_layout
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
        .layout              = renderer.common_pipeline_layout
    };
    VkPipelineRenderingCreateInfo pr = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = (VkFormat[]){ swapchain.swapchain_format },
        // .depthAttachmentFormat = VK_FORMAT_UNDEFINED,
        // .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
    };
    graphics_info.pNext = &pr;
    VK_CHECK(vkCreateGraphicsPipelines(machine.device, VK_NULL_HANDLE, 1, &graphics_info, NULL, &renderer.main_pipeline));
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

    // UNIFORMS
    create_buffer_and_memory(machine.device, machine.physical_device, sizeof(struct Uniforms),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        host_visible_coherent, &renderer.buffer_uniforms, &renderer.memory_uniforms);

    pf_timestamp("Buffers created");

    // upload the data for: uvs, positions, normals, indices, instances
    struct Instance {short x, y; char yaw, mesh_id; char padding[2];};
    {
        upload_to_buffer(machine.device, renderer.memory_positions, sizeof(uint32_t)*kVertsPerMesh, g_positions_mesh);
        upload_to_buffer(machine.device, renderer.memory_normals,   sizeof(uint32_t)*kVertsPerMesh, g_normals_mesh);
        upload_to_buffer(machine.device, renderer.memory_uvs,       sizeof(uint16_t)*2*kVertsPerMesh, g_uvs_mesh);
        upload_to_buffer(machine.device, renderer.memory_index_ib,  sizeof(uint32_t)*kIdxPerMesh, g_indices_mesh);
        struct Instance instance_zeroed = (struct Instance) {.x=0,.y=0,.yaw=32,.mesh_id=0};
        upload_to_buffer(machine.device, renderer.memory_instances, sizeof(uint64_t)*1, &instance_zeroed);
    }
    
    // --- CAMERA SETUP ---
    // 1. time-based animation or user-controlled camera
    float t = (float)((pf_ns_now() - pf_ns_start()) * 1e-9f);
    // Example: orbit around origin at radius 5
    float eye[3] = {
        5.0f * sinf(0.5f * t),
        2.0f,
       -5.0f * cosf(0.5f * t)
    };
    float at[3] = { 0.0f, 1.5f, 0.0f };
    float up[3] = { 0.0f, 1.0f, 0.0f };
    // 2. view matrix
    float V[16];
    make_lookat_rh(eye, at, up, V);
    // 3. projection matrix
    float P[16];
    float aspect = (float)swapchain.swapchain_extent.width /
                   (float)swapchain.swapchain_extent.height;
    make_perspective_vk(60.0f * 3.14159265f/180.0f, aspect, 0.1f, 100.0f, P);
    // 4. combined view-projection
    float VP[16];
    #define M(i,j) (i + 4*j)
    for (int c=0;c<4;c++)
      for (int r=0;r<4;r++)
        VP[M(r,c)] = P[M(r,0)]*V[M(0,c)] + P[M(r,1)]*V[M(1,c)] +
                     P[M(r,2)]*V[M(2,c)] + P[M(r,3)]*V[M(3,c)];
    #undef M
    // 5. light direction (world-space)
    float lightDirWS[3] = { 0.4f, -1.0f, 0.3f };
    float len = sqrtf(lightDirWS[0]*lightDirWS[0] +
                      lightDirWS[1]*lightDirWS[1] +
                      lightDirWS[2]*lightDirWS[2]);
    lightDirWS[0]/=len; lightDirWS[1]/=len; lightDirWS[2]/=len;
    // --- WRITE TO UBO ---
    struct Uniforms uniforms = {0};
    // Transpose to row-major memory layout before uploading
    memcpy(uniforms.camera_vp, VP, sizeof(VP));           // <— no mat4_to_rowvec4s

    static const float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    memcpy(uniforms.light_vp, I, sizeof(I));
    uniforms.camera_ws[0]=eye[0]; uniforms.camera_ws[1]=eye[1]; uniforms.camera_ws[2]=eye[2]; uniforms.camera_ws[3]=1.0f;
    uniforms.light_ws [0]=lightDirWS[0]; uniforms.light_ws [1]=lightDirWS[1]; uniforms.light_ws [2]=lightDirWS[2]; uniforms.light_ws [3]=0.0f;

    void* dst = 0;
    VK_CHECK(vkMapMemory(machine.device, renderer.memory_uniforms, 0, sizeof uniforms, 0, &dst));
    memcpy(dst, &uniforms, sizeof uniforms);
    vkUnmapMemory(machine.device, renderer.memory_uniforms);

    /* -------- Descriptor Pool & Set -------- */
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  BINDINGS-1 }, // bindings 0..4
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1 }, // binding 5  <<< add this
        // keep these only if/when you actually use them:
        // { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1 },
        // { VK_DESCRIPTOR_TYPE_SAMPLER,         1 },
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2,
        .poolSizeCount = (uint32_t)(sizeof(pool_sizes)/sizeof(pool_sizes[0])),
        .pPoolSizes = pool_sizes,
    };
    VK_CHECK(vkCreateDescriptorPool(machine.device, &pool_info, NULL, &renderer.descriptor_pool));
    VkDescriptorSetAllocateInfo set_alloc_info = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = renderer.descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &renderer.common_set_layout
    };
    VK_CHECK(vkAllocateDescriptorSets(machine.device, &set_alloc_info, &renderer.descriptor_set));

    VkDescriptorBufferInfo buffer_infos[BINDINGS] = {
        { renderer.buffer_instances, 0, size_instances }, // binding 0
        { renderer.buffer_visible,   0, size_visible   }, // binding 1
        { renderer.buffer_counters,  0, size_counters  }, // binding 2
        { renderer.buffer_positions, 0, size_positions }, // binding 3
        { renderer.buffer_normals,   0, size_normals   },  // binding 4
        { renderer.buffer_uniforms,  0, sizeof(struct Uniforms)   }  // binding 5
    };
    VkWriteDescriptorSet writes[BINDINGS];
    for (uint32_t i = 0; i < BINDINGS; ++i) {
        writes[i] = (VkWriteDescriptorSet) {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = renderer.descriptor_set,
            .dstBinding      = i,
            .descriptorCount = 1,
            // set the last one as uniform
            .descriptorType  = (i<5)?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo     = &buffer_infos[i]
        };
    }
    vkUpdateDescriptorSets(machine.device, BINDINGS, writes, 0, NULL);
    pf_timestamp("Descriptors created");
    
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
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VK_CHECK(vkCreateSemaphore(machine.device, &semaphore_info, NULL, &renderer.sem_image_available[i]));
    }
    
    // take out two of the four images to have only two images in use
    // this way we only ever render a third frame when we're sure the first has been scanned out
    #if DEBUG_APP == 1
    printf("SWAPCHAIN IMAGE COUNT: %d\n", swapchain.swapchain_image_count);
    #endif
    VkSemaphore parking_semaphores[3]; uint32_t parked_images[3];
    for (uint32_t i = 0; i < swapchain.swapchain_image_count - MAX_FRAMES_IN_FLIGHT - 1; ++i) {
        printf("parking an image\n");
        VK_CHECK(vkCreateSemaphore(machine.device, &semaphore_info, NULL, &parking_semaphores[i]));
        vkAcquireNextImageKHR(machine.device, swapchain.swapchain, UINT64_MAX, parking_semaphores[i], VK_NULL_HANDLE, &parked_images[i]);
    }

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
        VkResult acquire_result = vkAcquireNextImageKHR(machine.device, swapchain.swapchain, 100000000, renderer.sem_image_available[renderer.frame_slot], VK_NULL_HANDLE, &swap_image_index);
        // recreate the swapchain if the window resized
        if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(&machine, &renderer, &swapchain, window); continue; }
        if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) { printf("vkAcquireNextImageKHR failed: %d\n", acquire_result); break; }
        
        // update uniforms        
        // for now: identity for light; later fill with ortho light VP
        memcpy(uniforms.camera_vp, VP, sizeof(VP));                // column-major
        static const float I[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1
        };
        memcpy(uniforms.light_vp, I,  sizeof(I));
        uniforms.camera_ws[0]=eye[0]; uniforms.camera_ws[1]=eye[1]; uniforms.camera_ws[2]=eye[2];
        uniforms.light_ws[0]=lightDirWS[0]; uniforms.light_ws[1]=lightDirWS[1]; uniforms.light_ws[2]=lightDirWS[2];
        // upload (HOST_COHERENT)
        void* dst=NULL;
        VK_CHECK(vkMapMemory(machine.device, renderer.memory_uniforms, 0, sizeof(uniforms), 0, &dst));
        memcpy(dst, &uniforms, sizeof(uniforms));
        vkUnmapMemory(machine.device, renderer.memory_uniforms);

        // start recording for this frame's command buffer
        f32 frame_start_time = (f32) (pf_ns_now() - pf_ns_start()) / 1e6;
        VkCommandBuffer cmd = swapchain.command_buffers_per_image[swap_image_index];
        if (!swapchain.command_buffers_recorded[swap_image_index]) {
            // VK_CHECK(vkResetCommandBuffer(cmd, 0)); // don't need to reset if recorded once and reused
            VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));
            #if DEBUG_APP == 1
            uint32_t q0 = swap_image_index * QUERIES_PER_IMAGE + 0; // index of the first query of this frame
            // Reset the time queries for frame
            vkCmdResetQueryPool(cmd, swapchain.query_pool, q0, QUERIES_PER_IMAGE);
            // Start timing
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, swapchain.query_pool, q0 + Q_BEGIN);
            #endif
            
            // make writes visible
            VkMemoryBarrier2 cam_host_to_shader = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask  = VK_PIPELINE_STAGE_2_HOST_BIT,
                .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT,
                .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT
            };
            // record once, before your other work (e.g. right after your first timestamp)
            vkCmdPipelineBarrier2(cmd, &(VkDependencyInfo){
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1,
                .pMemoryBarriers    = &cam_host_to_shader
            });
            
            // Zero the counters on GPU
            vkCmdFillBuffer(cmd, renderer.buffer_counters, 0, size_counters, 0);
            // Make the writes visible to COMPUTE
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

            // --- Compute passes ---
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,renderer.common_pipeline_layout, 0, 1, &renderer.descriptor_set, 0, NULL);
            // Build visible (no culling): instanceCount = numInstances
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.compute_pipelines[0]);
            vkCmdDispatch(cmd, (numInstances+63)/64, 1, 1);
            #if DEBUG_APP == 1
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, swapchain.query_pool, q0 + Q_AFTER_COMP_BUILD);
            #endif

            // Prepare indirect
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.compute_pipelines[1]);
            vkCmdDispatch(cmd, 1, 1, 1);
            #if DEBUG_APP == 1
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, swapchain.query_pool, q0 + Q_AFTER_COMP_INDIRECT);
            #endif
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
            #if DEBUG_APP == 1
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, swapchain.query_pool, q0 + Q_AFTER_COMP_TO_GFX);
            #endif

            // --- render pass (use persistent framebuffer) ---
            VkImageMemoryBarrier2 to_color = {
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
              .srcStageMask  = VK_PIPELINE_STAGE_2_NONE,
              .srcAccessMask = 0,
              .dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
              .oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED,          // swapchain image is undefined after acquire
              .newLayout     = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
              .image         = swapchain.swapchain_images[swap_image_index],
              .subresourceRange = { .aspectMask=VK_IMAGE_ASPECT_COLOR_BIT, .levelCount=1, .layerCount=1 }
            };
            vkCmdPipelineBarrier2(cmd, &(VkDependencyInfo){ .sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
              .imageMemoryBarrierCount=1, .pImageMemoryBarriers=&to_color });
            VkRenderingAttachmentInfo swap_att = {
              .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
              .imageView = swapchain.swapchain_views[swap_image_index],
              .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
              .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
              .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            };
            VkRenderingInfo ri_swap = {
              .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
              .renderArea = { .offset = {0,0}, .extent = swapchain.swapchain_extent },
              .layerCount = 1,
              .colorAttachmentCount = 1,
              .pColorAttachments = &swap_att,
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
            VkBuffer vbs[2]     = { renderer.buffer_visible, renderer.buffer_uvs };
            VkDeviceSize ofs[2] = { 0, 0 };
            vkCmdBindVertexBuffers(cmd, 0, 2, vbs, ofs);
            // index buffer: fixed 1152 indices (values 0..255 in your mesh order)
            vkCmdBindIndexBuffer(cmd, renderer.buffer_index_ib, 0, VK_INDEX_TYPE_UINT32);
            // one GPU-driven draw
            vkCmdDrawIndexedIndirect(cmd, renderer.buffer_counters, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
            vkCmdEndRendering(cmd);
            VkImageMemoryBarrier2 to_present = {
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
              .srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
              .dstStageMask  = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
              .dstAccessMask = 0,
              .oldLayout     = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
              .newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
              .image         = swapchain.swapchain_images[swap_image_index],
              .subresourceRange = { .aspectMask=VK_IMAGE_ASPECT_COLOR_BIT, .levelCount=1, .layerCount=1 }
            };
            vkCmdPipelineBarrier2(cmd, &(VkDependencyInfo){ .sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
              .imageMemoryBarrierCount=1, .pImageMemoryBarriers=&to_present });
            #if DEBUG_APP == 1
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, swapchain.query_pool, q0 + Q_AFTER_RENDERPASS);
            #endif
           
            #if DEBUG_APP == 1
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, swapchain.query_pool, q0 + Q_AFTER_BLIT);
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
