// build (example): cc x11_wgpu.c -o x11_wgpu -lX11 -lwgpu_native
#include "header.h"
#include "wgpu.h" // todo: conditionally only include webgpu.h for wasm/web

#include <X11/Xlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {WIDTH = 800, HEIGHT = 600, DISCRETE_GPU = 0, HDR = 0, MAX_VERTICES=3*256, MAX_INSTANCES=3*256};
const char *BACKENDS[] = {"NA","NULL","WebGPU","D3D11","D3D12","Metal","Vulkan","OpenGL","GLES"};

void print_adapter_info(WGPUAdapter adapter) {WGPUAdapterInfo info={0};wgpuAdapterGetInfo(adapter,&info);}
void on_device(WGPURequestDeviceStatus s, WGPUDevice d, WGPUStringView m, void* ud1, void* ud2){*(WGPUDevice*)ud1=d;}
void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter a, WGPUStringView m, void* ud1, void* ud2) {
  *(WGPUAdapter*)ud1=a;
  WGPUAdapterInfo info={0};wgpuAdapterGetInfo(a,&info);
  printf("GPU: %s | BACKEND %s\n",info.device.data,BACKENDS[info.backendType]);
}
void my_error_cb(WGPUErrorType type, const char* message, void* user_data){fprintf(stderr,"WebGPU Error [%d]: %s\n",type,message);}
static inline WGPUStringView WGPUSTRING(const char* s){return (WGPUStringView){ .data = s, .length = strlen(s)};}
WGPUBuffer mkbuf(const char*label,WGPUDevice d,i64 s,WGPUBufferUsage u){WGPUBufferDescriptor desc={.size=s,.label=WGPUSTRING(label),.usage=u};return wgpuDeviceCreateBuffer(d,&desc);}

void key_input_callback(void* ud, enum KEYBOARD_BUTTON key, enum BUTTON_STATE state)
{
  if (key == KEYBOARD_ESCAPE) {_exit(0);}
}
void mouse_input_callback(void* ud, i32 x, i32 y, enum MOUSE_BUTTON button, enum BUTTON_STATE state)
{
}

int main(void){
  pf_time_reset();
  WINDOW w = pf_create_window(NULL, key_input_callback,mouse_input_callback);
  pf_timestamp("X window created");

  // set env
  if (!DISCRETE_GPU) { // todo: && __linux__
    putenv((char*)"VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json");
    putenv((char*)"VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json");
    putenv("VK_INSTANCE_LAYERS=");        // empty disables implicit layers
    putenv("VK_LOADER_LAYERS_ENABLE=");   // empty disables loader scanning layers
    putenv("VK_LOADER_DEBUG=");           // make sure no loader logging slows things
    putenv("WGPU_BACKEND=vk");              // force vulkan
    putenv("WGPU_ADAPTER_NAME=intel");      // string match against adapter name
    putenv("WGPU_POWER_PREF=low");          // or "high" if you want discrete
    putenv("WGPU_DEBUG=0");
    putenv("WGPU_VALIDATION=0");
    putenv("WGPU_GPU_BASED_VALIDATION=0");
  }

  /* ---------- INSTANCE & SURFACE (X11) ---------- */
  WGPUInstance ins=wgpuCreateInstance(&(WGPUInstanceDescriptor){.nextInChain=(const WGPUChainedStruct*)&(WGPUInstanceExtras){
    .backends=WGPUInstanceBackend_Vulkan,
    .flags=0,.gles3MinorVersion=WGPUGles3MinorVersion_Version0}});
  pf_timestamp("Instance created");
  WGPUSurfaceSourceXlibWindow xsd = {
    .chain.sType = WGPUSType_SurfaceSourceXlibWindow,
    .display = pf_display_or_instance(w), .window = (u64) pf_surface_or_hwnd(w)
  };
  WGPUSurfaceDescriptor sdesc = {.nextInChain = &xsd.chain};
  WGPUSurface surface = wgpuInstanceCreateSurface(ins, &sdesc);
  pf_timestamp("Surface created");

  /* ---------- ADAPTER & DEVICE ---------- */
  WGPUAdapter adapter=NULL;
  WGPURequestAdapterOptions aopt = { .compatibleSurface = surface };
  wgpuInstanceRequestAdapter(ins, &aopt, (WGPURequestAdapterCallbackInfo){.callback = on_adapter, .userdata1 = &adapter});
  while(!adapter){wgpuInstanceProcessEvents(ins);}
  WGPUDevice dev=NULL; WGPUDeviceDescriptor ddesc={};
  WGPURequestDeviceCallbackInfo cinfo = {.callback = on_device, .userdata1 = &dev};
  wgpuAdapterRequestDevice(adapter,&ddesc,cinfo); while(!dev){wgpuInstanceProcessEvents(ins);}
  WGPUQueue q=wgpuDeviceGetQueue(dev);
  pf_timestamp("Device created");

  /* ---------- SURFACE CONFIG ---------- */
  WGPUTextureFormat fmt= HDR?WGPUTextureFormat_RGBA16Float:WGPUTextureFormat_BGRA8UnormSrgb; // HDR uncommon on X11; keep off by default
  WGPUSurfaceConfiguration cfg={.device=dev,.format=fmt,.usage=WGPUTextureUsage_RenderAttachment,
    .width=(u32)pf_window_width(w),.height=(u32)pf_window_height(w),.presentMode=WGPUPresentMode_Fifo};
  wgpuSurfaceConfigure(surface,&cfg);
  pf_timestamp("Surface configured");

  /* ---------- mesh data ---------- */
  f32 vertices[6]={ 0.0f,0.5f,  -0.5f,-0.5f,  0.5f,-0.5f };
  struct {f32 x,y;} instances[]={{-0.5f,-0.5f},{0.5f,-0.5f},{-0.5f,0.5f},{0.5f,0.5f}};

  /* ---------- GPU buffers ---------- */
  WGPUBuffer VERTICES=mkbuf("vertices",dev,sizeof vertices,WGPUBufferUsage_Storage|WGPUBufferUsage_CopyDst);
  WGPUBuffer INSTANCES=mkbuf("instances", dev,sizeof instances,WGPUBufferUsage_Storage|WGPUBufferUsage_CopyDst);
  WGPUBuffer VISIBLE = mkbuf("visible",dev,256*sizeof(i32),WGPUBufferUsage_Storage);
  WGPUBuffer COUNTERS = mkbuf("counters",dev,5*sizeof(i32),WGPUBufferUsage_Storage|WGPUBufferUsage_Indirect|WGPUBufferUsage_CopyDst);
  WGPUBuffer VARYINGS = mkbuf("varyings",dev,MAX_VERTICES*sizeof(f32)*2,WGPUBufferUsage_Storage|WGPUBufferUsage_Vertex);
  WGPUBuffer INDICES  = mkbuf("indices",dev,MAX_INSTANCES*sizeof(i16)*2,WGPUBufferUsage_Storage|WGPUBufferUsage_Index);
  WGPUBuffer DISPATCH = mkbuf("dispatch",dev,3*sizeof(u32),WGPUBufferUsage_Storage|WGPUBufferUsage_Indirect);
  pf_timestamp("Buffers created");

  /* ---------- load shader ---------- */
  FILE*f=fopen("shaders.wgsl","rb"); if(!f){fprintf(stderr,":(\n");return 1;}
  fseek(f,0,SEEK_END); i64 sz=ftell(f); rewind(f); char*src=malloc(sz+1); fread(src,1,sz,f); src[sz]=0; fclose(f);
  WGPUShaderModule sm=wgpuDeviceCreateShaderModule(dev,&(WGPUShaderModuleDescriptor){
    .nextInChain=&(WGPUShaderSourceWGSL){.chain.sType = WGPUSType_ShaderSourceWGSL, .code=WGPUSTRING(src)}.chain, .label = "wgsl"}); free(src);
  pf_timestamp("Shader module created");

  /* ---------- bind group ---------- */
  WGPUBindGroupEntry be[7]={{NULL,0,INSTANCES,0,sizeof instances,NULL,NULL},{NULL,1,VISIBLE,0,256*4,NULL,NULL},
    {NULL,2,COUNTERS,0,sizeof(i32)*5,NULL,NULL},{NULL,3,VERTICES,0,sizeof vertices,NULL,NULL},
    {NULL,4,VARYINGS,0,MAX_VERTICES*8,NULL,NULL},{NULL,5,INDICES,0,MAX_INSTANCES*4,NULL,NULL},
    {NULL,6,DISPATCH,0,3*sizeof(u32),NULL,NULL}};
  #define C_STORAGE(x)  {NULL,(x),WGPUShaderStage_Compute,{.type=WGPUBufferBindingType_Storage},{0},{0},{0}}
  #define C_READONLY(x) {NULL,(x),WGPUShaderStage_Compute,{.type=WGPUBufferBindingType_ReadOnlyStorage},{0},{0},{0}}
  WGPUBindGroupLayoutEntry l[7]={C_READONLY(0),C_STORAGE(1),C_STORAGE(2),C_READONLY(3),C_STORAGE(4),C_STORAGE(5),C_STORAGE(6)};
  WGPUBindGroupLayout bgl=wgpuDeviceCreateBindGroupLayout(dev,&(WGPUBindGroupLayoutDescriptor){.entries=l,.entryCount=7});
  WGPUBindGroup bg=wgpuDeviceCreateBindGroup(dev,&(WGPUBindGroupDescriptor){.layout=bgl,.entries=be,.entryCount=7});
  pf_timestamp("Bindgroups created");

  /* ---------- pipelines ---------- */
  WGPUPipelineLayout pl=wgpuDeviceCreatePipelineLayout(dev,&(WGPUPipelineLayoutDescriptor){.bindGroupLayouts=&bgl,.bindGroupLayoutCount=1});
  WGPUComputePipeline cp1=wgpuDeviceCreateComputePipeline(dev,&(WGPUComputePipelineDescriptor){.layout=pl,.compute={.module=sm,.entryPoint=WGPUSTRING("cs_instance")}});
  WGPUComputePipeline cp2=wgpuDeviceCreateComputePipeline(dev,&(WGPUComputePipelineDescriptor){.layout=pl,.compute={.module=sm,.entryPoint=WGPUSTRING("cs_prepare")}});
  WGPUComputePipeline cp3=wgpuDeviceCreateComputePipeline(dev,&(WGPUComputePipelineDescriptor){.layout=pl,.compute={.module=sm,.entryPoint=WGPUSTRING("cs_meshlet")}});
  WGPUVertexAttribute vat={.format=WGPUVertexFormat_Float32x2,.offset=0,.shaderLocation=0};
  WGPUVertexBufferLayout vbl={.arrayStride=8,.attributeCount=1,.attributes=&vat,.stepMode=WGPUVertexStepMode_Vertex};
  WGPURenderPipeline rp=wgpuDeviceCreateRenderPipeline(dev,&(WGPURenderPipelineDescriptor){
    .layout=wgpuDeviceCreatePipelineLayout(dev,&(WGPUPipelineLayoutDescriptor){0}),
    .vertex={.module=sm,.entryPoint=WGPUSTRING("vs_main"),.buffers=&vbl,.bufferCount=1},
    .primitive={.topology=WGPUPrimitiveTopology_TriangleList},
    .multisample={.count=1,.mask=0xFFFFFFFF,.alphaToCoverageEnabled=0},
    .fragment=&(WGPUFragmentState){.module=sm,.entryPoint=WGPUSTRING("fs_main"),.targetCount=1,
      .targets=&(WGPUColorTargetState){.format=fmt,.writeMask=WGPUColorWriteMask_All}}
  });
  pf_timestamp("Pipelines created");

  /* ---------- event/render loop ---------- */
  wgpuQueueWriteBuffer(q,VERTICES,0,vertices,sizeof vertices);
  wgpuQueueWriteBuffer(q,INSTANCES,0,&instances,sizeof instances);
  while(pf_poll_events(w)){
    u32 zero[5]={0,0,0,0,0}; wgpuQueueWriteBuffer(q,COUNTERS,0,zero,sizeof zero);
    WGPUCommandEncoder enc=wgpuDeviceCreateCommandEncoder(dev,NULL);

    WGPUComputePassEncoder cpe=wgpuCommandEncoderBeginComputePass(enc,NULL);
    wgpuComputePassEncoderSetPipeline(cpe,cp1); wgpuComputePassEncoderSetBindGroup(cpe,0,bg,0,NULL); wgpuComputePassEncoderDispatchWorkgroups(cpe,1,1,1);
    wgpuComputePassEncoderSetPipeline(cpe,cp2); wgpuComputePassEncoderDispatchWorkgroups(cpe,1,1,1);
    wgpuComputePassEncoderSetPipeline(cpe,cp3); wgpuComputePassEncoderDispatchWorkgroupsIndirect(cpe,DISPATCH,0);
    wgpuComputePassEncoderEnd(cpe); wgpuComputePassEncoderRelease(cpe);

    WGPUSurfaceTexture st; wgpuSurfaceGetCurrentTexture(surface,&st);
    if(st.status!=1&&st.status!=2){wgpuSurfaceConfigure(surface,&cfg); continue;}
    WGPUTextureView tv=wgpuTextureCreateView(st.texture,NULL);
    WGPURenderPassColorAttachment ca={.view=tv,.loadOp=WGPULoadOp_Clear,.storeOp=WGPUStoreOp_Store,.clearValue={0,0,0,1}};
    WGPURenderPassEncoder rpe=wgpuCommandEncoderBeginRenderPass(enc,&(WGPURenderPassDescriptor){.colorAttachmentCount=1,.colorAttachments=&ca});
    wgpuRenderPassEncoderSetPipeline(rpe,rp);
    wgpuRenderPassEncoderSetVertexBuffer(rpe,0,VARYINGS,0,MAX_VERTICES*8);
    wgpuRenderPassEncoderSetIndexBuffer(rpe,INDICES,WGPUIndexFormat_Uint32,0,MAX_INSTANCES*4);
    wgpuRenderPassEncoderDrawIndexedIndirect(rpe,COUNTERS,0);
    wgpuRenderPassEncoderEnd(rpe); wgpuRenderPassEncoderRelease(rpe);

    WGPUCommandBuffer cb=wgpuCommandEncoderFinish(enc,NULL);
    wgpuQueueSubmit(q,1,&cb); wgpuCommandBufferRelease(cb);
    wgpuSurfacePresent(surface); wgpuTextureViewRelease(tv);
    pf_timestamp("Submit frame");
  }
  return 0;
}
