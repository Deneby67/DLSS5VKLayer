// Test the actual Win32 surface/swapchain path, including direct PE exports.
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <vector>
static unsigned errors=0;
static VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT s,VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* d,void*) {
    if(s&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)++errors;
    fprintf(stderr,"[wsi-validation] %s\n",d->pMessage);return VK_FALSE;
}
#define REQUIRE(c) do {if(!(c)){fprintf(stderr,"WSI failed line %d: %s\n",__LINE__,#c);return 2;}}while(0)
int main() {
    auto mod=LoadLibraryW(L"vulkan-1.dll");REQUIRE(mod);
    auto gipa=(PFN_vkGetInstanceProcAddr)GetProcAddress(mod,"vkGetInstanceProcAddr");REQUIRE(gipa);
    // Unlike the HDR fixture, call vkCreateInstance before the first GIPA.
    auto create=(PFN_vkCreateInstance)GetProcAddress(mod,"vkCreateInstance");REQUIRE(create);
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};ai.apiVersion=VK_API_VERSION_1_1;
    const char* ext[]={"VK_KHR_surface","VK_KHR_win32_surface","VK_EXT_debug_utils"};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&ai;
    ici.enabledExtensionCount=3;ici.ppEnabledExtensionNames=ext;
    VkInstance instance{};REQUIRE(create(&ici,nullptr,&instance)==VK_SUCCESS);
#define I(fn) auto fn=(PFN_##fn)gipa(instance,#fn);REQUIRE(fn)
    I(vkDestroyInstance);I(vkCreateDebugUtilsMessengerEXT);I(vkDestroyDebugUtilsMessengerEXT);
    I(vkCreateWin32SurfaceKHR);I(vkDestroySurfaceKHR);I(vkEnumeratePhysicalDevices);
    I(vkGetPhysicalDeviceQueueFamilyProperties);I(vkGetPhysicalDeviceSurfaceSupportKHR);
    I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);I(vkGetPhysicalDeviceSurfaceFormatsKHR);I(vkCreateDevice);
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
    debug.pfnUserCallback=Validation;
    VkDebugUtilsMessengerEXT messenger{};REQUIRE(vkCreateDebugUtilsMessengerEXT(instance,&debug,nullptr,&messenger)==VK_SUCCESS);
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"NRBootstrapProbe";
    REQUIRE(RegisterClassW(&wc));
    auto window=CreateWindowW(wc.lpszClassName,L"DLSSNR isolated Vulkan startup test",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
        32,32,640,480,nullptr,nullptr,wc.hInstance,nullptr);REQUIRE(window);
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};sci.hinstance=wc.hInstance;sci.hwnd=window;
    VkSurfaceKHR surface{};REQUIRE(vkCreateWin32SurfaceKHR(instance,&sci,nullptr,&surface)==VK_SUCCESS);
    uint32_t count=0;REQUIRE(vkEnumeratePhysicalDevices(instance,&count,nullptr)==VK_SUCCESS && count);
    std::vector<VkPhysicalDevice> physicals(count);REQUIRE(vkEnumeratePhysicalDevices(instance,&count,physicals.data())==VK_SUCCESS);
    VkPhysicalDevice physical{};uint32_t family=0;
    for(auto candidate:physicals) {
        uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,nullptr);
        std::vector<VkQueueFamilyProperties> qs(n);vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,qs.data());
        for(uint32_t j=0;j<n;++j){VkBool32 present=VK_FALSE;
            REQUIRE(vkGetPhysicalDeviceSurfaceSupportKHR(candidate,j,surface,&present)==VK_SUCCESS);
            if(present && (qs[j].queueFlags&VK_QUEUE_GRAPHICS_BIT)){physical=candidate;family=j;break;}}
        if(physical)break;
    }
    REQUIRE(physical);float priority=1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
    const char* deviceExt="VK_KHR_swapchain";
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;
    dci.enabledExtensionCount=1;dci.ppEnabledExtensionNames=&deviceExt;
    VkDevice device{};REQUIRE(vkCreateDevice(physical,&dci,nullptr,&device)==VK_SUCCESS);
    auto gdpa=(PFN_vkGetDeviceProcAddr)gipa(instance,"vkGetDeviceProcAddr");REQUIRE(gdpa);
#define D(fn) auto fn=(PFN_##fn)gdpa(device,#fn);REQUIRE(fn)
    D(vkDestroyDevice);D(vkGetDeviceQueue);D(vkCreateSwapchainKHR);D(vkGetSwapchainImagesKHR);D(vkDestroySwapchainKHR);
    D(vkCreateCommandPool);D(vkAllocateCommandBuffers);D(vkDestroyCommandPool);D(vkResetCommandBuffer);
    D(vkBeginCommandBuffer);D(vkEndCommandBuffer);D(vkCmdPipelineBarrier);D(vkCmdClearColorImage);
    D(vkCreateSemaphore);D(vkDestroySemaphore);D(vkAcquireNextImageKHR);D(vkQueueSubmit);D(vkQueuePresentKHR);D(vkQueueWaitIdle);
    VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);
    VkSurfaceCapabilitiesKHR caps{};REQUIRE(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps)==VK_SUCCESS);
    REQUIRE(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    count=0;REQUIRE(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,nullptr)==VK_SUCCESS && count);
    std::vector<VkSurfaceFormatKHR> formats(count);REQUIRE(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,formats.data())==VK_SUCCESS);
    auto format=formats[0];if(format.format==VK_FORMAT_UNDEFINED)format.format=VK_FORMAT_B8G8R8A8_UNORM;
    VkSwapchainCreateInfoKHR swapInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};swapInfo.surface=surface;
    swapInfo.minImageCount=caps.minImageCount;swapInfo.imageFormat=format.format;swapInfo.imageColorSpace=format.colorSpace;
    swapInfo.imageExtent=caps.currentExtent;swapInfo.imageArrayLayers=1;swapInfo.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapInfo.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;swapInfo.preTransform=caps.currentTransform;
    swapInfo.compositeAlpha=VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha&(~caps.supportedCompositeAlpha+1));
    swapInfo.presentMode=VK_PRESENT_MODE_FIFO_KHR;swapInfo.clipped=VK_TRUE;
    VkSwapchainKHR swap{};REQUIRE(vkCreateSwapchainKHR(device,&swapInfo,nullptr,&swap)==VK_SUCCESS);
    count=0;REQUIRE(vkGetSwapchainImagesKHR(device,swap,&count,nullptr)==VK_SUCCESS);
    std::vector<VkImage> images(count);REQUIRE(vkGetSwapchainImagesKHR(device,swap,&count,images.data())==VK_SUCCESS);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pci.queueFamilyIndex=family;pci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool{};REQUIRE(vkCreateCommandPool(device,&pci,nullptr,&pool)==VK_SUCCESS);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;
    VkCommandBuffer cmd{};REQUIRE(vkAllocateCommandBuffers(device,&cai,&cmd)==VK_SUCCESS);
    VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};VkSemaphore ready{},done{};
    REQUIRE(vkCreateSemaphore(device,&semInfo,nullptr,&ready)==VK_SUCCESS);REQUIRE(vkCreateSemaphore(device,&semInfo,nullptr,&done)==VK_SUCCESS);
    for(unsigned frame=0;frame<3;++frame) {
        MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        uint32_t index=0;REQUIRE(vkAcquireNextImageKHR(device,swap,UINT64_MAX,ready,VK_NULL_HANDLE,&index)==VK_SUCCESS);
        REQUIRE(vkResetCommandBuffer(cmd,0)==VK_SUCCESS);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};REQUIRE(vkBeginCommandBuffer(cmd,&begin)==VK_SUCCESS);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=images[index];barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VkClearColorValue color{{.1f,.2f,.3f,1.f}};vkCmdClearColorImage(cmd,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&barrier.subresourceRange);
        barrier.oldLayout=barrier.newLayout;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=0;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        REQUIRE(vkEndCommandBuffer(cmd)==VK_SUCCESS);
        VkPipelineStageFlags wait=VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&ready;submit.pWaitDstStageMask=&wait;
        submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&done;
        REQUIRE(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)==VK_SUCCESS);
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};present.waitSemaphoreCount=1;present.pWaitSemaphores=&done;
        present.swapchainCount=1;present.pSwapchains=&swap;present.pImageIndices=&index;
        REQUIRE(vkQueuePresentKHR(queue,&present)==VK_SUCCESS);REQUIRE(vkQueueWaitIdle(queue)==VK_SUCCESS);
    }
    vkDestroySemaphore(device,ready,nullptr);vkDestroySemaphore(device,done,nullptr);vkDestroyCommandPool(device,pool,nullptr);
    vkDestroySwapchainKHR(device,swap,nullptr);vkDestroyDevice(device,nullptr);vkDestroySurfaceKHR(instance,surface,nullptr);
    DestroyWindow(window);UnregisterClassW(wc.lpszClassName,wc.hInstance);
    vkDestroyDebugUtilsMessengerEXT(instance,messenger,nullptr);vkDestroyInstance(instance,nullptr);
    fprintf(stderr,"[bootstrap-wsi] frames=3 validationErrors=%u\n",errors);return errors?3:0;
}
