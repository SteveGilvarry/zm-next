// 1c: import the VA Y (R8) + UV (R8G8) planes, run nv12_to_chw on the iGPU
// (letterbox + BT.601 YUV->RGB + normalize), download the CHW tensor and dump it
// as a PPM so we can confirm it's a correct aspect-preserved color frame.
// Usage: vk_va_preproc <clip> [dev] [frame] [net]
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <unistd.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
}
#include <va/va.h>
#include <va/va_drmcommon.h>
#define VK(x) do{VkResult _r=(x);if(_r!=VK_SUCCESS){fprintf(stderr,"VK %d @ %d\n",_r,__LINE__);exit(1);}}while(0)
static std::vector<uint32_t> spv(const char*p){FILE*f=fopen(p,"rb");fseek(f,0,2);long n=ftell(f);fseek(f,0,0);std::vector<uint32_t>v(n/4);if(fread(v.data(),1,n,f)!=(size_t)n){}fclose(f);return v;}
static enum AVPixelFormat ghw(AVCodecContext*,const enum AVPixelFormat*f){for(;*f!=-1;f++)if(*f==AV_PIX_FMT_VAAPI)return *f;return AV_PIX_FMT_NONE;}
static uint32_t mi(VkPhysicalDevice pd,uint32_t b,VkMemoryPropertyFlags w){VkPhysicalDeviceMemoryProperties m;vkGetPhysicalDeviceMemoryProperties(pd,&m);for(uint32_t i=0;i<m.memoryTypeCount;i++)if((b&(1u<<i))&&(m.memoryTypes[i].propertyFlags&w)==w)return i;return 0;}

int main(int argc,char**argv){
    const char* clip=argv[1]; const char* dev=argc>2?argv[2]:"/dev/dri/renderD129";
    int wantF=argc>3?atoi(argv[3]):30; int NET=argc>4?atoi(argv[4]):640;
    AVFormatContext* fmt=0; avformat_open_input(&fmt,clip,0,0); avformat_find_stream_info(fmt,0);
    int vsx=-1; for(unsigned i=0;i<fmt->nb_streams;i++)if(fmt->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO){vsx=i;break;}
    const AVCodec* dec=avcodec_find_decoder(fmt->streams[vsx]->codecpar->codec_id);
    AVBufferRef* hw=0; av_hwdevice_ctx_create(&hw,AV_HWDEVICE_TYPE_VAAPI,dev,0,0);
    AVCodecContext* dc=avcodec_alloc_context3(dec); avcodec_parameters_to_context(dc,fmt->streams[vsx]->codecpar); dc->hw_device_ctx=av_buffer_ref(hw); dc->get_format=ghw; avcodec_open2(dc,dec,0);
    AVPacket* pk=av_packet_alloc(); AVFrame* fr=av_frame_alloc(); int idx=0; bool got=false;
    while(!got&&av_read_frame(fmt,pk)>=0){if(pk->stream_index==vsx&&avcodec_send_packet(dc,pk)==0)while(avcodec_receive_frame(dc,fr)==0){if(fr->format==AV_PIX_FMT_VAAPI&&idx++>=wantF){got=true;break;}av_frame_unref(fr);}av_packet_unref(pk);}
    const int W=fr->width,H=fr->height;
    float scale=(float)NET/(W>H?W:H); int nw=(int)lround(W*scale),nh=(int)lround(H*scale); int padx=(NET-nw)/2,pady=(NET-nh)/2;
    printf("frame %d %dx%d -> letterbox net=%d scale=%.4f pad=(%d,%d)\n",wantF,W,H,NET,scale,padx,pady);

    AVHWFramesContext* fc=(AVHWFramesContext*)fr->hw_frames_ctx->data; VADisplay dpy=((AVVAAPIDeviceContext*)fc->device_ctx->hwctx)->display;
    VASurfaceID s=(VASurfaceID)(uintptr_t)fr->data[3]; vaSyncSurface(dpy,s);
    VADRMPRIMESurfaceDescriptor d{}; vaExportSurfaceHandle(dpy,s,VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,VA_EXPORT_SURFACE_READ_ONLY|VA_EXPORT_SURFACE_SEPARATE_LAYERS,&d);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ii.pApplicationInfo=&app; VkInstance inst; VK(vkCreateInstance(&ii,0,&inst));
    uint32_t n=0; vkEnumeratePhysicalDevices(inst,&n,0); std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(inst,&n,pds.data());
    VkPhysicalDevice pd=0; for(auto p:pds){VkPhysicalDeviceProperties r;vkGetPhysicalDeviceProperties(p,&r);if(r.vendorID==0x1002&&r.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU){pd=p;break;}} if(!pd)for(auto p:pds){VkPhysicalDeviceProperties r;vkGetPhysicalDeviceProperties(p,&r);if(r.vendorID==0x1002){pd=p;break;}}
    uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,0); std::vector<VkQueueFamilyProperties> qf(qn); vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,qf.data());
    uint32_t qfi=0; for(uint32_t i=0;i<qn;i++)if(qf[i].queueFlags&VK_QUEUE_COMPUTE_BIT){qfi=i;break;}
    const char* ex[]={"VK_KHR_external_memory","VK_KHR_external_memory_fd","VK_EXT_external_memory_dma_buf","VK_EXT_image_drm_format_modifier","VK_EXT_queue_family_foreign"};
    float pr=1; VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qc.queueFamilyIndex=qfi; qc.queueCount=1; qc.pQueuePriorities=&pr;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qc; dci.enabledExtensionCount=5; dci.ppEnabledExtensionNames=ex;
    VkDevice dv; VK(vkCreateDevice(pd,&dci,0,&dv)); VkQueue q; vkGetDeviceQueue(dv,qfi,0,&q);
    auto fdp=(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dv,"vkGetMemoryFdPropertiesKHR");

    // import one plane (layer L) as `fmt` image of size pw x ph
    auto imp=[&](int L,VkFormat f,int pw,int ph,VkImage&img,VkDeviceMemory&mem,VkImageView&view){
        uint32_t o=d.layers[L].object_index[0];
        VkSubresourceLayout sl{}; sl.offset=d.layers[L].offset[0]; sl.rowPitch=d.layers[L].pitch[0];
        VkImageDrmFormatModifierExplicitCreateInfoEXT m{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT}; m.drmFormatModifier=d.objects[o].drm_format_modifier; m.drmFormatModifierPlaneCount=1; m.pPlaneLayouts=&sl;
        VkExternalMemoryImageCreateInfo e{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO}; e.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; e.pNext=&m;
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ic.pNext=&e; ic.imageType=VK_IMAGE_TYPE_2D; ic.format=f; ic.extent={(uint32_t)pw,(uint32_t)ph,1}; ic.mipLevels=1; ic.arrayLayers=1; ic.samples=VK_SAMPLE_COUNT_1_BIT; ic.tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT; ic.usage=VK_IMAGE_USAGE_SAMPLED_BIT; ic.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ic.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        VK(vkCreateImage(dv,&ic,0,&img));
        VkImageMemoryRequirementsInfo2 ri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2}; ri.image=img; VkMemoryRequirements2 mr{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2}; vkGetImageMemoryRequirements2(dv,&ri,&mr);
        int fd=dup(d.objects[o].fd); VkMemoryFdPropertiesKHR fp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR}; fdp(dv,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,fd,&fp);
        VkImportMemoryFdInfoKHR im{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR}; im.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; im.fd=fd;
        VkMemoryDedicatedAllocateInfo de{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; de.image=img; de.pNext=&im;
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.pNext=&de; ma.allocationSize=mr.memoryRequirements.size; ma.memoryTypeIndex=mi(pd,mr.memoryRequirements.memoryTypeBits&fp.memoryTypeBits,0);
        VK(vkAllocateMemory(dv,&ma,0,&mem)); VK(vkBindImageMemory(dv,img,mem,0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=img; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=f; vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; VK(vkCreateImageView(dv,&vi,0,&view));
    };
    VkImage yI,uvI; VkDeviceMemory yM,uvM; VkImageView yV,uvV;
    imp(0,VK_FORMAT_R8_UNORM,W,H,yI,yM,yV);
    imp(1,VK_FORMAT_R8G8_UNORM,W/2,H/2,uvI,uvM,uvV);
    for(uint32_t k=0;k<d.num_objects;k++) close(d.objects[k].fd);

    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sc.magFilter=sc.minFilter=VK_FILTER_LINEAR; sc.addressModeU=sc.addressModeV=sc.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; sc.unnormalizedCoordinates=VK_FALSE; VkSampler smp; VK(vkCreateSampler(dv,&sc,0,&smp));
    // output CHW buffer
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=(VkDeviceSize)3*NET*NET*4; bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; VkBuffer ob; VK(vkCreateBuffer(dv,&bi,0,&ob)); VkMemoryRequirements br; vkGetBufferMemoryRequirements(dv,ob,&br);
    VkMemoryAllocateInfo ba{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ba.allocationSize=br.size; ba.memoryTypeIndex=mi(pd,br.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); VkDeviceMemory om; VK(vkAllocateMemory(dv,&ba,0,&om)); VK(vkBindBufferMemory(dv,ob,om,0)); void* omap; VK(vkMapMemory(dv,om,0,bi.size,0,&omap));

    auto code=spv("/home/steve/code/zm-next/bench/vk/nv12_to_chw.spv"); VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize=code.size()*4; sm.pCode=code.data(); VkShaderModule mod; VK(vkCreateShaderModule(dv,&sm,0,&mod));
    VkDescriptorSetLayoutBinding lb[3]={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,0},{1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,0},{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,0}};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount=3; dl.pBindings=lb; VkDescriptorSetLayout dsl; VK(vkCreateDescriptorSetLayout(dv,&dl,0,&dsl));
    struct PC{int sw,sh,net;float scale;int px,py;} pc{W,H,NET,scale,padx,pady};
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(PC)}; VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount=1; pli.pSetLayouts=&dsl; pli.pushConstantRangeCount=1; pli.pPushConstantRanges=&pcr; VkPipelineLayout pgl; VK(vkCreatePipelineLayout(dv,&pli,0,&pgl));
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cpi.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cpi.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module=mod; cpi.stage.pName="main"; cpi.layout=pgl; VkPipeline pipe; VK(vkCreateComputePipelines(dv,VK_NULL_HANDLE,1,&cpi,0,&pipe));
    VkDescriptorPoolSize psz[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}}; VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets=1; dp.poolSizeCount=2; dp.pPoolSizes=psz; VkDescriptorPool pool; VK(vkCreateDescriptorPool(dv,&dp,0,&pool));
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool=pool; da.descriptorSetCount=1; da.pSetLayouts=&dsl; VkDescriptorSet set; VK(vkAllocateDescriptorSets(dv,&da,&set));
    VkDescriptorImageInfo y{smp,yV,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},u{smp,uvV,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; VkDescriptorBufferInfo ow{ob,0,VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[3]={{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstSet=set;w[0].dstBinding=0;w[0].descriptorCount=1;w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[0].pImageInfo=&y;
    w[1].dstSet=set;w[1].dstBinding=1;w[1].descriptorCount=1;w[1].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[1].pImageInfo=&u;
    w[2].dstSet=set;w[2].dstBinding=2;w[2].descriptorCount=1;w[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w[2].pBufferInfo=&ow; vkUpdateDescriptorSets(dv,3,w,0,0);

    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cp.queueFamilyIndex=qfi; VkCommandPool cmdp; VK(vkCreateCommandPool(dv,&cp,0,&cmdp));
    VkCommandBufferAllocateInfo cba{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cba.commandPool=cmdp; cba.commandBufferCount=1; VkCommandBuffer cb; VK(vkAllocateCommandBuffers(dv,&cba,&cb));
    VkCommandBufferBeginInfo bg{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bg.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; VK(vkBeginCommandBuffer(cb,&bg));
    VkImageMemoryBarrier ba2[2]; for(int i=0;i<2;i++){ba2[i]={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; ba2[i].dstAccessMask=VK_ACCESS_SHADER_READ_BIT; ba2[i].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; ba2[i].newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; ba2[i].srcQueueFamilyIndex=VK_QUEUE_FAMILY_FOREIGN_EXT; ba2[i].dstQueueFamilyIndex=qfi; ba2[i].image=i?uvI:yI; ba2[i].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,0,0,0,2,ba2);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe); vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pgl,0,1,&set,0,0); vkCmdPushConstants(cb,pgl,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pc),&pc);
    vkCmdDispatch(cb,(NET+15)/16,(NET+15)/16,1); VK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&cb; VK(vkQueueSubmit(q,1,&si,VK_NULL_HANDLE)); VK(vkQueueWaitIdle(q));

    // dump CHW -> PPM
    float* chw=(float*)omap; int plane=NET*NET; FILE* f=fopen("/tmp/vk_pre.ppm","wb"); fprintf(f,"P6\n%d %d\n255\n",NET,NET);
    for(int yy=0;yy<NET;yy++)for(int xx=0;xx<NET;xx++)for(int c=0;c<3;c++){int v=(int)(chw[c*plane+yy*NET+xx]*255.f+0.5f);fputc(v<0?0:v>255?255:v,f);} fclose(f);
    printf("wrote /tmp/vk_pre.ppm (GPU preprocess output)\n");
    return 0;
}
