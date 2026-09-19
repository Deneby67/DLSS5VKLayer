#pragma once
// Shared GPU utilities for the NR service and the isolated FG conformance probe.
#include "../core/ngx_abi.h"
#include "../core/logging.h"
#include "../common/shm_protocol.h"
#include "mvec_deadzone_spv.h"
#include <vector>
#include <cstring>
#include <unistd.h>
using dlssnr::Log;

// ---------------------------------------------------------------------------
// Vulkan context (from standalone_runner/main.cpp, verified)
// ---------------------------------------------------------------------------
static HMODULE g_vkModule = nullptr;
static PFN_vkGetInstanceProcAddr g_gipa = nullptr;

#define VK_FN(name) static PFN_##name name = nullptr;
VK_FN(vkCreateInstance) VK_FN(vkDestroyInstance) VK_FN(vkEnumeratePhysicalDevices)
VK_FN(vkGetPhysicalDeviceProperties) VK_FN(vkGetPhysicalDeviceFormatProperties) VK_FN(vkEnumerateDeviceExtensionProperties)
VK_FN(vkGetPhysicalDeviceQueueFamilyProperties) VK_FN(vkCreateDevice) VK_FN(vkDestroyDevice)
VK_FN(vkGetDeviceQueue) VK_FN(vkCreateCommandPool) VK_FN(vkDestroyCommandPool)
VK_FN(vkAllocateCommandBuffers) VK_FN(vkBeginCommandBuffer) VK_FN(vkEndCommandBuffer) VK_FN(vkResetCommandBuffer)
VK_FN(vkQueueSubmit) VK_FN(vkCreateFence) VK_FN(vkDestroyFence) VK_FN(vkWaitForFences)
VK_FN(vkResetFences) VK_FN(vkCreateImage) VK_FN(vkDestroyImage) VK_FN(vkCreateImageView) VK_FN(vkDestroyImageView)
VK_FN(vkGetImageMemoryRequirements) VK_FN(vkAllocateMemory) VK_FN(vkFreeMemory)
VK_FN(vkMapMemory) VK_FN(vkUnmapMemory) VK_FN(vkBindImageMemory)
VK_FN(vkGetImageSubresourceLayout)
VK_FN(vkCreateBuffer) VK_FN(vkDestroyBuffer) VK_FN(vkGetBufferMemoryRequirements)
VK_FN(vkBindBufferMemory) VK_FN(vkCmdCopyBufferToImage) VK_FN(vkCmdCopyImageToBuffer)
VK_FN(vkGetMemoryHostPointerPropertiesEXT)
VK_FN(vkGetMemoryFdKHR) VK_FN(vkGetMemoryFdPropertiesKHR)
VK_FN(vkCmdCopyImage) VK_FN(vkCmdPipelineBarrier) VK_FN(vkDeviceWaitIdle)
VK_FN(vkGetPhysicalDeviceProperties2) VK_FN(vkGetPhysicalDeviceOpticalFlowImageFormatsNV)
VK_FN(vkCreateOpticalFlowSessionNV) VK_FN(vkDestroyOpticalFlowSessionNV)
VK_FN(vkBindOpticalFlowSessionImageNV) VK_FN(vkCmdOpticalFlowExecuteNV) VK_FN(vkCmdBlitImage)
VK_FN(vkCmdPipelineBarrier2) VK_FN(vkQueueSubmit2) VK_FN(vkCmdWriteTimestamp2)
VK_FN(vkCreateQueryPool) VK_FN(vkDestroyQueryPool) VK_FN(vkCmdResetQueryPool)
VK_FN(vkCmdWriteTimestamp) VK_FN(vkCmdCopyQueryPoolResults)
VK_FN(vkCreateSemaphore) VK_FN(vkDestroySemaphore) VK_FN(vkCmdClearColorImage)
VK_FN(vkCreateShaderModule) VK_FN(vkDestroyShaderModule)
VK_FN(vkCreatePipelineLayout) VK_FN(vkDestroyPipelineLayout)
VK_FN(vkCreateComputePipelines) VK_FN(vkDestroyPipeline)
VK_FN(vkCmdBindPipeline) VK_FN(vkCmdDispatch) VK_FN(vkCmdBindDescriptorSets)
VK_FN(vkCreateDescriptorSetLayout) VK_FN(vkDestroyDescriptorSetLayout)
VK_FN(vkCreateDescriptorPool) VK_FN(vkDestroyDescriptorPool)
VK_FN(vkAllocateDescriptorSets) VK_FN(vkUpdateDescriptorSets)
#undef VK_FN



struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags aspect() const {
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
};

struct VkCtx {
    VkInstance instance = nullptr;
    VkPhysicalDevice physical = nullptr;
    VkDevice device = nullptr;
    VkQueue queue = nullptr;
    uint32_t queueFamily = 0;
    VkQueue opticalQueue = VK_NULL_HANDLE;
    uint32_t opticalQueueFamily = UINT32_MAX;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandPool cmdPoolFlow = VK_NULL_HANDLE;
    VkCommandBuffer cmdScratch = VK_NULL_HANDLE;
    VkCommandBuffer cmdCreate = VK_NULL_HANDLE;
    VkCommandBuffer cmdEval = VK_NULL_HANDLE;
    VkCommandBuffer cmdFlow = VK_NULL_HANDLE;
    VkCommandBuffer cmdFlowPost = VK_NULL_HANDLE;
    // Fence ring: every submit takes the next fence; the CPU only blocks on a
    // fence when its result is genuinely needed (never per-submit).
    static constexpr uint32_t kFenceRing = 8;
    VkFence fences[kFenceRing] = {};
    uint32_t fenceCursor = 0;
    // Graphics -> optical-flow -> graphics handoff for the async NVOF stages.
    VkSemaphore semPrep = VK_NULL_HANDLE;
    VkSemaphore semFlow = VK_NULL_HANDLE;
    VkBuffer uploadStaging = VK_NULL_HANDLE;
    VkBuffer readStaging = VK_NULL_HANDLE;
    VkDeviceMemory uploadMem = VK_NULL_HANDLE;
    VkDeviceMemory readMem = VK_NULL_HANDLE;
    void* uploadMap = nullptr;
    void* readMap = nullptr;
    size_t stagingSize = 0;
    bool opticalFlow = false;
    bool sync2 = false;
    // Phase 5: the dma-buf exchange. This process owns both images that cross the boundary -- the
    // proxy it reads and the answer it writes -- and exports each as a dma-buf whose fd number it
    // names in the shared header. The layer takes its own reference through pidfd_getfd.
    // The export fds stay open for as long as the images do; the layer's open is a fresh one.
    bool dmaBuf = false;
    uint32_t linuxPid = 0;
    GpuImage proxyIn{};
    int proxyExportFd = -1;
    uint32_t proxyGen = 0, proxyW = 0, proxyH = 0, proxySeq = 0;
    GpuImage answerOut{};
    int answerExportFd = -1;
    uint32_t answerGen = 0, answerSeq = 0;
    VkQueryPool flowQuery = nullptr;
    VkBuffer queryStaging = nullptr;
    VkDeviceMemory queryMem = nullptr;
    void* queryMap = nullptr;
    bool flowQueryAvailable = false;
    float timestampPeriod = 1.0f;
    uint32_t flowTimestampBits = 0;
    // Persistent MVec post pass (pipeline itself is per-size). The deadzone shader comes in two
    // compile-time variants -- one per flow format -- because a spec-constant branch on this driver
    // mispredicted and wrote NaN into MVec; the pipeline picks the module that matches the session.
    bool mvComputeSupported = false;
    VkShaderModule mvShaderFixed5 = nullptr;
    VkShaderModule mvShaderFloat = nullptr;
    VkDescriptorSetLayout mvDescLayout = nullptr;
    VkPipelineLayout mvPipeLayout = nullptr;
    // Transport: the shared-memory pixel regions imported as buffers via
    // VK_EXT_external_memory_host. When the driver accepts the import, the proxy upload and the
    // answer readback are GPU copies into and out of the mapping itself -- no staging, no memcpy.
    VkBuffer transportIn = VK_NULL_HANDLE;
    VkBuffer transportOut = VK_NULL_HANDLE;
    VkDeviceMemory transportInMem = VK_NULL_HANDLE;
    VkDeviceMemory transportOutMem = VK_NULL_HANDLE;
    void* transportInPtr = nullptr;
    void* transportOutPtr = nullptr;
    size_t transportBytes = 0;
};

static uint32_t FindMemoryType(VkCtx& c, uint32_t bits, VkMemoryPropertyFlags want);
static void DestroyImage2D(VkCtx& c, GpuImage& img);
static uint32_t FindHostMemoryType(VkCtx& c, uint32_t bits, bool preferCached);
static bool CreateStaging(VkCtx& c, size_t bytes);

static bool HasDeviceExt(VkPhysicalDevice phys, const char* name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, props.data());
    for (auto& p : props) if (!std::strcmp(p.extensionName, name)) return true;
    return false;
}

static bool CreateContext(VkCtx& c, bool frameGeneration = false, unsigned enumerateGroups = 0, bool extAddress = false) {
    g_vkModule = LoadLibraryA("vulkan-1.dll");
    if (!g_vkModule) { Log("[helper] no vulkan-1.dll"); return false; }
    g_gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(g_vkModule, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)g_gipa(nullptr, "vkCreateInstance");

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "dlssnr_helper";
    app.apiVersion = VK_API_VERSION_1_3;
    const char* instExts[] = { "VK_KHR_get_physical_device_properties2", "VK_EXT_debug_utils", "VK_KHR_device_group_creation" };
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = enumerateGroups==2 ? 3 : 2;
    ici.ppEnabledExtensionNames = instExts;
    if (vkCreateInstance(&ici, nullptr, &c.instance) != VK_SUCCESS) {
        // debug_utils unavailable: retry without it
        ici.enabledExtensionCount = 1;
        if (vkCreateInstance(&ici, nullptr, &c.instance) != VK_SUCCESS) { Log("[helper] vkCreateInstance failed"); return false; }
    }

#define LOAD(name) name = (PFN_##name)g_gipa(c.instance, #name);
    LOAD(vkDestroyInstance) LOAD(vkEnumeratePhysicalDevices) LOAD(vkGetPhysicalDeviceProperties)
    LOAD(vkGetPhysicalDeviceFormatProperties)
    LOAD(vkEnumerateDeviceExtensionProperties) LOAD(vkGetPhysicalDeviceQueueFamilyProperties)
    LOAD(vkCreateDevice) LOAD(vkDestroyDevice) LOAD(vkGetDeviceQueue) LOAD(vkCreateCommandPool)
    LOAD(vkDestroyCommandPool) LOAD(vkAllocateCommandBuffers) LOAD(vkBeginCommandBuffer)
    LOAD(vkEndCommandBuffer) LOAD(vkResetCommandBuffer) LOAD(vkQueueSubmit) LOAD(vkCreateFence) LOAD(vkDestroyFence)
    LOAD(vkWaitForFences) LOAD(vkResetFences) LOAD(vkCreateImage) LOAD(vkDestroyImage)
    LOAD(vkCreateImageView) LOAD(vkDestroyImageView) LOAD(vkGetImageMemoryRequirements)
    LOAD(vkAllocateMemory) LOAD(vkFreeMemory) LOAD(vkMapMemory) LOAD(vkUnmapMemory)
    LOAD(vkBindImageMemory) LOAD(vkGetImageSubresourceLayout) LOAD(vkCreateBuffer) LOAD(vkDestroyBuffer)
    LOAD(vkGetBufferMemoryRequirements) LOAD(vkBindBufferMemory) LOAD(vkCmdCopyBufferToImage)
    LOAD(vkCmdCopyImageToBuffer) LOAD(vkCmdCopyImage) LOAD(vkCmdPipelineBarrier) LOAD(vkDeviceWaitIdle)
    LOAD(vkGetMemoryHostPointerPropertiesEXT) LOAD(vkGetMemoryFdKHR) LOAD(vkGetMemoryFdPropertiesKHR)
    LOAD(vkGetPhysicalDeviceProperties2) LOAD(vkGetPhysicalDeviceOpticalFlowImageFormatsNV)
    LOAD(vkCreateOpticalFlowSessionNV) LOAD(vkDestroyOpticalFlowSessionNV)
    LOAD(vkBindOpticalFlowSessionImageNV) LOAD(vkCmdOpticalFlowExecuteNV) LOAD(vkCmdBlitImage)
    LOAD(vkCmdPipelineBarrier2) LOAD(vkQueueSubmit2) LOAD(vkCmdWriteTimestamp2)
    LOAD(vkCreateQueryPool) LOAD(vkDestroyQueryPool) LOAD(vkCmdResetQueryPool)
    LOAD(vkCmdWriteTimestamp) LOAD(vkCmdCopyQueryPoolResults)
    LOAD(vkCreateSemaphore) LOAD(vkDestroySemaphore) LOAD(vkCmdClearColorImage)
    LOAD(vkCreateShaderModule) LOAD(vkDestroyShaderModule)
    LOAD(vkCreatePipelineLayout) LOAD(vkDestroyPipelineLayout)
    LOAD(vkCreateComputePipelines) LOAD(vkDestroyPipeline)
    LOAD(vkCmdBindPipeline) LOAD(vkCmdDispatch) LOAD(vkCmdBindDescriptorSets)
    LOAD(vkCreateDescriptorSetLayout) LOAD(vkDestroyDescriptorSetLayout)
    LOAD(vkCreateDescriptorPool) LOAD(vkDestroyDescriptorPool)
    LOAD(vkAllocateDescriptorSets) LOAD(vkUpdateDescriptorSets)
#undef LOAD

    uint32_t devCount = 0;
    std::vector<VkPhysicalDevice> phys;
    if(enumerateGroups) {
        auto enumerate=(PFN_vkEnumeratePhysicalDeviceGroups)g_gipa(c.instance,
            enumerateGroups==2 ? "vkEnumeratePhysicalDeviceGroupsKHR" : "vkEnumeratePhysicalDeviceGroups");
        if(!enumerate || enumerate(c.instance,&devCount,nullptr)!=VK_SUCCESS)return false;
        std::vector<VkPhysicalDeviceGroupProperties> groups(devCount);
        for(auto& group:groups)group.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;
        if(enumerate(c.instance,&devCount,groups.data())!=VK_SUCCESS)return false;
        for(unsigned j=0;j<devCount;++j)for(unsigned k=0;k<groups[j].physicalDeviceCount;++k)
            phys.push_back(groups[j].physicalDevices[k]);
    } else {
        vkEnumeratePhysicalDevices(c.instance, &devCount, nullptr);
        phys.resize(devCount);
        vkEnumeratePhysicalDevices(c.instance, &devCount, phys.data());
    }
    for (auto p : phys) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(p, &props);
        if (props.vendorID != 0x10DE) continue;
        if (!HasDeviceExt(p, "VK_NVX_binary_import") || !HasDeviceExt(p, "VK_NVX_image_view_handle")) continue;
        c.physical = p;
        Log("[helper] device: %s", props.deviceName);
        break;
    }
    if (!c.physical) { Log("[helper] no NVIDIA device with NVX exts"); return false; }
    c.opticalFlow = HasDeviceExt(c.physical, VK_NV_OPTICAL_FLOW_EXTENSION_NAME);

    uint32_t famCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c.physical, &famCount, nullptr);
    std::vector<VkQueueFamilyProperties> fams(famCount);
    vkGetPhysicalDeviceQueueFamilyProperties(c.physical, &famCount, fams.data());
    bool haveQueue = false;
    for (uint32_t i = 0; i < famCount; ++i) {
        if ((fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            c.queueFamily = i; haveQueue = true;
            if (fams[i].queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV) break;
        }
    }
    if (!haveQueue) { Log("[helper] no graphics+compute queue"); return false; }
    for (uint32_t i = 0; i < famCount; ++i) {
        if (fams[i].queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV) {
            c.opticalQueueFamily = i;
            break;
        }
    }
    if (c.opticalQueueFamily == UINT32_MAX) {
        Log("[helper] no optical-flow queue family");
        c.opticalFlow = false;
    }
    Log("[helper] queue family=%u flags=%#x optical=%u",
        c.queueFamily, fams[c.queueFamily].queueFlags, c.opticalQueueFamily);

    float prio = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> qcis;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = c.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    qcis.push_back(qci);
    if (c.opticalFlow && c.opticalQueueFamily != UINT32_MAX && c.opticalQueueFamily != c.queueFamily) {
        VkDeviceQueueCreateInfo qciFlow{};
        qciFlow.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qciFlow.queueFamilyIndex = c.opticalQueueFamily;
        qciFlow.queueCount = 1;
        qciFlow.pQueuePriorities = &prio;
        qcis.push_back(qciFlow);
    }
    std::vector<const char*> enabled;
    for (const char* e : { "VK_NVX_binary_import", "VK_NVX_image_view_handle",
                           "VK_KHR_maintenance1", "VK_KHR_maintenance2", "VK_KHR_maintenance3",
                           "VK_KHR_maintenance4", "VK_KHR_buffer_device_address", "VK_KHR_push_descriptor",
                           "VK_KHR_synchronization2", VK_NV_OPTICAL_FLOW_EXTENSION_NAME,
                           VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
                           VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                           VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME })
        if (HasDeviceExt(c.physical, e)) enabled.push_back(e);
    if(extAddress) {
        if(!HasDeviceExt(c.physical,"VK_EXT_buffer_device_address"))return false;
        for(auto& name:enabled)if(!strcmp(name,"VK_KHR_buffer_device_address"))name="VK_EXT_buffer_device_address";
    }
    c.sync2 = HasDeviceExt(c.physical, "VK_KHR_synchronization2");
    VkPhysicalDeviceOpticalFlowFeaturesNV flowFeatures{};
    flowFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV;
    flowFeatures.opticalFlow = VK_TRUE;
    VkPhysicalDeviceSynchronization2Features sync2Features{};
    sync2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    sync2Features.synchronization2 = VK_TRUE;
    if (c.opticalFlow && c.sync2) flowFeatures.pNext = &sync2Features;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    if (c.opticalFlow) dci.pNext = &flowFeatures;
    else if (c.sync2) dci.pNext = &sync2Features;
    VkPhysicalDeviceBufferDeviceAddressFeatures bda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceBufferDeviceAddressFeaturesEXT extBda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT};
    if (frameGeneration) {
        auto getFeatures=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(g_gipa(c.instance,"vkGetPhysicalDeviceFeatures2"));
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext=extAddress?static_cast<void*>(&extBda):static_cast<void*>(&bda);
        if (!getFeatures) return false;
        getFeatures(c.physical,&features);
        if (!(extAddress?extBda.bufferDeviceAddress:bda.bufferDeviceAddress)) { Log("[fg] bufferDeviceAddress is unavailable"); return false; }
        if(extAddress) {
            extBda.bufferDeviceAddressCaptureReplay=VK_FALSE;extBda.bufferDeviceAddressMultiDevice=VK_FALSE;
            extBda.pNext=const_cast<void*>(dci.pNext);dci.pNext=&extBda;
        } else {
            bda.bufferDeviceAddressCaptureReplay=VK_FALSE; bda.bufferDeviceAddressMultiDevice=VK_FALSE;
            bda.pNext=const_cast<void*>(dci.pNext); dci.pNext=&bda;
        }
    }
    dci.queueCreateInfoCount = (uint32_t)qcis.size();
    dci.pQueueCreateInfos = qcis.data();
    dci.enabledExtensionCount = (uint32_t)enabled.size();
    dci.ppEnabledExtensionNames = enabled.data();
    if (vkCreateDevice(c.physical, &dci, nullptr, &c.device) != VK_SUCCESS) { Log("[helper] vkCreateDevice failed"); return false; }
    vkGetDeviceQueue(c.device, c.queueFamily, 0, &c.queue);
    if (c.opticalFlow && c.opticalQueueFamily != UINT32_MAX)
        vkGetDeviceQueue(c.device, c.opticalQueueFamily, 0, &c.opticalQueue);

    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(c.physical, &props);
        c.timestampPeriod = props.limits.timestampPeriod > 0.0f ? props.limits.timestampPeriod : 1.0f;
        const uint32_t queryFamily = (c.opticalFlow && c.opticalQueueFamily != UINT32_MAX)
            ? c.opticalQueueFamily : c.queueFamily;
        if (queryFamily < famCount) c.flowTimestampBits = fams[queryFamily].timestampValidBits;
    }

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = c.queueFamily;
    if (vkCreateCommandPool(c.device, &cpci, nullptr, &c.cmdPool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = c.cmdPool;
    cbai.commandBufferCount = 4;
    VkCommandBuffer bufs[4];
    if (vkAllocateCommandBuffers(c.device, &cbai, bufs) != VK_SUCCESS) return false;
    c.cmdScratch = bufs[0]; c.cmdCreate = bufs[1]; c.cmdEval = bufs[2]; c.cmdFlowPost = bufs[3];

    if (c.opticalFlow && c.opticalQueueFamily != UINT32_MAX) {
        VkCommandPoolCreateInfo fpci{};
        fpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        fpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        fpci.queueFamilyIndex = c.opticalQueueFamily;
        if (vkCreateCommandPool(c.device, &fpci, nullptr, &c.cmdPoolFlow) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo fcbai{};
        fcbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        fcbai.commandPool = c.cmdPoolFlow;
        fcbai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(c.device, &fcbai, &c.cmdFlow) != VK_SUCCESS) return false;
    }

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    for (uint32_t i = 0; i < VkCtx::kFenceRing; ++i) {
        if (vkCreateFence(c.device, &fci, nullptr, &c.fences[i]) != VK_SUCCESS) return false;
    }
    if (c.opticalFlow && c.opticalQueue && vkCreateSemaphore) {
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(c.device, &sci, nullptr, &c.semPrep) != VK_SUCCESS ||
            vkCreateSemaphore(c.device, &sci, nullptr, &c.semFlow) != VK_SUCCESS) {
            Log("[helper] semaphore creation failed, NVOF stays synchronous");
            if (c.semPrep) { vkDestroySemaphore(c.device, c.semPrep, nullptr); c.semPrep = nullptr; }
            if (c.semFlow) { vkDestroySemaphore(c.device, c.semFlow, nullptr); c.semFlow = nullptr; }
        }
    }

    // GPU MVec deadzone pass needs storage access on both flow and MVec formats.
    if (vkGetPhysicalDeviceFormatProperties && vkCreateShaderModule && vkCreateComputePipelines &&
        vkCmdDispatch && vkCreateDescriptorSetLayout && vkCreateDescriptorPool &&
        vkAllocateDescriptorSets && vkUpdateDescriptorSets && kMVecDeadzoneSpvFixed5Len) {
        VkFormatProperties u16{};
        VkFormatProperties f16{};
        vkGetPhysicalDeviceFormatProperties(c.physical, VK_FORMAT_R16G16_UINT, &u16);
        vkGetPhysicalDeviceFormatProperties(c.physical, VK_FORMAT_R16G16_SFLOAT, &f16);
        c.mvComputeSupported =
            (u16.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) &&
            (f16.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    }
    Log("[helper] mvec gpu compute supported=%d", int(c.mvComputeSupported));

    if (c.opticalFlow && c.flowTimestampBits && vkCreateQueryPool && vkCmdResetQueryPool &&
        vkCmdCopyQueryPoolResults && (vkCmdWriteTimestamp2 || vkCmdWriteTimestamp)) {
        VkQueryPoolCreateInfo qpi{};
        qpi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = 2;
        if (vkCreateQueryPool(c.device, &qpi, nullptr, &c.flowQuery) == VK_SUCCESS) {
            VkBufferCreateInfo bci{};
            bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bci.size = 16;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (vkCreateBuffer(c.device, &bci, nullptr, &c.queryStaging) == VK_SUCCESS) {
                VkMemoryRequirements req{};
                vkGetBufferMemoryRequirements(c.device, c.queryStaging, &req);
                VkMemoryAllocateInfo mai{};
                mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                mai.allocationSize = req.size;
                mai.memoryTypeIndex = FindHostMemoryType(c, req.memoryTypeBits, true);
                if (mai.memoryTypeIndex != UINT32_MAX &&
                    vkAllocateMemory(c.device, &mai, nullptr, &c.queryMem) == VK_SUCCESS &&
                    vkBindBufferMemory(c.device, c.queryStaging, c.queryMem, 0) == VK_SUCCESS &&
                    vkMapMemory(c.device, c.queryMem, 0, VK_WHOLE_SIZE, 0, &c.queryMap) == VK_SUCCESS) {
                    c.flowQueryAvailable = true;
                } else {
                    if (c.queryMem) vkFreeMemory(c.device, c.queryMem, nullptr);
                    if (c.queryStaging) vkDestroyBuffer(c.device, c.queryStaging, nullptr);
                    if (c.flowQuery) vkDestroyQueryPool(c.device, c.flowQuery, nullptr);
                    c.queryMem = nullptr; c.queryStaging = nullptr; c.flowQuery = nullptr; c.queryMap = nullptr;
                }
            } else if (c.flowQuery) {
                vkDestroyQueryPool(c.device, c.flowQuery, nullptr);
                c.flowQuery = nullptr;
            }
        }
    }
    Log("[helper] flow timestamp query=%d bits=%u period=%.3f",
        int(c.flowQueryAvailable), c.flowTimestampBits, c.timestampPeriod);

    // Staging is allocated on the first frame, at that frame's size, rather than at the largest frame
    // the protocol can carry. Every path that needs it grows it on demand already. Reserving the
    // maximum up front cost two host-visible buffers of kMaxFrame each -- which, once the protocol
    // grew to cover a supersampled 4K model raster, is a quarter of a gigabyte of pinned memory for a
    // game that may present at 1080p.
    return true;
}

static uint32_t FindMemoryType(VkCtx& c, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp{};
    PFN_vkGetPhysicalDeviceMemoryProperties getMP =
        (PFN_vkGetPhysicalDeviceMemoryProperties)g_gipa(c.instance, "vkGetPhysicalDeviceMemoryProperties");
    getMP(c.physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static uint32_t FindHostMemoryType(VkCtx& c, uint32_t bits, bool preferCached) {
    VkPhysicalDeviceMemoryProperties mp{};
    PFN_vkGetPhysicalDeviceMemoryProperties getMP =
        (PFN_vkGetPhysicalDeviceMemoryProperties)g_gipa(c.instance, "vkGetPhysicalDeviceMemoryProperties");
    getMP(c.physical, &mp);
    const VkMemoryPropertyFlags required =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int best = -1, bestScore = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(bits & (1u << i))) continue;
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & required) != required) continue;
        int score = 0;
        if (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) score += preferCached ? 100 : 20;
        if (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) score += 10;
        if (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) score -= 50;
        if (score > bestScore) { bestScore = score; best = (int)i; }
    }
    return best >= 0 ? (uint32_t)best : UINT32_MAX;
}

static bool CreateStaging(VkCtx& c, size_t bytes) {
    if (bytes <= c.stagingSize && c.uploadMap && c.readMap) return true;
    if (c.device) vkDeviceWaitIdle(c.device);
    auto destroy = [&]() {
        if (c.uploadStaging) vkDestroyBuffer(c.device, c.uploadStaging, nullptr);
        if (c.readStaging) vkDestroyBuffer(c.device, c.readStaging, nullptr);
        if (c.uploadMem) vkFreeMemory(c.device, c.uploadMem, nullptr);
        if (c.readMem) vkFreeMemory(c.device, c.readMem, nullptr);
        c.uploadStaging = c.readStaging = nullptr;
        c.uploadMem = c.readMem = nullptr;
        c.uploadMap = c.readMap = nullptr;
        c.stagingSize = 0;
    };
    destroy();

    auto make = [&](VkBuffer& buf, VkDeviceMemory& mem, void** map) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(c.device, &bci, nullptr, &buf) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(c.device, buf, &req);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindHostMemoryType(c, req.memoryTypeBits, true);
        if (mai.memoryTypeIndex == UINT32_MAX) {
            destroy();
            return false;
        }
        if (vkAllocateMemory(c.device, &mai, nullptr, &mem) != VK_SUCCESS ||
            vkBindBufferMemory(c.device, buf, mem, 0) != VK_SUCCESS ||
            vkMapMemory(c.device, mem, 0, VK_WHOLE_SIZE, 0, map) != VK_SUCCESS) {
            destroy();
            return false;
        }
        return true;
    };

    if (!make(c.uploadStaging, c.uploadMem, &c.uploadMap) ||
        !make(c.readStaging, c.readMem, &c.readMap)) {
        destroy();
        return false;
    }
    c.stagingSize = bytes;
    return true;
}

// Import one shared-memory region as a buffer (VK_EXT_external_memory_host). The driver names the
// memory type that may back the pointer, so the type is taken from that intersection rather than
// scored on property flags like ordinary host-visible memory.
static bool ImportTransportOne(VkCtx& c, void* ptr, size_t bytes, VkBufferUsageFlags usage,
                               VkBuffer& buf, VkDeviceMemory& mem) {
    VkMemoryHostPointerPropertiesEXT props{};
    props.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
    if (vkGetMemoryHostPointerPropertiesEXT(c.device,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr, &props) != VK_SUCCESS)
        return false;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkExternalMemoryBufferCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    bci.pNext = &ext;
    if (vkCreateBuffer(c.device, &bci, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(c.device, buf, &req);
    const uint32_t typeBits = req.memoryTypeBits & props.memoryTypeBits;
    if (!typeBits) { vkDestroyBuffer(c.device, buf, nullptr); buf = VK_NULL_HANDLE; return false; }
    uint32_t type = 0;
    while (!(typeBits & (1u << type))) ++type;
    VkImportMemoryHostPointerInfoEXT hpi{};
    hpi.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
    hpi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    hpi.pHostPointer = ptr;
    // The allocation size must be a multiple of the driver's import alignment (64 KiB on NVIDIA);
    // kMaxFrame already is, but the rounding keeps this correct for any region size.
    VkDeviceSize align = 65536;
    if (vkGetPhysicalDeviceProperties2) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProps{};
        hostProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &hostProps;
        vkGetPhysicalDeviceProperties2(c.physical, &p2);
        if (hostProps.minImportedHostPointerAlignment) align = hostProps.minImportedHostPointerAlignment;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &hpi;
    mai.allocationSize = (req.size + align - 1) & ~(align - 1);
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(c.device, &mai, nullptr, &mem) != VK_SUCCESS ||
        vkBindBufferMemory(c.device, buf, mem, 0) != VK_SUCCESS) {
        vkDestroyBuffer(c.device, buf, nullptr);
        buf = VK_NULL_HANDLE;
        mem = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

// Make the two pixel regions the GPU's own buffers. When this succeeds the proxy upload and the
// answer readback are device-side copies into and out of the file's pages -- no staging buffer and
// no host memcpy in the frame. When it fails (no extension, misaligned view, driver refuses) the
// staging paths below carry on exactly as before.
static bool ImportTransport(VkCtx& c, void* in, void* out, size_t bytes) {
    if (c.transportIn && c.transportInPtr == in && c.transportOutPtr == out && c.transportBytes == bytes)
        return true;
    if (c.transportIn) vkDestroyBuffer(c.device, c.transportIn, nullptr);
    if (c.transportOut) vkDestroyBuffer(c.device, c.transportOut, nullptr);
    if (c.transportInMem) vkFreeMemory(c.device, c.transportInMem, nullptr);
    if (c.transportOutMem) vkFreeMemory(c.device, c.transportOutMem, nullptr);
    c.transportIn = c.transportOut = VK_NULL_HANDLE;
    c.transportInMem = c.transportOutMem = VK_NULL_HANDLE;
    c.transportInPtr = c.transportOutPtr = nullptr;
    c.transportBytes = 0;
    if (!vkGetMemoryHostPointerPropertiesEXT || !in || !out || !bytes) return false;

    VkDeviceSize align = 0;
    if (vkGetPhysicalDeviceProperties2) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProps{};
        hostProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &hostProps;
        vkGetPhysicalDeviceProperties2(c.physical, &props2);
        align = hostProps.minImportedHostPointerAlignment;
    }
    if (!align) align = 1;
    if ((reinterpret_cast<uintptr_t>(in) | reinterpret_cast<uintptr_t>(out)) % align) {
        Log("[helper] view is not %llu-byte aligned; keeping staging transport",
            (unsigned long long)align);
        return false;
    }
    if (!ImportTransportOne(c, in, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            c.transportIn, c.transportInMem))
        return false;
    if (!ImportTransportOne(c, out, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            c.transportOut, c.transportOutMem)) {
        vkDestroyBuffer(c.device, c.transportIn, nullptr);
        vkFreeMemory(c.device, c.transportInMem, nullptr);
        c.transportIn = VK_NULL_HANDLE;
        c.transportInMem = VK_NULL_HANDLE;
        return false;
    }
    c.transportInPtr = in;
    c.transportOutPtr = out;
    c.transportBytes = bytes;
    Log("[helper] transport imported: the shared-memory regions are the GPU's buffers");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 5: dma-buf transport
// ---------------------------------------------------------------------------
// The proxy arrives as a file descriptor over the socket and becomes an image of this device over
// the proxy it reads and the answer it writes. Both are optional -- without the channel, the
// extension, or a successful export, the shared-memory transport above carries the frame. The
// images are CONCURRENT and cross the boundary through FOREIGN_EXT: the layer acquires the proxy
// from FOREIGN before its first write and releases it back before this process reads it, and the
// answer is released to FOREIGN before the answer number moves.

// Build an RGBA8 image in memory that can leave this process as a dma-buf, and hand out the fd.
// The fd stays open here: the layer opens its own reference through /proc, and the number must
// stay meaningful until the image is rebuilt.
static bool CreateExportable(VkCtx& c, GpuImage& img, int& exportFd, uint32_t w, uint32_t h,
                             VkFormat fmt, const char* what) {
    if (img.image && img.width == w && img.height == h && img.format == fmt && exportFd >= 0)
        return true;
    DestroyImage2D(c, img);
    if (exportFd >= 0) close(exportFd);
    exportFd = -1;
    if (!vkGetMemoryFdKHR || !w || !h) return false;

    VkExternalMemoryImageCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.pNext = &ext;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = { w, h, 1 };
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(c.device, &ci, nullptr, &img.image) != VK_SUCCESS) return false;
    img.format = fmt;
    img.width = w;
    img.height = h;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(c.device, img.image, &req);
    uint32_t type = FindMemoryType(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) { DestroyImage2D(c, img); return false; }
    VkExportMemoryAllocateInfo exp{};
    exp.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &exp;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(c.device, &mai, nullptr, &img.memory) != VK_SUCCESS ||
        vkBindImageMemory(c.device, img.image, img.memory, 0) != VK_SUCCESS) {
        DestroyImage2D(c, img);
        return false;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(c.device, &vi, nullptr, &img.view) != VK_SUCCESS) {
        DestroyImage2D(c, img);
        return false;
    }
    VkMemoryGetFdInfoKHR gfi{};
    gfi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    gfi.memory = img.memory;
    gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    if (vkGetMemoryFdKHR(c.device, &gfi, &exportFd) != VK_SUCCESS) {
        DestroyImage2D(c, img);
        exportFd = -1;
        return false;
    }
    Log("[fd] %s exportable %ux%u fd=%d", what, w, h, exportFd);
    return true;
}

// Keep the proxy image at the current raster and publish its descriptor. The layer writes this
// image; this process reads it. Returns true when the image exists at this size.
static bool EnsureProxyOut(VkCtx& c, ShmHeader* hdr, uint32_t w, uint32_t h, VkFormat fmt) {
    const bool sizeChanged = c.proxyIn.image &&
                             (c.proxyIn.width != w || c.proxyIn.height != h || c.proxyIn.format != fmt);
    if (!CreateExportable(c, c.proxyIn, c.proxyExportFd, w, h, fmt, "proxy")) {
        c.proxyW = c.proxyH = 0;
        return false;
    }
    c.proxyW = w;
    c.proxyH = h;
    if (sizeChanged || c.proxySeq == 0) {
        ++c.proxyGen;
        ++c.proxySeq;
    }
    // Restate the descriptor whenever the header does not carry this sequence -- first frame,
    // rebuild, or a header re-initialised by another process under us.
    if (hdr->proxyExportSeq.load() != c.proxySeq) {
        hdr->proxyPid.store(c.linuxPid);
        hdr->proxyFd.store(uint32_t(c.proxyExportFd));
        hdr->proxyGen.store(c.proxyGen);
        std::atomic_thread_fence(std::memory_order_release);
        hdr->proxyExportSeq.store(c.proxySeq);
    }
    return true;
}

static bool EnsureAnswerOut(VkCtx& c, ShmHeader* hdr, uint32_t w, uint32_t h, VkFormat fmt) {
    const bool sizeChanged = c.answerOut.image &&
                             (c.answerOut.width != w || c.answerOut.height != h || c.answerOut.format != fmt);
    if (!CreateExportable(c, c.answerOut, c.answerExportFd, w, h, fmt, "answer")) {
        if (c.answerOut.image == VK_NULL_HANDLE && hdr->answerExportSeq.load()) {
            ++c.answerGen;
            hdr->answerExportSeq.store(0);  // withdrawn: the layer must not open a stale number
        }
        return false;
    }
    if (sizeChanged || c.answerSeq == 0) {
        ++c.answerGen;
        ++c.answerSeq;
    }
    if (hdr->answerExportSeq.load() != c.answerSeq) {
        hdr->answerPid.store(c.linuxPid);
        hdr->answerFd.store(uint32_t(c.answerExportFd));
        hdr->answerGen.store(c.answerGen);
        std::atomic_thread_fence(std::memory_order_release);
        hdr->answerExportSeq.store(c.answerSeq);
    }
    return true;
}

// The buffer a colorIn upload should read from: the imported mapping when there is one, the
// staging copy otherwise. Callers that upload something other than the proxy pass uploadStaging
// explicitly.
static VkBuffer UploadSource(VkCtx& c) {
    return c.transportIn ? c.transportIn : c.uploadStaging;
}

static bool CreateImage2DUsage(VkCtx& c, VkFormat fmt, uint32_t w, uint32_t h,
                               VkImageUsageFlags usage, GpuImage& out) {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = { w, h, 1 };
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    const uint32_t queueFamilies[] = { c.queueFamily, c.opticalQueueFamily };
    const bool crossQueue = c.opticalFlow && c.opticalQueueFamily != UINT32_MAX &&
                            c.opticalQueueFamily != c.queueFamily;
    ci.sharingMode = crossQueue ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    ci.queueFamilyIndexCount = crossQueue ? 2u : 0u;
    ci.pQueueFamilyIndices = crossQueue ? queueFamilies : nullptr;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(c.device, &ci, nullptr, &out.image) != VK_SUCCESS) return false;
    out.format = fmt; out.width = w; out.height = h; out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(c.device, out.image, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) { DestroyImage2D(c, out); return false; }
    if (vkAllocateMemory(c.device, &mai, nullptr, &out.memory) != VK_SUCCESS) {
        DestroyImage2D(c, out);
        return false;
    }
    if (vkBindImageMemory(c.device, out.image, out.memory, 0) != VK_SUCCESS) {
        DestroyImage2D(c, out);
        return false;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = { out.aspect(), 0, 1, 0, 1 };
    if (vkCreateImageView(c.device, &vi, nullptr, &out.view) != VK_SUCCESS) {
        DestroyImage2D(c, out);
        return false;
    }
    return true;
}

static bool CreateImage2D(VkCtx& c, VkFormat fmt, uint32_t w, uint32_t h, GpuImage& out) {
    return CreateImage2DUsage(c, fmt, w, h,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, out);
}

static bool CreateImage2DOpticalFlow(VkCtx& c, VkFormat fmt, uint32_t w, uint32_t h,
                                     VkOpticalFlowUsageFlagsNV ofUsage, GpuImage& out) {
    VkOpticalFlowImageFormatInfoNV ofInfo{};
    ofInfo.sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV;
    ofInfo.usage = ofUsage;
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.pNext = &ofInfo;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = { w, h, 1 };
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    // STORAGE: the MVec deadzone compute pass reads the raw flow texels through
    // a size-compatible R16G16_UINT view (no image->image copy out of the
    // optical-flow output, which the driver cannot sample/copy as bits).
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    const uint32_t queueFamilies[] = { c.queueFamily, c.opticalQueueFamily };
    const bool crossQueue = c.opticalFlow && c.opticalQueueFamily != UINT32_MAX &&
                            c.opticalQueueFamily != c.queueFamily;
    ci.sharingMode = crossQueue ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    ci.queueFamilyIndexCount = crossQueue ? 2u : 0u;
    ci.pQueueFamilyIndices = crossQueue ? queueFamilies : nullptr;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(c.device, &ci, nullptr, &out.image) != VK_SUCCESS) return false;
    out.format = fmt; out.width = w; out.height = h; out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(c.device, out.image, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) { DestroyImage2D(c, out); return false; }
    if (vkAllocateMemory(c.device, &mai, nullptr, &out.memory) != VK_SUCCESS) {
        DestroyImage2D(c, out);
        return false;
    }
    if (vkBindImageMemory(c.device, out.image, out.memory, 0) != VK_SUCCESS) {
        DestroyImage2D(c, out);
        return false;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = { out.aspect(), 0, 1, 0, 1 };
    if (vkCreateImageView(c.device, &vi, nullptr, &out.view) != VK_SUCCESS) {
        DestroyImage2D(c, out);
        return false;
    }
    return true;
}

static void DestroyImage2D(VkCtx& c, GpuImage& img) {
    if (img.view) vkDestroyImageView(c.device, img.view, nullptr);
    if (img.image) vkDestroyImage(c.device, img.image, nullptr);
    if (img.memory) vkFreeMemory(c.device, img.memory, nullptr);
    img = {};
}

static size_t ImageSizeBytes(VkCtx& c, GpuImage& img) {
    if (vkGetImageSubresourceLayout) {
        VkSubresourceLayout sl{};
        VkImageSubresource sub{};
        sub.aspectMask = img.aspect();
        vkGetImageSubresourceLayout(c.device, img.image, &sub, &sl);
        if (sl.size) return sl.size;
    }
    switch (img.format) {
        case VK_FORMAT_R16G16_SFLOAT: return size_t(img.width) * img.height * 4;
        case VK_FORMAT_R16G16_SFIXED5_NV: return size_t(img.width) * img.height * 4;
        case VK_FORMAT_R32_SFLOAT: return size_t(img.width) * img.height * 4;
        default: return size_t(img.width) * img.height * 4;
    }
}

static void TransitionImage(VkCtx& c, VkCommandBuffer cb, GpuImage& img, VkImageLayout dst,
                            VkAccessFlags srcA, VkAccessFlags dstA,
                            VkPipelineStageFlags ss, VkPipelineStageFlags ds);

static void TransitionImage2(VkCtx& c, VkCommandBuffer cb, GpuImage& img, VkImageLayout dst,
                             VkAccessFlags2 srcA, VkAccessFlags2 dstA,
                             VkPipelineStageFlags2 ss, VkPipelineStageFlags2 ds) {
    if (img.layout == dst) return;
    if (c.sync2 && vkCmdPipelineBarrier2) {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = ss;
        b.srcAccessMask = srcA;
        b.dstStageMask = ds;
        b.dstAccessMask = dstA;
        b.oldLayout = img.layout;
        b.newLayout = dst;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img.image;
        b.subresourceRange = { img.aspect(), 0, 1, 0, 1 };
        VkDependencyInfo di{};
        di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &di);
        img.layout = dst;
        return;
    }
    TransitionImage(c, cb, img, dst,
                    VkAccessFlags(srcA & 0xFFFFFFFFu), VkAccessFlags(dstA & 0xFFFFFFFFu),
                    VkPipelineStageFlags(ss & 0xFFFFFFFFu), VkPipelineStageFlags(ds & 0xFFFFFFFFu));
}

static void TransitionImage(VkCtx& c, VkCommandBuffer cb, GpuImage& img, VkImageLayout dst,
                            VkAccessFlags srcA, VkAccessFlags dstA,
                            VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    if (img.layout == dst) return;
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = img.layout; b.newLayout = dst;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = { img.aspect(), 0, 1, 0, 1 };
    b.srcAccessMask = srcA; b.dstAccessMask = dstA;
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    img.layout = dst;
}

static bool BeginCmd(VkCommandBuffer cb) {
    if (vkResetCommandBuffer) vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS;
}

// Submits asynchronously onto the fence ring; returns the fence index (or -1).
// The caller waits only when it actually needs the result, so the CPU never
// blocks on intermediate stages (upload, NVOF prep, flow post).
static int SubmitAsync(VkCtx& c, VkCommandBuffer cb, VkQueue queue,
                       uint32_t waitCount, const VkSemaphore* waits,
                       VkPipelineStageFlags2 waitStage, VkSemaphore signal) {
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) return -1;
    if ((waitCount || signal) && (!c.sync2 || !vkQueueSubmit2)) return -1;
    if (waitCount || signal) {
        VkCommandBufferSubmitInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cbi.commandBuffer = cb;
        VkSemaphoreSubmitInfo wsi{};
        if (waitCount) {
            wsi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            wsi.semaphore = waits[0];
            wsi.stageMask = waitStage;
        }
        VkSemaphoreSubmitInfo ssi{};
        if (signal) {
            ssi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            ssi.semaphore = signal;
            ssi.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
        VkSubmitInfo2 si2{};
        si2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        si2.waitSemaphoreInfoCount = waitCount;
        si2.pWaitSemaphoreInfos = waitCount ? &wsi : nullptr;
        si2.commandBufferInfoCount = 1;
        si2.pCommandBufferInfos = &cbi;
        si2.signalSemaphoreInfoCount = signal ? 1u : 0u;
        si2.pSignalSemaphoreInfos = signal ? &ssi : nullptr;
        const uint32_t idx = c.fenceCursor;
        const VkResult sr = vkQueueSubmit2(queue, 1, &si2, c.fences[idx]);
        if (sr != VK_SUCCESS) { Log("[vk] queue submit2 failed: %d", (int)sr); return -1; }
        c.fenceCursor = (c.fenceCursor + 1) % VkCtx::kFenceRing;
        return (int)idx;
    }
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    const uint32_t idx = c.fenceCursor;
    const VkResult sr = vkQueueSubmit(queue, 1, &si, c.fences[idx]);
    if (sr != VK_SUCCESS) { Log("[vk] queue submit failed: %d", (int)sr); return -1; }
    c.fenceCursor = (c.fenceCursor + 1) % VkCtx::kFenceRing;
    return (int)idx;
}

static bool WaitFence(VkCtx& c, int idx) {
    if (idx < 0) return false;
    if (vkWaitForFences(c.device, 1, &c.fences[idx], VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;
    vkResetFences(c.device, 1, &c.fences[idx]);
    return true;
}

static bool SubmitAndWaitQueue(VkCtx& c, VkCommandBuffer cb, VkQueue queue) {
    const int idx = SubmitAsync(c, cb, queue, 0, nullptr, 0, nullptr);
    return idx >= 0 && WaitFence(c, idx);
}

static bool SubmitAndWait(VkCtx& c, VkCommandBuffer cb) {
    return SubmitAndWaitQueue(c, cb, c.queue);
}

static bool UploadMappedPixels(VkCtx& c, GpuImage& img, size_t bytes,
                               VkBuffer srcOverride = VK_NULL_HANDLE) {
    const VkBuffer src = srcOverride ? srcOverride : c.uploadStaging;
    if (!src) return false;
    if (!srcOverride && (!c.uploadMap || bytes > c.stagingSize)) return false;
    if (!BeginCmd(c.cmdScratch)) return false;
    TransitionImage(c, c.cmdScratch, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { img.aspect(), 0, 0, 1 };
    region.imageExtent = { img.width, img.height, 1 };
    vkCmdCopyBufferToImage(c.cmdScratch, src, img.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    return SubmitAndWait(c, c.cmdScratch);
}

static bool ReadbackPixels(VkCtx& c, GpuImage& img, size_t bytes) {
    if (c.transportOut) {
        // The answer lands in the shared-memory region itself; the layer's acquire fence on
        // seq_resp is the only ordering the bytes need beyond this submit.
        if (!BeginCmd(c.cmdEval)) return false;
        TransitionImage(c, c.cmdEval, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = { img.aspect(), 0, 0, 1 };
        region.imageExtent = { img.width, img.height, 1 };
        vkCmdCopyImageToBuffer(c.cmdEval, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               c.transportOut, 1, &region);
        return SubmitAndWait(c, c.cmdEval);
    }
    if (bytes > c.stagingSize && !CreateStaging(c, bytes)) return false;
    if (!c.readMap) return false;
    if (!BeginCmd(c.cmdEval)) return false;
    TransitionImage(c, c.cmdEval, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { img.aspect(), 0, 0, 1 };
    region.imageExtent = { img.width, img.height, 1 };
    vkCmdCopyImageToBuffer(c.cmdEval, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           c.readStaging, 1, &region);
    return SubmitAndWait(c, c.cmdEval);
}

static bool UploadPixels(VkCtx& c, GpuImage& img, const void* pixels, size_t bytes) {
    if (bytes > c.stagingSize && !CreateStaging(c, bytes)) return false;
    if (!c.uploadMap) return false;
    if (pixels != c.uploadMap) std::memcpy(c.uploadMap, pixels, bytes);
    return UploadMappedPixels(c, img, bytes);
}
