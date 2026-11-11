// sod_rt64.exe — 64-bit hardware ray tracing helper for SoD-RT (Shadow of Destiny).
// Started by the SoD-RT DXVK layer inside the 32-bit game; NVIDIA (and others) only expose
// VK_KHR_acceleration_structure / VK_KHR_ray_query to 64-bit processes.
//
// Bridge milestone B1: connect, find the same GPU, create an RT-capable device and prove it works
// with a self-test (BLAS + TLAS build and two ray queries). Results go back through shared memory.

#define WIN32_LEAN_AND_MEAN
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <thread>
#include <atomic>

#include "sod_bridge_proto.h"
#include "selftest_spv.h"
#include "rtmask_spv.h"

// ---------------------------------------------------------------- logging
static FILE* g_log = nullptr;
static void Log(const char* fmt, ...) {
    va_list a; va_start(a, fmt);
    if (g_log) { vfprintf(g_log, fmt, a); fputc('\n', g_log); fflush(g_log); }
    va_end(a);
}
static SodBridgeShared* g_shared = nullptr;
static void Fail(const char* fmt, ...) {
    char buf[512]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    Log("FAILED: %s", buf);
    if (g_shared) { strncpy(g_shared->error, buf, sizeof g_shared->error - 1); g_shared->state = SOD_BRIDGE_FAILED; }
}

// ---------------------------------------------------------------- vulkan entry points
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr_;
#define VK_GLOBAL(n)   static PFN_##n n##_;
#define VK_INSTANCE(n) static PFN_##n n##_;
#define VK_DEVICE(n)   static PFN_##n n##_;
VK_GLOBAL(vkCreateInstance) VK_GLOBAL(vkEnumerateInstanceVersion)
VK_INSTANCE(vkEnumeratePhysicalDevices) VK_INSTANCE(vkGetPhysicalDeviceProperties2) VK_INSTANCE(vkEnumerateDeviceExtensionProperties)
VK_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties) VK_INSTANCE(vkGetPhysicalDeviceMemoryProperties) VK_INSTANCE(vkCreateDevice)
VK_INSTANCE(vkGetDeviceProcAddr) VK_INSTANCE(vkDestroyInstance) VK_INSTANCE(vkGetPhysicalDeviceFeatures2)
VK_DEVICE(vkGetDeviceQueue) VK_DEVICE(vkCreateBuffer) VK_DEVICE(vkGetBufferMemoryRequirements) VK_DEVICE(vkAllocateMemory)
VK_DEVICE(vkBindBufferMemory) VK_DEVICE(vkMapMemory) VK_DEVICE(vkGetBufferDeviceAddress) VK_DEVICE(vkCreateCommandPool)
VK_DEVICE(vkAllocateCommandBuffers) VK_DEVICE(vkBeginCommandBuffer) VK_DEVICE(vkEndCommandBuffer) VK_DEVICE(vkQueueSubmit)
VK_DEVICE(vkCreateFence) VK_DEVICE(vkWaitForFences) VK_DEVICE(vkCmdPipelineBarrier) VK_DEVICE(vkCreateShaderModule)
VK_DEVICE(vkCreateDescriptorSetLayout) VK_DEVICE(vkCreatePipelineLayout) VK_DEVICE(vkCreateComputePipelines)
VK_DEVICE(vkCreateDescriptorPool) VK_DEVICE(vkAllocateDescriptorSets) VK_DEVICE(vkUpdateDescriptorSets)
VK_DEVICE(vkCmdBindPipeline) VK_DEVICE(vkCmdBindDescriptorSets) VK_DEVICE(vkCmdDispatch) VK_DEVICE(vkDeviceWaitIdle)
VK_DEVICE(vkDestroyDevice) VK_DEVICE(vkGetAccelerationStructureBuildSizesKHR) VK_DEVICE(vkCreateAccelerationStructureKHR)
VK_DEVICE(vkCmdBuildAccelerationStructuresKHR) VK_DEVICE(vkGetAccelerationStructureDeviceAddressKHR)
VK_DEVICE(vkCreateSemaphore) VK_DEVICE(vkImportSemaphoreWin32HandleKHR) VK_DEVICE(vkResetFences) VK_DEVICE(vkGetFenceStatus)
VK_DEVICE(vkResetCommandBuffer) VK_DEVICE(vkCmdFillBuffer) VK_DEVICE(vkCmdPushConstants) VK_DEVICE(vkDestroyBuffer)
VK_DEVICE(vkFreeMemory) VK_DEVICE(vkGetSemaphoreCounterValue)
VK_DEVICE(vkCreateQueryPool) VK_DEVICE(vkCmdResetQueryPool) VK_DEVICE(vkCmdWriteTimestamp) VK_DEVICE(vkGetQueryPoolResults)
VK_DEVICE(vkDestroyAccelerationStructureKHR) VK_DEVICE(vkCmdCopyBuffer) VK_DEVICE(vkWaitSemaphores)

#define LOAD_I(n) n##_ = (PFN_##n)vkGetInstanceProcAddr_(inst, #n)
#define LOAD_D(n) n##_ = (PFN_##n)vkGetDeviceProcAddr_(dev, #n)

#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { Fail("%s -> VkResult %d (line %d)", #x, (int)_r, __LINE__); return false; } } while (0)

// ---------------------------------------------------------------- small helpers
struct Buf { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkDeviceAddress addr = 0; void* map = nullptr; };

static VkPhysicalDevice g_phys = VK_NULL_HANDLE;
static VkDevice         g_dev  = VK_NULL_HANDLE;
static VkPhysicalDeviceMemoryProperties g_memProps;
static VkQueue          g_queue = VK_NULL_HANDLE;
static uint32_t         g_family = 0;
static float            g_timestampPeriod = 1.0f;
static uint32_t         g_timestampBits = 0;

static uint32_t FindMemType(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_memProps.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_memProps.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
static bool MakeBuffer(Buf& b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props) {
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CHECK(vkCreateBuffer_(g_dev, &bi, nullptr, &b.buf));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements_(g_dev, b.buf, &req);
    VkMemoryAllocateFlagsInfo fl = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.pNext = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ? &fl : nullptr;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemType(req.memoryTypeBits, props);
    if (ai.memoryTypeIndex == UINT32_MAX) { Fail("no memory type for usage %x props %x", usage, props); return false; }
    CHECK(vkAllocateMemory_(g_dev, &ai, nullptr, &b.mem));
    CHECK(vkBindBufferMemory_(g_dev, b.buf, b.mem, 0));
    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) CHECK(vkMapMemory_(g_dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map));
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo da = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO }; da.buffer = b.buf;
        b.addr = vkGetBufferDeviceAddress_(g_dev, &da);
    }
    return true;
}
static void AsBarrier(VkCommandBuffer cb, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier_(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, dstStage, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

// ---------------------------------------------------------------- B1: device + self-test
static bool Run() {
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { Fail("vulkan-1.dll (64-bit) not found"); return false; }
    vkGetInstanceProcAddr_ = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
    VkInstance inst = VK_NULL_HANDLE;
    LOAD_I(vkCreateInstance); LOAD_I(vkEnumerateInstanceVersion);
    uint32_t apiVer = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion_) vkEnumerateInstanceVersion_(&apiVer);
    if (apiVer < VK_API_VERSION_1_2) { Fail("Vulkan instance %u.%u < 1.2", VK_VERSION_MAJOR(apiVer), VK_VERSION_MINOR(apiVer)); return false; }

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "sod_rt64"; app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ici.pApplicationInfo = &app;
    CHECK(vkCreateInstance_(&ici, nullptr, &inst));
    LOAD_I(vkEnumeratePhysicalDevices); LOAD_I(vkGetPhysicalDeviceProperties2); LOAD_I(vkEnumerateDeviceExtensionProperties);
    LOAD_I(vkGetPhysicalDeviceQueueFamilyProperties); LOAD_I(vkGetPhysicalDeviceMemoryProperties); LOAD_I(vkCreateDevice);
    LOAD_I(vkGetDeviceProcAddr); LOAD_I(vkDestroyInstance); LOAD_I(vkGetPhysicalDeviceFeatures2);

    // ---- same GPU as DXVK in the game
    uint32_t n = 0; vkEnumeratePhysicalDevices_(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices_(inst, &n, pds.data());
    VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    for (auto pd : pds) {
        VkPhysicalDeviceIDProperties id = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 p = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 }; p.pNext = &id;
        vkGetPhysicalDeviceProperties2_(pd, &p);
        Log("GPU %s", p.properties.deviceName);
        if (!memcmp(id.deviceUUID, g_shared->deviceUUID, 16)) { g_phys = pd; props = p; g_shared->uuidMatched = 1; }
    }
    if (!g_phys) { Fail("GPU used by the game not found from the 64-bit side (UUID mismatch)"); return false; }
    strncpy(g_shared->deviceName, props.properties.deviceName, sizeof g_shared->deviceName - 1);
    g_shared->driverVersion = props.properties.driverVersion;
    g_shared->apiVersion    = props.properties.apiVersion;

    // ---- extensions
    vkEnumerateDeviceExtensionProperties_(g_phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n); vkEnumerateDeviceExtensionProperties_(g_phys, nullptr, &n, exts.data());
    auto has = [&](const char* e) { for (auto& x : exts) if (!strcmp(x.extensionName, e)) return true; return false; };
    g_shared->hasAccelStruct       = has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    g_shared->hasRayQuery          = has(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    g_shared->hasExternalMemory    = has(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
    g_shared->hasExternalSemaphore = has(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
    Log("extensions: accel=%u rayquery=%u extmem=%u extsem=%u", g_shared->hasAccelStruct, g_shared->hasRayQuery,
        g_shared->hasExternalMemory, g_shared->hasExternalSemaphore);
    if (!g_shared->hasAccelStruct || !g_shared->hasRayQuery) { Fail("this GPU/driver has no Vulkan hardware ray tracing in 64-bit either"); return false; }

    // ---- features
    VkPhysicalDeviceRayQueryFeaturesKHR rqF = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asF = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR }; asF.pNext = &rqF;
    VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES }; v12.pNext = &asF;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 }; f2.pNext = &v12;
    vkGetPhysicalDeviceFeatures2_(g_phys, &f2);
    g_shared->hasTimelineSemaphore = v12.timelineSemaphore;
    if (!asF.accelerationStructure || !rqF.rayQuery || !v12.bufferDeviceAddress) {
        Fail("features missing: accelerationStructure=%u rayQuery=%u bufferDeviceAddress=%u",
             asF.accelerationStructure, rqF.rayQuery, v12.bufferDeviceAddress); return false;
    }
    VkPhysicalDeviceRayQueryFeaturesKHR rqE = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR }; rqE.rayQuery = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asE = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    asE.accelerationStructure = VK_TRUE; asE.pNext = &rqE;
    VkPhysicalDeviceVulkan12Features v12E = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    v12E.bufferDeviceAddress = VK_TRUE; v12E.timelineSemaphore = v12.timelineSemaphore; v12E.pNext = &asE;

    // ---- queue (graphics+compute family)
    vkGetPhysicalDeviceQueueFamilyProperties_(g_phys, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n); vkGetPhysicalDeviceQueueFamilyProperties_(g_phys, &n, qf.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < n; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
    if (family == UINT32_MAX) { Fail("no compute queue"); return false; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    std::vector<const char*> en = { VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                                    VK_KHR_RAY_QUERY_EXTENSION_NAME };
    if (g_shared->hasExternalMemory)    en.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
    if (g_shared->hasExternalSemaphore) en.push_back(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
    // high GPU scheduling priority: the game's queue waits for this short burst every frame
    const char* prioExt = has("VK_KHR_global_priority") ? "VK_KHR_global_priority" : has("VK_EXT_global_priority") ? "VK_EXT_global_priority" : nullptr;
    VkDeviceQueueGlobalPriorityCreateInfoKHR gp = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR };
    gp.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &v12E; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkResult dr = VK_ERROR_INITIALIZATION_FAILED;
    if (prioExt) {
        std::vector<const char*> en2 = en; en2.push_back(prioExt);
        qci.pNext = &gp;
        dci.enabledExtensionCount = (uint32_t)en2.size(); dci.ppEnabledExtensionNames = en2.data();
        dr = vkCreateDevice_(g_phys, &dci, nullptr, &g_dev);
        Log("queue priority HIGH via %s: %s (VkResult %d)", prioExt, dr == VK_SUCCESS ? "granted" : "refused", (int)dr);
        qci.pNext = nullptr;
    } else {
        Log("queue priority: no global priority extension, normal priority");
    }
    if (dr != VK_SUCCESS) {
        dci.enabledExtensionCount = (uint32_t)en.size(); dci.ppEnabledExtensionNames = en.data();
        CHECK(vkCreateDevice_(g_phys, &dci, nullptr, &g_dev));
    }
    VkDevice dev = g_dev;
    LOAD_D(vkGetDeviceQueue); LOAD_D(vkCreateBuffer); LOAD_D(vkGetBufferMemoryRequirements); LOAD_D(vkAllocateMemory);
    LOAD_D(vkBindBufferMemory); LOAD_D(vkMapMemory); LOAD_D(vkGetBufferDeviceAddress); LOAD_D(vkCreateCommandPool);
    LOAD_D(vkAllocateCommandBuffers); LOAD_D(vkBeginCommandBuffer); LOAD_D(vkEndCommandBuffer); LOAD_D(vkQueueSubmit);
    LOAD_D(vkCreateFence); LOAD_D(vkWaitForFences); LOAD_D(vkCmdPipelineBarrier); LOAD_D(vkCreateShaderModule);
    LOAD_D(vkCreateDescriptorSetLayout); LOAD_D(vkCreatePipelineLayout); LOAD_D(vkCreateComputePipelines);
    LOAD_D(vkCreateDescriptorPool); LOAD_D(vkAllocateDescriptorSets); LOAD_D(vkUpdateDescriptorSets);
    LOAD_D(vkCmdBindPipeline); LOAD_D(vkCmdBindDescriptorSets); LOAD_D(vkCmdDispatch); LOAD_D(vkDeviceWaitIdle);
    LOAD_D(vkDestroyDevice); LOAD_D(vkGetAccelerationStructureBuildSizesKHR); LOAD_D(vkCreateAccelerationStructureKHR);
    LOAD_D(vkCmdBuildAccelerationStructuresKHR); LOAD_D(vkGetAccelerationStructureDeviceAddressKHR);
    LOAD_D(vkCreateSemaphore); LOAD_D(vkImportSemaphoreWin32HandleKHR); LOAD_D(vkResetFences); LOAD_D(vkGetFenceStatus);
    LOAD_D(vkResetCommandBuffer); LOAD_D(vkCmdFillBuffer); LOAD_D(vkCmdPushConstants); LOAD_D(vkDestroyBuffer);
    LOAD_D(vkFreeMemory); LOAD_D(vkGetSemaphoreCounterValue);
    LOAD_D(vkCreateQueryPool); LOAD_D(vkCmdResetQueryPool); LOAD_D(vkCmdWriteTimestamp); LOAD_D(vkGetQueryPoolResults);
    LOAD_D(vkDestroyAccelerationStructureKHR); LOAD_D(vkCmdCopyBuffer); LOAD_D(vkWaitSemaphores);
    g_timestampPeriod = props.properties.limits.timestampPeriod;
    g_timestampBits = qf[family].timestampValidBits;    vkGetPhysicalDeviceMemoryProperties_(g_phys, &g_memProps);
    VkQueue queue; vkGetDeviceQueue_(dev, family, 0, &queue);
    g_queue = queue; g_family = family;
    Log("RT device created on %s", props.properties.deviceName);

    // ---- self-test geometry: one triangle in z = 0
    const VkMemoryPropertyFlags HOST = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags GPU  = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const VkBufferUsageFlags ASIN = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const VkBufferUsageFlags ASST = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const VkBufferUsageFlags SCR  = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    Buf vb; if (!MakeBuffer(vb, 3 * 12, ASIN, HOST)) return false;
    const float tri[9] = { -1.f, -1.f, 0.f,  1.f, -1.f, 0.f,  0.f, 1.f, 0.f };
    memcpy(vb.map, tri, sizeof tri);

    VkAccelerationStructureGeometryKHR geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR; geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geo.geometry.triangles.vertexData.deviceAddress = vb.addr;
    geo.geometry.triangles.vertexStride = 12; geo.geometry.triangles.maxVertex = 2;
    geo.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR bg = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    bg.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR; bg.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bg.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; bg.geometryCount = 1; bg.pGeometries = &geo;
    uint32_t primCount = 1;
    VkAccelerationStructureBuildSizesInfoKHR bs = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR_(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bg, &primCount, &bs);
    Buf blasBuf; if (!MakeBuffer(blasBuf, bs.accelerationStructureSize, ASST, GPU)) return false;
    VkAccelerationStructureCreateInfoKHR aci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    aci.buffer = blasBuf.buf; aci.size = bs.accelerationStructureSize; aci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VkAccelerationStructureKHR blas; CHECK(vkCreateAccelerationStructureKHR_(dev, &aci, nullptr, &blas));
    Buf blasScratch; if (!MakeBuffer(blasScratch, bs.buildScratchSize + 256, SCR, GPU)) return false;
    bg.dstAccelerationStructure = blas; bg.scratchData.deviceAddress = (blasScratch.addr + 255) & ~VkDeviceAddress(255);

    VkAccelerationStructureDeviceAddressInfoKHR dai = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
    dai.accelerationStructure = blas;
    VkDeviceAddress blasAddr = vkGetAccelerationStructureDeviceAddressKHR_(dev, &dai);

    Buf instBuf; if (!MakeBuffer(instBuf, sizeof(VkAccelerationStructureInstanceKHR), ASIN, HOST)) return false;
    VkAccelerationStructureInstanceKHR ins = { };
    ins.transform.matrix[0][0] = ins.transform.matrix[1][1] = ins.transform.matrix[2][2] = 1.0f;
    ins.mask = 0xFF; ins.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    ins.accelerationStructureReference = blasAddr;
    memcpy(instBuf.map, &ins, sizeof ins);
    VkAccelerationStructureGeometryKHR tgeo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    tgeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tgeo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tgeo.geometry.instances.data.deviceAddress = instBuf.addr;
    VkAccelerationStructureBuildGeometryInfoKHR tg = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    tg.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; tg.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tg.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; tg.geometryCount = 1; tg.pGeometries = &tgeo;
    uint32_t instCount = 1;
    VkAccelerationStructureBuildSizesInfoKHR ts = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR_(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tg, &instCount, &ts);
    Buf tlasBuf; if (!MakeBuffer(tlasBuf, ts.accelerationStructureSize, ASST, GPU)) return false;
    aci.buffer = tlasBuf.buf; aci.size = ts.accelerationStructureSize; aci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VkAccelerationStructureKHR tlas; CHECK(vkCreateAccelerationStructureKHR_(dev, &aci, nullptr, &tlas));
    Buf tlasScratch; if (!MakeBuffer(tlasScratch, ts.buildScratchSize + 256, SCR, GPU)) return false;
    tg.dstAccelerationStructure = tlas; tg.scratchData.deviceAddress = (tlasScratch.addr + 255) & ~VkDeviceAddress(255);

    // ---- ray query compute pipeline
    Buf result; if (!MakeBuffer(result, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, HOST)) return false;
    memset(result.map, 0, 16);
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smi.codeSize = sizeof kSelfTestSpv; smi.pCode = kSelfTestSpv;
    VkShaderModule sm; CHECK(vkCreateShaderModule_(dev, &smi, nullptr, &sm));
    VkDescriptorSetLayoutBinding bnd[2] = {
        { 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO }; dli.bindingCount = 2; dli.pBindings = bnd;
    VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout_(dev, &dli, nullptr, &dsl));
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO }; pli.setLayoutCount = 1; pli.pSetLayouts = &dsl;
    VkPipelineLayout pl; CHECK(vkCreatePipelineLayout_(dev, &pli, nullptr, &pl));
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = sm; cpi.stage.pName = "main"; cpi.layout = pl;
    VkPipeline pipe; CHECK(vkCreateComputePipelines_(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO }; dpi.maxSets = 1; dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
    VkDescriptorPool dp; CHECK(vkCreateDescriptorPool_(dev, &dpi, nullptr, &dp));
    VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO }; dsa.descriptorPool = dp; dsa.descriptorSetCount = 1; dsa.pSetLayouts = &dsl;
    VkDescriptorSet ds; CHECK(vkAllocateDescriptorSets_(dev, &dsa, &ds));
    VkWriteDescriptorSetAccelerationStructureKHR wa = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
    wa.accelerationStructureCount = 1; wa.pAccelerationStructures = &tlas;
    VkDescriptorBufferInfo rbi = { result.buf, 0, 16 };
    VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
    w[0].pNext = &wa; w[0].dstSet = ds; w[0].dstBinding = 0; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    w[1].dstSet = ds; w[1].dstBinding = 1; w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &rbi;
    vkUpdateDescriptorSets_(dev, 2, w, 0, nullptr);

    // ---- record: BLAS -> TLAS -> trace
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; cpci.queueFamilyIndex = family;
    VkCommandPool pool; CHECK(vkCreateCommandPool_(dev, &cpci, nullptr, &pool));
    VkCommandBufferAllocateInfo cba = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cba.commandPool = pool; cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cba.commandBufferCount = 1;
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers_(dev, &cba, &cb));
    VkCommandBufferBeginInfo cbb = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; cbb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vkBeginCommandBuffer_(cb, &cbb));
    VkAccelerationStructureBuildRangeInfoKHR br = { 1, 0, 0, 0 };
    const VkAccelerationStructureBuildRangeInfoKHR* pbr = &br;
    vkCmdBuildAccelerationStructuresKHR_(cb, 1, &bg, &pbr);
    AsBarrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    VkAccelerationStructureBuildRangeInfoKHR tr = { 1, 0, 0, 0 };
    const VkAccelerationStructureBuildRangeInfoKHR* ptr = &tr;
    vkCmdBuildAccelerationStructuresKHR_(cb, 1, &tg, &ptr);
    AsBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    vkCmdBindPipeline_(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets_(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);
    vkCmdDispatch_(cb, 1, 1, 1);
    VkMemoryBarrier hb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER }; hb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier_(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
    CHECK(vkEndCommandBuffer_(cb));

    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence; CHECK(vkCreateFence_(dev, &fci, nullptr, &fence));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    CHECK(vkQueueSubmit_(queue, 1, &si, fence));
    CHECK(vkWaitForFences_(dev, 1, &fence, VK_TRUE, 5000000000ull));
    QueryPerformanceCounter(&t1);
    const uint32_t* res = (const uint32_t*)result.map;
    g_shared->selfTestHit  = res[0];
    g_shared->selfTestMiss = res[1];
    g_shared->selfTestMs   = float(double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart));
    Log("self-test: hit=%u miss=%u marker=%08x time=%.3f ms", res[0], res[1], res[2], g_shared->selfTestMs);
    if (res[2] != 0x5D5D5D5Du) { Fail("self-test shader did not run"); return false; }
    if (!res[0] || !res[1])     { Fail("self-test rays gave wrong results (hit=%u miss=%u)", res[0], res[1]); return false; }
    vkDeviceWaitIdle_(dev);
    return true;
}


// ---------------------------------------------------------------- B2: per-frame link with the game
// The game's queue waits for 2k+2 after signalling 2k+1, so EVERY request is served, in order.
struct Imported { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkDeviceSize size = 0; };

static void LinkFail(const char* fmt, ...) {
    char buf[256]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    Log("LINK FAILED: %s", buf);
    strncpy(g_shared->linkError, buf, sizeof g_shared->linkError - 1);
    g_shared->linkState = SOD_LINK_FAILED;
}
#define LCHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { LinkFail("%s -> VkResult %d (line %d)", #x, (int)_r, __LINE__); return false; } } while (0)

static bool ImportBuffer(Imported& b, uint64_t handle, VkDeviceSize size, VkDeviceSize allocSize, uint32_t typeBits) {
    VkExternalMemoryBufferCreateInfo ext = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ext };
    bi.size = size; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
             | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;   // same as the exporter
    LCHECK(vkCreateBuffer_(g_dev, &bi, nullptr, &b.buf));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements_(g_dev, b.buf, &req);
    uint32_t type = FindMemType(req.memoryTypeBits & typeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) type = FindMemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) { LinkFail("no memory type to import (bits %x / %x)", req.memoryTypeBits, typeBits); return false; }
    VkMemoryAllocateFlagsInfo fl = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO }; fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &fl }; ded.buffer = b.buf;
    VkImportMemoryWin32HandleInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &ded };
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT; imp.handle = (HANDLE)(uintptr_t)handle;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp };
    ai.allocationSize = allocSize; ai.memoryTypeIndex = type;
    LCHECK(vkAllocateMemory_(g_dev, &ai, nullptr, &b.mem));
    LCHECK(vkBindBufferMemory_(g_dev, b.buf, b.mem, 0));
    b.size = size;
    CloseHandle((HANDLE)(uintptr_t)handle);          // the import keeps its own reference
    return true;
}
static void FreeImported(Imported& b) {
    if (b.buf) vkDestroyBuffer_(g_dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory_(g_dev, b.mem, nullptr);
    b = Imported();
}

static void FreeBuf(Buf& b) {
    if (b.buf) vkDestroyBuffer_(g_dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory_(g_dev, b.mem, nullptr);
    b = Buf();
}

// per request slot: frame header, triangles, acceleration structures, descriptor set
struct LightAS {
    Buf buf, scratch;
    VkDeviceSize cap = 0, scratchCap = 0;
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    VkDeviceAddress addr = 0;
};
struct Slot {
    VkCommandBuffer cb = VK_NULL_HANDLE;           // trace: waits for the game's depth (2k+1), signals 2k+2
    VkCommandBuffer cbBuild = VK_NULL_HANDLE;      // geometry upload + BLAS/TLAS: no wait, overlaps the game's frame
    VkFence fence = VK_NULL_HANDLE;
    uint64_t pendingValue = 0, pendingQpc = 0;
    bool timed = false;
    uint32_t softBucket = 0, softK = 0;               // light count of the frame, soft samples used (0 = hard)
    Buf frame, staging, verts, inst, tlasBuf, tlasScratch;
    VkDeviceSize vertCap = 0;
    LightAS las[SOD_MAX_LIGHTS];
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    VkDescriptorSet ds = VK_NULL_HANDLE;
};

static const VkMemoryPropertyFlags kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
static const VkMemoryPropertyFlags kGpu  = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
static const VkBufferUsageFlags kAsIn  = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
static const VkBufferUsageFlags kAsSt  = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
static const VkBufferUsageFlags kScr   = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

// Per light l: one opaque BLAS with exactly the casters whose light mask has bit l (one geometry each; the
// triangles live once in the slot's device-local vertex buffer). TLAS: one instance per light, mask 1 << l.
struct FrameBuild {
    std::vector<VkAccelerationStructureGeometryKHR> geos[SOD_MAX_LIGHTS];
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges[SOD_MAX_LIGHTS];
    VkAccelerationStructureBuildGeometryInfoKHR bg[SOD_MAX_LIGHTS];
    const VkAccelerationStructureBuildRangeInfoKHR* pr[SOD_MAX_LIGHTS];
    uint32_t light[SOD_MAX_LIGHTS];      // light index of build b
    uint32_t count = 0;
};

// BLAS refit: the casters' triangles are the same from one request to the next (same characters, same counts),
// only their positions move. Then the light's BLAS is UPDATED from the previous request's one (src = that slot's
// BLAS, dst = this slot's) instead of built from scratch: same triangles, same rays, same shadows - only the
// tree is adjusted to the new positions instead of re-made. Tree quality drifts as characters move, so every
// kRefitLimit-th request per chain is a full build again. A change in the casters (count, order of sizes) = build.
struct BlasHist { std::vector<uint32_t> prims; VkAccelerationStructureKHR as = VK_NULL_HANDLE; uint32_t refits = 0; };
static std::vector<BlasHist> g_hist[SOD_BRIDGE_SLOTS];
static uint64_t g_histReq[SOD_BRIDGE_SLOTS] = { };
static const uint32_t kRefitLimit = 30;
static uint64_t g_statBuilds = 0, g_statRefits = 0;

static const BlasHist* FindRefitSource(uint64_t reqIdx, uint32_t slot, const std::vector<uint32_t>& prims) {
    for (uint64_t back = 1; back < SOD_BRIDGE_SLOTS && back <= reqIdx; back++) {
        const uint64_t idx = reqIdx - back;
        const uint32_t js = uint32_t(idx % SOD_BRIDGE_SLOTS);
        if (js == slot || g_histReq[js] != idx) continue;
        for (const BlasHist& h : g_hist[js])
            if (h.as && h.refits < kRefitLimit && h.prims == prims) return &h;
    }
    return nullptr;
}

static bool EnsureSlotBuffers(Slot& sl, const SodGeomFrame& g, FrameBuild& fb, uint32_t slot, uint64_t reqIdx) {
    static const bool envNoRefit = [] { char v[8] = { }; return GetEnvironmentVariableA("SODRT_NO_REFIT", v, 8) && v[0] == '1'; }();
    std::vector<BlasHist> hist;
    VkDeviceSize need = VkDeviceSize(g.triCount) * 36;
    if (need > sl.vertCap) {
        FreeBuf(sl.verts); FreeBuf(sl.staging);
        VkDeviceSize cap = need + need / 2 + 4096;
        if (!MakeBuffer(sl.staging, cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kHost)) return false;
        if (!MakeBuffer(sl.verts, cap, kAsIn | VK_BUFFER_USAGE_TRANSFER_DST_BIT, kGpu)) return false;
        sl.vertCap = cap;
    }
    fb.count = 0;
    for (uint32_t l = 0; l < g.lightCount; l++) {
        uint32_t b = fb.count;
        auto& geos = fb.geos[b]; auto& ranges = fb.ranges[b];
        geos.clear(); ranges.clear();
        std::vector<uint32_t> prims;
        for (uint32_t c = 0; c < g.casterCount; c++) {
            if (!((g.casters[c].lightMask >> l) & 1u)) continue;
            VkAccelerationStructureGeometryKHR ge = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
            ge.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            ge.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            ge.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            ge.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            ge.geometry.triangles.vertexData.deviceAddress = sl.verts.addr + VkDeviceAddress(g.casters[c].firstTri) * 36;
            ge.geometry.triangles.vertexStride = 12;
            ge.geometry.triangles.maxVertex = g.casters[c].triCount * 3 - 1;
            ge.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
            geos.push_back(ge);
            prims.push_back(g.casters[c].triCount);
            ranges.push_back({ g.casters[c].triCount, 0, 0, 0 });
        }
        if (geos.empty()) continue;                     // nothing casts for this light
        VkAccelerationStructureBuildGeometryInfoKHR& bg = fb.bg[b];
        bg = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        bg.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bg.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        bg.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bg.geometryCount = uint32_t(geos.size()); bg.pGeometries = geos.data();
        VkAccelerationStructureBuildSizesInfoKHR bs = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        vkGetAccelerationStructureBuildSizesKHR_(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bg, prims.data(), &bs);
        LightAS& la = sl.las[b];
        if (bs.accelerationStructureSize > la.cap || !la.as) {
            // a later request may still refit from this BLAS on the GPU: let everything finish before it goes
            if (la.as) { vkDeviceWaitIdle_(g_dev); vkDestroyAccelerationStructureKHR_(g_dev, la.as, nullptr); }
            FreeBuf(la.buf);
            VkDeviceSize cap = bs.accelerationStructureSize + bs.accelerationStructureSize / 2 + 65536;
            if (!MakeBuffer(la.buf, cap, kAsSt, kGpu)) return false;
            VkAccelerationStructureCreateInfoKHR aci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
            aci.buffer = la.buf.buf; aci.size = cap; aci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            if (vkCreateAccelerationStructureKHR_(g_dev, &aci, nullptr, &la.as) != VK_SUCCESS) { LinkFail("BLAS create"); return false; }
            la.cap = cap;
            VkAccelerationStructureDeviceAddressInfoKHR dai = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
            dai.accelerationStructure = la.as;
            la.addr = vkGetAccelerationStructureDeviceAddressKHR_(g_dev, &dai);
        }
        const VkDeviceSize scr = bs.buildScratchSize > bs.updateScratchSize ? bs.buildScratchSize : bs.updateScratchSize;
        if (scr + 256 > la.scratchCap) {
            FreeBuf(la.scratch);
            VkDeviceSize cap = scr + scr / 2 + 65536;
            if (!MakeBuffer(la.scratch, cap, kScr, kGpu)) return false;
            la.scratchCap = cap;
        }
        bg.dstAccelerationStructure = la.as;
        bg.scratchData.deviceAddress = (la.scratch.addr + 255) & ~VkDeviceAddress(255);
        const BlasHist* src = envNoRefit ? nullptr : FindRefitSource(reqIdx, slot, prims);
        BlasHist h; h.prims = prims; h.as = la.as;
        if (src) {
            bg.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
            bg.srcAccelerationStructure = src->as;
            h.refits = src->refits + 1;
            g_statRefits++;
        } else g_statBuilds++;
        hist.push_back(std::move(h));
        fb.pr[b] = ranges.data();
        fb.light[b] = l;
        fb.count++;
    }
    // TLAS for up to SOD_MAX_LIGHTS instances (created once per slot)
    if (!sl.tlas) {
        if (!MakeBuffer(sl.inst, SOD_MAX_LIGHTS * sizeof(VkAccelerationStructureInstanceKHR), kAsIn, kHost)) return false;
        VkAccelerationStructureGeometryKHR tg = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        tg.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tg.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        tg.geometry.instances.data.deviceAddress = sl.inst.addr;
        VkAccelerationStructureBuildGeometryInfoKHR tb = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        tb.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; tb.geometryCount = 1; tb.pGeometries = &tg;
        uint32_t maxInst = SOD_MAX_LIGHTS;
        VkAccelerationStructureBuildSizesInfoKHR ts = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        vkGetAccelerationStructureBuildSizesKHR_(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tb, &maxInst, &ts);
        if (!MakeBuffer(sl.tlasBuf, ts.accelerationStructureSize, kAsSt, kGpu)) return false;
        if (!MakeBuffer(sl.tlasScratch, ts.buildScratchSize + 256, kScr, kGpu)) return false;
        VkAccelerationStructureCreateInfoKHR aci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        aci.buffer = sl.tlasBuf.buf; aci.size = ts.accelerationStructureSize; aci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        if (vkCreateAccelerationStructureKHR_(g_dev, &aci, nullptr, &sl.tlas) != VK_SUCCESS) { LinkFail("TLAS create"); return false; }
    }
    g_hist[slot] = std::move(hist);
    g_histReq[slot] = reqIdx;
    if (((g_statBuilds + g_statRefits) % 3000u) < fb.count && fb.count)
        Log("BLAS: %llu refits, %llu full builds so far", (unsigned long long)g_statRefits, (unsigned long long)g_statBuilds);
    return true;
}

static void Serve(HANDLE parentProc) {
    char name[64], gname[64];
    snprintf(name,  sizeof name,  SOD_BRIDGE_REQUEST  "%lu", (unsigned long)g_shared->parentPid);
    snprintf(gname, sizeof gname, SOD_BRIDGE_GEOMETRY "%lu", (unsigned long)g_shared->parentPid);
    HANDLE reqEvent = nullptr, geomMap = nullptr;
    const uint8_t* geom = nullptr;
    VkSemaphore sem = VK_NULL_HANDLE;
    Imported depth, result;
    uint32_t imported = 0;
    uint64_t served = 0;               // last even value submitted

    // ---- ray-query shadow kernel
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smi.codeSize = sizeof kRtMaskSpv; smi.pCode = kRtMaskSpv;
    VkShaderModule sm; if (vkCreateShaderModule_(g_dev, &smi, nullptr, &sm) != VK_SUCCESS) { LinkFail("shader module"); return; }
    VkDescriptorSetLayoutBinding bnd[8] = {
        { 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },   // soft: hit bits
        { 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },   // soft: cone result
        { 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },   // soft: tiles
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 2, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO }; dli.bindingCount = 8; dli.pBindings = bnd;
    VkDescriptorSetLayout dsl; vkCreateDescriptorSetLayout_(g_dev, &dli, nullptr, &dsl);
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1; pli.pSetLayouts = &dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    VkPipelineLayout pl; vkCreatePipelineLayout_(g_dev, &pli, nullptr, &pl);
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = sm; cpi.stage.pName = "main"; cpi.layout = pl;
    VkPipeline pipe; if (vkCreateComputePipelines_(g_dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe) != VK_SUCCESS) { LinkFail("pipeline"); return; }
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7 * SOD_BRIDGE_SLOTS }, { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, SOD_BRIDGE_SLOTS } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO }; dpi.maxSets = SOD_BRIDGE_SLOTS; dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
    VkDescriptorPool dp; vkCreateDescriptorPool_(g_dev, &dpi, nullptr, &dp);
    Buf counts; if (!MakeBuffer(counts, 4 * SOD_BRIDGE_SLOTS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, kHost)) { LinkFail("count buffer"); return; }

    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpci.queueFamilyIndex = g_family;
    VkCommandPool pool; vkCreateCommandPool_(g_dev, &cpci, nullptr, &pool);
    VkQueryPool qp = VK_NULL_HANDLE;
    if (g_timestampBits) {
        VkQueryPoolCreateInfo qpi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO }; qpi.queryType = VK_QUERY_TYPE_TIMESTAMP; qpi.queryCount = 4 * SOD_BRIDGE_SLOTS;
        vkCreateQueryPool_(g_dev, &qpi, nullptr, &qp);
    }
    Slot slots[SOD_BRIDGE_SLOTS];
    for (uint32_t i = 0; i < SOD_BRIDGE_SLOTS; i++) {
        Slot& sl = slots[i];
        VkCommandBufferAllocateInfo cba = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cba.commandPool = pool; cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cba.commandBufferCount = 1;
        vkAllocateCommandBuffers_(g_dev, &cba, &sl.cb);
        vkAllocateCommandBuffers_(g_dev, &cba, &sl.cbBuild);
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence_(g_dev, &fci, nullptr, &sl.fence);
        if (!MakeBuffer(sl.frame, sizeof(SodGeomFrame), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kHost)) { LinkFail("frame buffer"); return; }
        VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO }; dsa.descriptorPool = dp; dsa.descriptorSetCount = 1; dsa.pSetLayouts = &dsl;
        vkAllocateDescriptorSets_(g_dev, &dsa, &sl.ds);
    }
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    double latSum = 0, gpuSum = 0, buildSum = 0; uint32_t latN = 0, gpuN = 0; float latMax = 0, gpuMax = 0;

    uint32_t scratchW = 0, scratchH = 0;
    // budget guard: soft-shadow sample cap, lowered when the trace (which the game's GPU waits for) gets slow.
    // Kept per light count, so rooms where the light count changes from frame to frame (lamps switching in and
    // out) do not keep resetting it: each setup remembers its own cap, timing and back-off.
    struct SoftGuard { uint32_t cap = 64, hold = 0, backoff = 15, lastK = 0, n = 0; double sum = 0; };
    SoftGuard guard[SOD_MAX_LIGHTS + 1];
    uint32_t softSamples = 0; bool softNow = false;
    auto complete = [&](uint32_t s) {                  // fence of slot s signalled: publish results
        Slot& sl = slots[s];
        if (!sl.pendingValue) return;
        const uint32_t* c = (const uint32_t*)counts.map;
        g_shared->resultCount[s] = c[s];
        MemoryBarrier();
        g_shared->resultValue[s] = sl.pendingValue + 1;
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        float us = float(double(int64_t(t.QuadPart - sl.pendingQpc)) * 1e6 / double(qf.QuadPart));
        latSum += us; latN++; if (us > latMax) latMax = us;
        if (sl.timed && qp) {
            uint64_t ts[4] = { };     // build start/end, trace start/end
            if (vkGetQueryPoolResults_(g_dev, qp, 4 * s, 4, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                uint64_t mask = g_timestampBits >= 64 ? ~0ull : ((1ull << g_timestampBits) - 1);
                float bus = float(double((ts[1] - ts[0]) & mask) * g_timestampPeriod / 1000.0);
                float tus = float(double((ts[3] - ts[2]) & mask) * g_timestampPeriod / 1000.0);
                gpuSum += tus; gpuN++; if (tus > gpuMax) gpuMax = tus;
                SoftGuard& sg = guard[sl.softBucket <= SOD_MAX_LIGHTS ? sl.softBucket : SOD_MAX_LIGHTS];
                sg.sum += tus; sg.n++;
                if (sg.n >= 60) {               // over budget (avg trace > 4 ms of the 33 ms frame): halve the samples,
                    float avg = float(sg.sum / sg.n);   // down to hard; back up once there is plenty of room
                    if (sg.hold) sg.hold--;
                    if (sg.lastK >= 4 && avg > 4000.0f) {
                        sg.cap = sg.lastK / 2; sg.hold = sg.backoff;
                        if (sg.cap < 4) sg.backoff = sg.backoff < 240 ? sg.backoff * 2 : 240;
                        Log("soft shadows (%u lights): trace %.0f us over budget -> %u samples%s", sl.softBucket, avg, sg.cap, sg.cap < 4 ? " (hard)" : "");
                    } else if (sg.cap < 64 && avg < 1500.0f && !sg.hold) {
                        sg.cap = sg.cap < 4 ? 4 : sg.cap * 2;
                        Log("soft shadows (%u lights): trace %.0f us -> cap %u samples", sl.softBucket, avg, sg.cap);
                    }
                    sg.sum = 0; sg.n = 0;
                }
                buildSum += bus;
            }
        }
        if (latN >= 60) {
            g_shared->latencyAvgUs = float(latSum / latN); g_shared->latencyMaxUs = latMax;
            g_shared->latencySamples += latN; latSum = 0; latN = 0; latMax = 0;
            if (gpuN) { g_shared->gpuUsAvg = float(gpuSum / gpuN); g_shared->traceUsAvg = g_shared->gpuUsAvg; g_shared->gpuUsMax = gpuMax;
                        g_shared->buildUsAvg = float(buildSum / gpuN);
                        g_shared->softSamples = softNow ? softSamples : 0; }
            gpuSum = 0; gpuN = 0; gpuMax = 0; buildSum = 0;
        }
        sl.pendingValue = 0;
    };
    // soft shadows: scratch for the hit bits, the cone result and the tile flags; sized with the shared buffers
    Buf sBits, sSoft, sTiles;
    auto makeScratch = [&](uint32_t w, uint32_t h) -> bool {
        FreeBuf(sBits); FreeBuf(sSoft); FreeBuf(sTiles);
        VkDeviceSize px = (VkDeviceSize(w) * h + 3) & ~VkDeviceSize(3), tl = VkDeviceSize((w + 7) / 8) * ((h + 7) / 8) * 8;
        return MakeBuffer(sBits, px ? px : 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kGpu)
            && MakeBuffer(sSoft, px ? px * 4 : 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kGpu)    // 1 uint per soft pixel
            && MakeBuffer(sTiles, tl ? tl : 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kGpu);
    };
    auto updateSharedDescriptors = [&]() {
        for (uint32_t i = 0; i < SOD_BRIDGE_SLOTS; i++) {
            VkDescriptorBufferInfo bi[7] = { { depth.buf, 0, VK_WHOLE_SIZE }, { result.buf, 0, VK_WHOLE_SIZE },
                                             { slots[i].frame.buf, 0, VK_WHOLE_SIZE }, { counts.buf, 0, VK_WHOLE_SIZE },
                                             { sBits.buf, 0, VK_WHOLE_SIZE }, { sSoft.buf, 0, VK_WHOLE_SIZE }, { sTiles.buf, 0, VK_WHOLE_SIZE } };
            uint32_t bindings[7] = { 0, 1, 3, 4, 5, 6, 7 };
            VkWriteDescriptorSet wr[7] = { };
            for (uint32_t k = 0; k < 7; k++) {
                wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[k].dstSet = slots[i].ds; wr[k].dstBinding = bindings[k];
                wr[k].descriptorCount = 1; wr[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[k].pBufferInfo = &bi[k];
            }
            vkUpdateDescriptorSets_(g_dev, 7, wr, 0, nullptr);
        }
    };

    static FrameBuild fb;
    uint32_t tracedLogs = 0;
    double lagSum = 0; uint32_t lagN = 0;

    // profiling monitor: wall-clock time at which the game's GPU reaches each request (2k+1) and at which the
    // helper releases it (2k+2); the difference is exactly how long the game's GPU waits for the helper
    std::atomic<bool> monStop = { false };
    std::thread monitor;
    auto startMonitor = [&]() {
        monitor = std::thread([&]() {
            uint64_t v = 1; double reachSum = 0, stallSum = 0; float stallMax = 0; uint32_t n = 0;
            while (!monStop.load()) {
                uint64_t req = (uint64_t)InterlockedCompareExchange64((volatile LONG64*)&g_shared->requestValue, 0, 0);
                if (req < v) { Sleep(1); continue; }
                uint64_t cur = 0; vkGetSemaphoreCounterValue_(g_dev, sem, &cur);
                if (cur >= v) { v = (cur & 1u) ? cur + 2u : cur + 1u; continue; }   // missed it: next request not reached yet
                uint64_t posted = g_shared->req[(v >> 1) % SOD_BRIDGE_SLOTS].postedQpc;
                VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
                wi.semaphoreCount = 1; wi.pSemaphores = &sem;
                uint64_t want = v; wi.pValues = &want;
                VkResult r1 = VK_TIMEOUT;
                while (!monStop.load() && (r1 = vkWaitSemaphores_(g_dev, &wi, 100000000ull)) == VK_TIMEOUT) { }
                if (r1 != VK_SUCCESS) continue;
                LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
                want = v + 1;
                VkResult r2 = VK_TIMEOUT;
                while (!monStop.load() && (r2 = vkWaitSemaphores_(g_dev, &wi, 100000000ull)) == VK_TIMEOUT) { }
                if (r2 != VK_SUCCESS) continue;
                LARGE_INTEGER t2; QueryPerformanceCounter(&t2);
                double reach = double(int64_t(t1.QuadPart - posted)) * 1e6 / double(qf.QuadPart);
                double stall = double(t2.QuadPart - t1.QuadPart) * 1e6 / double(qf.QuadPart);
                if (reach >= 0 && reach < 1e6) { reachSum += reach; stallSum += stall; if (stall > stallMax) stallMax = float(stall); n++; }
                if (n >= 60) {
                    g_shared->reachUsAvg = float(reachSum / n); g_shared->stallUsAvg = float(stallSum / n);
                    g_shared->stallUsMax = stallMax; g_shared->profSamples += n;
                    reachSum = stallSum = 0; stallMax = 0; n = 0;
                }
                v += 2;
            }
        });
    };

    for (;;) {
        if (!reqEvent) reqEvent = OpenEventA(SYNCHRONIZE, FALSE, name);
        HANDLE hs[2] = { parentProc, reqEvent };
        DWORD w = WaitForMultipleObjects(reqEvent ? 2 : 1, hs, FALSE, reqEvent ? 2 : 250);
        if (w == WAIT_OBJECT_0) break;                                     // game exited
        for (uint32_t s = 0; s < SOD_BRIDGE_SLOTS; s++)
            if (slots[s].pendingValue && vkGetFenceStatus_(g_dev, slots[s].fence) == VK_SUCCESS) complete(s);
        if (g_shared->linkAbort || g_shared->linkState == SOD_LINK_FAILED) continue;

        for (;;) {
            uint64_t req = (uint64_t)InterlockedCompareExchange64((volatile LONG64*)&g_shared->requestValue, 0, 0);
            uint64_t next = served + 1;
            if (next > req) break;
            uint32_t s = (next >> 1) % SOD_BRIDGE_SLOTS;
            SodFrameRequest r = g_shared->req[s];
            if (r.value != next) { LinkFail("request ring overrun: expected %llu, slot holds %llu", (unsigned long long)next, (unsigned long long)r.value); break; }

            // (re)import the shared objects when the game created a new generation
            if (r.generation != imported) {
                uint32_t gen = g_shared->linkGeneration;
                if (gen != r.generation) break;                          // game is mid-recreation; it re-posts after
                vkDeviceWaitIdle_(g_dev);
                for (uint32_t k = 0; k < SOD_BRIDGE_SLOTS; k++) complete(k);
                FreeImported(depth); FreeImported(result);
                if (!sem) {
                    VkSemaphoreTypeCreateInfo ti = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO }; ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
                    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti };
                    if (vkCreateSemaphore_(g_dev, &sci, nullptr, &sem) != VK_SUCCESS) { LinkFail("semaphore create"); break; }
                    VkImportSemaphoreWin32HandleInfoKHR isi = { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
                    isi.semaphore = sem; isi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
                    isi.handle = (HANDLE)(uintptr_t)g_shared->hSemaphore;
                    VkResult vr = vkImportSemaphoreWin32HandleKHR_(g_dev, &isi);
                    if (vr != VK_SUCCESS) { LinkFail("semaphore import -> VkResult %d", (int)vr); break; }
                    CloseHandle((HANDLE)(uintptr_t)g_shared->hSemaphore);
                    startMonitor();
                }
                if (!ImportBuffer(depth,  g_shared->hDepthMem,  g_shared->depthBufSize,  g_shared->depthAllocSize,  g_shared->depthMemTypeBits)) break;
                if (!ImportBuffer(result, g_shared->hResultMem, g_shared->resultBufSize, g_shared->resultAllocSize, g_shared->resultMemTypeBits)) break;
                if (!makeScratch(g_shared->linkWidth, g_shared->linkHeight)) { LinkFail("soft-shadow scratch buffers"); break; }
                scratchW = g_shared->linkWidth; scratchH = g_shared->linkHeight;
                updateSharedDescriptors();
                imported = r.generation;
                g_shared->linkImportedGeneration = imported;
                g_shared->linkState = SOD_LINK_ACTIVE;
                Log("link: imported generation %u: %ux%u, semaphore at %llu", imported, g_shared->linkWidth,
                    g_shared->linkHeight, (unsigned long long)served);
            }
            if (!geom) {
                geomMap = OpenFileMappingA(FILE_MAP_READ, FALSE, gname);
                if (geomMap) geom = (const uint8_t*)MapViewOfFile(geomMap, FILE_MAP_READ, 0, 0, 0);
                if (!geom) { LinkFail("geometry mapping %s not found", gname); break; }
            }

            Slot& sl = slots[s];
            if (sl.pendingValue) { vkWaitForFences_(g_dev, 1, &sl.fence, VK_TRUE, 2000000000ull); complete(s); }
            const SodGeomFrame& g = *(const SodGeomFrame*)(geom + size_t(s) * SOD_GEOM_SLOT_BYTES);
            bool trace = r.hasGeometry && g.magic == SOD_GEOM_MAGIC && g.triCount > 0 && g.triCount <= SOD_GEOM_MAX_TRIS
                      && g.casterCount > 0 && g.casterCount <= SOD_MAX_CASTERS && g.lightCount > 0 && g.lightCount <= SOD_MAX_LIGHTS
                      && g.width == r.width && g.height == r.height && r.width * r.height * 4 <= depth.size
                      && r.width <= scratchW && r.height <= scratchH && size_t(r.width) * r.height <= size_t(scratchW) * scratchH;
            if (trace) {
                for (uint32_t c = 0; c < g.casterCount && trace; c++)
                    trace = g.casters[c].triCount > 0 && g.casters[c].firstTri + g.casters[c].triCount <= g.triCount;
                if (trace && !EnsureSlotBuffers(sl, g, fb, s, next >> 1)) break;
                trace = trace && fb.count > 0;
            }
            if (trace) {
                memcpy(sl.frame.map, &g, sizeof g);
                if (g.flags & 2u) {                // soft shadows: apply the budget guard's sample cap
                    SodGeomFrame* fm = (SodGeomFrame*)sl.frame.map;
                    SoftGuard& sg = guard[g.lightCount <= SOD_MAX_LIGHTS ? g.lightCount : SOD_MAX_LIGHTS];
                    uint32_t want = (g.flags >> 8) & 255u, k = want < sg.cap ? want : sg.cap;
                    sg.lastK = k >= 4 ? k : 0;
                    if (k < 4) fm->flags = g.flags & ~0xFF02u;                         // over budget: hard shadows
                    else       fm->flags = (g.flags & ~0xFF00u) | (k << 8);
                    softNow = k >= 4; softSamples = k;
                } else softNow = false;
                sl.softBucket = g.lightCount; sl.softK = softNow ? softSamples : 0;
                memcpy(sl.staging.map, (const uint8_t*)&g + sizeof(SodGeomFrame), size_t(g.triCount) * 36);
                VkAccelerationStructureInstanceKHR ins[SOD_MAX_LIGHTS] = { };
                for (uint32_t b = 0; b < fb.count; b++) {
                    ins[b].transform.matrix[0][0] = ins[b].transform.matrix[1][1] = ins[b].transform.matrix[2][2] = 1.0f;
                    ins[b].mask = uint8_t(1u << fb.light[b]);
                    ins[b].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
                    ins[b].accelerationStructureReference = sl.las[b].addr;
                }
                memcpy(sl.inst.map, ins, sizeof(VkAccelerationStructureInstanceKHR) * fb.count);
                VkWriteDescriptorSetAccelerationStructureKHR wa = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
                wa.accelerationStructureCount = 1; wa.pAccelerationStructures = &sl.tlas;
                VkWriteDescriptorSet wd = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &wa };
                wd.dstSet = sl.ds; wd.dstBinding = 2; wd.descriptorCount = 1; wd.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                vkUpdateDescriptorSets_(g_dev, 1, &wd, 0, nullptr);
            }

            vkResetFences_(g_dev, 1, &sl.fence);
            VkCommandBufferBeginInfo cbb = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; cbb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            sl.timed = trace && qp;
            // ---- (1) build: needs only the geometry (already here), so it runs while the game's GPU is still
            //      rendering the room - it is not part of the time the game's GPU waits for
            VkCommandBuffer cbB = sl.cbBuild;
            vkResetCommandBuffer_(cbB, 0);
            vkBeginCommandBuffer_(cbB, &cbb);
            if (sl.timed) {
                vkCmdResetQueryPool_(cbB, qp, 4 * s, 4);
                vkCmdWriteTimestamp_(cbB, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 4 * s);
            }
            if (trace) {
                VkBufferCopy cp = { 0, 0, VkDeviceSize(g.triCount) * 36 };
                vkCmdCopyBuffer_(cbB, sl.staging.buf, sl.verts.buf, 1, &cp);
                // the copy, and earlier requests' BLAS builds (refit sources; also their reads of a BLAS this
                // build now overwrites) -> this build
                VkMemoryBarrier vb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                vb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
                vb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
                vkCmdPipelineBarrier_(cbB, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &vb, 0, nullptr, 0, nullptr);
                vkCmdBuildAccelerationStructuresKHR_(cbB, fb.count, fb.bg, fb.pr);
                AsBarrier(cbB, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
                VkAccelerationStructureGeometryKHR tg = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
                tg.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
                tg.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
                tg.geometry.instances.data.deviceAddress = sl.inst.addr;
                VkAccelerationStructureBuildGeometryInfoKHR tb = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
                tb.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; tb.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
                tb.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
                tb.geometryCount = 1; tb.pGeometries = &tg; tb.dstAccelerationStructure = sl.tlas;
                tb.scratchData.deviceAddress = (sl.tlasScratch.addr + 255) & ~VkDeviceAddress(255);
                VkAccelerationStructureBuildRangeInfoKHR tr = { fb.count, 0, 0, 0 };
                const VkAccelerationStructureBuildRangeInfoKHR* ptr = &tr;
                vkCmdBuildAccelerationStructuresKHR_(cbB, 1, &tb, &ptr);
            }
            if (sl.timed) vkCmdWriteTimestamp_(cbB, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 4 * s + 1);
            vkEndCommandBuffer_(cbB);

            // ---- (2) trace: waits for the game's depth copy (2k+1), releases the game (2k+2)
            VkCommandBuffer cb = sl.cb;
            vkResetCommandBuffer_(cb, 0);
            vkBeginCommandBuffer_(cb, &cbb);
            if (sl.timed) vkCmdWriteTimestamp_(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 4 * s + 2);
            {   // the build above (earlier submission on this queue) -> ray queries here
                VkMemoryBarrier ab = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                ab.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                ab.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier_(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &ab, 0, nullptr, 0, nullptr);
            }
            vkCmdFillBuffer_(cb, counts.buf, s * 4, 4, 0);
            VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier_(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
            if (trace) {
                vkCmdBindPipeline_(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
                vkCmdBindDescriptorSets_(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &sl.ds, 0, nullptr);
                uint32_t words = (r.width * r.height + 3) / 4, ntiles = ((r.width + 7) / 8) * ((r.height + 7) / 8);
                for (uint32_t pass = 0; pass < (softNow ? 5u : 1u); pass++) {
                    if (pass) {
                        VkMemoryBarrier cb2 = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                        cb2.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; cb2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                        vkCmdPipelineBarrier_(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &cb2, 0, nullptr, 0, nullptr);
                    }
                    uint32_t pcv[2] = { s, pass };
                    vkCmdPushConstants_(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pcv);
                    uint32_t items = (pass == 1 || pass == 2) ? ntiles : pass == 3 ? r.width * r.height : words;
                    vkCmdDispatch_(cb, (items + 63) / 64, 1, 1);
                }
            } else {
                vkCmdFillBuffer_(cb, result.buf, 0, VK_WHOLE_SIZE, 0xFFFFFFFFu);   // nothing to trace: fully lit
            }
            VkMemoryBarrier hb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            hb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
            vkCmdPipelineBarrier_(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
            if (sl.timed) vkCmdWriteTimestamp_(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 4 * s + 3);
            vkEndCommandBuffer_(cb);

            uint64_t waitV = next, sigV = next + 1;
            VkTimelineSemaphoreSubmitInfo tsi = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
            tsi.waitSemaphoreValueCount = 1; tsi.pWaitSemaphoreValues = &waitV;
            tsi.signalSemaphoreValueCount = 1; tsi.pSignalSemaphoreValues = &sigV;
            VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo si[2] = { { VK_STRUCTURE_TYPE_SUBMIT_INFO }, { VK_STRUCTURE_TYPE_SUBMIT_INFO, &tsi } };
            si[0].commandBufferCount = 1; si[0].pCommandBuffers = &cbB;
            si[1].waitSemaphoreCount = 1; si[1].pWaitSemaphores = &sem; si[1].pWaitDstStageMask = &waitStage;
            si[1].commandBufferCount = 1; si[1].pCommandBuffers = &cb;
            si[1].signalSemaphoreCount = 1; si[1].pSignalSemaphores = &sem;
            VkResult vr = vkQueueSubmit_(g_queue, 2, si, sl.fence);
            if (vr != VK_SUCCESS) { LinkFail("submit for request %llu -> VkResult %d", (unsigned long long)next, (int)vr); break; }
            sl.pendingValue = next; sl.pendingQpc = r.postedQpc;
            { LARGE_INTEGER ts; QueryPerformanceCounter(&ts);
              double lag = double(int64_t(ts.QuadPart - r.postedQpc)) * 1e6 / double(qf.QuadPart);
              if (lag >= 0 && lag < 1e6) { lagSum += lag; lagN++; }
              if (lagN >= 60) { g_shared->submitLagUsAvg = float(lagSum / lagN); lagSum = 0; lagN = 0; } }
            served = sigV;
            g_shared->servedValue = served;
            g_shared->lastTris = trace ? g.triCount : 0; g_shared->lastCasters = trace ? g.casterCount : 0;
            g_shared->lastLights = trace ? g.lightCount : 0;
            if (tracedLogs < 3 || (served % 6000) == 0) {
                tracedLogs++;
                Log("link: request %llu (frame %u): %s %u light(s), %u caster(s), %u tris; GPU avg %.0f us (build %.0f) max %.0f us",
                    (unsigned long long)next, r.frame, trace ? "traced" : "skipped", g.lightCount, g.casterCount, g.triCount,
                    g_shared->gpuUsAvg, g_shared->buildUsAvg, g_shared->gpuUsMax);
            }
        }
    }
    monStop = true;
    if (monitor.joinable()) monitor.join();
    vkDeviceWaitIdle_(g_dev);
    FreeImported(depth); FreeImported(result);
    FreeBuf(sBits); FreeBuf(sSoft); FreeBuf(sTiles);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmd, int) {
    // log: %LOCALAPPDATA%\SoDRT\sod_rt64.log (Program Files is not writable for a 64-bit process)
    char path[MAX_PATH] = { };
    if (GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH)) {
        strcat_s(path, "\\SoDRT"); CreateDirectoryA(path, nullptr);
        strcat_s(path, "\\sod_rt64.log"); g_log = fopen(path, "w");
    }
    DWORD parent = (DWORD)strtoul(cmd ? cmd : "0", nullptr, 10);
    Log("sod_rt64 started, parent pid %lu", (unsigned long)parent);
    char name[64];
    snprintf(name, sizeof name, SOD_BRIDGE_MAPPING "%lu", (unsigned long)parent);
    HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!map) { Log("no bridge mapping %s", name); return 1; }
    g_shared = (SodBridgeShared*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SodBridgeShared));
    if (!g_shared || g_shared->magic != SOD_BRIDGE_MAGIC || g_shared->version != SOD_BRIDGE_VERSION) { Log("bad bridge header"); return 1; }
    g_shared->helperPid = GetCurrentProcessId();
    snprintf(name, sizeof name, SOD_BRIDGE_READY "%lu", (unsigned long)parent);
    HANDLE ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
    HANDLE parentProc = OpenProcess(SYNCHRONIZE, FALSE, parent);

    if (Run()) { g_shared->state = SOD_BRIDGE_READY_OK; Log("bridge ready"); }
    else if (g_shared->state != SOD_BRIDGE_FAILED) g_shared->state = SOD_BRIDGE_FAILED;
    if (ready) SetEvent(ready);

    // serve the game's frames (B2 link) until it exits
    if (g_shared->state == SOD_BRIDGE_READY_OK && parentProc) Serve(parentProc);
    else if (parentProc) WaitForSingleObject(parentProc, INFINITE);
    Log("parent exited, helper shutting down");
    return 0;
}
