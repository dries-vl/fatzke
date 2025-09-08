#include "header.h"
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_CHECK(x) do{ VkResult _r=(x); if(_r!=VK_SUCCESS){ fprintf(stderr,"VK_ERR %d @%s:%d\n",(int)_r,__FILE__,__LINE__); exit(1);} }while(0)

struct VKC{
    VkInstance inst; VkPhysicalDevice phys; u32 qfam; VkDevice dev; VkQueue q;
    PFN_vkGetSemaphoreFdKHR GetSemaphoreFdKHR;

    /* imported KMS images */
    u32 img_n;
    struct { u32 w,h; u64 mod; VkImage img; VkDeviceMemory mem; VkSemaphore done_sem; } sc[4];

    /* texture + sampler */
    VkImage tex; VkDeviceMemory tex_mem; VkImageView tex_view; VkSampler tex_samp;

    /* pipeline to draw full-screen textured triangle into sc[i] */
    VkDescriptorSetLayout dsl; VkDescriptorPool dpool; VkDescriptorSet dset;
    VkRenderPass rp; VkPipelineLayout pl; VkPipeline gp;

    VkCommandPool pool;
} g;

static u32 find_mem_type(u32 typeBits, VkMemoryPropertyFlags req){
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g.phys,&mp);
    for(u32 i=0;i<mp.memoryTypeCount;i++) if((typeBits&(1u<<i)) && ((mp.memoryTypes[i].propertyFlags&req)==req)) return i;
    fprintf(stderr,"no memory type\n"); exit(1);
}

/* -------- init / device -------- */
void vk_init_instance(void){
    VkApplicationInfo ai={ .sType=VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName="direct-vk", .apiVersion=VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ci={ .sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo=&ai };
    VK_CHECK(vkCreateInstance(&ci,NULL,&g.inst));
}

void vk_pick_phys_and_queue(void){
    u32 n=0; VK_CHECK(vkEnumeratePhysicalDevices(g.inst,&n,NULL)); if(!n){fprintf(stderr,"no GPUs\n"); exit(1);}
    VkPhysicalDevice list[8]; if(n>8)n=8; VK_CHECK(vkEnumeratePhysicalDevices(g.inst,&n,list)); g.phys=list[0];
    u32 qn=0; vkGetPhysicalDeviceQueueFamilyProperties(g.phys,&qn,NULL);
    VkQueueFamilyProperties qfp[32]; if(qn>32) qn=32; vkGetPhysicalDeviceQueueFamilyProperties(g.phys,&qn,qfp);
    g.qfam=~0u; for(u32 i=0;i<qn;i++) if(qfp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){ g.qfam=i; break; }
    if(g.qfam==~0u){ fprintf(stderr,"no gfx queue\n"); exit(1); }
}

static int dev_ext_avail(const char* name){
    u32 n=0; vkEnumerateDeviceExtensionProperties(g.phys,NULL,&n,NULL); VkExtensionProperties e[128]; if(n>128)n=128;
    vkEnumerateDeviceExtensionProperties(g.phys,NULL,&n,e); for(u32 i=0;i<n;i++) if(strcmp(e[i].extensionName,name)==0) return 1; return 0;
}

void vk_create_device(void){
    float pr=1.0f; VkDeviceQueueCreateInfo qci={ .sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex=g.qfam, .queueCount=1, .pQueuePriorities=&pr };
    const char* exts[8]; u32 xn=0;
    if(!dev_ext_avail("VK_KHR_external_memory_fd")||!dev_ext_avail("VK_KHR_external_semaphore_fd")){
        fprintf(stderr,"driver lacks external fd extensions\n"); exit(1);
    }
    exts[xn++]="VK_KHR_external_memory_fd";
    exts[xn++]="VK_KHR_external_semaphore_fd";
    if(dev_ext_avail("VK_EXT_image_drm_format_modifier")) exts[xn++]="VK_EXT_image_drm_format_modifier";

    VkDeviceCreateInfo dci={ .sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount=1, .pQueueCreateInfos=&qci, .enabledExtensionCount=xn, .ppEnabledExtensionNames=exts };
    VK_CHECK(vkCreateDevice(g.phys,&dci,NULL,&g.dev)); vkGetDeviceQueue(g.dev,g.qfam,0,&g.q);
    g.GetSemaphoreFdKHR=(PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(g.dev,"vkGetSemaphoreFdKHR");
    VkCommandPoolCreateInfo cpci={ .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex=g.qfam, .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    VK_CHECK(vkCreateCommandPool(g.dev,&cpci,NULL,&g.pool));
}

/* -------- ASTC KTX2 upload (embedded) -------- */
struct KTX2Header{ u8 id[12]; u32 vkFormat, typeSize, w,h,d; u32 layers, faces, levels, superc; u32 dfdOfs, dfdLen; u32 kvdOfs,kvdLen; u64 sgdOfs,sgdLen; };
struct KTX2Level{ u64 ofs,len,ulen; };
static void upload_astc_from_embed(void){
    const u8* file=(const u8*)font_atlas; size_t len=(size_t)(font_atlas_end-font_atlas);
    if(len<sizeof(struct KTX2Header)){ fprintf(stderr,"ktx2 too small\n"); exit(1); }
    struct KTX2Header hdr; memcpy(&hdr,file,sizeof hdr);
    static const u8 magic[12]={0xAB,'K','T','X',' ','2','0',0xBB,0x0D,0x0A,0x1A,0x0A};
    if(memcmp(hdr.id,magic,12)!=0){ fprintf(stderr,"not KTX2\n"); exit(1); }
    VkFormat fmt=(VkFormat)hdr.vkFormat;
    if(!(fmt==VK_FORMAT_ASTC_12x12_UNORM_BLOCK || fmt==VK_FORMAT_ASTC_12x12_SRGB_BLOCK)){ fprintf(stderr,"need ASTC 12x12\n"); exit(1); }
    const u32 mips=hdr.levels?hdr.levels:1;
    const struct KTX2Level* lv=(const struct KTX2Level*)(file+sizeof(struct KTX2Header));

    VkImageCreateInfo ici={ .sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType=VK_IMAGE_TYPE_2D, .format=fmt,
        .extent={hdr.w,hdr.h,1}, .mipLevels=mips, .arrayLayers=1, .samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL, .usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT };
    VK_CHECK(vkCreateImage(g.dev,&ici,NULL,&g.tex));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g.dev,g.tex,&mr);
    VkMemoryAllocateInfo mai={ .sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize=mr.size, .memoryTypeIndex=find_mem_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VK_CHECK(vkAllocateMemory(g.dev,&mai,NULL,&g.tex_mem)); VK_CHECK(vkBindImageMemory(g.dev,g.tex,g.tex_mem,0));

    size_t total=0; for(u32 i=0;i<mips;i++) total+=(size_t)lv[i].len;
    VkBuffer stg; VkDeviceMemory stg_mem;
    { VkBufferCreateInfo bci={ .sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size=total, .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
      VK_CHECK(vkCreateBuffer(g.dev,&bci,NULL,&stg));
      VkMemoryRequirements mr2; vkGetBufferMemoryRequirements(g.dev,stg,&mr2);
      VkMemoryAllocateInfo am={ .sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize=mr2.size, .memoryTypeIndex=find_mem_type(mr2.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
      VK_CHECK(vkAllocateMemory(g.dev,&am,NULL,&stg_mem)); VK_CHECK(vkBindBufferMemory(g.dev,stg,stg_mem,0)); }

    size_t* offs=(size_t*)malloc(mips*sizeof(size_t)); size_t off=0; void* map=0; VK_CHECK(vkMapMemory(g.dev,stg_mem,0,total,0,&map));
    for(u32 i=0;i<mips;i++){ offs[i]=off; memcpy((u8*)map+off, file+(size_t)lv[i].ofs, (size_t)lv[i].len); off+=(size_t)lv[i].len; }
    vkUnmapMemory(g.dev,stg_mem);

    VkCommandBuffer cb; { VkCommandBufferAllocateInfo ai={ .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool=g.pool, .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount=1 }; VK_CHECK(vkAllocateCommandBuffers(g.dev,&ai,&cb)); }
    VkCommandBufferBeginInfo bi={ .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }; VK_CHECK(vkBeginCommandBuffer(cb,&bi));
    VkImageMemoryBarrier to_dst={ .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED, .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .image=g.tex, .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,mips,0,1} };
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&to_dst);
    u32 w=hdr.w,h=hdr.h;
    for(u32 i=0;i<mips;i++){
        VkBufferImageCopy bic={ .bufferOffset=offs[i], .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,i,0,1}, .imageExtent={ w?w:1, h?h:1, 1 } };
        vkCmdCopyBufferToImage(cb,stg,g.tex,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&bic);
        if(w>1) w>>=1; if(h>1) h>>=1;
    }
    VkImageMemoryBarrier to_ro={ .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask=VK_ACCESS_SHADER_READ_BIT, .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .image=g.tex, .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,mips,0,1} };
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,NULL,0,NULL,1,&to_ro);
    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si={ .sType=VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount=1, .pCommandBuffers=&cb };
    VK_CHECK(vkQueueSubmit(g.q,1,&si,VK_NULL_HANDLE)); VK_CHECK(vkQueueWaitIdle(g.q));
    vkFreeCommandBuffers(g.dev,g.pool,1,&cb);
    vkDestroyBuffer(g.dev,stg,NULL); vkFreeMemory(g.dev,stg_mem,NULL); free(offs);

    VkImageViewCreateInfo vci={ .sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image=g.tex, .viewType=VK_IMAGE_VIEW_TYPE_2D, .format=fmt, .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,mips,0,1} };
    VK_CHECK(vkCreateImageView(g.dev,&vci,NULL,&g.tex_view));
    VkSamplerCreateInfo sci={ .sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter=VK_FILTER_LINEAR, .minFilter=VK_FILTER_LINEAR, .mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR, .addressModeU=VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeV=VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT, .maxLod=(f32)(mips-1) };
    VK_CHECK(vkCreateSampler(g.dev,&sci,NULL,&g.tex_samp));
}

/* -------- pipeline (full-screen tri) -------- */
static VkShaderModule make_shader(const void* p, size_t n){ VkShaderModuleCreateInfo ci={ .sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize=n, .pCode=(const u32*)p }; VkShaderModule m; VK_CHECK(vkCreateShaderModule(g.dev,&ci,NULL,&m)); return m; }

static void build_pipe_for_format(VkFormat fmt){
    VkAttachmentDescription ad={ .format=fmt, .samples=VK_SAMPLE_COUNT_1_BIT, .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=VK_ATTACHMENT_STORE_OP_STORE, .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout=VK_IMAGE_LAYOUT_GENERAL };
    VkAttachmentReference ar={ .attachment=0, .layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp={ .pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount=1, .pColorAttachments=&ar };
    VkRenderPassCreateInfo rpci={ .sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount=1, .pAttachments=&ad, .subpassCount=1, .pSubpasses=&sp };
    VK_CHECK(vkCreateRenderPass(g.dev,&rpci,NULL,&g.rp));

    VkDescriptorSetLayoutBinding b[2]={
        { .binding=0, .descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding=1, .descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER,       .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT }
    };
    VkDescriptorSetLayoutCreateInfo dlci={ .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount=2, .pBindings=b };
    VK_CHECK(vkCreateDescriptorSetLayout(g.dev,&dlci,NULL,&g.dsl));
    VkDescriptorPoolSize ps[2]={ {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1}, {VK_DESCRIPTOR_TYPE_SAMPLER,1} };
    VkDescriptorPoolCreateInfo dpci={ .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets=1, .poolSizeCount=2, .pPoolSizes=ps };
    VK_CHECK(vkCreateDescriptorPool(g.dev,&dpci,NULL,&g.dpool));
    VkDescriptorSetAllocateInfo dsai={ .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool=g.dpool, .descriptorSetCount=1, .pSetLayouts=&g.dsl };
    VK_CHECK(vkAllocateDescriptorSets(g.dev,&dsai,&g.dset));
    VkDescriptorImageInfo ii={ .imageView=g.tex_view, .imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo si={ .sampler=g.tex_samp };
    VkWriteDescriptorSet W[2]={
        { .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet=g.dset, .dstBinding=0, .descriptorCount=1, .descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo=&ii },
        { .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet=g.dset, .dstBinding=1, .descriptorCount=1, .descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER,       .pImageInfo=&si }
    };
    vkUpdateDescriptorSets(g.dev,2,W,0,NULL);

    VkShaderModule mod=make_shader(shaders,(size_t)(shaders_end - shaders));
    VkPipelineShaderStageCreateInfo st[2]={
        { .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage=VK_SHADER_STAGE_VERTEX_BIT,   .module=mod, .pName="VS_Blit" },
        { .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage=VK_SHADER_STAGE_FRAGMENT_BIT, .module=mod, .pName="PS_Blit" }
    };
    VkPipelineLayoutCreateInfo plci={ .sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount=1, .pSetLayouts=&g.dsl };
    VK_CHECK(vkCreatePipelineLayout(g.dev,&plci,NULL,&g.pl));
    VkPipelineVertexInputStateCreateInfo vi={ .sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia={ .sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp={ .sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount=1, .scissorCount=1 };
    VkDynamicState dyns[2]={ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn={ .sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount=2, .pDynamicStates=dyns };
    VkPipelineRasterizationStateCreateInfo rs={ .sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode=VK_POLYGON_MODE_FILL, .cullMode=VK_CULL_MODE_NONE, .frontFace=VK_FRONT_FACE_CLOCKWISE, .lineWidth=1.0f };
    VkPipelineMultisampleStateCreateInfo ms={ .sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba={ .colorWriteMask=0xF, .blendEnable=VK_FALSE };
    VkPipelineColorBlendStateCreateInfo cb={ .sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount=1, .pAttachments=&cba };
    VkGraphicsPipelineCreateInfo pci={ .sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount=2, .pStages=st, .pVertexInputState=&vi, .pInputAssemblyState=&ia, .pViewportState=&vp, .pDynamicState=&dyn, .pRasterizationState=&rs, .pMultisampleState=&ms, .pColorBlendState=&cb, .layout=g.pl, .renderPass=g.rp, .subpass=0 };
    VK_CHECK(vkCreateGraphicsPipelines(g.dev,VK_NULL_HANDLE,1,&pci,NULL,&g.gp));
    vkDestroyShaderModule(g.dev,mod,NULL);
}

/* adopt GBM scanout images and build sampler/pipeline */
void vk_adopt_scanout_images(const struct ScanoutImage* imgs, u32 count){
    if(count>4) count=4; g.img_n=count;
    for(u32 i=0;i<count;i++){
        const u32 W=imgs[i].width, H=imgs[i].height; const u64 MOD=imgs[i].modifier;
        g.sc[i].w=W; g.sc[i].h=H; g.sc[i].mod=MOD;
        VkExternalMemoryImageCreateInfo extimg={ .sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
        VkSubresourceLayout plane={ .offset=0, .rowPitch=imgs[i].stride };
        VkImageDrmFormatModifierExplicitCreateInfoEXT modinfo={ .sType=VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, .drmFormatModifier=MOD, .drmFormatModifierPlaneCount=1, .pPlaneLayouts=&plane };
        extimg.pNext=&modinfo;
        VkImageCreateInfo ici={ .sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext=&extimg, .imageType=VK_IMAGE_TYPE_2D, .format=VK_FORMAT_B8G8R8A8_UNORM, .extent={W,H,1}, .mipLevels=1, .arrayLayers=1, .samples=VK_SAMPLE_COUNT_1_BIT, .tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT, .sharingMode=VK_SHARING_MODE_EXCLUSIVE, .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED };
        VK_CHECK(vkCreateImage(g.dev,&ici,NULL,&g.sc[i].img));
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(g.dev,g.sc[i].img,&mr);
        u32 memType=find_mem_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkImportMemoryFdInfoKHR imp={ .sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd=imgs[i].dma_fd };
        VkMemoryDedicatedAllocateInfo dai={ .sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image=g.sc[i].img };
        dai.pNext=&imp;
        VkMemoryAllocateInfo mai={ .sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize=mr.size, .memoryTypeIndex=memType, .pNext=&dai };
        VK_CHECK(vkAllocateMemory(g.dev,&mai,NULL,&g.sc[i].mem)); VK_CHECK(vkBindImageMemory(g.dev,g.sc[i].img,g.sc[i].mem,0));
        VkExportSemaphoreCreateInfo ex={ .sType=VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, .handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT };
        VkSemaphoreCreateInfo sci={ .sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext=&ex }; VK_CHECK(vkCreateSemaphore(g.dev,&sci,NULL,&g.sc[i].done_sem));
    }
    upload_astc_from_embed();
    build_pipe_for_format(VK_FORMAT_B8G8R8A8_UNORM);
}

/* draw and export sync-fd for KMS IN_FENCE_FD */
int vk_draw_and_export_sync(u32 i){
    if(i>=g.img_n) i%=g.img_n;
    VkImage target=g.sc[i].img; const u32 W=g.sc[i].w, H=g.sc[i].h;

    VkCommandBuffer cb; { VkCommandBufferAllocateInfo ai={ .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool=g.pool, .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount=1 }; VK_CHECK(vkAllocateCommandBuffers(g.dev,&ai,&cb)); }
    VkCommandBufferBeginInfo bi={ .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }; VK_CHECK(vkBeginCommandBuffer(cb,&bi));

    VkImageMemoryBarrier to_ca={ .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED, .newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .image=target, .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1} };
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,NULL,0,NULL,1,&to_ca);

    VkImageView view; { VkImageViewCreateInfo vci={ .sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image=target, .viewType=VK_IMAGE_VIEW_TYPE_2D, .format=VK_FORMAT_B8G8R8A8_UNORM, .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1} }; VK_CHECK(vkCreateImageView(g.dev,&vci,NULL,&view)); }
    VkFramebuffer fb; { VkImageView atts[]={view}; VkFramebufferCreateInfo fci={ .sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass=g.rp, .attachmentCount=1, .pAttachments=atts, .width=W, .height=H, .layers=1 }; VK_CHECK(vkCreateFramebuffer(g.dev,&fci,NULL,&fb)); }

    VkClearValue cv={ .color={{0,0,0,1}} };
    VkRenderPassBeginInfo rbi={ .sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass=g.rp, .framebuffer=fb, .renderArea={{0,0},{W,H}}, .clearValueCount=1, .pClearValues=&cv };
    vkCmdBeginRenderPass(cb,&rbi,VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp={0,0,(float)W,(float)H,0,1}; VkRect2D sc={{0,0},{W,H}};
    vkCmdSetViewport(cb,0,1,&vp); vkCmdSetScissor(cb,0,1,&sc);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,g.gp);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,g.pl,0,1,&g.dset,0,NULL);
    vkCmdDraw(cb,3,1,0,0);
    vkCmdEndRenderPass(cb);

    VkImageMemoryBarrier to_gen={ .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .newLayout=VK_IMAGE_LAYOUT_GENERAL, .image=target, .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1} };
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&to_gen);
    VK_CHECK(vkEndCommandBuffer(cb));

    VkSubmitInfo si={ .sType=VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount=1, .pCommandBuffers=&cb, .signalSemaphoreCount=1, .pSignalSemaphores=&g.sc[i].done_sem };
    VK_CHECK(vkQueueSubmit(g.q,1,&si,VK_NULL_HANDLE));

    int sync_fd=-1; VkSemaphoreGetFdInfoKHR gi={ .sType=VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore=g.sc[i].done_sem, .handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT };
    VK_CHECK(g.GetSemaphoreFdKHR(g.dev,&gi,&sync_fd));

    vkDestroyFramebuffer(g.dev,fb,NULL); vkDestroyImageView(g.dev,view,NULL);
    vkFreeCommandBuffers(g.dev,g.pool,1,&cb);
    return sync_fd;
}

/* -------- teardown -------- */
void vk_wait_idle(void){ if(g.dev) vkDeviceWaitIdle(g.dev); }
void vk_shutdown(void){
    if(g.dev){
        if(g.gp) vkDestroyPipeline(g.dev,g.gp,NULL);
        if(g.pl) vkDestroyPipelineLayout(g.dev,g.pl,NULL);
        if(g.rp) vkDestroyRenderPass(g.dev,g.rp,NULL);
        if(g.dpool) vkDestroyDescriptorPool(g.dev,g.dpool,NULL);
        if(g.dsl) vkDestroyDescriptorSetLayout(g.dev,g.dsl,NULL);
        if(g.tex_samp) vkDestroySampler(g.dev,g.tex_samp,NULL);
        if(g.tex_view) vkDestroyImageView(g.dev,g.tex_view,NULL);
        if(g.tex) vkDestroyImage(g.dev,g.tex,NULL);
        if(g.tex_mem) vkFreeMemory(g.dev,g.tex_mem,NULL);
        for(u32 i=0;i<g.img_n;i++){
            if(g.sc[i].done_sem) vkDestroySemaphore(g.dev,g.sc[i].done_sem,NULL);
            if(g.sc[i].img) vkDestroyImage(g.dev,g.sc[i].img,NULL);
            if(g.sc[i].mem) vkFreeMemory(g.dev,g.sc[i].mem,NULL);
        }
        if(g.pool) vkDestroyCommandPool(g.dev,g.pool,NULL);
        vkDestroyDevice(g.dev,NULL);
    }
    if(g.inst) vkDestroyInstance(g.inst,NULL);
    memset(&g,0,sizeof(g));
}
