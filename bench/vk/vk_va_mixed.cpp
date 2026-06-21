// Mixed-mode iGPU front-half: VAAPI decode + Vulkan motion gate + (on motion)
// Vulkan NV12->CHW preprocess, all on the AMD iGPU. Reports throughput + per-stage
// ms + the handoff payload (the CHW that would cross to NVIDIA for YOLO). This is
// the "everything except the YOLO matmuls" half of the mixed-GPU pipeline.
//
// Usage: vk_va_igpu <clip> [dev] [frames] [net]
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <chrono>
#include <unistd.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
}
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <onnxruntime_cxx_api.h>   // NVIDIA-side YOLO (ORT CUDA EP)
#define VK(x) do{VkResult _r=(x);if(_r!=VK_SUCCESS){fprintf(stderr,"VK %d @ %d\n",_r,__LINE__);exit(1);}}while(0)
using clk=std::chrono::high_resolution_clock;
static double ms(clk::time_point a,clk::time_point b){return std::chrono::duration<double,std::milli>(b-a).count();}
static std::vector<uint32_t> spv(const char*p){FILE*f=fopen(p,"rb");fseek(f,0,2);long n=ftell(f);fseek(f,0,0);std::vector<uint32_t>v(n/4);if(fread(v.data(),1,n,f)!=(size_t)n){}fclose(f);return v;}
static enum AVPixelFormat ghw(AVCodecContext*,const enum AVPixelFormat*f){for(;*f!=-1;f++)if(*f==AV_PIX_FMT_VAAPI)return *f;return AV_PIX_FMT_NONE;}
static uint32_t mix(VkPhysicalDevice pd,uint32_t b,VkMemoryPropertyFlags w){VkPhysicalDeviceMemoryProperties m;vkGetPhysicalDeviceMemoryProperties(pd,&m);for(uint32_t i=0;i<m.memoryTypeCount;i++)if((b&(1u<<i))&&(m.memoryTypes[i].propertyFlags&w)==w)return i;return 0;}

struct G{VkInstance inst;VkPhysicalDevice pd;VkDevice dv;VkQueue q;uint32_t qfi;PFN_vkGetMemoryFdPropertiesKHR fdp;};
static G g;
static bool importPlane(VADRMPRIMESurfaceDescriptor&d,int L,VkFormat f,int pw,int ph,VkImage&img,VkDeviceMemory&mem,VkImageView&view){
    uint32_t o=d.layers[L].object_index[0];
    VkSubresourceLayout sl{};sl.offset=d.layers[L].offset[0];sl.rowPitch=d.layers[L].pitch[0];
    VkImageDrmFormatModifierExplicitCreateInfoEXT mi{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};mi.drmFormatModifier=d.objects[o].drm_format_modifier;mi.drmFormatModifierPlaneCount=1;mi.pPlaneLayouts=&sl;
    VkExternalMemoryImageCreateInfo em{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};em.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;em.pNext=&mi;
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ic.pNext=&em;ic.imageType=VK_IMAGE_TYPE_2D;ic.format=f;ic.extent={(uint32_t)pw,(uint32_t)ph,1};ic.mipLevels=1;ic.arrayLayers=1;ic.samples=VK_SAMPLE_COUNT_1_BIT;ic.tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;ic.usage=VK_IMAGE_USAGE_SAMPLED_BIT;ic.sharingMode=VK_SHARING_MODE_EXCLUSIVE;ic.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    if(vkCreateImage(g.dv,&ic,0,&img)!=VK_SUCCESS)return false;
    VkImageMemoryRequirementsInfo2 ri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};ri.image=img;VkMemoryRequirements2 mr{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};vkGetImageMemoryRequirements2(g.dv,&ri,&mr);
    int fd=dup(d.objects[o].fd);VkMemoryFdPropertiesKHR fp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};g.fdp(g.dv,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,fd,&fp);
    VkImportMemoryFdInfoKHR im{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};im.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;im.fd=fd;
    VkMemoryDedicatedAllocateInfo de{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};de.image=img;de.pNext=&im;
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.pNext=&de;ma.allocationSize=mr.memoryRequirements.size;ma.memoryTypeIndex=mix(g.pd,mr.memoryRequirements.memoryTypeBits&fp.memoryTypeBits,0);
    if(vkAllocateMemory(g.dv,&ma,0,&mem)!=VK_SUCCESS)return false;vkBindImageMemory(g.dv,img,mem,0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=img;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=f;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCreateImageView(g.dv,&vi,0,&view);return true;
}

int main(int argc,char**argv){
    const char* clip=argv[1];const char* dev=argc>2?argv[2]:"/dev/dri/renderD129";int MAXF=argc>3?atoi(argv[3]):300;int NET=argc>4?atoi(argv[4]):640;int DS=8,THR=18;
    AVFormatContext* fmt=0;avformat_open_input(&fmt,clip,0,0);avformat_find_stream_info(fmt,0);int vs=-1;for(unsigned i=0;i<fmt->nb_streams;i++)if(fmt->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO){vs=i;break;}
    const AVCodec* dec=avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);AVBufferRef* hw=0;av_hwdevice_ctx_create(&hw,AV_HWDEVICE_TYPE_VAAPI,dev,0,0);
    AVCodecContext* dc=avcodec_alloc_context3(dec);avcodec_parameters_to_context(dc,fmt->streams[vs]->codecpar);dc->hw_device_ctx=av_buffer_ref(hw);dc->get_format=ghw;avcodec_open2(dc,dec,0);
    // vulkan
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_2;VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ii.pApplicationInfo=&app;VK(vkCreateInstance(&ii,0,&g.inst));
    uint32_t n=0;vkEnumeratePhysicalDevices(g.inst,&n,0);std::vector<VkPhysicalDevice>pds(n);vkEnumeratePhysicalDevices(g.inst,&n,pds.data());for(auto p:pds){VkPhysicalDeviceProperties r;vkGetPhysicalDeviceProperties(p,&r);if(r.vendorID==0x1002&&r.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU){g.pd=p;break;}}
    uint32_t qn=0;vkGetPhysicalDeviceQueueFamilyProperties(g.pd,&qn,0);std::vector<VkQueueFamilyProperties>qf(qn);vkGetPhysicalDeviceQueueFamilyProperties(g.pd,&qn,qf.data());for(uint32_t i=0;i<qn;i++)if(qf[i].queueFlags&VK_QUEUE_COMPUTE_BIT){g.qfi=i;break;}
    const char* ex[]={"VK_KHR_external_memory","VK_KHR_external_memory_fd","VK_EXT_external_memory_dma_buf","VK_EXT_image_drm_format_modifier","VK_EXT_queue_family_foreign"};
    float pr=1;VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qc.queueFamilyIndex=g.qfi;qc.queueCount=1;qc.pQueuePriorities=&pr;VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qc;dci.enabledExtensionCount=5;dci.ppEnabledExtensionNames=ex;VK(vkCreateDevice(g.pd,&dci,0,&g.dv));vkGetDeviceQueue(g.dv,g.qfi,0,&g.q);g.fdp=(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(g.dv,"vkGetMemoryFdPropertiesKHR");
    // probe dims
    AVPacket* pk=av_packet_alloc();AVFrame* fr=av_frame_alloc();bool got=false;while(!got&&av_read_frame(fmt,pk)>=0){if(pk->stream_index==vs&&avcodec_send_packet(dc,pk)==0)while(avcodec_receive_frame(dc,fr)==0){if(fr->format==AV_PIX_FMT_VAAPI){got=true;break;}av_frame_unref(fr);}av_packet_unref(pk);}
    const int W=fr->width,H=fr->height,GW=W/DS,GH=H/DS;float scale=(float)NET/(W>H?W:H);int nw=(int)lround(W*scale),nh=(int)lround(H*scale),padx=(NET-nw)/2,pady=(NET-nh)/2;
    // buffers + 2 pipelines
    auto mkbuf=[&](VkDeviceSize sz,VkBuffer&b,VkDeviceMemory&m,void**mp){VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=sz;bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;vkCreateBuffer(g.dv,&bi,0,&b);VkMemoryRequirements r;vkGetBufferMemoryRequirements(g.dv,b,&r);VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=r.size;a.memoryTypeIndex=mix(g.pd,r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);vkAllocateMemory(g.dv,&a,0,&m);vkBindBufferMemory(g.dv,b,m,0);if(mp)vkMapMemory(g.dv,m,0,sz,0,mp);};
    VkBuffer prevB,verdB,chwB;VkDeviceMemory prevM,verdM,chwM;void*verdMap,*chwMap;mkbuf((VkDeviceSize)GW*GH*4,prevB,prevM,0);mkbuf(20,verdB,verdM,&verdMap);mkbuf((VkDeviceSize)3*NET*NET*4,chwB,chwM,&chwMap);
    auto mkpipe=[&](const char*sp,VkDescriptorSetLayout&dsl,VkPipelineLayout&pl,VkPipeline&pp,bool twoImg,uint32_t pcsz){VkDescriptorSetLayoutBinding b[3];b[0]={0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,0};b[1]=twoImg?VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,0}:VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,0};b[2]={2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,0};VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=3;dl.pBindings=b;vkCreateDescriptorSetLayout(g.dv,&dl,0,&dsl);VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT,0,pcsz};VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pli.setLayoutCount=1;pli.pSetLayouts=&dsl;pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&pc;vkCreatePipelineLayout(g.dv,&pli,0,&pl);auto c=spv(sp);VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=c.size()*4;sm.pCode=c.data();VkShaderModule m;vkCreateShaderModule(g.dv,&sm,0,&m);VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cp.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;cp.stage.module=m;cp.stage.pName="main";cp.layout=pl;vkCreateComputePipelines(g.dv,VK_NULL_HANDLE,1,&cp,0,&pp);};
    VkDescriptorSetLayout mDsl,pDsl;VkPipelineLayout mPL,pPL;VkPipeline mPipe,pPipe;
    mkpipe("/home/steve/code/zm-next/bench/vk/motion_diff_img.spv",mDsl,mPL,mPipe,false,28);
    mkpipe("/home/steve/code/zm-next/bench/vk/nv12_to_chw.spv",pDsl,pPL,pPipe,true,24);
    VkSamplerCreateInfo s0{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};s0.magFilter=s0.minFilter=VK_FILTER_NEAREST;s0.addressModeU=s0.addressModeV=s0.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;VkSampler nn;vkCreateSampler(g.dv,&s0,0,&nn);s0.magFilter=s0.minFilter=VK_FILTER_LINEAR;VkSampler lin;vkCreateSampler(g.dv,&s0,0,&lin);
    VkDescriptorPoolSize ps[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,4*(MAXF+4)},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,4*(MAXF+4)}};VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=2*(MAXF+4);dp.poolSizeCount=2;dp.pPoolSizes=ps;dp.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;VkDescriptorPool pool;vkCreateDescriptorPool(g.dv,&dp,0,&pool);
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cpi.queueFamilyIndex=g.qfi;cpi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;VkCommandPool cmdp;vkCreateCommandPool(g.dv,&cpi,0,&cmdp);VkCommandBufferAllocateInfo cba{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cba.commandPool=cmdp;cba.commandBufferCount=1;VkCommandBuffer cb;vkAllocateCommandBuffers(g.dv,&cba,&cb);

    auto barrier=[&](VkImage im){VkCommandBufferBeginInfo bg{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bg.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;return bg;};
    auto submit=[&](){VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cb;vkQueueSubmit(g.q,1,&si,VK_NULL_HANDLE);vkQueueWaitIdle(g.q);};

    // ---- NVIDIA: ORT CUDA YOLO session (the mixed-mode inference half, on the 5070 Ti) ----
    const char* modelPath=argc>5?argv[5]:"/home/steve/code/zm-next/bench/models/yolo26n_static.onnx";
    Ort::Env ortEnv(ORT_LOGGING_LEVEL_WARNING,"mixed"); Ort::SessionOptions so; OrtCUDAProviderOptions cu{}; so.AppendExecutionProvider_CUDA(cu);
    Ort::Session ortSess(ortEnv,modelPath,so); Ort::AllocatorWithDefaultOptions al;
    std::string inNm=ortSess.GetInputNameAllocated(0,al).get(), outNm=ortSess.GetOutputNameAllocated(0,al).get();
    Ort::MemoryInfo cpuMem=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
    double tInf=0; long dets=0; int infers=0;
    // warm up the CUDA EP (kernel autotune / cudnn) so the timed Runs are steady-state
    { int64_t shp[4]={1,3,NET,NET}; const char* iN[]={inNm.c_str()}; const char* oN[]={outNm.c_str()};
      for(int w=0;w<5;w++){ Ort::Value t=Ort::Value::CreateTensor<float>(cpuMem,(float*)chwMap,(size_t)3*NET*NET,shp,4); ortSess.Run(Ort::RunOptions{nullptr},iN,&t,1,oN,1); }
      double tw=0; for(int w=0;w<20;w++){ auto a=clk::now(); Ort::Value t=Ort::Value::CreateTensor<float>(cpuMem,(float*)chwMap,(size_t)3*NET*NET,shp,4); ortSess.Run(Ort::RunOptions{nullptr},iN,&t,1,oN,1); tw+=ms(a,clk::now()); }
      printf("ORT CUDA YOLO warm (back-to-back, NO Vulkan between): %.2f ms/run\n", tw/20); }

    int frames=0,motion=0;double tDec=0,tMot=0,tPre=0;bool hasPrev=false;int minCells=std::max(8,GW*GH/400);
    auto t_all0=clk::now();
    auto process=[&](AVFrame* f){
        AVHWFramesContext* fc=(AVHWFramesContext*)f->hw_frames_ctx->data;VADisplay dpy=((AVVAAPIDeviceContext*)fc->device_ctx->hwctx)->display;VASurfaceID s=(VASurfaceID)(uintptr_t)f->data[3];vaSyncSurface(dpy,s);
        VADRMPRIMESurfaceDescriptor d{};vaExportSurfaceHandle(dpy,s,VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,VA_EXPORT_SURFACE_READ_ONLY|VA_EXPORT_SURFACE_SEPARATE_LAYERS,&d);
        // ---- motion ----
        VkImage yI;VkDeviceMemory yM;VkImageView yV;importPlane(d,0,VK_FORMAT_R8_UNORM,W,H,yI,yM,yV);
        auto m0=clk::now();
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=pool;da.descriptorSetCount=1;da.pSetLayouts=&mDsl;VkDescriptorSet mset;vkAllocateDescriptorSets(g.dv,&da,&mset);
        VkDescriptorImageInfo di{nn,yV,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkDescriptorBufferInfo b1{prevB,0,VK_WHOLE_SIZE},b2{verdB,0,VK_WHOLE_SIZE};VkWriteDescriptorSet w[3]={{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};w[0].dstSet=mset;w[0].dstBinding=0;w[0].descriptorCount=1;w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[0].pImageInfo=&di;w[1].dstSet=mset;w[1].dstBinding=1;w[1].descriptorCount=1;w[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w[1].pBufferInfo=&b1;w[2].dstSet=mset;w[2].dstBinding=2;w[2].descriptorCount=1;w[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w[2].pBufferInfo=&b2;vkUpdateDescriptorSets(g.dv,3,w,0,0);
        int* vr=(int*)verdMap;vr[0]=0;vr[1]=GW;vr[2]=GH;vr[3]=-1;vr[4]=-1;
        auto bg=barrier(yI);vkBeginCommandBuffer(cb,&bg);VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};ib.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;ib.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;ib.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;ib.srcQueueFamilyIndex=VK_QUEUE_FAMILY_FOREIGN_EXT;ib.dstQueueFamilyIndex=g.qfi;ib.image=yI;ib.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,0,0,0,1,&ib);vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,mPipe);vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,mPL,0,1,&mset,0,0);int pcm[7]={W,H,DS,GW,GH,THR,hasPrev?1:0};vkCmdPushConstants(cb,mPL,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pcm),pcm);vkCmdDispatch(cb,(GW+15)/16,(GH+15)/16,1);vkEndCommandBuffer(cb);submit();
        tMot+=ms(m0,clk::now());
        bool active=hasPrev&&vr[0]>=minCells;hasPrev=true;if(active)++motion;
        // ---- preprocess + NVIDIA YOLO every frame (apples-to-apples with the single-GPU baseline) ----
        {
            VkImage uvI;VkDeviceMemory uvM;VkImageView uvV;importPlane(d,1,VK_FORMAT_R8G8_UNORM,W/2,H/2,uvI,uvM,uvV);
            auto p0=clk::now();
            VkDescriptorSetAllocateInfo pa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};pa.descriptorPool=pool;pa.descriptorSetCount=1;pa.pSetLayouts=&pDsl;VkDescriptorSet pset;vkAllocateDescriptorSets(g.dv,&pa,&pset);
            VkDescriptorImageInfo yi{lin,yV,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},ui{lin,uvV,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkDescriptorBufferInfo ow{chwB,0,VK_WHOLE_SIZE};VkWriteDescriptorSet pw[3]={{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};pw[0].dstSet=pset;pw[0].dstBinding=0;pw[0].descriptorCount=1;pw[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;pw[0].pImageInfo=&yi;pw[1].dstSet=pset;pw[1].dstBinding=1;pw[1].descriptorCount=1;pw[1].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;pw[1].pImageInfo=&ui;pw[2].dstSet=pset;pw[2].dstBinding=2;pw[2].descriptorCount=1;pw[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;pw[2].pBufferInfo=&ow;vkUpdateDescriptorSets(g.dv,3,pw,0,0);
            auto bg2=barrier(uvI);vkBeginCommandBuffer(cb,&bg2);VkImageMemoryBarrier ib2[2];for(int i=0;i<2;i++){ib2[i]={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};ib2[i].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;ib2[i].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;ib2[i].newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;ib2[i].srcQueueFamilyIndex=VK_QUEUE_FAMILY_FOREIGN_EXT;ib2[i].dstQueueFamilyIndex=g.qfi;ib2[i].image=i?uvI:yI;ib2[i].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,0,0,0,2,ib2);vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pPipe);vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pPL,0,1,&pset,0,0);struct{int sw,sh,net;float sc;int px,py;}pcp{W,H,NET,scale,padx,pady};vkCmdPushConstants(cb,pPL,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pcp),&pcp);vkCmdDispatch(cb,(NET+15)/16,(NET+15)/16,1);vkEndCommandBuffer(cb);submit();
            tPre+=ms(p0,clk::now());
            // ---- HANDOFF + NVIDIA inference: run the host CHW through ORT CUDA YOLO ----
            auto i0=clk::now();
            int64_t shp[4]={1,3,NET,NET};
            Ort::Value inT=Ort::Value::CreateTensor<float>(cpuMem,(float*)chwMap,(size_t)3*NET*NET,shp,4);
            const char* inN[]={inNm.c_str()};const char* outN[]={outNm.c_str()};
            auto outs=ortSess.Run(Ort::RunOptions{nullptr},inN,&inT,1,outN,1);
            auto sh=outs[0].GetTensorTypeAndShapeInfo().GetShape();
            const float* od=outs[0].GetTensorData<float>();
            int num=sh.size()==3?(int)sh[1]:(int)sh[0];
            for(int r=0;r<num;r++) if(od[r*6+4]>0.25f) ++dets;   // [1,300,6] NMS-free head
            tInf+=ms(i0,clk::now());++infers;
            vkDestroyImageView(g.dv,uvV,0);vkDestroyImage(g.dv,uvI,0);vkFreeMemory(g.dv,uvM,0);vkFreeDescriptorSets(g.dv,pool,1,&pset);
        }
        vkDestroyImageView(g.dv,yV,0);vkDestroyImage(g.dv,yI,0);vkFreeMemory(g.dv,yM,0);vkFreeDescriptorSets(g.dv,pool,1,&mset);
        for(uint32_t k=0;k<d.num_objects;k++)close(d.objects[k].fd);
        ++frames;
    };
    process(fr);av_frame_unref(fr);
    while(frames<MAXF&&av_read_frame(fmt,pk)>=0){if(pk->stream_index==vs&&avcodec_send_packet(dc,pk)==0){auto dd=clk::now();while(frames<MAXF&&avcodec_receive_frame(dc,fr)==0){tDec+=ms(dd,clk::now());if(fr->format==AV_PIX_FMT_VAAPI)process(fr);av_frame_unref(fr);dd=clk::now();}}av_packet_unref(pk);}
    double wall=ms(t_all0,clk::now());
    printf("\n=== END-TO-END MIXED pipeline (iGPU decode+motion+preprocess -> NVIDIA YOLO) ===\n");
    printf("frames=%d  motion=%d (%.0f%%)  detections=%ld\n",frames,motion,frames?100.0*motion/frames:0,dets);
    printf("--- iGPU (AMD) ---\n");
    printf("  motion (Vulkan)     : %.2f ms/frame\n",frames?tMot/frames:0);
    printf("  preprocess (Vulkan) : %.2f ms/frame\n",infers?tPre/infers:0);
    printf("--- handoff + NVIDIA ---\n");
    printf("  CHW handoff payload : %.2f MB/frame\n",3.0*NET*NET*4/1e6);
    printf("  YOLO (ORT CUDA,host): %.2f ms/frame (upload+infer)\n",infers?tInf/infers:0);
    printf("--- end-to-end ---\n");
    printf("  MIXED throughput    : %.1f fps (%.0f ms total, sequential)\n",frames?frames*1000.0/wall:0,wall);
    printf("  (single-GPU 720p baseline for comparison: 404 fps)\n");
    return 0;
}
