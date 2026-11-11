#include <algorithm>
#include <cmath>
// Offline harness for the SoD-RT helper's shadow kernel (rtmask.comp): runs it on an SDMK file with the same
// acceleration-structure layout the helper builds (one BLAS geometry per caster, not opaque,
// NO_DUPLICATE_ANY_HIT, one TLAS instance with facing-cull disabled) and writes the mask back into the file.
// Linux + any Vulkan driver with VK_KHR_ray_query (lavapipe here). Build:
//   g++ -O2 -std=c++17 -I<dxvk>/include/vulkan/include rt_harness.cpp -ldl -o rt_harness
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../../sod-dxvk/src/sod_bridge/sod_bridge_proto.h"
#include "../../sod-dxvk/src/sod_bridge/rtmask_spv.h"

#define F(n) static PFN_##n n
F(vkGetInstanceProcAddr); F(vkCreateInstance); F(vkEnumeratePhysicalDevices); F(vkGetPhysicalDeviceProperties);
F(vkCreateDevice); F(vkGetDeviceProcAddr); F(vkGetPhysicalDeviceMemoryProperties); F(vkGetDeviceQueue);
F(vkCreateBuffer); F(vkGetBufferMemoryRequirements); F(vkAllocateMemory); F(vkBindBufferMemory); F(vkMapMemory);
F(vkGetBufferDeviceAddress); F(vkCreateCommandPool); F(vkAllocateCommandBuffers); F(vkBeginCommandBuffer);
F(vkEndCommandBuffer); F(vkQueueSubmit); F(vkQueueWaitIdle); F(vkCmdPipelineBarrier); F(vkCreateShaderModule);
F(vkCreateDescriptorSetLayout); F(vkCreatePipelineLayout); F(vkCreateComputePipelines); F(vkCreateDescriptorPool);
F(vkAllocateDescriptorSets); F(vkUpdateDescriptorSets); F(vkCmdBindPipeline); F(vkCmdBindDescriptorSets);
F(vkCmdDispatch); F(vkCmdPushConstants); F(vkCmdFillBuffer); F(vkGetAccelerationStructureBuildSizesKHR);
F(vkCreateAccelerationStructureKHR); F(vkCmdBuildAccelerationStructuresKHR); F(vkGetAccelerationStructureDeviceAddressKHR);
#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s -> %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static VkDevice dev; static VkPhysicalDeviceMemoryProperties mp;
struct Buf { VkBuffer b; VkDeviceMemory m; VkDeviceAddress a; void* p; };
static Buf mk(VkDeviceSize size, VkBufferUsageFlags u) {
    Buf r = { };
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO }; bi.size = size; bi.usage = u | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    CK(vkCreateBuffer(dev, &bi, nullptr, &r.b));
    VkMemoryRequirements q; vkGetBufferMemoryRequirements(dev, r.b, &q);
    uint32_t t = 0; for (; t < mp.memoryTypeCount; t++) if ((q.memoryTypeBits >> t & 1) && (mp.memoryTypes[t].propertyFlags & 6) == 6) break;
    VkMemoryAllocateFlagsInfo fl = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO }; fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl }; ai.allocationSize = q.size; ai.memoryTypeIndex = t;
    CK(vkAllocateMemory(dev, &ai, nullptr, &r.m)); CK(vkBindBufferMemory(dev, r.b, r.m, 0));
    CK(vkMapMemory(dev, r.m, 0, VK_WHOLE_SIZE, 0, &r.p));
    VkBufferDeviceAddressInfo da = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO }; da.buffer = r.b; r.a = vkGetBufferDeviceAddress(dev, &da);
    return r;
}
static void barrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d, VkAccessFlags sa, VkAccessFlags dacc) {
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER }; mb.srcAccessMask = sa; mb.dstAccessMask = dacc;
    vkCmdPipelineBarrier(cb, s, d, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: rt_harness file.sdmk\n"); return 1; }
    FILE* fp = fopen(argv[1], "r+b"); if (!fp) return 1;
    uint32_t hdr[6]; fread(hdr, 4, 6, fp);
    uint32_t W = hdr[3], H = hdr[4], gbytes = hdr[5];
    std::vector<uint8_t> geom(gbytes); fread(geom.data(), 1, gbytes, fp);
    std::vector<float> depth(size_t(W) * H); fread(depth.data(), 4, depth.size(), fp);
    long maskOff = ftell(fp);
    const SodGeomFrame& g = *(const SodGeomFrame*)geom.data();
    printf("frame %u %ux%u lights %u casters %u tris %u\n", g.frame, W, H, g.lightCount, g.casterCount, g.triCount);

    void* lib = dlopen("libvulkan.so.1", RTLD_NOW); vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(nullptr, "vkCreateInstance");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO }; app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ici.pApplicationInfo = &app;
    VkInstance inst; CK(vkCreateInstance(&ici, nullptr, &inst));
#define LI(n) n = (PFN_##n)vkGetInstanceProcAddr(inst, #n)
    LI(vkEnumeratePhysicalDevices); LI(vkGetPhysicalDeviceProperties); LI(vkCreateDevice); LI(vkGetDeviceProcAddr); LI(vkGetPhysicalDeviceMemoryProperties);
    uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &n, &pd);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp); printf("device %s\n", pp.deviceName);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkPhysicalDeviceRayQueryFeaturesKHR rq = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR }; rq.rayQuery = 1;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rq }; as.accelerationStructure = 1;
    VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &as }; v12.bufferDeviceAddress = 1;
    const char* ext[] = { "VK_KHR_acceleration_structure", "VK_KHR_deferred_host_operations", "VK_KHR_ray_query" };
    float pr = 1; VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO }; qci.queueCount = 1; qci.pQueuePriorities = &pr;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &v12 }; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 3; dci.ppEnabledExtensionNames = ext;
    CK(vkCreateDevice(pd, &dci, nullptr, &dev));
#define LD(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    LD(vkGetDeviceQueue); LD(vkCreateBuffer); LD(vkGetBufferMemoryRequirements); LD(vkAllocateMemory); LD(vkBindBufferMemory); LD(vkMapMemory);
    LD(vkGetBufferDeviceAddress); LD(vkCreateCommandPool); LD(vkAllocateCommandBuffers); LD(vkBeginCommandBuffer); LD(vkEndCommandBuffer);
    LD(vkQueueSubmit); LD(vkQueueWaitIdle); LD(vkCmdPipelineBarrier); LD(vkCreateShaderModule); LD(vkCreateDescriptorSetLayout);
    LD(vkCreatePipelineLayout); LD(vkCreateComputePipelines); LD(vkCreateDescriptorPool); LD(vkAllocateDescriptorSets);
    LD(vkUpdateDescriptorSets); LD(vkCmdBindPipeline); LD(vkCmdBindDescriptorSets); LD(vkCmdDispatch); LD(vkCmdPushConstants);
    LD(vkCmdFillBuffer); LD(vkGetAccelerationStructureBuildSizesKHR); LD(vkCreateAccelerationStructureKHR);
    LD(vkCmdBuildAccelerationStructuresKHR); LD(vkGetAccelerationStructureDeviceAddressKHR);
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

    const VkBufferUsageFlags ASIN = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, SSBO = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    Buf dB = mk(depth.size() * 4, SSBO); memcpy(dB.p, depth.data(), depth.size() * 4);
    Buf rB = mk((size_t(W) * H + 3) & ~size_t(3), SSBO | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    SodGeomFrame gm = g;
    if (getenv("SOFT")) { uint32_t K = atoi(getenv("SOFT")); uint32_t tn = getenv("TAN") ? uint32_t(atof(getenv("TAN")) * 1e4) : 250;
        gm.flags = (g.flags & 1u) | (K ? 2u : 0u) | (K << 8) | (tn << 16); }
    printf("flags 0x%08x\n", gm.flags);
    Buf fB = mk(sizeof(SodGeomFrame), SSBO); memcpy(fB.p, &gm, sizeof gm);
    uint32_t tX = (W + 7) / 8, tY = (H + 7) / 8, nT = tX * tY;
    Buf bitsB = mk((size_t(W) * H + 3) & ~size_t(3), SSBO), softB = mk(size_t(W) * H * 4, SSBO);
    Buf tilB = mk(size_t(nT) * 8, SSBO | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Buf cB = mk(64, SSBO | VK_BUFFER_USAGE_TRANSFER_DST_BIT); memset(cB.p, 0, 64);
    Buf vB = mk(size_t(g.triCount) * 36, ASIN); memcpy(vB.p, geom.data() + sizeof(SodGeomFrame), size_t(g.triCount) * 36);
    // REFIT test: the BLAS is first built from a different pose (every vertex moved), then UPDATED to the real one
    const bool refit = getenv("REFIT") != nullptr;
    Buf vA = mk(size_t(g.triCount) * 36, ASIN);
    { const float* src = (const float*)vB.p; float* dst = (float*)vA.p;
      for (size_t i = 0; i < size_t(g.triCount) * 9; i += 3) {
        dst[i] = src[i] + 60.0f * sinf(src[i + 1] * 0.01f); dst[i + 1] = src[i + 1] * 0.9f + 40.0f; dst[i + 2] = src[i + 2] - 50.0f * cosf(src[i] * 0.013f); } }
    std::vector<VkAccelerationStructureGeometryKHR> geosA[8];
    VkAccelerationStructureBuildGeometryInfoKHR bgA[8];

    // same layout as sod_rt64: one opaque BLAS per light (casters with that light bit), one TLAS instance per light (mask 1 << l)
    std::vector<VkAccelerationStructureGeometryKHR> geos[8];
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges[8];
    VkAccelerationStructureBuildGeometryInfoKHR bgs[8];
    const VkAccelerationStructureBuildRangeInfoKHR* prs[8];
    VkAccelerationStructureInstanceKHR insts[8] = { };
    uint32_t nb = 0;
    for (uint32_t l = 0; l < g.lightCount; l++) {
        std::vector<uint32_t> prims;
        for (uint32_t c = 0; c < g.casterCount; c++) {
            if (!((g.casters[c].lightMask >> l) & 1u)) continue;
            VkAccelerationStructureGeometryKHR ge = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
            ge.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR; ge.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            ge.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            ge.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            ge.geometry.triangles.vertexData.deviceAddress = vB.a + VkDeviceAddress(g.casters[c].firstTri) * 36;
            ge.geometry.triangles.vertexStride = 12; ge.geometry.triangles.maxVertex = g.casters[c].triCount * 3 - 1;
            ge.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
            geos[nb].push_back(ge); prims.push_back(g.casters[c].triCount); ranges[nb].push_back({ g.casters[c].triCount, 0, 0, 0 });
        }
        if (prims.empty()) continue;
        VkAccelerationStructureBuildGeometryInfoKHR& bg = bgs[nb];
        bg = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        bg.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR; bg.flags = refit ? (VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR) : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bg.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; bg.geometryCount = uint32_t(geos[nb].size()); bg.pGeometries = geos[nb].data();
        VkAccelerationStructureBuildSizesInfoKHR bs = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bg, prims.data(), &bs);
        Buf blB = mk(bs.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR), blS = mk(std::max(bs.buildScratchSize, bs.updateScratchSize) + 256, SSBO);
        VkAccelerationStructureCreateInfoKHR aci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        aci.buffer = blB.b; aci.size = bs.accelerationStructureSize; aci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        VkAccelerationStructureKHR blas; CK(vkCreateAccelerationStructureKHR(dev, &aci, nullptr, &blas));
        bg.dstAccelerationStructure = blas; bg.scratchData.deviceAddress = (blS.a + 255) & ~VkDeviceAddress(255);
        if (refit) {
            geosA[nb] = geos[nb];
            for (auto& ge : geosA[nb]) ge.geometry.triangles.vertexData.deviceAddress += vA.a - vB.a;
            Buf aB = mk(bs.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR), aS = mk(bs.buildScratchSize + 256, SSBO);
            VkAccelerationStructureCreateInfoKHR a2 = aci; a2.buffer = aB.b;
            VkAccelerationStructureKHR blasA; CK(vkCreateAccelerationStructureKHR(dev, &a2, nullptr, &blasA));
            bgA[nb] = bg; bgA[nb].pGeometries = geosA[nb].data(); bgA[nb].dstAccelerationStructure = blasA;
            bgA[nb].scratchData.deviceAddress = (aS.a + 255) & ~VkDeviceAddress(255);
            bg.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR; bg.srcAccelerationStructure = blasA;
        }
        prs[nb] = ranges[nb].data();
        VkAccelerationStructureDeviceAddressInfoKHR dai = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR }; dai.accelerationStructure = blas;
        auto& in = insts[nb];
        in.transform.matrix[0][0] = in.transform.matrix[1][1] = in.transform.matrix[2][2] = 1;
        in.mask = uint8_t(1u << l);
        in.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
        in.accelerationStructureReference = vkGetAccelerationStructureDeviceAddressKHR(dev, &dai);
        nb++;
    }
    Buf iB = mk(8 * sizeof(VkAccelerationStructureInstanceKHR), ASIN); memcpy(iB.p, insts, sizeof insts);
    VkAccelerationStructureGeometryKHR tg = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    tg.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR; tg.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tg.geometry.instances.data.deviceAddress = iB.a;
    VkAccelerationStructureBuildGeometryInfoKHR tb = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    tb.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; tb.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; tb.geometryCount = 1; tb.pGeometries = &tg;
    uint32_t maxInst = 8; VkAccelerationStructureBuildSizesInfoKHR ts = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tb, &maxInst, &ts);
    Buf tlB = mk(ts.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR), tlS = mk(ts.buildScratchSize + 256, SSBO);
    VkAccelerationStructureCreateInfoKHR taci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    taci.buffer = tlB.b; taci.size = ts.accelerationStructureSize; taci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VkAccelerationStructureKHR tlas; CK(vkCreateAccelerationStructureKHR(dev, &taci, nullptr, &tlas));
    tb.dstAccelerationStructure = tlas; tb.scratchData.deviceAddress = (tlS.a + 255) & ~VkDeviceAddress(255);

    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO }; smi.codeSize = sizeof kRtMaskSpv; smi.pCode = kRtMaskSpv;
    VkShaderModule sm; CK(vkCreateShaderModule(dev, &smi, nullptr, &sm));
    VkDescriptorSetLayoutBinding bnd[8] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT }, { 2, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT }, { 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        { 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT }, { 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT }, { 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT } };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO }; dli.bindingCount = 8; dli.pBindings = bnd;
    VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev, &dli, nullptr, &dsl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO }; pli.setLayoutCount = 1; pli.pSetLayouts = &dsl;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &pli, nullptr, &pl));
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main" }; cpi.layout = pl;
    VkPipeline pipe; CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7 }, { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO }; dpi.maxSets = 1; dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
    VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpi, nullptr, &dp));
    VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO }; dsa.descriptorPool = dp; dsa.descriptorSetCount = 1; dsa.pSetLayouts = &dsl;
    VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev, &dsa, &ds));
    VkDescriptorBufferInfo bi[7] = { { dB.b, 0, VK_WHOLE_SIZE }, { rB.b, 0, VK_WHOLE_SIZE }, { fB.b, 0, VK_WHOLE_SIZE }, { cB.b, 0, VK_WHOLE_SIZE },
        { bitsB.b, 0, VK_WHOLE_SIZE }, { softB.b, 0, VK_WHOLE_SIZE }, { tilB.b, 0, VK_WHOLE_SIZE } };
    uint32_t bindings[7] = { 0, 1, 3, 4, 5, 6, 7 };
    VkWriteDescriptorSet wr[8] = { };
    for (int k = 0; k < 7; k++) { wr[k] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; wr[k].dstSet = ds; wr[k].dstBinding = bindings[k];
        wr[k].descriptorCount = 1; wr[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[k].pBufferInfo = &bi[k]; }
    VkWriteDescriptorSetAccelerationStructureKHR wa = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR }; wa.accelerationStructureCount = 1; wa.pAccelerationStructures = &tlas;
    wr[7] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &wa }; wr[7].dstSet = ds; wr[7].dstBinding = 2; wr[7].descriptorCount = 1;
    wr[7].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(dev, 8, wr, 0, nullptr);

    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; VkCommandPool pool; CK(vkCreateCommandPool(dev, &cpci, nullptr, &pool));
    VkCommandBufferAllocateInfo cba = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO }; cba.commandPool = pool; cba.commandBufferCount = 1;
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cba, &cb));
    VkCommandBufferBeginInfo cbb = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; CK(vkBeginCommandBuffer(cb, &cbb));
    if (nb && refit) {
        vkCmdBuildAccelerationStructuresKHR(cb, nb, bgA, prs);
        barrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        printf("refit: built from a moved pose, updated to the real one\n");
    }
    if (nb) vkCmdBuildAccelerationStructuresKHR(cb, nb, bgs, prs);
    barrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    VkAccelerationStructureBuildRangeInfoKHR tr = { nb, 0, 0, 0 }; const VkAccelerationStructureBuildRangeInfoKHR* ptr = &tr;
    vkCmdBuildAccelerationStructuresKHR(cb, 1, &tb, &ptr);
    barrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);
    uint32_t slot = 3, words = (W * H + 3) / 4;
    bool softMode = gm.flags & 2u, forceAll = getenv("FORCEALL") != nullptr;
    for (uint32_t pass = 0; pass < (softMode ? 5u : 1u); pass++) {
        if (forceAll && (pass == 1 || pass == 2)) {
            if (pass == 2) { vkCmdFillBuffer(cb, tilB.b, 0, VK_WHOLE_SIZE, 1u);
                barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT); }
            continue;
        }
        uint32_t pcv[2] = { slot, pass }; vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pcv);
        uint32_t items = (pass == 1 || pass == 2) ? nT : pass == 3 ? W * H : words;
        vkCmdDispatch(cb, (items + 63) / 64, 1, 1);
        barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));
    const uint8_t* res = (const uint8_t*)rB.p; uint32_t shadowed = 0;
    for (size_t i = 0; i < size_t(W) * H; i++) shadowed += res[i] != 255;
    printf("shadowed pixels %u, kernel counted %u\n", shadowed, ((uint32_t*)cB.p)[slot]);
    if (softMode) { uint32_t e = 0, a = 0; for (uint32_t t = 0; t < nT; t++) { e += ((uint32_t*)tilB.p)[t] != 0; a += ((uint32_t*)tilB.p)[nT + t] != 0; }
        printf("tiles %u: edge %u, active %u (%.1f%% of the screen)\n", nT, e, a, 100.0 * a / nT); }
    fseek(fp, maskOff, SEEK_SET); fwrite(res, 1, size_t(W) * H, fp); fclose(fp);
    return shadowed == ((uint32_t*)cB.p)[slot] ? 0 : 2;
}
