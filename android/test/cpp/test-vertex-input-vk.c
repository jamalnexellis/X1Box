/* Generated pixels for production-owned dynamic vertex input layouts. */
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include "hw/xbox/nv2a/pgraph/vk/vertex-input.h"
#include "hw/xbox/nv2a/pgraph/vk/pipeline-compile.h"

#define CHECK(x) do { VkResult r_=(x); if(r_!=VK_SUCCESS) { fprintf(stderr,"%s: %d\n",#x,r_); abort(); } } while(0)
#define N 16
#define W 64
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static VkRenderPass pass;
static VkPipelineLayout layout;
static VkShaderModule modules[3];
static int empty_input;
static bool unoptimized;
static VkImage image;
static VkFramebuffer framebuffer;
static VkBuffer vertices, readback;
static VkCommandPool pool;
static PFN_vkCmdSetVertexInputEXT set_input;
static VkPipeline static_switch;

static uint64_t ns(void) {
    struct timespec ts; assert(clock_gettime(CLOCK_MONOTONIC,&ts)==0);
    return (uint64_t)ts.tv_sec*1000000000+ts.tv_nsec;
}
static uint32_t memory_type(uint32_t bits,VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p; vkGetPhysicalDeviceMemoryProperties(physical,&p);
    for(uint32_t i=0;i<p.memoryTypeCount;i++) if((bits&(1u<<i)) && (p.memoryTypes[i].propertyFlags&flags)==flags) return i;
    abort();
}
static VkDeviceMemory allocate(VkMemoryRequirements m,VkMemoryPropertyFlags flags) {
    VkMemoryAllocateInfo info={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=m.size,.memoryTypeIndex=memory_type(m.memoryTypeBits,flags)};
    VkDeviceMemory memory; CHECK(vkAllocateMemory(device,&info,NULL,&memory)); return memory;
}
static VkDeviceMemory buffer(VkDeviceSize size,VkBufferUsageFlags usage,VkBuffer *out) {
    VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(device,&info,NULL,out)); VkMemoryRequirements m; vkGetBufferMemoryRequirements(device,*out,&m);
    VkDeviceMemory memory=allocate(m,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkBindBufferMemory(device,*out,memory,0)); return memory;
}
static VkShaderModule shader(const char *path) {
    FILE *f=fopen(path,"rb"); assert(f); assert(fseek(f,0,SEEK_END)==0); long n=ftell(f); assert(n>0 && n%4==0); rewind(f);
    uint32_t *code=malloc((size_t)n); assert(code && fread(code,1,(size_t)n,f)==(size_t)n); fclose(f);
    VkShaderModuleCreateInfo info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)n,.pCode=code};
    VkShaderModule module; CHECK(vkCreateShaderModule(device,&info,NULL,&module)); free(code); return module;
}
static void descriptions(unsigned i,VkVertexInputBindingDescription *binding,VkVertexInputAttributeDescription attr[2]) {
    uint32_t pos=(i%3)*4, color=pos+8;
    *binding=(VkVertexInputBindingDescription){.binding=0,.stride=color+(i%2?16:4)+(i%7)*4,.inputRate=VK_VERTEX_INPUT_RATE_VERTEX};
    attr[0]=(VkVertexInputAttributeDescription){.location=0,.binding=0,.format=VK_FORMAT_R32G32_SFLOAT,.offset=pos};
    attr[1]=(VkVertexInputAttributeDescription){.location=1,.binding=0,.format=i%2?VK_FORMAT_R32G32B32A32_SFLOAT:VK_FORMAT_R8G8B8A8_UNORM,.offset=color};
}
static VkPipeline pipeline(unsigned i,int dynamic,VkPipelineCache cache,uint64_t *create_ns,unsigned *hits) {
    VkVertexInputBindingDescription binding; VkVertexInputAttributeDescription attr[2]; descriptions(i,&binding,attr);
    VkPipelineVertexInputStateCreateInfo input={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,.vertexBindingDescriptionCount=empty_input?0:1,.pVertexBindingDescriptions=&binding,.vertexAttributeDescriptionCount=empty_input?0:2,.pVertexAttributeDescriptions=attr};
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=modules[empty_input?2:0],.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=modules[1],.pName="main"}};
    VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport={.width=W,.height=W,.maxDepth=1}; VkRect2D scissor={.extent={W,W}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState attachment={.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment};
    VkDynamicState state=VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
    VkPipelineDynamicStateCreateInfo dyn={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=1,.pDynamicStates=&state};
    VkPipelineCreationFeedback feedback={0};
    VkPipelineCreationFeedbackCreateInfo feedback_info={.sType=VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,.pPipelineCreationFeedback=&feedback};
    VkGraphicsPipelineCreateInfo info={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=&feedback_info,.flags=nv2a_vk_draw_pipeline_flags(unoptimized),.stageCount=2,.pStages=stages,.pVertexInputState=dynamic?NULL:&input,.pInputAssemblyState=&assembly,.pViewportState=&vp,.pRasterizationState=&raster,.pMultisampleState=&ms,.pColorBlendState=&blend,.pDynamicState=dynamic?&dyn:NULL,.layout=layout,.renderPass=pass};
    VkPipeline out; uint64_t before=ns(); CHECK(vkCreateGraphicsPipelines(device,cache,1,&info,NULL,&out)); *create_ns+=ns()-before;
    if(feedback.flags&VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) (*hits)++;
    return out;
}
static uint64_t draw(VkPipeline pipeline_value,unsigned i,int dynamic,void *pixels) {
    VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(device,&ai,&cmd));
    uint64_t before=ns(); VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; CHECK(vkBeginCommandBuffer(cmd,&bi));
    VkClearValue clear={.color={{1,0,0,1}}};
    VkRenderPassBeginInfo begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=framebuffer,.renderArea={.extent={W,W}},.clearValueCount=1,.pClearValues=&clear};
    vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE); if(dynamic) vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,static_switch);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_value);
    if(dynamic) {
        VkVertexInputBindingDescription binding; VkVertexInputAttributeDescription attr[2]; descriptions(i,&binding,attr);
        NV2AVkVertexInput snapshot;
        vk_vertex_input_snapshot(&snapshot,empty_input?0:1,empty_input?NULL:&binding,empty_input?0:2,empty_input?NULL:attr);
        /* The live layout changes before this deferred entry is emitted. */
        memset(&binding,0xcc,sizeof(binding));memset(attr,0xdd,sizeof(attr));
        vk_vertex_input_emit(set_input,cmd,&snapshot);
    }
    VkDeviceSize offset=i*256; vkCmdBindVertexBuffers(cmd,0,1,&vertices,&offset); vkCmdDraw(cmd,3,1,0,0); vkCmdEndRenderPass(cmd);
    VkBufferImageCopy copy={.imageSubresource={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.layerCount=1},.imageExtent={W,W,1}};
    vkCmdCopyImageToBuffer(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
    VkBufferMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=readback,.size=VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&barrier,0,NULL);
    CHECK(vkEndCommandBuffer(cmd)); VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; CHECK(vkCreateFence(device,&fi,NULL,&fence));
    CHECK(vkQueueSubmit(queue,1,&submit,fence)); CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX)); uint64_t elapsed=ns()-before;
    const uint8_t *p=pixels; for(unsigned j=0;j<W*W;j++) assert(p[j*4]==0 && p[j*4+1]==255 && p[j*4+2]==0 && p[j*4+3]==255);
    vkDestroyFence(device,fence,NULL); vkFreeCommandBuffers(device,pool,1,&cmd); return elapsed;
}
int main(int argc,char **argv) {
    assert(argc==4 || argc==5);
    if(argc==5) {assert(!strcmp(argv[4],"0") || !strcmp(argv[4],"1"));unoptimized=!strcmp(argv[4],"1");}
    printf("pipeline_fast_compile=%d\n",unoptimized);
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="x1box-generated-input-layouts",.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ic={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app}; VkInstance instance; CHECK(vkCreateInstance(&ic,NULL,&instance));
    uint32_t n=1; CHECK(vkEnumeratePhysicalDevices(instance,&n,&physical)); assert(n);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(physical,&props); printf("GPU %s\n",props.deviceName);
    uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&qn,NULL); VkQueueFamilyProperties *qp=calloc(qn,sizeof(*qp)); assert(qp); vkGetPhysicalDeviceQueueFamilyProperties(physical,&qn,qp);
    uint32_t family=UINT32_MAX; for(uint32_t i=0;i<qn;i++) if(qp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){family=i;break;} free(qp); assert(family!=UINT32_MAX);
    float priority=1; VkDeviceQueueCreateInfo qc={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT feature={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT,.vertexInputDynamicState=VK_TRUE};
    const char *ext[]={VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME};
    VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&feature,.queueCreateInfoCount=1,.pQueueCreateInfos=&qc,.enabledExtensionCount=1,.ppEnabledExtensionNames=ext};
    CHECK(vkCreateDevice(physical,&dc,NULL,&device)); vkGetDeviceQueue(device,family,0,&queue);
    set_input=(PFN_vkCmdSetVertexInputEXT)vkGetDeviceProcAddr(device,"vkCmdSetVertexInputEXT"); assert(set_input);
    VkCommandPoolCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family}; CHECK(vkCreateCommandPool(device,&pc,NULL,&pool));
    modules[0]=shader(argv[1]); modules[1]=shader(argv[2]); modules[2]=shader(argv[3]); VkPipelineLayoutCreateInfo lc={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; CHECK(vkCreatePipelineLayout(device,&lc,NULL,&layout));
    VkAttachmentDescription attachment={.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}; VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&ref};
    VkSubpassDependency dep={.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT};
    VkRenderPassCreateInfo rc={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass,.dependencyCount=1,.pDependencies=&dep}; CHECK(vkCreateRenderPass(device,&rc,NULL,&pass));
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,.extent={W,W,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE}; CHECK(vkCreateImage(device,&ii,NULL,&image));
    VkMemoryRequirements im; vkGetImageMemoryRequirements(device,image,&im); VkDeviceMemory image_mem=allocate(im,0); CHECK(vkBindImageMemory(device,image,image_mem,0));
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=ii.format,.subresourceRange={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=1,.layerCount=1}}; VkImageView view; CHECK(vkCreateImageView(device,&vi,NULL,&view));
    VkFramebufferCreateInfo fc={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,.attachmentCount=1,.pAttachments=&view,.width=W,.height=W,.layers=1}; CHECK(vkCreateFramebuffer(device,&fc,NULL,&framebuffer));
    VkDeviceMemory vm=buffer(N*256,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,&vertices), rm=buffer(W*W*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,&readback); void *vertex_data,*pixels; CHECK(vkMapMemory(device,vm,0,VK_WHOLE_SIZE,0,&vertex_data)); CHECK(vkMapMemory(device,rm,0,VK_WHOLE_SIZE,0,&pixels)); memset(vertex_data,0,N*256);
    float positions[3][2]={{-1,-1},{3,-1},{-1,3}}, color[4]={0,1,0,1}; uint8_t packed[4]={0,255,0,255};
    for(unsigned i=0;i<N;i++) {VkVertexInputBindingDescription b; VkVertexInputAttributeDescription a[2];descriptions(i,&b,a);
        for(unsigned j=0;j<3;j++) {uint8_t *p=(uint8_t*)vertex_data+i*256+j*b.stride;memcpy(p+a[0].offset,positions[j],8);memcpy(p+a[1].offset,i%2?(void*)color:(void*)packed,i%2?16:4);}}
    for(unsigned run=0;run<4;run++) {int dynamic=run==1||run==2; VkPipelineCacheCreateInfo ci={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO}; VkPipelineCache cache; CHECK(vkCreatePipelineCache(device,&ci,NULL,&cache));
        uint64_t create_ns=0,draw_ns=0;unsigned hits=0;
        uint64_t ignored=0;unsigned ignored_hits=0;
        static_switch=pipeline(0,0,cache,&ignored,&ignored_hits); VkPipeline values[N]={0};
        for(unsigned i=0;i<(dynamic?1:N);i++) values[i]=pipeline(i,dynamic,cache,&create_ns,&hits);
        for(unsigned i=0;i<N;i++) draw_ns+=draw(values[dynamic?0:i],i,dynamic,pixels);
        printf("run=%u dynamic=%d pipelines=%u create_ms=%.3f record_submit_wait_ms=%.3f application_cache_hits=%u exact_pixels=%u\n",run,dynamic,dynamic?1:N,create_ns/1e6,draw_ns/1e6,hits,N*W*W);
        for(unsigned i=0;i<(dynamic?1:N);i++) vkDestroyPipeline(device,values[i],NULL);vkDestroyPipeline(device,static_switch,NULL);vkDestroyPipelineCache(device,cache,NULL);
    }
    empty_input=1;
    VkPipelineCacheCreateInfo empty_ci={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache empty_cache;CHECK(vkCreatePipelineCache(device,&empty_ci,NULL,&empty_cache));
    uint64_t ignored=0;unsigned ignored_hits=0;
    static_switch=pipeline(0,0,empty_cache,&ignored,&ignored_hits);
    VkPipeline empty_pipeline=pipeline(0,1,empty_cache,&ignored,&ignored_hits);
    draw(static_switch,0,0,pixels);draw(empty_pipeline,0,1,pixels);
    vkDestroyPipeline(device,empty_pipeline,NULL);vkDestroyPipeline(device,static_switch,NULL);vkDestroyPipelineCache(device,empty_cache,NULL);
    puts("PASS zero-input static/dynamic draws: 8192 exact RGBA pixels");
    vkUnmapMemory(device,vm);vkUnmapMemory(device,rm);vkDestroyBuffer(device,vertices,NULL);vkDestroyBuffer(device,readback,NULL);vkFreeMemory(device,vm,NULL);vkFreeMemory(device,rm,NULL);
    vkDestroyFramebuffer(device,framebuffer,NULL);vkDestroyImageView(device,view,NULL);vkDestroyImage(device,image,NULL);vkFreeMemory(device,image_mem,NULL);vkDestroyRenderPass(device,pass,NULL);vkDestroyPipelineLayout(device,layout,NULL);vkDestroyShaderModule(device,modules[0],NULL);vkDestroyShaderModule(device,modules[1],NULL);vkDestroyShaderModule(device,modules[2],NULL);vkDestroyCommandPool(device,pool,NULL);vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);
    puts("PASS generated pixels; synthetic creation/draw timings are not PGR2 FPS");return 0;
}
