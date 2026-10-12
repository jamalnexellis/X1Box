/* Generated pixels: fenced direct/staged vertex/index/indirect/uniform data. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw/xbox/nv2a/pgraph/vk/draw-upload.h"

#define CHECK(x) do { VkResult result_=(x); if(result_!=VK_SUCCESS) { fprintf(stderr,"%s: %d\n",#x,result_); abort(); } } while(0)
#define W 32u
#define N 32u
#define ROWS 16u
static VkDevice device;
static VkPhysicalDevice physical;
static unsigned validation_errors;
static VKAPI_ATTR VkBool32 VKAPI_CALL validation(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
    (void)severity; (void)type; (void)user;
    fprintf(stderr,"Vulkan validation: %s\n",data->pMessage); validation_errors++;
    return VK_FALSE;
}

static VkDeviceMemory memory(VkMemoryRequirements req, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties props; vkGetPhysicalDeviceMemoryProperties(physical,&props);
    unsigned i; for(i=0;i<props.memoryTypeCount;i++)
        if((req.memoryTypeBits&(1u<<i)) && (props.memoryTypes[i].propertyFlags&flags)==flags) break;
    assert(i<props.memoryTypeCount);
    VkMemoryAllocateInfo info={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=i};
    VkDeviceMemory out; CHECK(vkAllocateMemory(device,&info,NULL,&out)); return out;
}
static VkDeviceMemory buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *out)
{
    VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(device,&info,NULL,out)); VkMemoryRequirements req; vkGetBufferMemoryRequirements(device,*out,&req);
    VkDeviceMemory mem=memory(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    CHECK(vkBindBufferMemory(device,*out,mem,0)); return mem;
}
static VkShaderModule shader(const char *path)
{
    FILE *file=fopen(path,"rb"); assert(file); assert(!fseek(file,0,SEEK_END)); long size=ftell(file); assert(size>0 && !(size%4)); rewind(file);
    uint32_t *code=malloc((size_t)size); assert(code); assert(fread(code,1,(size_t)size,file)==(size_t)size); fclose(file);
    VkShaderModuleCreateInfo info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)size,.pCode=code};
    VkShaderModule out; CHECK(vkCreateShaderModule(device,&info,NULL,&out)); free(code); return out;
}
static unsigned value(unsigned which, unsigned row, unsigned component)
{
    return (which*17+row*13+component*5)%61;
}
static VkPipeline pipeline(VkRenderPass pass, VkPipelineLayout layout, VkShaderModule vert, VkShaderModule frag)
{
    VkPipelineShaderStageCreateInfo stages[2]={{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vert,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=frag,.pName="main"}};
    VkVertexInputBindingDescription binding={0,24,VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2]={{0,0,VK_FORMAT_R32G32_SFLOAT,0},{1,0,VK_FORMAT_R32G32B32A32_UINT,8}};
    VkPipelineVertexInputStateCreateInfo input={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,.vertexBindingDescriptionCount=1,.pVertexBindingDescriptions=&binding,.vertexAttributeDescriptionCount=2,.pVertexAttributeDescriptions=attrs};
    VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport={.width=W,.height=W,.minDepth=0,.maxDepth=1}; VkRect2D scissor={.extent={W,W}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState attachment={.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment};
    VkGraphicsPipelineCreateInfo info={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,.pVertexInputState=&input,
        .pInputAssemblyState=&assembly,.pViewportState=&vp,.pRasterizationState=&raster,.pMultisampleState=&ms,.pColorBlendState=&blend,.layout=layout,.renderPass=pass};
    VkPipeline out; CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&info,NULL,&out)); return out;
}
int main(int argc,char **argv)
{
    assert(argc==3);
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="x1box-generated-fenced-draw-upload",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ic={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    uint32_t layer_count=0; CHECK(vkEnumerateInstanceLayerProperties(&layer_count,NULL));
    VkLayerProperties *layers=calloc(layer_count?layer_count:1,sizeof(*layers)); assert(layers);
    CHECK(vkEnumerateInstanceLayerProperties(&layer_count,layers)); bool validate=false;
    for(unsigned i=0;i<layer_count;i++) if(!strcmp(layers[i].layerName,"VK_LAYER_KHRONOS_validation")) validate=true;
    free(layers);
    const char *layer="VK_LAYER_KHRONOS_validation",*extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT debug={.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,.pfnUserCallback=validation};
    VkValidationFeatureEnableEXT sync=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT features={.sType=VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,.pNext=&debug,.enabledValidationFeatureCount=1,.pEnabledValidationFeatures=&sync};
    if(validate){ic.enabledLayerCount=1;ic.ppEnabledLayerNames=&layer;ic.enabledExtensionCount=1;ic.ppEnabledExtensionNames=&extension;ic.pNext=&features;}
    VkInstance instance; CHECK(vkCreateInstance(&ic,NULL,&instance)); uint32_t count=1; CHECK(vkEnumeratePhysicalDevices(instance,&count,&physical)); assert(count);
    VkDebugUtilsMessengerEXT messenger=VK_NULL_HANDLE;
    if(validate){PFN_vkCreateDebugUtilsMessengerEXT create=(PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT");assert(create);CHECK(create(instance,&debug,NULL,&messenger));}
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(physical,&props); printf("GPU %s\n",props.deviceName);
    VkDeviceSize stride=props.limits.minUniformBufferOffsetAlignment; if(stride<16) stride=16; assert(stride*ROWS<=UINT32_MAX);
    uint32_t qcount=0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&qcount,NULL); VkQueueFamilyProperties *qp=calloc(qcount,sizeof(*qp)); assert(qp);
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&qcount,qp); uint32_t family=UINT32_MAX;
    for(unsigned i=0;i<qcount;i++) {
        if(qp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){family=i;break;}
    }
    free(qp); assert(family!=UINT32_MAX);
    float priority=1; VkDeviceQueueCreateInfo qc={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qc}; CHECK(vkCreateDevice(physical,&dc,NULL,&device));
    VkQueue queue; vkGetDeviceQueue(device,family,0,&queue);
    VkDescriptorSetLayoutBinding bindings[2]={{.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_VERTEX_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT}};
    VkDescriptorSetLayoutCreateInfo sl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
    VkDescriptorSetLayout set_layout; CHECK(vkCreateDescriptorSetLayout(device,&sl,NULL,&set_layout));
    VkPipelineLayout layouts[2]; VkPushConstantRange range={.stageFlags=VK_SHADER_STAGE_VERTEX_BIT,.size=4};
    for(unsigned i=0;i<2;i++){VkPipelineLayoutCreateInfo lc={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout,.pushConstantRangeCount=i,.pPushConstantRanges=i?&range:NULL};CHECK(vkCreatePipelineLayout(device,&lc,NULL,&layouts[i]));}
    VkDescriptorPoolSize pool_size={VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,6};
    VkDescriptorPoolCreateInfo pc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=3,.poolSizeCount=1,.pPoolSizes=&pool_size};
    VkDescriptorPool descriptor_pool; CHECK(vkCreateDescriptorPool(device,&pc,NULL,&descriptor_pool));
    VkDescriptorSetLayout alloc_layouts[3]={set_layout,set_layout,set_layout}; VkDescriptorSetAllocateInfo sa={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=descriptor_pool,.descriptorSetCount=3,.pSetLayouts=alloc_layouts};
    VkDescriptorSet sets[3]; CHECK(vkAllocateDescriptorSets(device,&sa,sets)); VkBuffer readback;
    enum { TYPES=3, SLOTS=3 };
    VkBuffer uploads[SLOTS][TYPES], destinations[SLOTS][TYPES];
    VkDeviceMemory upload_memory[SLOTS][TYPES], destination_memory[SLOTS][TYPES];
    void *mapped[SLOTS][TYPES];
    VkDeviceSize sizes[TYPES]={36,144,stride+16};
    for(unsigned slot=0;slot<SLOTS;slot++)for(unsigned type=0;type<TYPES;type++) {
        VkBufferUsageFlags draw=nv2a_vk_draw_upload_usage((NV2AVkDrawUpload)type);
        upload_memory[slot][type]=buffer(sizes[type],draw|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,&uploads[slot][type]);
        destination_memory[slot][type]=buffer(sizes[type],draw|VK_BUFFER_USAGE_TRANSFER_DST_BIT,&destinations[slot][type]);
        CHECK(vkMapMemory(device,upload_memory[slot][type],0,VK_WHOLE_SIZE,0,&mapped[slot][type]));
    }
    VkDeviceMemory rm=buffer(N*W*W*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,&readback); void *pixels; CHECK(vkMapMemory(device,rm,0,VK_WHOLE_SIZE,0,&pixels));
    VkAttachmentDescription attachment={.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}; VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&ref};
    VkSubpassDependency deps[2]={{.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT,.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
        {.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT}};
    VkRenderPassCreateInfo rc={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass,.dependencyCount=2,.pDependencies=deps};
    VkRenderPass pass; CHECK(vkCreateRenderPass(device,&rc,NULL,&pass));
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=attachment.format,.extent={W,W,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkImage image; CHECK(vkCreateImage(device,&ii,NULL,&image)); VkMemoryRequirements req; vkGetImageMemoryRequirements(device,image,&req); VkDeviceMemory im=memory(req,0); CHECK(vkBindImageMemory(device,image,im,0));
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=ii.format,.subresourceRange={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=1,.layerCount=1}};
    VkImageView view; CHECK(vkCreateImageView(device,&vi,NULL,&view)); VkFramebufferCreateInfo fc={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,.attachmentCount=1,.pAttachments=&view,.width=W,.height=W,.layers=1};
    VkFramebuffer framebuffer; CHECK(vkCreateFramebuffer(device,&fc,NULL,&framebuffer)); VkShaderModule vert=shader(argv[1]), frag=shader(argv[2]);
    VkPipeline pipelines[2]={pipeline(pass,layouts[0],vert,frag),pipeline(pass,layouts[1],vert,frag)};
    VkCommandPoolCreateInfo cp={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};
    VkCommandPool command_pool; CHECK(vkCreateCommandPool(device,&cp,NULL,&command_pool));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=command_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=SLOTS};
    VkCommandBuffer cmds[SLOTS]; CHECK(vkAllocateCommandBuffers(device,&ca,cmds));
    VkFence fences[SLOTS]; VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for(unsigned slot=0;slot<SLOTS;slot++) CHECK(vkCreateFence(device,&fi,NULL,&fences[slot]));
    unsigned exact_pixels=0;
    for(unsigned direct=0;direct<=1;direct++)for(unsigned slots=1;slots<=3;slots++) {
        bool in_flight[SLOTS]={0};
        for(unsigned slot=0;slot<slots;slot++) {
            VkBuffer selected=direct?uploads[slot][NV2A_VK_UPLOAD_UNIFORM]:destinations[slot][NV2A_VK_UPLOAD_UNIFORM];
            VkDescriptorBufferInfo infos[2]={{selected,0,16},{selected,0,16}};
            VkWriteDescriptorSet writes[2]={{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=sets[slot],.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,.pBufferInfo=&infos[0]},
                {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=sets[slot],.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,.pBufferInfo=&infos[1]}};
            vkUpdateDescriptorSets(device,2,writes,0,NULL);
        }
        for(unsigned pass_index=0;pass_index<N;pass_index++) {
            unsigned slot=pass_index%slots;
            if(in_flight[slot]) CHECK(vkWaitForFences(device,1,&fences[slot],VK_TRUE,UINT64_MAX));
            CHECK(vkResetFences(device,1,&fences[slot])); CHECK(vkResetCommandBuffer(cmds[slot],0));
            unsigned variant=pass_index%2;
            uint16_t indices[3]={(uint16_t)(variant*3),(uint16_t)(variant*3+1),(uint16_t)(variant*3+2)};
            memcpy(mapped[slot][NV2A_VK_UPLOAD_INDEX],indices,sizeof(indices));
            VkDrawIndexedIndirectCommand indirect={.indexCount=3,.instanceCount=1};
            memcpy((char*)mapped[slot][NV2A_VK_UPLOAD_INDEX]+16,&indirect,sizeof(indirect));
            for(unsigned which=0;which<2;which++)for(unsigned point=0;point<3;point++) {
                char *vertex=(char*)mapped[slot][NV2A_VK_UPLOAD_VERTEX]+(which*3+point)*24;
                float pos[2]={point==1?3.f:-1.f,point==2?3.f:-1.f}; memcpy(vertex,pos,8);
                for(unsigned c=0;c<4;c++) {uint32_t color=value(pass_index,which,c);memcpy(vertex+8+c*4,&color,4);}
            }
            for(unsigned c=0;c<4;c++) {
                uint32_t a=value(5,pass_index,c),b=value(7,pass_index,c);
                memcpy((char*)mapped[slot][NV2A_VK_UPLOAD_UNIFORM]+c*4,&a,4);
                memcpy((char*)mapped[slot][NV2A_VK_UPLOAD_UNIFORM]+stride+c*4,&b,4);
            }
            for(unsigned type=0;type<TYPES;type++) {
                VkMappedMemoryRange flush={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=upload_memory[slot][type],.size=VK_WHOLE_SIZE};
                CHECK(vkFlushMappedMemoryRanges(device,1,&flush));
            }
            VkCommandBuffer cmd=cmds[slot];
            VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd,&begin));
            for(unsigned type=0;type<TYPES;type++) {
                if(direct) nv2a_vk_draw_upload_barrier(cmd,uploads[slot][type],sizes[type],(NV2AVkDrawUpload)type);
                else {
                    VkBufferMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=uploads[slot][type],.size=sizes[type]};
                    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,1,&host,0,NULL);
                    VkBufferCopy copy={.size=sizes[type]}; vkCmdCopyBuffer(cmd,uploads[slot][type],destinations[slot][type],1,&copy);
                    VkBufferMemoryBarrier gpu={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=destinations[slot][type],.size=sizes[type]};
                    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,1,&gpu,0,NULL);
                }
            }
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelines[0]);
            uint32_t offsets[2]={0,(uint32_t)stride}; vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layouts[0],0,1,&sets[slot],2,offsets);
            VkBuffer vertex=direct?uploads[slot][NV2A_VK_UPLOAD_VERTEX]:destinations[slot][NV2A_VK_UPLOAD_VERTEX]; VkDeviceSize offset=0;
            VkBuffer index=direct?uploads[slot][NV2A_VK_UPLOAD_INDEX]:destinations[slot][NV2A_VK_UPLOAD_INDEX];
            vkCmdBindVertexBuffers(cmd,0,1,&vertex,&offset); vkCmdBindIndexBuffer(cmd,index,0,VK_INDEX_TYPE_UINT16);
            VkClearValue clear={.color={{1,0,1,1}}}; VkRenderPassBeginInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=framebuffer,.renderArea={.extent={W,W}},.clearValueCount=1,.pClearValues=&clear};
            vkCmdBeginRenderPass(cmd,&rp,VK_SUBPASS_CONTENTS_INLINE);
            if(pass_index%3) vkCmdDrawIndexed(cmd,3,1,0,0,0); else vkCmdDrawIndexedIndirect(cmd,index,16,1,sizeof(indirect));
            vkCmdEndRenderPass(cmd);
            VkBufferImageCopy copy={.bufferOffset=pass_index*W*W*4,.imageSubresource={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.layerCount=1},.imageExtent={W,W,1}};
            vkCmdCopyImageToBuffer(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
            VkBufferMemoryBarrier read={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=readback,.offset=copy.bufferOffset,.size=W*W*4};
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&read,0,NULL); CHECK(vkEndCommandBuffer(cmd));
            VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd}; CHECK(vkQueueSubmit(queue,1,&submit,fences[slot]));
            in_flight[slot]=true;
        }
        for(unsigned slot=0;slot<slots;slot++) CHECK(vkWaitForFences(device,1,&fences[slot],VK_TRUE,UINT64_MAX));
        VkMappedMemoryRange invalidate={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=rm,.size=VK_WHOLE_SIZE}; CHECK(vkInvalidateMappedMemoryRanges(device,1,&invalidate));
        for(unsigned i=0;i<N;i++)for(unsigned p=0;p<W*W;p++)for(unsigned c=0;c<4;c++)
            assert(((uint8_t*)pixels)[(i*W*W+p)*4+c]==value(i,i%2,c)+value(5,i,c)+value(7,i,c));
        exact_pixels+=N*W*W; printf("PASS direct=%u slots=%u: %u exact RGBA pixels, indexed/indirect draws and both shader uniform stages\n",direct,slots,N*W*W);
    }
    for(unsigned slot=0;slot<SLOTS;slot++) vkDestroyFence(device,fences[slot],NULL);
    vkDestroyCommandPool(device,command_pool,NULL); vkDestroyPipeline(device,pipelines[0],NULL); vkDestroyPipeline(device,pipelines[1],NULL);
    vkDestroyShaderModule(device,vert,NULL); vkDestroyShaderModule(device,frag,NULL); vkDestroyFramebuffer(device,framebuffer,NULL); vkDestroyImageView(device,view,NULL);
    vkDestroyImage(device,image,NULL); vkFreeMemory(device,im,NULL); vkDestroyRenderPass(device,pass,NULL); vkUnmapMemory(device,rm); vkDestroyBuffer(device,readback,NULL); vkFreeMemory(device,rm,NULL);
    vkDestroyDescriptorPool(device,descriptor_pool,NULL);
    for(unsigned slot=0;slot<SLOTS;slot++)for(unsigned type=0;type<TYPES;type++) {
        vkUnmapMemory(device,upload_memory[slot][type]); vkDestroyBuffer(device,uploads[slot][type],NULL); vkFreeMemory(device,upload_memory[slot][type],NULL);
        vkDestroyBuffer(device,destinations[slot][type],NULL);vkFreeMemory(device,destination_memory[slot][type],NULL);
    }
    for(unsigned i=0;i<2;i++) vkDestroyPipelineLayout(device,layouts[i],NULL);
    vkDestroyDescriptorSetLayout(device,set_layout,NULL); vkDestroyDevice(device,NULL);
    assert(!validation_errors);
    printf("Synchronization validation enabled=%d errors=%u\n",validate,validation_errors);
    if(messenger){PFN_vkDestroyDebugUtilsMessengerEXT destroy=(PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT");assert(destroy);destroy(instance,messenger,NULL);}
    vkDestroyInstance(instance,NULL);
    printf("PASS fenced direct draw upload: %u exact pixels, generated data only; not game FPS\n",exact_pixels);
    return 0;
}
