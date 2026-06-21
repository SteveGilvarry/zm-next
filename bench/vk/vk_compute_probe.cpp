// Milestone 1a: prove Vulkan compute runs correctly on the RADV iGPU.
// Selects the integrated AMD device, runs motion_downsample.comp on a host-filled
// luma buffer, reads the grid back, and checks it matches a CPU downsample.
// (1b will replace the input buffer with a VkImage imported from a VAAPI dma_buf.)
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

#define VK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "VK fail %d at %s:%d\n", _r, __FILE__, __LINE__); exit(1);} } while(0)

static std::vector<uint32_t> load_spv(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) { fprintf(stderr,"open spv %s failed\n",path); exit(1); }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    std::vector<uint32_t> v(n/4); fread(v.data(),1,n,f); fclose(f); return v;
}

static uint32_t find_mem(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd,&mp);
    for (uint32_t i=0;i<mp.memoryTypeCount;i++)
        if ((bits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&want)==want) return i;
    fprintf(stderr,"no mem type\n"); exit(1);
}

struct Buf { VkBuffer buf; VkDeviceMemory mem; void* map; };
static Buf make_buf(VkPhysicalDevice pd, VkDevice dev, VkDeviceSize sz) {
    Buf b{};
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size=sz; bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VK(vkCreateBuffer(dev,&bi,nullptr,&b.buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev,b.buf,&mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize=mr.size;
    ai.memoryTypeIndex=find_mem(pd,mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK(vkAllocateMemory(dev,&ai,nullptr,&b.mem));
    VK(vkBindBufferMemory(dev,b.buf,b.mem,0));
    VK(vkMapMemory(dev,b.mem,0,sz,0,&b.map));
    return b;
}

int main() {
    const int W=3840, H=2160, DS=8;
    const int GW=W/DS, GH=H/DS;

    // --- instance ---
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app;
    VkInstance inst; VK(vkCreateInstance(&ici,nullptr,&inst));

    // --- pick the integrated AMD (RADV) device ---
    uint32_t n=0; vkEnumeratePhysicalDevices(inst,&n,nullptr);
    std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(inst,&n,pds.data());
    VkPhysicalDevice pd=VK_NULL_HANDLE; std::string picked;
    for (auto p:pds){ VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(p,&pr);
        if (pr.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && pr.vendorID==0x1002){pd=p;picked=pr.deviceName;break;}}
    if (!pd){ // fallback: any AMD
        for (auto p:pds){ VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(p,&pr);
            if (pr.vendorID==0x1002){pd=p;picked=pr.deviceName;break;}}}
    if (!pd){ fprintf(stderr,"no AMD device found\n"); return 1; }
    printf("device: %s\n", picked.c_str());

    // --- compute queue family ---
    uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn); vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,qf.data());
    uint32_t qfi=UINT32_MAX;
    for (uint32_t i=0;i<qn;i++) if (qf[i].queueFlags&VK_QUEUE_COMPUTE_BIT){qfi=i;break;}
    if (qfi==UINT32_MAX){ fprintf(stderr,"no compute queue\n"); return 1; }

    float prio=1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex=qfi; qci.queueCount=1; qci.pQueuePriorities=&prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
    VkDevice dev; VK(vkCreateDevice(pd,&dci,nullptr,&dev));
    VkQueue queue; vkGetDeviceQueue(dev,qfi,0,&queue);

    // --- buffers (UMA: host-visible is device-local on the iGPU) ---
    Buf in  = make_buf(pd,dev,(VkDeviceSize)W*H*4);
    Buf out = make_buf(pd,dev,(VkDeviceSize)GW*GH*4);
    uint32_t* ip=(uint32_t*)in.map;
    // synthetic luma: gradient so a point-sample is easy to predict
    for (int y=0;y<H;y++) for (int x=0;x<W;x++) ip[y*W+x]=(uint32_t)((x*7+y*13)&0xFF);

    // --- pipeline ---
    auto spv=load_spv("/home/steve/code/zm-next/bench/vk/motion_downsample.spv");
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize=spv.size()*4; smi.pCode=spv.data();
    VkShaderModule sm; VK(vkCreateShaderModule(dev,&smi,nullptr,&sm));

    VkDescriptorSetLayoutBinding b0{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutBinding b1{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutBinding bs[2]={b0,b1};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount=2; dl.pBindings=bs;
    VkDescriptorSetLayout dsl; VK(vkCreateDescriptorSetLayout(dev,&dl,nullptr,&dsl));

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT,0,5*sizeof(int)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount=1; pli.pSetLayouts=&dsl; pli.pushConstantRangeCount=1; pli.pPushConstantRanges=&pcr;
    VkPipelineLayout pl; VK(vkCreatePipelineLayout(dev,&pli,nullptr,&pl));

    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module=sm; cpi.stage.pName="main";
    cpi.layout=pl;
    VkPipeline pipe; VK(vkCreateComputePipelines(dev,VK_NULL_HANDLE,1,&cpi,nullptr,&pipe));

    // --- descriptors ---
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets=1; dpi.poolSizeCount=1; dpi.pPoolSizes=&ps;
    VkDescriptorPool dp; VK(vkCreateDescriptorPool(dev,&dpi,nullptr,&dp));
    VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dsa.descriptorPool=dp; dsa.descriptorSetCount=1; dsa.pSetLayouts=&dsl;
    VkDescriptorSet ds; VK(vkAllocateDescriptorSets(dev,&dsa,&ds));
    VkDescriptorBufferInfo bi0{in.buf,0,VK_WHOLE_SIZE}, bi1{out.buf,0,VK_WHOLE_SIZE};
    VkWriteDescriptorSet w0{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, w1=w0;
    w0.dstSet=ds; w0.dstBinding=0; w0.descriptorCount=1; w0.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w0.pBufferInfo=&bi0;
    w1.dstSet=ds; w1.dstBinding=1; w1.descriptorCount=1; w1.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w1.pBufferInfo=&bi1;
    VkWriteDescriptorSet ws[2]={w0,w1}; vkUpdateDescriptorSets(dev,2,ws,0,nullptr);

    // --- command buffer ---
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpci.queueFamilyIndex=qfi;
    VkCommandPool cp; VK(vkCreateCommandPool(dev,&cpci,nullptr,&cp));
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cbi.commandPool=cp; cbi.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount=1;
    VkCommandBuffer cb; VK(vkAllocateCommandBuffers(dev,&cbi,&cb));
    VkCommandBufferBeginInfo cbb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; cbb.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK(vkBeginCommandBuffer(cb,&cbb));
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&ds,0,nullptr);
    int pcv[5]={W,H,DS,GW,GH}; vkCmdPushConstants(cb,pl,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pcv),pcv);
    vkCmdDispatch(cb,(GW+15)/16,(GH+15)/16,1);
    VK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&cb;
    VK(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE));
    VK(vkQueueWaitIdle(queue));

    // --- verify vs CPU downsample ---
    uint32_t* op=(uint32_t*)out.map;
    long mism=0; uint32_t firstbad_gpu=0,firstbad_cpu=0; int bx=-1,by=-1;
    for (int j=0;j<GH;j++) for (int i=0;i<GW;i++){
        int sx=i*DS; if(sx>W-1)sx=W-1; int sy=j*DS; if(sy>H-1)sy=H-1;
        uint32_t cpu=(uint32_t)((sx*7+sy*13)&0xFF);
        uint32_t gpu=op[j*GW+i];
        if (cpu!=gpu){ if(!mism){firstbad_gpu=gpu;firstbad_cpu=cpu;bx=i;by=j;} ++mism; }
    }
    printf("grid %dx%d (%d cells): %ld mismatches\n", GW,GH,GW*GH, mism);
    if (mism) printf("  first @ (%d,%d): gpu=%u cpu=%u\n",bx,by,firstbad_gpu,firstbad_cpu);
    printf(mism? "FAIL\n" : "PASS — Vulkan compute on the iGPU matches the CPU downsample\n");
    return mism? 1:0;
}
