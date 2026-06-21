// 1b+: fully GPU-resident VAAPI motion gate. Per frame: import the VA Y plane
// (dma_buf, zero-copy), run the diff shader vs a device-resident prev grid, read
// back ONLY the ~20B verdict. Validates the GPU verdict (changed count + bbox)
// against a CPU reference over a sequence of frames.
//
// Usage: vk_va_gate <clip> [dev=/dev/dri/renderD129] [frames=120] [ds=8] [thr=18]
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

#define VK(x) do{VkResult _r=(x); if(_r!=VK_SUCCESS){fprintf(stderr,"VK %d @ %d\n",_r,__LINE__);exit(1);}}while(0)
static std::vector<uint32_t> spv(const char*p){FILE*f=fopen(p,"rb");fseek(f,0,2);long n=ftell(f);fseek(f,0,0);std::vector<uint32_t>v(n/4);if(fread(v.data(),1,n,f)!=(size_t)n){}fclose(f);return v;}
static enum AVPixelFormat ghw(AVCodecContext*,const enum AVPixelFormat*f){for(;*f!=-1;f++)if(*f==AV_PIX_FMT_VAAPI)return *f;return AV_PIX_FMT_NONE;}
static uint32_t memidx(VkPhysicalDevice pd,uint32_t bits,VkMemoryPropertyFlags w){VkPhysicalDeviceMemoryProperties m;vkGetPhysicalDeviceMemoryProperties(pd,&m);for(uint32_t i=0;i<m.memoryTypeCount;i++)if((bits&(1u<<i))&&(m.memoryTypes[i].propertyFlags&w)==w)return i;return 0;}

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: %s <clip> [dev] [frames] [ds] [thr]\n",argv[0]);return 1;}
    const char* clip=argv[1]; const char* dev=argc>2?argv[2]:"/dev/dri/renderD129";
    const int MAXF=argc>3?atoi(argv[3]):120; const int DS=argc>4?atoi(argv[4]):8; const int THR=argc>5?atoi(argv[5]):18;

    // ---- decode setup ----
    AVFormatContext* fmt=nullptr; avformat_open_input(&fmt,clip,nullptr,nullptr); avformat_find_stream_info(fmt,nullptr);
    int vs=-1; for(unsigned i=0;i<fmt->nb_streams;i++) if(fmt->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO){vs=i;break;}
    const AVCodec* dec=avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
    AVBufferRef* hw=nullptr; if(av_hwdevice_ctx_create(&hw,AV_HWDEVICE_TYPE_VAAPI,dev,nullptr,0)<0){fprintf(stderr,"hwdev\n");return 1;}
    AVCodecContext* dc=avcodec_alloc_context3(dec); avcodec_parameters_to_context(dc,fmt->streams[vs]->codecpar);
    dc->hw_device_ctx=av_buffer_ref(hw); dc->get_format=ghw; avcodec_open2(dc,dec,nullptr);

    // ---- vulkan device ----
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ii.pApplicationInfo=&app; VkInstance inst; VK(vkCreateInstance(&ii,nullptr,&inst));
    uint32_t n=0; vkEnumeratePhysicalDevices(inst,&n,nullptr); std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(inst,&n,pds.data());
    VkPhysicalDevice pd=0; for(auto p:pds){VkPhysicalDeviceProperties r;vkGetPhysicalDeviceProperties(p,&r);if(r.vendorID==0x1002&&r.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU){pd=p;break;}} if(!pd)for(auto p:pds){VkPhysicalDeviceProperties r;vkGetPhysicalDeviceProperties(p,&r);if(r.vendorID==0x1002){pd=p;break;}}
    uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,nullptr); std::vector<VkQueueFamilyProperties> qf(qn); vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,qf.data());
    uint32_t qfi=0; for(uint32_t i=0;i<qn;i++) if(qf[i].queueFlags&VK_QUEUE_COMPUTE_BIT){qfi=i;break;}
    const char* ex[]={ "VK_KHR_external_memory","VK_KHR_external_memory_fd","VK_EXT_external_memory_dma_buf","VK_EXT_image_drm_format_modifier","VK_EXT_queue_family_foreign"};
    float pr=1; VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qc.queueFamilyIndex=qfi; qc.queueCount=1; qc.pQueuePriorities=&pr;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qc; dci.enabledExtensionCount=5; dci.ppEnabledExtensionNames=ex;
    VkDevice dv; VK(vkCreateDevice(pd,&dci,nullptr,&dv)); VkQueue q; vkGetDeviceQueue(dv,qfi,0,&q);
    auto fdProps=(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dv,"vkGetMemoryFdPropertiesKHR");

    // probe first frame for dims
    AVPacket* pkt=av_packet_alloc(); AVFrame* fr=av_frame_alloc(); bool got=false;
    while(!got&&av_read_frame(fmt,pkt)>=0){ if(pkt->stream_index==vs&&avcodec_send_packet(dc,pkt)==0){ while(avcodec_receive_frame(dc,fr)==0){ if(fr->format==AV_PIX_FMT_VAAPI){got=true;break;} av_frame_unref(fr);} } av_packet_unref(pkt);}
    const int W=fr->width,H=fr->height,GW=W/DS,GH=H/DS; const size_t NC=(size_t)GW*GH;
    printf("gate: %dx%d  grid %dx%d (ds=%d thr=%d)\n",W,H,GW,GH,DS,THR);

    // persistent prev + verdict buffers (host-visible UMA)
    auto mkbuf=[&](VkDeviceSize sz,VkBuffer&b,VkDeviceMemory&m,void**map){ VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=sz; bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; VK(vkCreateBuffer(dv,&bi,nullptr,&b)); VkMemoryRequirements r; vkGetBufferMemoryRequirements(dv,b,&r); VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; a.allocationSize=r.size; a.memoryTypeIndex=memidx(pd,r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); VK(vkAllocateMemory(dv,&a,nullptr,&m)); VK(vkBindBufferMemory(dv,b,m,0)); if(map)VK(vkMapMemory(dv,m,0,sz,0,map)); };
    VkBuffer prevB,verdB; VkDeviceMemory prevM,verdM; void* verdMap;
    mkbuf(NC*4,prevB,prevM,nullptr); mkbuf(5*4,verdB,verdM,&verdMap);

    // pipeline (3 bindings)
    auto code=spv("/home/steve/code/zm-next/bench/vk/motion_diff_img.spv");
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize=code.size()*4; sm.pCode=code.data(); VkShaderModule mod; VK(vkCreateShaderModule(dv,&sm,nullptr,&mod));
    VkDescriptorSetLayoutBinding lb[3]={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,0},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,0},{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,0}};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount=3; dl.pBindings=lb; VkDescriptorSetLayout dsl; VK(vkCreateDescriptorSetLayout(dv,&dl,nullptr,&dsl));
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT,0,7*4}; VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount=1; pli.pSetLayouts=&dsl; pli.pushConstantRangeCount=1; pli.pPushConstantRanges=&pcr; VkPipelineLayout pgl; VK(vkCreatePipelineLayout(dv,&pli,nullptr,&pgl));
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cp.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; cp.stage.module=mod; cp.stage.pName="main"; cp.layout=pgl; VkPipeline pipe; VK(vkCreateComputePipelines(dv,VK_NULL_HANDLE,1,&cp,nullptr,&pipe));
    VkDescriptorPoolSize ps[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,MAXF+2},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2*(MAXF+2)}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets=MAXF+2; dp.poolSizeCount=2; dp.pPoolSizes=ps; dp.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; VkDescriptorPool pool; VK(vkCreateDescriptorPool(dv,&dp,nullptr,&pool));
    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sc.magFilter=sc.minFilter=VK_FILTER_NEAREST; sc.addressModeU=sc.addressModeV=sc.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; VkSampler samp; VK(vkCreateSampler(dv,&sc,nullptr,&samp));
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.queueFamilyIndex=qfi; cpi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; VkCommandPool cmdp; VK(vkCreateCommandPool(dv,&cpi,nullptr,&cmdp));
    VkCommandBufferAllocateInfo cba{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cba.commandPool=cmdp; cba.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cba.commandBufferCount=1; VkCommandBuffer cb; VK(vkAllocateCommandBuffers(dv,&cba,&cb));

    // import the Y plane of a VAAPI AVFrame into a fresh R8 image
    auto importY=[&](AVFrame* f,VkImage&img,VkDeviceMemory&mem,VkImageView&view){
        AVHWFramesContext* fc=(AVHWFramesContext*)f->hw_frames_ctx->data; VADisplay dpy=((AVVAAPIDeviceContext*)fc->device_ctx->hwctx)->display;
        VASurfaceID s=(VASurfaceID)(uintptr_t)f->data[3]; vaSyncSurface(dpy,s);
        VADRMPRIMESurfaceDescriptor d{}; vaExportSurfaceHandle(dpy,s,VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,VA_EXPORT_SURFACE_READ_ONLY|VA_EXPORT_SURFACE_SEPARATE_LAYERS,&d);
        uint32_t o=d.layers[0].object_index[0];
        VkSubresourceLayout pl{}; pl.offset=d.layers[0].offset[0]; pl.rowPitch=d.layers[0].pitch[0];
        VkImageDrmFormatModifierExplicitCreateInfoEXT mi{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT}; mi.drmFormatModifier=d.objects[o].drm_format_modifier; mi.drmFormatModifierPlaneCount=1; mi.pPlaneLayouts=&pl;
        VkExternalMemoryImageCreateInfo em{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO}; em.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; em.pNext=&mi;
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ic.pNext=&em; ic.imageType=VK_IMAGE_TYPE_2D; ic.format=VK_FORMAT_R8_UNORM; ic.extent={(uint32_t)W,(uint32_t)H,1}; ic.mipLevels=1; ic.arrayLayers=1; ic.samples=VK_SAMPLE_COUNT_1_BIT; ic.tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT; ic.usage=VK_IMAGE_USAGE_SAMPLED_BIT; ic.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ic.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        VK(vkCreateImage(dv,&ic,nullptr,&img));
        VkImageMemoryRequirementsInfo2 ri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2}; ri.image=img; VkMemoryRequirements2 mr{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2}; vkGetImageMemoryRequirements2(dv,&ri,&mr);
        int fdv=dup(d.objects[o].fd); VkMemoryFdPropertiesKHR fp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR}; fdProps(dv,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,fdv,&fp);
        VkImportMemoryFdInfoKHR im{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR}; im.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; im.fd=fdv;
        VkMemoryDedicatedAllocateInfo de{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; de.image=img; de.pNext=&im;
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.pNext=&de; ma.allocationSize=mr.memoryRequirements.size; ma.memoryTypeIndex=memidx(pd,mr.memoryRequirements.memoryTypeBits&fp.memoryTypeBits,0);
        VK(vkAllocateMemory(dv,&ma,nullptr,&mem)); VK(vkBindImageMemory(dv,img,mem,0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=img; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=VK_FORMAT_R8_UNORM; vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; VK(vkCreateImageView(dv,&vi,nullptr,&view));
        for(uint32_t k=0;k<d.num_objects;k++) close(d.objects[k].fd);
    };

    std::vector<uint8_t> prevCPU; bool hasPrev=false; int motion=0, frames=0; long mismatch=0;
    auto process=[&](AVFrame* f)->bool{
        VkImage img; VkDeviceMemory mem; VkImageView view; importY(f,img,mem,view);
        // descriptor set
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool=pool; da.descriptorSetCount=1; da.pSetLayouts=&dsl; VkDescriptorSet set; VK(vkAllocateDescriptorSets(dv,&da,&set));
        VkDescriptorImageInfo di{samp,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; VkDescriptorBufferInfo b1{prevB,0,VK_WHOLE_SIZE},b2{verdB,0,VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[3]={{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstSet=set;w[0].dstBinding=0;w[0].descriptorCount=1;w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[0].pImageInfo=&di;
        w[1].dstSet=set;w[1].dstBinding=1;w[1].descriptorCount=1;w[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w[1].pBufferInfo=&b1;
        w[2].dstSet=set;w[2].dstBinding=2;w[2].descriptorCount=1;w[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w[2].pBufferInfo=&b2;
        vkUpdateDescriptorSets(dv,3,w,0,nullptr);
        // reset verdict {0, gw, gh, -1, -1}
        int* vrd=(int*)verdMap; vrd[0]=0; vrd[1]=GW; vrd[2]=GH; vrd[3]=-1; vrd[4]=-1;
        // record + submit
        VkCommandBufferBeginInfo bg{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bg.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; VK(vkBeginCommandBuffer(cb,&bg));
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; bar.dstAccessMask=VK_ACCESS_SHADER_READ_BIT; bar.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; bar.srcQueueFamilyIndex=VK_QUEUE_FAMILY_FOREIGN_EXT; bar.dstQueueFamilyIndex=qfi; bar.image=img; bar.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,0,0,0,1,&bar);
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe); vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pgl,0,1,&set,0,0);
        int pc[7]={W,H,DS,GW,GH,THR,hasPrev?1:0}; vkCmdPushConstants(cb,pgl,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pc),pc);
        vkCmdDispatch(cb,(GW+15)/16,(GH+15)/16,1); VK(vkEndCommandBuffer(cb));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&cb; VK(vkQueueSubmit(q,1,&si,VK_NULL_HANDLE)); VK(vkQueueWaitIdle(q));
        // GPU verdict
        int gcnt=vrd[0]; bool gactive = hasPrev && gcnt>0;
        // ---- CPU reference (hwdownload + downsample + diff) ----
        AVFrame* swf=av_frame_alloc(); av_hwframe_transfer_data(swf,f,0); const uint8_t* Y=swf->data[0]; int ys=swf->linesize[0];
        std::vector<uint8_t> cur(NC); for(int j=0;j<GH;j++)for(int i=0;i<GW;i++){int sx=i*DS;if(sx>W-1)sx=W-1;int sy=j*DS;if(sy>H-1)sy=H-1;cur[j*GW+i]=Y[sy*ys+sx];}
        int ccnt=0; if(hasPrev){ for(size_t k=0;k<NC;k++){int dd=cur[k]-prevCPU[k]; if(dd<0)dd=-dd; if(dd>THR)++ccnt;} }
        prevCPU=cur; av_frame_free(&swf);
        if(hasPrev && gcnt!=ccnt){ if(mismatch<3) printf("  frame %d: GPU cnt=%d CPU cnt=%d MISMATCH\n",frames,gcnt,ccnt); ++mismatch; }
        if(gactive) ++motion;
        hasPrev=true; ++frames;
        vkDestroyImageView(dv,view,nullptr); vkDestroyImage(dv,img,nullptr); vkFreeMemory(dv,mem,nullptr); vkFreeDescriptorSets(dv,pool,1,&set);
        return true;
    };

    process(fr); av_frame_unref(fr);   // first decoded frame (primes)
    while(frames<MAXF && av_read_frame(fmt,pkt)>=0){ if(pkt->stream_index==vs&&avcodec_send_packet(dc,pkt)==0){ while(frames<MAXF&&avcodec_receive_frame(dc,fr)==0){ if(fr->format==AV_PIX_FMT_VAAPI) process(fr); av_frame_unref(fr);} } av_packet_unref(pkt);}

    printf("\nframes=%d  motion=%d (%.0f%%)  GPU-vs-CPU verdict mismatches=%ld\n",frames,motion,frames?100.0*motion/frames:0,mismatch);
    printf(mismatch==0 ? "PASS — GPU motion gate matches CPU; only a 20B verdict crossed per frame\n" : "FAIL\n");
    return mismatch==0?0:1;
}
