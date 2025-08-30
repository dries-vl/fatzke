// cc main.c -lwayland-client -lvulkan -ldl -lpthread -O2 -o tri2 && ./tri2
#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
#include <xdg-shell-client-protocol.c>

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>

typedef uint32_t u32;
typedef int32_t i32;

/* --- timing --- */
typedef unsigned long long u64;

static inline u64 now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static u64 T0;
#define TINIT() do{T0=now_ns();}while(0)
#define TSTAMP(msg) do{u64 _t=now_ns(); fprintf(stderr,"[+%7.3f ms] %s\n",(_t-T0)/1e6,(msg));}while(0)

/* --- Wayland --- */
static struct wl_display* dpy;
static struct wl_compositor* comp;
static struct xdg_wm_base* xdg;
static struct wl_surface* surf;
static struct xdg_surface* xsurf;
static struct xdg_toplevel* xtop;
static i32 win_w = 1280, win_h = 720;
static bool running = true, need_swapchain = false;

static void xdg_ping(void* d, struct xdg_wm_base* b, u32 s)
{
    (void)d;
    xdg_wm_base_pong(b, s);
}

static const struct xdg_wm_base_listener xdg_wm = {.ping = xdg_ping};

static void top_config(void* d, struct xdg_toplevel* t, i32 w, i32 h, struct wl_array* st)
{
    (void)d;
    (void)t;
    (void)st;
    if (w > 0 && h > 0)
    {
        win_w = w;
        win_h = h;
    }
}

static void top_close(void* d, struct xdg_toplevel* t)
{
    (void)d;
    (void)t;
    running = false;
}

static void top_bounds(void* d, struct xdg_toplevel* t, i32 w, i32 h)
{
    (void)d;
    (void)t;
    (void)w;
    (void)h;
}

static void top_caps(void* d, struct xdg_toplevel* t, struct wl_array* c)
{
    (void)d;
    (void)t;
    (void)c;
}

static const struct xdg_toplevel_listener top_l = {
    .configure = top_config, .close = top_close, .configure_bounds = top_bounds, .wm_capabilities = top_caps
};

static void xsurf_conf(void* d, struct xdg_surface* s, u32 serial)
{
    (void)d;
    xdg_surface_ack_configure(s, serial);
    need_swapchain = true;
    TSTAMP("xdg_surface configure");
}

static const struct xdg_surface_listener xsurf_l = {.configure = xsurf_conf};

static void reg_add(void* d, struct wl_registry* r, uint32_t name, const char* iface, uint32_t ver){
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        uint32_t v = ver < 4 ? ver : 4; // we only need <= v4 features; v1 is fine too
        comp = wl_registry_bind(r, name, &wl_compositor_interface, v);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        uint32_t v = ver < 6 ? ver : 6; // clamp to server's version (yours is 1)
        xdg = wl_registry_bind(r, name, &xdg_wm_base_interface, v);
        xdg_wm_base_add_listener(xdg, &xdg_wm, NULL); // only .ping used → v1 OK
    }
}

static void reg_rem(void* d, struct wl_registry* r, u32 name)
{
    (void)d;
    (void)r;
    (void)name;
}

static const struct wl_registry_listener reg_l = {.global = reg_add, .global_remove = reg_rem};

/* --- Vulkan helpers --- */
#define VKC(x) do{ VkResult _r=(x); if(_r!=VK_SUCCESS){ fprintf(stderr,"VK ERR %d @%s:%d\n",_r,__FILE__,__LINE__); exit(1);} }while(0)
#define VKC_ENUM(x) do{ VkResult _r=(x); if(_r!=VK_SUCCESS && _r!=VK_INCOMPLETE){ fprintf(stderr,"VK ENUM ERR %d @%s:%d\n",_r,__FILE__,__LINE__); exit(1);} }while(0)

static VkInstance inst;
static VkSurfaceKHR vsurf;
static VkPhysicalDevice phys;
static u32 qfam;
static VkDevice dev;
static VkQueue q;

/* swapchain & onscreen */
static VkSwapchainKHR sc;
static VkFormat sc_fmt;
static VkExtent2D sc_ext;
static VkImage sc_img[8];
static u32 sc_img_count;
static VkImageView sc_view[8];
static VkRenderPass rpB;
static VkFramebuffer fb[8];
static VkCommandPool cpool;
static VkCommandBuffer cmdbuf[8];
static VkSemaphore sem_acquire, sem_render;
static VkFence fence;

/* offscreen 8x8 */
static const u32 OFF_W = 100, OFF_H = 100;
static VkImage off_img;
static VkDeviceMemory off_mem;
static VkImageView off_view;
static VkSampler off_samp;
static VkRenderPass rpA;
static VkFramebuffer off_fb;

/* pipelines */
static VkPipelineLayout plA = VK_NULL_HANDLE, plB = VK_NULL_HANDLE;
static VkPipeline gpA = VK_NULL_HANDLE, gpB = VK_NULL_HANDLE;

/* descriptors for sampling off_img in pass B */
static VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
static VkDescriptorPool dpool = VK_NULL_HANDLE;
static VkDescriptorSet dset = VK_NULL_HANDLE;

/* --- file IO --- */
static void* read_file(const char* path, size_t* out_sz)
{
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        perror(path);
        return NULL;
    }
    fseek(f, 0,SEEK_END);
    long n = ftell(f);
    fseek(f, 0,SEEK_SET);
    void* p = malloc((size_t)n);
    if (!p)
    {
        fclose(f);
        return NULL;
    }
    if (fread(p, 1, (size_t)n, f) != (size_t)n)
    {
        fclose(f);
        free(p);
        return NULL;
    }
    fclose(f);
    *out_sz = (size_t)n;
    return p;
}

static VkShaderModule sm_from_file(const char* path)
{
    size_t sz = 0;
    void* bytes = read_file(path, &sz);
    if (!bytes)
    {
        fprintf(stderr, "missing %s\n", path);
        exit(1);
    }
    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sz, .pCode = (const u32*)bytes
    };
    VkShaderModule m;
    VKC(vkCreateShaderModule(dev,&ci,NULL,&m));
    free(bytes);
    return m;
}

/* --- Vulkan core --- */
static void vk_make_instance(void)
{
    VkApplicationInfo ai = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "tri2", .applicationVersion = 1,
        .pEngineName = "none", .engineVersion = 0, .apiVersion = VK_API_VERSION_1_1
    };
    const char* exts[] = {"VK_KHR_surface", "VK_KHR_wayland_surface"};
    VkInstanceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = exts
    };
    VKC(vkCreateInstance(&ci,NULL,&inst));
    TSTAMP("vkCreateInstance");
}

static void vk_make_surface(void)
{
    VkWaylandSurfaceCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, .display = dpy, .surface = surf
    };
    VKC(vkCreateWaylandSurfaceKHR(inst,&sci,NULL,&vsurf));
    TSTAMP("vkCreateWaylandSurfaceKHR");
}

static void vk_choose_phys_and_queue(void)
{
    u32 n = 0;
    VKC(vkEnumeratePhysicalDevices(inst,&n,NULL));
    VkPhysicalDevice list[8];
    if (n > 8)n = 8;
    VKC(vkEnumeratePhysicalDevices(inst,&n,list));
    phys = list[0];
    for (u32 i = 0; i < n; i++)
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(list[i], &p);
        if (p.vendorID == 0x8086)
        {
            phys = list[i];
            break;
        }
    }
    u32 qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn,NULL);
    VkQueueFamilyProperties qfp[16];
    if (qn > 16) qn = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qfp);
    qfam = ~0u;
    for (u32 i = 0; i < qn; i++)
    {
        VkBool32 pres = false;
        VKC(vkGetPhysicalDeviceSurfaceSupportKHR(phys,i,vsurf,&pres));
        if ((qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && pres)
        {
            qfam = i;
            break;
        }
    }
    if (qfam == ~0u)
    {
        fprintf(stderr, "no graphics+present queue\n");
        exit(1);
    }
    TSTAMP("pick phys+queue");
}

static void vk_make_device(void)
{
    float pr = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = 1,
        .pQueuePriorities = &pr
    };
    const char* exts[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = exts
    };
    VKC(vkCreateDevice(phys,&dci,NULL,&dev));
    vkGetDeviceQueue(dev, qfam, 0, &q);
    TSTAMP("vkCreateDevice");
}

static VkSurfaceFormatKHR choose_format(void)
{
    u32 n = 0;
    VKC_ENUM(vkGetPhysicalDeviceSurfaceFormatsKHR(phys,vsurf,&n,NULL));
    if (n == 0)
    {
        fprintf(stderr, "no formats\n");
        exit(1);
    }
    VkSurfaceFormatKHR* f = malloc(n * sizeof(*f));
    VKC_ENUM(vkGetPhysicalDeviceSurfaceFormatsKHR(phys,vsurf,&n,f));
    VkSurfaceFormatKHR pick = f[0];
    for (u32 i = 0; i < n; i++) if (f[i].format == VK_FORMAT_B8G8R8A8_UNORM && f[i].colorSpace ==
        VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
    {
        pick = f[i];
        break;
    }
    free(f);
    return pick;
}

static VkPresentModeKHR choose_present_mode(void)
{
    u32 n = 0;
    VKC_ENUM(vkGetPhysicalDeviceSurfacePresentModesKHR(phys,vsurf,&n,NULL));
    if (n == 0) return VK_PRESENT_MODE_FIFO_KHR;
    VkPresentModeKHR* m = malloc(n * sizeof(*m));
    VKC_ENUM(vkGetPhysicalDeviceSurfacePresentModesKHR(phys,vsurf,&n,m));
    VkPresentModeKHR pick = VK_PRESENT_MODE_FIFO_KHR;
    for (u32 i = 0; i < n; i++) if (m[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
    {
        pick = m[i];
        break;
    }
    free(m);
    return pick;
}

static VkCompositeAlphaFlagBitsKHR choose_alpha(const VkSurfaceCapabilitiesKHR caps)
{
    const VkCompositeAlphaFlagBitsKHR order[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
    };
    for (u32 i = 0; i < sizeof(order) / sizeof(order[0]); ++i) if (caps.supportedCompositeAlpha & order[i]) return order
        [i];
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

/* --- offscreen image (8x8 RGBA, color-attach + sampled) --- */
static u32 find_mem_type(u32 typeBits, VkMemoryPropertyFlags req)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; i++) if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & req)
        == req) return i;
    fprintf(stderr, "no memtype\n");
    exit(1);
}

static void make_offscreen(void)
{
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {OFF_W, OFF_H, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    VKC(vkCreateImage(dev,&ici,NULL,&off_img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, off_img, &mr);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    VKC(vkAllocateMemory(dev,&mai,NULL,&off_mem));
    VKC(vkBindImageMemory(dev,off_img,off_mem,0));
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = off_img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
    };
    VKC(vkCreateImageView(dev,&vci,NULL,&off_view));
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .minLod = 0.0f, .maxLod = 0.0f
    };
    VKC(vkCreateSampler(dev,&sci,NULL,&off_samp));
}

/* --- descriptor set for pass B --- */
static void make_descriptors(void)
{
    VkDescriptorSetLayoutBinding b0 = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
    };
    VkDescriptorSetLayoutCreateInfo dlci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b0
    };
    VKC(vkCreateDescriptorSetLayout(dev,&dlci,NULL,&dsl));
    VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1};
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps
    };
    VKC(vkCreateDescriptorPool(dev,&dpci,NULL,&dpool));
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1,
        .pSetLayouts = &dsl
    };
    VKC(vkAllocateDescriptorSets(dev,&dsai,&dset));
}

/* destroy everything tied to swapchain (and per-pass resources) */
static void vk_destroy_swapchain_all(void)
{
    if (dev == VK_NULL_HANDLE) return;
    if (fence)
    {
        vkDestroyFence(dev, fence,NULL);
        fence = VK_NULL_HANDLE;
    }
    if (sem_render)
    {
        vkDestroySemaphore(dev, sem_render,NULL);
        sem_render = VK_NULL_HANDLE;
    }
    if (sem_acquire)
    {
        vkDestroySemaphore(dev, sem_acquire,NULL);
        sem_acquire = VK_NULL_HANDLE;
    }
    if (cpool)
    {
        vkDestroyCommandPool(dev, cpool,NULL);
        cpool = VK_NULL_HANDLE;
    }

    if (gpB)
    {
        vkDestroyPipeline(dev, gpB,NULL);
        gpB = VK_NULL_HANDLE;
    }
    if (plB)
    {
        vkDestroyPipelineLayout(dev, plB,NULL);
        plB = VK_NULL_HANDLE;
    }
    if (gpA)
    {
        vkDestroyPipeline(dev, gpA,NULL);
        gpA = VK_NULL_HANDLE;
    }
    if (plA)
    {
        vkDestroyPipelineLayout(dev, plA,NULL);
        plA = VK_NULL_HANDLE;
    }

    for (u32 i = 0; i < sc_img_count; i++)
    {
        if (fb[i])
        {
            vkDestroyFramebuffer(dev, fb[i],NULL);
            fb[i] = VK_NULL_HANDLE;
        }
        if (sc_view[i])
        {
            vkDestroyImageView(dev, sc_view[i],NULL);
            sc_view[i] = VK_NULL_HANDLE;
        }
    }
    if (off_fb)
    {
        vkDestroyFramebuffer(dev, off_fb,NULL);
        off_fb = VK_NULL_HANDLE;
    }
    if (rpB)
    {
        vkDestroyRenderPass(dev, rpB,NULL);
        rpB = VK_NULL_HANDLE;
    }
    if (rpA)
    {
        vkDestroyRenderPass(dev, rpA,NULL);
        rpA = VK_NULL_HANDLE;
    }
    if (sc)
    {
        vkDestroySwapchainKHR(dev, sc,NULL);
        sc = VK_NULL_HANDLE;
    }

    if (dpool)
    {
        vkDestroyDescriptorPool(dev, dpool,NULL);
        dpool = VK_NULL_HANDLE;
    }
    if (dsl)
    {
        vkDestroyDescriptorSetLayout(dev, dsl,NULL);
        dsl = VK_NULL_HANDLE;
    }

    if (off_samp)
    {
        vkDestroySampler(dev, off_samp,NULL);
        off_samp = VK_NULL_HANDLE;
    }
    if (off_view)
    {
        vkDestroyImageView(dev, off_view,NULL);
        off_view = VK_NULL_HANDLE;
    }
    if (off_img)
    {
        vkDestroyImage(dev, off_img,NULL);
        off_img = VK_NULL_HANDLE;
    }
    if (off_mem)
    {
        vkFreeMemory(dev, off_mem,NULL);
        off_mem = VK_NULL_HANDLE;
    }
}

/* pipelines and passes */
static void make_passes_and_pipelines(VkFormat swap_fmt, VkExtent2D swap_ext)
{
    /* Pass A: offscreen render pass */
    VkAttachmentDescription aA = {
        .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };
    VkAttachmentReference arA = {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription spA = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &arA
    };
    VkRenderPassCreateInfo rpciA = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &aA,
        .subpassCount = 1, .pSubpasses = &spA
    };
    VKC(vkCreateRenderPass(dev,&rpciA,NULL,&rpA));

    /* Pass B: onscreen render pass */
    VkAttachmentDescription aB = {
        .format = swap_fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    };
    VkAttachmentReference arB = {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription spB = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &arB
    };
    VkRenderPassCreateInfo rpciB = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &aB,
        .subpassCount = 1, .pSubpasses = &spB
    };
    VKC(vkCreateRenderPass(dev,&rpciB,NULL,&rpB));

    /* pipeline A (triangle → offscreen) */
    VkShaderModule vA = sm_from_file("triA.vert.spv");
    VkShaderModule fA = sm_from_file("triA.frag.spv");
    VkPipelineShaderStageCreateInfo stA[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vA, .pName = "main"
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fA, .pName = "main"
        }
    };
    VkPipelineVertexInputStateCreateInfo vin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };
    VkPipelineViewportStateCreateInfo vpst = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1
    };
    VkDynamicState dyns[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyns
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.0f
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF, .blendEnable = VK_FALSE};
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba
    };
    VkPipelineLayoutCreateInfo plciA = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VKC(vkCreatePipelineLayout(dev,&plciA,NULL,&plA));
    VkGraphicsPipelineCreateInfo pciA = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stA,
        .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vpst, .pDynamicState = &dyn,
        .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb,
        .layout = plA, .renderPass = rpA, .subpass = 0
    };
    VKC(vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&pciA,NULL,&gpA));
    vkDestroyShaderModule(dev, vA,NULL);
    vkDestroyShaderModule(dev, fA,NULL);

    /* pipeline B (fullscreen sample → swapchain) */
    VkShaderModule vB = sm_from_file("triB.vert.spv");
    VkShaderModule fB = sm_from_file("triB.frag.spv");
    VkPipelineShaderStageCreateInfo stB[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vB, .pName = "main"
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fB, .pName = "main"
        }
    };
    VkPipelineLayoutCreateInfo plciB = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl
    };
    VKC(vkCreatePipelineLayout(dev,&plciB,NULL,&plB));
    VkGraphicsPipelineCreateInfo pciB = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stB,
        .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vpst, .pDynamicState = &dyn,
        .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb,
        .layout = plB, .renderPass = rpB, .subpass = 0
    };
    VKC(vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&pciB,NULL,&gpB));
    vkDestroyShaderModule(dev, vB,NULL);
    vkDestroyShaderModule(dev, fB,NULL);

    /* offscreen framebuffer */
    VkImageView off_att[] = {off_view};
    VkFramebufferCreateInfo fciA = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rpA, .attachmentCount = 1,
        .pAttachments = off_att, .width = OFF_W, .height = OFF_H, .layers = 1
    };
    VKC(vkCreateFramebuffer(dev,&fciA,NULL,&off_fb));

    /* swapchain framebuffers will be created later (need sc_view[]) */
}

/* build swapchain + per-frame CMDBUFs that record BOTH passes */
static void make_swapchain_and_record(u32 w, u32 h)
{
    VkSurfaceCapabilitiesKHR caps;
    VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys,vsurf,&caps));
    VkSurfaceFormatKHR sf = choose_format();
    sc_fmt = sf.format;
    VkPresentModeKHR pm = choose_present_mode();
    if (caps.currentExtent.width != UINT32_MAX) sc_ext = caps.currentExtent;
    else { sc_ext = (VkExtent2D){w, h}; }
    if (sc_ext.width == 0 || sc_ext.height == 0) return;
    u32 desired = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && desired > caps.maxImageCount) desired = caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = choose_alpha(caps);

    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = vsurf, .minImageCount = desired,
        .imageFormat = sc_fmt, .imageColorSpace = sf.colorSpace,
        .imageExtent = sc_ext, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = alpha, .presentMode = pm, .clipped = VK_TRUE, .oldSwapchain = sc
    };
    VKC(vkCreateSwapchainKHR(dev,&sci,NULL,&sc));
    if (sci.oldSwapchain) vkDestroySwapchainKHR(dev, sci.oldSwapchain,NULL);

    VKC(vkGetSwapchainImagesKHR(dev,sc,&sc_img_count,NULL));
    if (sc_img_count > 8) sc_img_count = 8;
    VKC(vkGetSwapchainImagesKHR(dev,sc,&sc_img_count,sc_img));
    for (u32 i = 0; i < sc_img_count; i++)
    {
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = sc_img[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = sc_fmt,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
        };
        VKC(vkCreateImageView(dev,&vci,NULL,&sc_view[i]));
    }

    /* swapchain render pass was created earlier; create fb[] now */
    for (u32 i = 0; i < sc_img_count; i++)
    {
        VkImageView atts[] = {sc_view[i]};
        VkFramebufferCreateInfo fciB = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rpB, .attachmentCount = 1,
            .pAttachments = atts, .width = sc_ext.width, .height = sc_ext.height, .layers = 1
        };
        VKC(vkCreateFramebuffer(dev,&fciB,NULL,&fb[i]));
    }

    /* command pool & buffers */
    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfam,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
    };
    VKC(vkCreateCommandPool(dev,&cpci,NULL,&cpool));
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = sc_img_count
    };
    VKC(vkAllocateCommandBuffers(dev,&cai,cmdbuf));

    /* write descriptor set for uTex (point at off_view+sampler) */
    VkDescriptorImageInfo dii = {
        .sampler = off_samp, .imageView = off_view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };
    VkWriteDescriptorSet w0 = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii
    };
    vkUpdateDescriptorSets(dev, 1, &w0, 0,NULL);

    /* record BOTH passes into each cmdbuf[i] */
    for (u32 i = 0; i < sc_img_count; i++)
    {
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VKC(vkBeginCommandBuffer(cmdbuf[i],&bi));

        /* Pass A: offscreen 8x8 */
        VkClearValue clrA = {.color = {{0.02f, 0.02f, 0.05f, 1.0f}}};
        VkRenderPassBeginInfo rbiA = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rpA, .framebuffer = off_fb,
            .renderArea = {{0, 0}, {OFF_W, OFF_H}}, .clearValueCount = 1, .pClearValues = &clrA
        };
        vkCmdBeginRenderPass(cmdbuf[i], &rbiA, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport vpA = {0, 0, (float)OFF_W, (float)OFF_H, 0, 1};
        VkRect2D scA = {{0, 0}, {OFF_W, OFF_H}};
        vkCmdSetViewport(cmdbuf[i], 0, 1, &vpA);
        vkCmdSetScissor(cmdbuf[i], 0, 1, &scA);
        vkCmdBindPipeline(cmdbuf[i], VK_PIPELINE_BIND_POINT_GRAPHICS, gpA);
        vkCmdDraw(cmdbuf[i], 3, 1, 0, 0);
        vkCmdEndRenderPass(cmdbuf[i]);

        /* Barrier: off_img COLOR_ATTACHMENT_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL is handled by rpA finalLayout,
           but we ensure visibility before pass B (no-op if driver handles subpass finalLayout fully). */

        /* Pass B: fullscreen sample into swapchain */
        VkClearValue clrB = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
        VkRenderPassBeginInfo rbiB = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rpB, .framebuffer = fb[i],
            .renderArea = {{0, 0}, {sc_ext.width, sc_ext.height}}, .clearValueCount = 1, .pClearValues = &clrB
        };
        vkCmdBeginRenderPass(cmdbuf[i], &rbiB, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport vpB = {0, 0, (float)sc_ext.width, (float)sc_ext.height, 0, 1};
        VkRect2D scB = {{0, 0}, {sc_ext.width, sc_ext.height}};
        vkCmdSetViewport(cmdbuf[i], 0, 1, &vpB);
        vkCmdSetScissor(cmdbuf[i], 0, 1, &scB);
        vkCmdBindPipeline(cmdbuf[i], VK_PIPELINE_BIND_POINT_GRAPHICS, gpB);
        vkCmdBindDescriptorSets(cmdbuf[i], VK_PIPELINE_BIND_POINT_GRAPHICS, plB, 0, 1, &dset, 0,NULL);
        vkCmdDraw(cmdbuf[i], 3, 1, 0, 0);
        vkCmdEndRenderPass(cmdbuf[i]);

        VKC(vkEndCommandBuffer(cmdbuf[i]));
    }

    /* sync objects */
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    VKC(vkCreateSemaphore(dev,&si,NULL,&sem_acquire));
    VKC(vkCreateSemaphore(dev,&si,NULL,&sem_render));
    VKC(vkCreateFence(dev,&fi,NULL,&fence));

    TSTAMP("swapchain+pipes+cmds");
}

static void recreate_all(void)
{
    vkDeviceWaitIdle(dev);
    vk_destroy_swapchain_all();
    /* re-create the pieces in order */
    make_offscreen();
    make_descriptors();
    make_passes_and_pipelines(sc_fmt, (VkExtent2D){(u32)win_w, (u32)win_h});
    /* sc_fmt may be uninit first time; fine, we set later */
    make_swapchain_and_record((u32)win_w, (u32)win_h);
}

/* present one frame */
static VkResult present_frame(void)
{
    u32 idx = 0;
    VkResult r;
    VKC(vkWaitForFences(dev,1,&fence,VK_TRUE,UINT64_MAX));
    VKC(vkResetFences(dev,1,&fence));
    r = vkAcquireNextImageKHR(dev, sc,UINT64_MAX, sem_acquire,VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return r;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
    {
        fprintf(stderr, "acquire err %d\n", r);
        exit(1);
    }
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem_acquire,
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 1, .pCommandBuffers = &cmdbuf[idx], .signalSemaphoreCount = 1,
        .pSignalSemaphores = &sem_render
    };
    VKC(vkQueueSubmit(q,1,&si,fence));
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem_render,
        .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &idx
    };
    r = vkQueuePresentKHR(q, &pi);
    return r;
}

int main(void)
{
    // /* Pin Intel Vulkan (avoid NVIDIA) if you need it */
    // setenv("VK_DRIVER_FILES", "/usr/share/vulkan/icd.d/intel_icd.x86_64.json", 1);
    // setenv("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/intel_icd.x86_64.json", 1);
    // unsetenv("VK_LAYER_PATH");
    // unsetenv("VK_ADD_LAYER_PATH");

    TINIT();
    dpy = wl_display_connect(NULL);
    if (!dpy)
    {
        fprintf(stderr, "wl connect failed\n");
        return 1;
    }
    TSTAMP("wl_display_connect");
    struct wl_registry* reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l,NULL);
    wl_display_roundtrip(dpy);
    TSTAMP("wl globals ready");
    if (!comp || !xdg)
    {
        fprintf(stderr, "missing compositor/xdg\n");
        return 1;
    }

    surf = wl_compositor_create_surface(comp);
    xsurf = xdg_wm_base_get_xdg_surface(xdg, surf);
    xdg_surface_add_listener(xsurf, &xsurf_l,NULL);
    xtop = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(xtop, &top_l,NULL);
    xdg_toplevel_set_title(xtop, "tri2");
    xdg_toplevel_set_app_id(xtop, "tri2");
    xdg_toplevel_set_fullscreen(xtop, NULL);
    wl_surface_commit(surf);
    TSTAMP("wl first commit");

    vk_make_instance();
    vk_make_surface();

    while (running && !need_swapchain) wl_display_dispatch(dpy); /* wait for initial size */
    vk_choose_phys_and_queue();
    vk_make_device();

    /* one-time creations + full graph */
    make_offscreen();
    make_descriptors();
    make_passes_and_pipelines(VK_FORMAT_B8G8R8A8_UNORM, (VkExtent2D){(u32)win_w, (u32)win_h});
    make_swapchain_and_record((u32)win_w, (u32)win_h);

    VkResult pr = present_frame();
    TSTAMP("first present");
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR)
    {
        recreate_all();
        present_frame();
    }

    while (running)
    {
        wl_display_dispatch(dpy);
        if (need_swapchain)
        {
            need_swapchain = false;
            vkDeviceWaitIdle(dev);
            vk_destroy_swapchain_all();
            make_offscreen();
            make_descriptors();
            make_passes_and_pipelines(sc_fmt, (VkExtent2D){(u32)win_w, (u32)win_h});
            make_swapchain_and_record((u32)win_w, (u32)win_h);
        }
        pr = present_frame();
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) { recreate_all(); }
    }

    vkDeviceWaitIdle(dev);
    vk_destroy_swapchain_all();
    if (dev) vkDestroyDevice(dev,NULL);
    if (vsurf) vkDestroySurfaceKHR(inst, vsurf,NULL);
    if (inst) vkDestroyInstance(inst,NULL);
    return 0;
}
