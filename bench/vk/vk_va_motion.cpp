// Milestone 1b: import a VAAPI-decoded surface's luma plane into Vulkan via
// dma_buf (zero-copy, never leaves the GPU), downsample it on the iGPU compute,
// and verify the grid matches a CPU hwdownload of the same frame.
//
// Usage: vk_va_motion <clip> [device=/dev/dri/renderD129] [frame=30]
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

#define VK(x) do{ VkResult _r=(x); if(_r!=VK_SUCCESS){fprintf(stderr,"VK fail %d @ %s:%d\n",_r,__FILE__,__LINE__);exit(1);} }while(0)

static std::vector<uint32_t> load_spv(const char* p){ FILE* f=fopen(p,"rb"); if(!f){fprintf(stderr,"spv %s?\n",p);exit(1);} fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); std::vector<uint32_t> v(n/4); if(fread(v.data(),1,n,f)!=(size_t)n){} fclose(f); return v; }
static uint32_t pick_mem(VkPhysicalDevice pd,uint32_t bits){ VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd,&mp); for(uint32_t i=0;i<mp.memoryTypeCount;i++) if(bits&(1u<<i)) return i; return 0; }

static enum AVPixelFormat get_hw(AVCodecContext*,const enum AVPixelFormat* f){ for(;*f!=AV_PIX_FMT_NONE;f++) if(*f==AV_PIX_FMT_VAAPI) return *f; return AV_PIX_FMT_NONE; }

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: %s <clip> [dev] [frame]\n",argv[0]);return 1;}
    const char* clip=argv[1];
    const char* dev =argc>2?argv[2]:"/dev/dri/renderD129";
    const int wantFrame=argc>3?atoi(argv[3]):30;

    // ---------- FFmpeg VAAPI decode ----------
    AVFormatContext* fmt=nullptr;
    if(avformat_open_input(&fmt,clip,nullptr,nullptr)<0){fprintf(stderr,"open\n");return 1;}
    avformat_find_stream_info(fmt,nullptr);
    int vs=-1; for(unsigned i=0;i<fmt->nb_streams;i++) if(fmt->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO){vs=i;break;}
    const AVCodec* dec=avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
    AVBufferRef* hw=nullptr;
    if(av_hwdevice_ctx_create(&hw,AV_HWDEVICE_TYPE_VAAPI,dev,nullptr,0)<0){fprintf(stderr,"hwdev\n");return 1;}
    AVCodecContext* dc=avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dc,fmt->streams[vs]->codecpar);
    dc->hw_device_ctx=av_buffer_ref(hw); dc->get_format=get_hw;
    if(avcodec_open2(dc,dec,nullptr)<0){fprintf(stderr,"open dec\n");return 1;}

    AVPacket* pkt=av_packet_alloc(); AVFrame* fr=av_frame_alloc();
    int idx=0; bool got=false;
    while(!got && av_read_frame(fmt,pkt)>=0){
        if(pkt->stream_index!=vs){av_packet_unref(pkt);continue;}
        if(avcodec_send_packet(dc,pkt)==0){
            while(avcodec_receive_frame(dc,fr)==0){ if(fr->format==AV_PIX_FMT_VAAPI && idx++>=wantFrame){got=true;break;} av_frame_unref(fr);} }
        av_packet_unref(pkt);
    }
    if(!got){fprintf(stderr,"no VAAPI frame\n");return 1;}
    const int W=fr->width,H=fr->height,DS=8,GW=W/DS,GH=H/DS;
    printf("frame %d: %dx%d VAAPI surface\n",wantFrame,W,H);

    // VADisplay + surface id from the AVFrame
    AVHWFramesContext* fctx=(AVHWFramesContext*)fr->hw_frames_ctx->data;
    AVVAAPIDeviceContext* vactx=(AVVAAPIDeviceContext*)fctx->device_ctx->hwctx;
    VADisplay dpy=vactx->display;
    VASurfaceID surf=(VASurfaceID)(uintptr_t)fr->data[3];
    vaSyncSurface(dpy,surf);

    // ---------- export the surface as dma_buf ----------
    VADRMPRIMESurfaceDescriptor d{};
    if(vaExportSurfaceHandle(dpy,surf,VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY|VA_EXPORT_SURFACE_SEPARATE_LAYERS,&d)!=VA_STATUS_SUCCESS){
        fprintf(stderr,"vaExportSurfaceHandle failed\n");return 1;}
    printf("exported: fourcc=%.4s objects=%u layers=%u\n",(char*)&d.fourcc,d.num_objects,d.num_layers);
    // Y plane = layer 0, plane 0
    uint32_t obj=d.layers[0].object_index[0];
    int yfd=d.objects[obj].fd; uint64_t mod=d.objects[obj].drm_format_modifier;
    uint64_t yoff=d.layers[0].offset[0], ypitch=d.layers[0].pitch[0];
    printf("Y plane: obj=%u fd=%d modifier=0x%llx offset=%llu pitch=%llu\n",obj,yfd,(unsigned long long)mod,(unsigned long long)yoff,(unsigned long long)ypitch);

    // ---------- Vulkan: instance + RADV iGPU device with import exts ----------
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app;
    VkInstance inst; VK(vkCreateInstance(&ici,nullptr,&inst));
    uint32_t n=0; vkEnumeratePhysicalDevices(inst,&n,nullptr); std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(inst,&n,pds.data());
    VkPhysicalDevice pd=VK_NULL_HANDLE;
    for(auto p:pds){VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(p,&pr); if(pr.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU&&pr.vendorID==0x1002){pd=p;break;}}
    if(!pd) for(auto p:pds){VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(p,&pr); if(pr.vendorID==0x1002){pd=p;break;}}
    if(!pd){fprintf(stderr,"no AMD vk device\n");return 1;}
    uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,nullptr); std::vector<VkQueueFamilyProperties> qf(qn); vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,qf.data());
    uint32_t qfi=0; for(uint32_t i=0;i<qn;i++) if(qf[i].queueFlags&VK_QUEUE_COMPUTE_BIT){qfi=i;break;}
    const char* exts[]={VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
    float prio=1; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex=qfi; qci.queueCount=1; qci.pQueuePriorities=&prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci; dci.enabledExtensionCount=5; dci.ppEnabledExtensionNames=exts;
    VkDevice dv; VK(vkCreateDevice(pd,&dci,nullptr,&dv)); VkQueue q; vkGetDeviceQueue(dv,qfi,0,&q);
    auto pfnFdProps=(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dv,"vkGetMemoryFdPropertiesKHR");

    // ---------- import the Y plane as an R8 image bound to the dma_buf ----------
    VkSubresourceLayout pl{}; pl.offset=yoff; pl.rowPitch=ypitch;
    VkImageDrmFormatModifierExplicitCreateInfoEXT mi{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    mi.drmFormatModifier=mod; mi.drmFormatModifierPlaneCount=1; mi.pPlaneLayouts=&pl;
    VkExternalMemoryImageCreateInfo em{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO}; em.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; em.pNext=&mi;
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ic.pNext=&em; ic.imageType=VK_IMAGE_TYPE_2D;
    ic.format=VK_FORMAT_R8_UNORM; ic.extent={(uint32_t)W,(uint32_t)H,1}; ic.mipLevels=1; ic.arrayLayers=1;
    ic.samples=VK_SAMPLE_COUNT_1_BIT; ic.tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT; ic.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
    ic.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ic.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img; VK(vkCreateImage(dv,&ic,nullptr,&img));

    VkImageMemoryRequirementsInfo2 mri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2}; mri.image=img;
    VkMemoryRequirements2 mr{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2}; vkGetImageMemoryRequirements2(dv,&mri,&mr);
    int dupfd=dup(yfd);
    VkMemoryFdPropertiesKHR fdp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VK(pfnFdProps(dv,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,dupfd,&fdp));
    uint32_t memBits=mr.memoryRequirements.memoryTypeBits & fdp.memoryTypeBits;
    VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR}; imp.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; imp.fd=dupfd;
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; ded.image=img; ded.pNext=&imp;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.pNext=&ded; mai.allocationSize=mr.memoryRequirements.size; mai.memoryTypeIndex=pick_mem(pd,memBits);
    VkDeviceMemory mem; VK(vkAllocateMemory(dv,&mai,nullptr,&mem)); VK(vkBindImageMemory(dv,img,mem,0));

    VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; iv.image=img; iv.viewType=VK_IMAGE_VIEW_TYPE_2D; iv.format=VK_FORMAT_R8_UNORM;
    iv.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; VkImageView view; VK(vkCreateImageView(dv,&iv,nullptr,&view));
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sci.magFilter=VK_FILTER_NEAREST; sci.minFilter=VK_FILTER_NEAREST;
    sci.addressModeU=sci.addressModeV=sci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; VkSampler samp; VK(vkCreateSampler(dv,&sci,nullptr,&samp));

    // output grid buffer (host-visible; UMA)
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=(VkDeviceSize)GW*GH*4; bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkBuffer outb; VK(vkCreateBuffer(dv,&bi,nullptr,&outb)); VkMemoryRequirements bmr; vkGetBufferMemoryRequirements(dv,outb,&bmr);
    VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; bai.allocationSize=bmr.size;
    { VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd,&mp); for(uint32_t i=0;i<mp.memoryTypeCount;i++) if((bmr.memoryTypeBits&(1u<<i))&&(mp.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){bai.memoryTypeIndex=i;break;} }
    VkDeviceMemory outm; VK(vkAllocateMemory(dv,&bai,nullptr,&outm)); VK(vkBindBufferMemory(dv,outb,outm,0)); void* outmap; VK(vkMapMemory(dv,outm,0,bi.size,0,&outmap));

    // pipeline
    auto spv=load_spv("/home/steve/code/zm-next/bench/vk/motion_downsample_img.spv");
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize=spv.size()*4; smi.pCode=spv.data(); VkShaderModule sm; VK(vkCreateShaderModule(dv,&smi,nullptr,&sm));
    VkDescriptorSetLayoutBinding lb[2]={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount=2; dl.pBindings=lb; VkDescriptorSetLayout dsl; VK(vkCreateDescriptorSetLayout(dv,&dl,nullptr,&dsl));
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT,0,5*sizeof(int)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount=1; pli.pSetLayouts=&dsl; pli.pushConstantRangeCount=1; pli.pPushConstantRanges=&pcr; VkPipelineLayout pgl; VK(vkCreatePipelineLayout(dv,&pli,nullptr,&pgl));
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cpi.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cpi.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module=sm; cpi.stage.pName="main"; cpi.layout=pgl; VkPipeline pipe; VK(vkCreateComputePipelines(dv,VK_NULL_HANDLE,1,&cpi,nullptr,&pipe));
    VkDescriptorPoolSize psz[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets=1; dpi.poolSizeCount=2; dpi.pPoolSizes=psz; VkDescriptorPool dp; VK(vkCreateDescriptorPool(dv,&dpi,nullptr,&dp));
    VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dsa.descriptorPool=dp; dsa.descriptorSetCount=1; dsa.pSetLayouts=&dsl; VkDescriptorSet ds; VK(vkAllocateDescriptorSets(dv,&dsa,&ds));
    VkDescriptorImageInfo dii{samp,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; VkDescriptorBufferInfo dbi{outb,0,VK_WHOLE_SIZE};
    VkWriteDescriptorSet ws[2]={{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    ws[0].dstSet=ds; ws[0].dstBinding=0; ws[0].descriptorCount=1; ws[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ws[0].pImageInfo=&dii;
    ws[1].dstSet=ds; ws[1].dstBinding=1; ws[1].descriptorCount=1; ws[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ws[1].pBufferInfo=&dbi; vkUpdateDescriptorSets(dv,2,ws,0,nullptr);

    // command buffer: acquire image from FOREIGN queue, transition, dispatch
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpci.queueFamilyIndex=qfi; VkCommandPool cp; VK(vkCreateCommandPool(dv,&cpci,nullptr,&cp));
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cbi.commandPool=cp; cbi.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount=1; VkCommandBuffer cb; VK(vkAllocateCommandBuffers(dv,&cbi,&cb));
    VkCommandBufferBeginInfo cbb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; cbb.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; VK(vkBeginCommandBuffer(cb,&cbb));
    VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bar.srcAccessMask=0; bar.dstAccessMask=VK_ACCESS_SHADER_READ_BIT; bar.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcQueueFamilyIndex=VK_QUEUE_FAMILY_FOREIGN_EXT; bar.dstQueueFamilyIndex=qfi; bar.image=img; bar.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&bar);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe); vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pgl,0,1,&ds,0,nullptr);
    int pcv[5]={W,H,DS,GW,GH}; vkCmdPushConstants(cb,pgl,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pcv),pcv);
    vkCmdDispatch(cb,(GW+15)/16,(GH+15)/16,1); VK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&cb; VK(vkQueueSubmit(q,1,&si,VK_NULL_HANDLE)); VK(vkQueueWaitIdle(q));

    // ---------- CPU reference: hwdownload the same frame, downsample Y ----------
    AVFrame* sw=av_frame_alloc(); if(av_hwframe_transfer_data(sw,fr,0)<0){fprintf(stderr,"hwdownload failed\n");return 1;}
    const uint8_t* Y=sw->data[0]; int ys=sw->linesize[0];
    uint32_t* gpu=(uint32_t*)outmap; long mism=0; int bx=-1,by=-1; uint32_t bg=0,bc=0;
    for(int j=0;j<GH;j++) for(int i=0;i<GW;i++){ int sx=i*DS; if(sx>W-1)sx=W-1; int sy=j*DS; if(sy>H-1)sy=H-1; uint32_t c=Y[sy*ys+sx]; uint32_t g=gpu[j*GW+i]; if(c!=g){ if(!mism){bx=i;by=j;bg=g;bc=c;} ++mism; } }
    printf("\ngrid %dx%d (%d cells): %ld mismatches vs CPU hwdownload\n",GW,GH,GW*GH,mism);
    if(mism) printf("  first @ (%d,%d): gpu=%u cpu=%u (allow ±1 for tiling/rounding)\n",bx,by,bg,bc);
    // allow a tiny tolerance for any subtle tiling/sample rounding
    long bad=0; for(int j=0;j<GH;j++) for(int i=0;i<GW;i++){ int sx=i*DS; if(sx>W-1)sx=W-1; int sy=j*DS; if(sy>H-1)sy=H-1; int c=Y[sy*ys+sx]; int g=(int)gpu[j*GW+i]; if(abs(c-g)>1) ++bad; }
    printf("cells differing by >1: %ld\n", bad);
    printf(bad==0 ? "\nPASS — Vulkan sampled the imported VA surface correctly (zero-copy)\n" : "\nFAIL — import/sampling mismatch\n");

    for(uint32_t i=0;i<d.num_objects;i++) close(d.objects[i].fd);
    return bad==0?0:1;
}
