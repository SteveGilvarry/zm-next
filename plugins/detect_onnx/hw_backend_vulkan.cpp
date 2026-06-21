#include "hw_backend.hpp"

// Vulkan HwBackend — the fully-GPU-resident path for AMD/Intel iGPUs and discrete
// AMD, validated on the RADV iGPU (see bench/vk/). Unlike the VAAPI backend (which
// downloads the grid and diffs on the CPU), this keeps the surface on the GPU the
// whole way: VAAPI decode -> dma_buf import into Vulkan -> compute-shader motion
// diff (20B verdict) + NV12->CHW preprocess -> ncnn-Vulkan YOLO inference. One
// Vulkan codebase covers AMD (iGPU+discrete), Intel, and even NVIDIA.
//
// Validated standalone in bench/vk: 1b (import), 1b+ (motion gate, 0 mismatches),
// 1c (preprocess), Phase 3 (ncnn-Vulkan inference). This file wires them together.
//
// HONEST STATUS (WIP — does not yet build): the proven kernels are consolidated
// here behind the HwBackend interface, but ncnn ships its OWN bundled Vulkan
// loader (ncnn/simplevk.h) whose macros collide with the system <vulkan/vulkan.h>
// we use for the import/compute. The fix is a translation-unit split: keep the
// raw-Vulkan VkCtx (import + motion + preprocess) in one TU that includes
// vulkan.h, and the ncnn inference in a separate TU that includes ncnn headers,
// communicating via the host CHW buffer. Until then this file is a reference for
// the consolidation; the *validated* Vulkan path lives in bench/vk/ (1b/1b+/1c +
// the ncnn-Vulkan inference prototype), all proven on the RADV iGPU.
//
// The CHW handoff to ncnn also bounces through host memory (a 4.7MB copy) because
// ncnn owns a separate VkDevice; the full zero-copy hand-off (preprocess writing
// into an ncnn::VkMat on ncnn's device) is the follow-up after the TU split.

#if defined(ZM_WITH_VULKAN)

#include "detect_postprocess.hpp"
#include <vulkan/vulkan.h>
#include <ncnn/net.h>
#include <ncnn/mat.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
}
#include <va/va.h>
#include <va/va_drmcommon.h>

#ifndef ZM_HW_VAAPI
#define ZM_HW_VAAPI 2u
#endif

namespace zm::hw {
namespace {

#define VKOK(x) do{ if((x)!=VK_SUCCESS) return false; }while(0)

// ---- shared SPIR-V (compiled from bench/vk/*.comp; embedded path at runtime) ----
static std::vector<uint32_t> load_spv(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb"); if (!f) return {};
    std::fseek(f,0,SEEK_END); long n=std::ftell(f); std::fseek(f,0,SEEK_SET);
    std::vector<uint32_t> v(n/4); if(std::fread(v.data(),1,n,f)!=(size_t)n){} std::fclose(f); return v;
}
static uint32_t mem_index(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd,&mp);
    for (uint32_t i=0;i<mp.memoryTypeCount;i++) if ((bits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&want)==want) return i;
    return 0;
}

// Minimal Vulkan compute context on the chosen physical device, owning the two
// pipelines (motion diff, nv12->chw) and the persistent gate buffers.
struct VkCtx {
    VkInstance inst=0; VkPhysicalDevice pd=0; VkDevice dev=0; VkQueue queue=0; uint32_t qfi=0;
    PFN_vkGetMemoryFdPropertiesKHR fdProps=0;
    VkCommandPool cmdPool=0; VkCommandBuffer cmd=0;
    VkSampler nearest=0, linear=0;
    // motion pipeline
    VkPipeline mPipe=0; VkPipelineLayout mLayout=0; VkDescriptorSetLayout mDsl=0;
    // preprocess pipeline
    VkPipeline pPipe=0; VkPipelineLayout pLayout=0; VkDescriptorSetLayout pDsl=0;
    VkDescriptorPool pool=0;
    // gate state buffers (prev grid, verdict, chw out)
    VkBuffer prevB=0,verdB=0,chwB=0; VkDeviceMemory prevM=0,verdM=0,chwM=0; void* verdMap=0; void* chwMap=0;
    int gw=0, gh=0, net=0;

    bool make_buf(VkDeviceSize sz, VkBuffer& b, VkDeviceMemory& m, void** map) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=sz; bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VKOK(vkCreateBuffer(dev,&bi,nullptr,&b)); VkMemoryRequirements r; vkGetBufferMemoryRequirements(dev,b,&r);
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; a.allocationSize=r.size;
        a.memoryTypeIndex=mem_index(pd,r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VKOK(vkAllocateMemory(dev,&a,nullptr,&m)); VKOK(vkBindBufferMemory(dev,b,m,0));
        if (map) VKOK(vkMapMemory(dev,m,0,sz,0,map)); return true;
    }
    VkPipeline make_pipe(const std::string& spvPath, VkDescriptorSetLayout dsl, uint32_t pcBytes, VkPipelineLayout& outLayout) {
        auto code=load_spv(spvPath); if (code.empty()) return 0;
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize=code.size()*4; smi.pCode=code.data();
        VkShaderModule sm; if (vkCreateShaderModule(dev,&smi,nullptr,&sm)!=VK_SUCCESS) return 0;
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT,0,pcBytes};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount=1; pli.pSetLayouts=&dsl; pli.pushConstantRangeCount=1; pli.pPushConstantRanges=&pcr;
        vkCreatePipelineLayout(dev,&pli,nullptr,&outLayout);
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cpi.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cpi.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module=sm; cpi.stage.pName="main"; cpi.layout=outLayout;
        VkPipeline p=0; vkCreateComputePipelines(dev,VK_NULL_HANDLE,1,&cpi,nullptr,&p); return p;
    }

    bool init(const std::string& shaderDir, int width, int height, int ds, int net_) {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_2;
        VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ii.pApplicationInfo=&app; VKOK(vkCreateInstance(&ii,nullptr,&inst));
        uint32_t n=0; vkEnumeratePhysicalDevices(inst,&n,nullptr); std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(inst,&n,pds.data());
        for (auto p:pds){VkPhysicalDeviceProperties r;vkGetPhysicalDeviceProperties(p,&r);if(r.vendorID==0x1002&&r.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU){pd=p;break;}}
        if(!pd) for(auto p:pds){VkPhysicalDeviceProperties r;vkGetPhysicalDeviceProperties(p,&r);if(r.vendorID==0x1002){pd=p;break;}}
        if(!pd && n) pd=pds[0];
        if(!pd) return false;
        uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,nullptr); std::vector<VkQueueFamilyProperties> qf(qn); vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,qf.data());
        for(uint32_t i=0;i<qn;i++) if(qf[i].queueFlags&VK_QUEUE_COMPUTE_BIT){qfi=i;break;}
        const char* ex[]={"VK_KHR_external_memory","VK_KHR_external_memory_fd","VK_EXT_external_memory_dma_buf","VK_EXT_image_drm_format_modifier","VK_EXT_queue_family_foreign"};
        float pr=1; VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qc.queueFamilyIndex=qfi; qc.queueCount=1; qc.pQueuePriorities=&pr;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qc; dci.enabledExtensionCount=5; dci.ppEnabledExtensionNames=ex;
        VKOK(vkCreateDevice(pd,&dci,nullptr,&dev)); vkGetDeviceQueue(dev,qfi,0,&queue);
        fdProps=(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev,"vkGetMemoryFdPropertiesKHR");

        gw=width/ds; gh=height/ds; net=net_;
        VkSamplerCreateInfo s0{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; s0.magFilter=s0.minFilter=VK_FILTER_NEAREST; s0.addressModeU=s0.addressModeV=s0.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; vkCreateSampler(dev,&s0,nullptr,&nearest);
        VkSamplerCreateInfo s1=s0; s1.magFilter=s1.minFilter=VK_FILTER_LINEAR; vkCreateSampler(dev,&s1,nullptr,&linear);

        // descriptor layouts
        VkDescriptorSetLayoutBinding mb[3]={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,0},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,0},{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,0}};
        VkDescriptorSetLayoutCreateInfo ml{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; ml.bindingCount=3; ml.pBindings=mb; vkCreateDescriptorSetLayout(dev,&ml,nullptr,&mDsl);
        VkDescriptorSetLayoutBinding pb[3]={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,0},{1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,0},{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,0}};
        VkDescriptorSetLayoutCreateInfo plc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; plc.bindingCount=3; plc.pBindings=pb; vkCreateDescriptorSetLayout(dev,&plc,nullptr,&pDsl);
        mPipe=make_pipe(shaderDir+"/motion_diff_img.spv",mDsl,7*4,mLayout);
        pPipe=make_pipe(shaderDir+"/nv12_to_chw.spv",pDsl,6*4,pLayout);
        if(!mPipe||!pPipe) return false;

        VkDescriptorPoolSize ps[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,512},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,512}};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets=512; dp.poolSizeCount=2; dp.pPoolSizes=ps; dp.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; vkCreateDescriptorPool(dev,&dp,nullptr,&pool);
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cp.queueFamilyIndex=qfi; cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; vkCreateCommandPool(dev,&cp,nullptr,&cmdPool);
        VkCommandBufferAllocateInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cb.commandPool=cmdPool; cb.commandBufferCount=1; vkAllocateCommandBuffers(dev,&cb,&cmd);
        if(!make_buf((VkDeviceSize)gw*gh*4,prevB,prevM,nullptr)) return false;
        if(!make_buf(5*4,verdB,verdM,&verdMap)) return false;
        if(!make_buf((VkDeviceSize)3*net*net*4,chwB,chwM,&chwMap)) return false;
        return true;
    }

    // import VA plane (layer L) as `fmt` image of pw x ph; caller destroys.
    bool import_plane(VADRMPRIMESurfaceDescriptor& d,int L,VkFormat fmt,int pw,int ph,VkImage& img,VkDeviceMemory& mem,VkImageView& view){
        uint32_t o=d.layers[L].object_index[0];
        VkSubresourceLayout sl{}; sl.offset=d.layers[L].offset[0]; sl.rowPitch=d.layers[L].pitch[0];
        VkImageDrmFormatModifierExplicitCreateInfoEXT mi{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT}; mi.drmFormatModifier=d.objects[o].drm_format_modifier; mi.drmFormatModifierPlaneCount=1; mi.pPlaneLayouts=&sl;
        VkExternalMemoryImageCreateInfo em{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO}; em.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; em.pNext=&mi;
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ic.pNext=&em; ic.imageType=VK_IMAGE_TYPE_2D; ic.format=fmt; ic.extent={(uint32_t)pw,(uint32_t)ph,1}; ic.mipLevels=1; ic.arrayLayers=1; ic.samples=VK_SAMPLE_COUNT_1_BIT; ic.tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT; ic.usage=VK_IMAGE_USAGE_SAMPLED_BIT; ic.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ic.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        VKOK(vkCreateImage(dev,&ic,nullptr,&img));
        VkImageMemoryRequirementsInfo2 ri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2}; ri.image=img; VkMemoryRequirements2 mr{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2}; vkGetImageMemoryRequirements2(dev,&ri,&mr);
        int fd=dup(d.objects[o].fd); VkMemoryFdPropertiesKHR fp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR}; fdProps(dev,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,fd,&fp);
        VkImportMemoryFdInfoKHR im{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR}; im.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; im.fd=fd;
        VkMemoryDedicatedAllocateInfo de{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; de.image=img; de.pNext=&im;
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.pNext=&de; ma.allocationSize=mr.memoryRequirements.size; ma.memoryTypeIndex=mem_index(pd,mr.memoryRequirements.memoryTypeBits&fp.memoryTypeBits,0);
        VKOK(vkAllocateMemory(dev,&ma,nullptr,&mem)); VKOK(vkBindImageMemory(dev,img,mem,0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=img; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=fmt; vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; VKOK(vkCreateImageView(dev,&vi,nullptr,&view));
        return true;
    }
};

class VulkanBackend : public HwBackend {
public:
    const char* name() const override { return "vulkan"; }

    bool load_model(const std::string& path, int net) override {
        net_ = net;
        // ncnn-Vulkan YOLO head model. Convention: <path-without-ext>.ncnn.param/.bin
        std::string base = path; auto dot=base.find_last_of('.'); if(dot!=std::string::npos) base=base.substr(0,dot);
        ncnn_.opt.use_vulkan_compute = true;
        if (ncnn_.load_param((base+".ncnn.param").c_str())) return false;
        if (ncnn_.load_model((base+".ncnn.bin").c_str())) return false;
        modelOK_ = true;
        return true;   // VkCtx is lazily init'd on the first frame (needs dims)
    }

    Surface acquire(uint64_t av_frame) override {
        Surface s; AVFrame* src=reinterpret_cast<AVFrame*>(av_frame); if(!src) return s;
        AVFrame* held=av_frame_clone(src); if(!held) return s;
        s.owner=held; s.hw_type=ZM_HW_VAAPI; s.pix_fmt=(uint32_t)AV_PIX_FMT_VAAPI; s.width=held->width; s.height=held->height;
        s.native=(uint64_t)(uintptr_t)held->data[3];
        return s;
    }
    void release(Surface& s) override { if(s.owner){AVFrame* f=(AVFrame*)s.owner; av_frame_free(&f); s.owner=nullptr;} }

    std::vector<Region> motion(const Surface& s) override {
        AVFrame* in=(AVFrame*)s.owner; if(!in) return {};
        if(!ensure_ctx(s.width,s.height)) return {};
        VADRMPRIMESurfaceDescriptor d; if(!export_surface(in,d)) return {};
        VkImage img; VkDeviceMemory mem; VkImageView view;
        bool ok=ctx_.import_plane(d,0,VK_FORMAT_R8_UNORM,s.width,s.height,img,mem,view);
        for(uint32_t k=0;k<d.num_objects;k++) close(d.objects[k].fd);
        if(!ok) return {};
        // verdict reset
        int* v=(int*)ctx_.verdMap; v[0]=0; v[1]=ctx_.gw; v[2]=ctx_.gh; v[3]=-1; v[4]=-1;
        run_motion(img,view);
        vkDestroyImageView(ctx_.dev,view,nullptr); vkDestroyImage(ctx_.dev,img,nullptr); vkFreeMemory(ctx_.dev,mem,nullptr);
        int cnt=v[0]; bool active = hasPrev_ && cnt>=minCells_; hasPrev_=true;
        if(!active) return {};
        Region r; r.x=v[1]*ds_; r.y=v[2]*ds_; r.w=std::min((v[3]+1)*ds_,s.width)-r.x; r.h=std::min((v[4]+1)*ds_,s.height)-r.y;
        return { r };
    }

    DeviceTensor preprocess(const Surface& s, Region /*crop*/) override {
        DeviceTensor t; t.net=net_; AVFrame* in=(AVFrame*)s.owner; if(!in||!ensure_ctx(s.width,s.height)) return t;
        VADRMPRIMESurfaceDescriptor d; if(!export_surface(in,d)) return t;
        VkImage yI,uvI; VkDeviceMemory yM,uvM; VkImageView yV,uvV;
        bool ok = ctx_.import_plane(d,0,VK_FORMAT_R8_UNORM,s.width,s.height,yI,yM,yV)
               && ctx_.import_plane(d,1,VK_FORMAT_R8G8_UNORM,s.width/2,s.height/2,uvI,uvM,uvV);
        for(uint32_t k=0;k<d.num_objects;k++) close(d.objects[k].fd);
        if(!ok) return t;
        t.lb = zm::detect::compute_letterbox(s.width,s.height,net_);
        run_preprocess(yV,uvV,s.width,s.height,t.lb);
        vkDestroyImageView(ctx_.dev,yV,nullptr); vkDestroyImage(ctx_.dev,yI,nullptr); vkFreeMemory(ctx_.dev,yM,nullptr);
        vkDestroyImageView(ctx_.dev,uvV,nullptr); vkDestroyImage(ctx_.dev,uvI,nullptr); vkFreeMemory(ctx_.dev,uvM,nullptr);
        t.ptr = ctx_.chwMap;   // host CHW (ncnn-handoff bounce; VkMat zero-copy is the TODO)
        return t;
    }

    std::vector<Detection> infer(const DeviceTensor& t, float conf, const std::vector<int>& allow) override {
        if(!t.ptr||!modelOK_) return {};
        // wrap our CHW (3,net,net) as an ncnn Mat; ncnn uploads to its Vulkan device.
        ncnn::Mat in(net_, net_, 3, (void*)t.ptr); // w,h,c sharing our buffer
        ncnn::Extractor ex=ncnn_.create_extractor();
        ex.input("in0", in);
        ncnn::Mat out; ex.extract("out0", out);   // [84, 8400] head
        std::vector<Detection> dets;
        const int C=out.h, A=out.w;  // C channels (84), A anchors
        for (int a=0; a<A; ++a) {
            float bestS=0; int bestC=-1;
            for (int c=4; c<C; ++c){ float sc=out.row(c)[a]; if(sc>bestS){bestS=sc;bestC=c-4;} }
            if (bestS<conf) continue;
            if (!allow.empty() && std::find(allow.begin(),allow.end(),bestC)==allow.end()) continue;
            float cx=out.row(0)[a],cy=out.row(1)[a],w=out.row(2)[a],h=out.row(3)[a];
            Detection b; b.x=(cx-w/2 - t.lb.pad_x)/t.lb.scale; b.y=(cy-h/2 - t.lb.pad_y)/t.lb.scale;
            b.w=w/t.lb.scale; b.h=h/t.lb.scale; b.confidence=bestS; b.class_id=bestC;
            dets.push_back(b);
        }
        return dets;  // NOTE: caller/tracker handles NMS (parity with detect_onnx flow)
    }

private:
    static bool export_surface(AVFrame* f, VADRMPRIMESurfaceDescriptor& d){
        AVHWFramesContext* fc=(AVHWFramesContext*)f->hw_frames_ctx->data; VADisplay dpy=((AVVAAPIDeviceContext*)fc->device_ctx->hwctx)->display;
        VASurfaceID s=(VASurfaceID)(uintptr_t)f->data[3]; vaSyncSurface(dpy,s);
        return vaExportSurfaceHandle(dpy,s,VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,VA_EXPORT_SURFACE_READ_ONLY|VA_EXPORT_SURFACE_SEPARATE_LAYERS,&d)==VA_STATUS_SUCCESS;
    }
    bool ensure_ctx(int w,int h){ if(ctxOK_) return true; const char* sd=std::getenv("ZM_VK_SHADER_DIR"); ctxOK_=ctx_.init(sd?sd:"bench/vk",w,h,ds_,net_); minCells_=std::max(8,(w/ds_)*(h/ds_)/400); return ctxOK_; }

    void run_motion(VkImage img, VkImageView view){
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool=ctx_.pool; da.descriptorSetCount=1; da.pSetLayouts=&ctx_.mDsl; VkDescriptorSet set; vkAllocateDescriptorSets(ctx_.dev,&da,&set);
        VkDescriptorImageInfo di{ctx_.nearest,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; VkDescriptorBufferInfo b1{ctx_.prevB,0,VK_WHOLE_SIZE},b2{ctx_.verdB,0,VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[3]={{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstSet=set;w[0].dstBinding=0;w[0].descriptorCount=1;w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[0].pImageInfo=&di;
        w[1].dstSet=set;w[1].dstBinding=1;w[1].descriptorCount=1;w[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w[1].pBufferInfo=&b1;
        w[2].dstSet=set;w[2].dstBinding=2;w[2].descriptorCount=1;w[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w[2].pBufferInfo=&b2;
        vkUpdateDescriptorSets(ctx_.dev,3,w,0,0);
        int pc[7]={ctx_.net? (ctx_.gw*ds_):0, 0, ds_, ctx_.gw, ctx_.gh, thr_, hasPrev_?1:0};
        pc[0]=ctx_.gw*ds_; pc[1]=ctx_.gh*ds_; // w,h
        dispatch(ctx_.mPipe,ctx_.mLayout,set,pc,sizeof(pc),(ctx_.gw+15)/16,(ctx_.gh+15)/16,img);
        vkFreeDescriptorSets(ctx_.dev,ctx_.pool,1,&set);
    }
    void run_preprocess(VkImageView yV, VkImageView uvV, int w, int h, const zm::detect::Letterbox& lb){
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool=ctx_.pool; da.descriptorSetCount=1; da.pSetLayouts=&ctx_.pDsl; VkDescriptorSet set; vkAllocateDescriptorSets(ctx_.dev,&da,&set);
        VkDescriptorImageInfo y{ctx_.linear,yV,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},u{ctx_.linear,uvV,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; VkDescriptorBufferInfo ow{ctx_.chwB,0,VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[3]={{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstSet=set;w[0].dstBinding=0;w[0].descriptorCount=1;w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[0].pImageInfo=&y;
        w[1].dstSet=set;w[1].dstBinding=1;w[1].descriptorCount=1;w[1].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[1].pImageInfo=&u;
        w[2].dstSet=set;w[2].dstBinding=2;w[2].descriptorCount=1;w[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w[2].pBufferInfo=&ow;
        vkUpdateDescriptorSets(ctx_.dev,3,w,0,0);
        struct { int sw,sh,net; float scale; int px,py; } pc{w,h,net_,lb.scale,lb.pad_x,lb.pad_y};
        VkImageView dummy=yV; (void)dummy;
        dispatch(ctx_.pPipe,ctx_.pLayout,set,&pc,sizeof(pc),(net_+15)/16,(net_+15)/16,VK_NULL_HANDLE,yV,uvV);
        vkFreeDescriptorSets(ctx_.dev,ctx_.pool,1,&set);
    }
    void dispatch(VkPipeline pipe,VkPipelineLayout layout,VkDescriptorSet set,void* pc,uint32_t pcsz,int gx,int gy,VkImage img,VkImageView extra0=VK_NULL_HANDLE,VkImageView extra1=VK_NULL_HANDLE){
        VkCommandBufferBeginInfo bg{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bg.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; vkBeginCommandBuffer(ctx_.cmd,&bg);
        // acquire imported image(s) from the foreign (VAAPI) queue
        std::vector<VkImageMemoryBarrier> bars;
        auto addbar=[&](VkImage im){ if(!im) return; VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT; b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_FOREIGN_EXT; b.dstQueueFamilyIndex=ctx_.qfi; b.image=im; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; bars.push_back(b); };
        addbar(img);
        if(bars.size()) vkCmdPipelineBarrier(ctx_.cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,0,0,0,(uint32_t)bars.size(),bars.data());
        vkCmdBindPipeline(ctx_.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipe); vkCmdBindDescriptorSets(ctx_.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,0);
        vkCmdPushConstants(ctx_.cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,pcsz,pc);
        vkCmdDispatch(ctx_.cmd,gx,gy,1); vkEndCommandBuffer(ctx_.cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&ctx_.cmd; vkQueueSubmit(ctx_.queue,1,&si,VK_NULL_HANDLE); vkQueueWaitIdle(ctx_.queue);
    }

    VkCtx ctx_; bool ctxOK_=false, modelOK_=false, hasPrev_=false;
    ncnn::Net ncnn_;
    int net_=640, ds_=8, thr_=18, minCells_=8;
};

}  // namespace

std::unique_ptr<HwBackend> make_vulkan_backend() { return std::make_unique<VulkanBackend>(); }

}  // namespace zm::hw

#endif  // ZM_WITH_VULKAN
