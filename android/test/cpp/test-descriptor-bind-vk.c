/* Generated RGBA readbacks through owned descriptor-command state. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw/xbox/nv2a/pgraph/vk/descriptor-bind.h"

#define CHECK(x) do { VkResult result_=(x); if(result_!=VK_SUCCESS) { fprintf(stderr,"%s: %d\n",#x,result_); abort(); } } while(0)
#define W 32u
#define N 32u
#define ROWS 16u
static VkDevice device;
static VkPhysicalDevice physical;

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
    VkDeviceMemory mem=memory(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
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
    VkPipelineVertexInputStateCreateInfo input={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
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
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="x1box-generated-descriptor-binds",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ic={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance; CHECK(vkCreateInstance(&ic,NULL,&instance)); uint32_t count=1; CHECK(vkEnumeratePhysicalDevices(instance,&count,&physical)); assert(count);
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
    VkDescriptorSetLayoutBinding bindings[2]={{.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT}};
    VkDescriptorSetLayoutCreateInfo sl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
    VkDescriptorSetLayout set_layout; CHECK(vkCreateDescriptorSetLayout(device,&sl,NULL,&set_layout));
    VkPipelineLayout layouts[2]; VkPushConstantRange range={.stageFlags=VK_SHADER_STAGE_VERTEX_BIT,.size=4};
    for(unsigned i=0;i<2;i++){VkPipelineLayoutCreateInfo lc={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout,.pushConstantRangeCount=i,.pPushConstantRanges=i?&range:NULL};CHECK(vkCreatePipelineLayout(device,&lc,NULL,&layouts[i]));}
    VkDescriptorPoolSize pool_size={VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,4};
    VkDescriptorPoolCreateInfo pc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=2,.poolSizeCount=1,.pPoolSizes=&pool_size};
    VkDescriptorPool descriptor_pool; CHECK(vkCreateDescriptorPool(device,&pc,NULL,&descriptor_pool));
    VkDescriptorSetLayout alloc_layouts[2]={set_layout,set_layout}; VkDescriptorSetAllocateInfo sa={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=descriptor_pool,.descriptorSetCount=2,.pSetLayouts=alloc_layouts};
    VkDescriptorSet sets[2]; CHECK(vkAllocateDescriptorSets(device,&sa,sets)); VkBuffer uniforms[2], readback; VkDeviceMemory um[2];
    for(unsigned which=0;which<2;which++){
        um[which]=buffer(stride*ROWS,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,&uniforms[which]); void *mapped; CHECK(vkMapMemory(device,um[which],0,VK_WHOLE_SIZE,0,&mapped));
        for(unsigned row=0;row<ROWS;row++)for(unsigned c=0;c<4;c++) {
            ((uint32_t*)((char*)mapped+row*stride))[c]=value(which,row,c);
        }
        vkUnmapMemory(device,um[which]);
        VkDescriptorBufferInfo infos[2]={{uniforms[which],0,16},{uniforms[which],0,16}};
        VkWriteDescriptorSet writes[2]={{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=sets[which],.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,.pBufferInfo=&infos[0]},
            {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=sets[which],.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,.pBufferInfo=&infos[1]}};
        vkUpdateDescriptorSets(device,2,writes,0,NULL);
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
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=command_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(device,&ca,&cmd)); NV2AVkDescriptorBind last={0};
    unsigned exact_pixels=0;
    for(unsigned run=0;run<4;run++){
        bool memo=run==1||run==2; unsigned emitted=0, skipped=0;
        CHECK(vkResetCommandBuffer(cmd,0)); nv2a_vk_descriptor_bind_invalidate(&last);
        VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd,&begin));
        for(unsigned i=0;i<N;i++){
            unsigned which=(i/8)%2, a=(i/2)%ROWS, b=(i/6)%ROWS, pipeline_index=(i/16)%2;
            uint32_t offsets[2]={(uint32_t)(a*stride),(uint32_t)(b*stride)};
            if(i%16==0){vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelines[pipeline_index]);nv2a_vk_descriptor_bind_invalidate(&last);}
            if(i%7==0){vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layouts[pipeline_index],0,1,&sets[1-which],2,offsets);nv2a_vk_descriptor_bind_invalidate(&last);}
            for(unsigned repeat=0;repeat<4;repeat++){
                bool needed=!memo||nv2a_vk_descriptor_bind_required(&last,cmd,layouts[pipeline_index],0,sets[which],2,offsets);
                if(needed){vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layouts[pipeline_index],0,1,&sets[which],2,offsets);emitted++;}else skipped++;
            }
            VkClearValue clear={.color={{1,0,1,1}}}; VkRenderPassBeginInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=framebuffer,.renderArea={.extent={W,W}},.clearValueCount=1,.pClearValues=&clear};
            vkCmdBeginRenderPass(cmd,&rp,VK_SUBPASS_CONTENTS_INLINE); vkCmdDraw(cmd,3,1,0,0); vkCmdEndRenderPass(cmd);
            VkBufferImageCopy copy={.bufferOffset=i*W*W*4,.imageSubresource={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.layerCount=1},.imageExtent={W,W,1}};
            vkCmdCopyImageToBuffer(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
        }
        VkBufferMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=readback,.size=VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&barrier,0,NULL); CHECK(vkEndCommandBuffer(cmd));
        VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; CHECK(vkCreateFence(device,&fi,NULL,&fence));
        VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd}; CHECK(vkQueueSubmit(queue,1,&submit,fence)); CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX)); vkDestroyFence(device,fence,NULL);
        for(unsigned i=0;i<N;i++)for(unsigned p=0;p<W*W;p++)for(unsigned c=0;c<4;c++){
            unsigned which=(i/8)%2, a=(i/2)%ROWS, b=(i/6)%ROWS;
            assert(((uint8_t*)pixels)[(i*W*W+p)*4+c]==value(which,a,c)+value(which,b,c));
        }
        assert(emitted+skipped==N*4 && (!memo||skipped>96)); exact_pixels+=N*W*W;
        printf("run=%u memo=%d emitted=%u skipped=%u exact_pixels=%u\n",run,memo,emitted,skipped,N*W*W);
    }
    vkDestroyCommandPool(device,command_pool,NULL); vkDestroyPipeline(device,pipelines[0],NULL); vkDestroyPipeline(device,pipelines[1],NULL);
    vkDestroyShaderModule(device,vert,NULL); vkDestroyShaderModule(device,frag,NULL); vkDestroyFramebuffer(device,framebuffer,NULL); vkDestroyImageView(device,view,NULL);
    vkDestroyImage(device,image,NULL); vkFreeMemory(device,im,NULL); vkDestroyRenderPass(device,pass,NULL); vkUnmapMemory(device,rm); vkDestroyBuffer(device,readback,NULL); vkFreeMemory(device,rm,NULL);
    vkDestroyDescriptorPool(device,descriptor_pool,NULL); for(unsigned i=0;i<2;i++){vkDestroyBuffer(device,uniforms[i],NULL);vkFreeMemory(device,um[i],NULL);vkDestroyPipelineLayout(device,layouts[i],NULL);}
    vkDestroyDescriptorSetLayout(device,set_layout,NULL); vkDestroyDevice(device,NULL); vkDestroyInstance(instance,NULL);
    printf("PASS generated descriptor binds: %u exact RGBA pixels; changed offsets/sets/layouts, foreign binds, repeated draws and reused command handle; not game FPS\n",exact_pixels);
    return 0;
}
