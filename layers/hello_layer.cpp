#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <mutex>
#include <unordered_map>
#include <cstdio>
#include <array>
#include <cstdint>
#include <dlfcn.h>

// FidelityFX Optical Flow headers (SDK submodule)
#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_interface.h>

struct OpticalFlowIntegration {
    FfxOpticalflowContext context{};
    bool contextCreated = false;
    bool pendingReset = true;
    VkExtent2D extent{0,0};
    std::vector<uint8_t> scratch; // backend scratch memory
    FfxInterface backendInterface{};
    FfxDevice ffxDevice{};
    // Shared resources for optical flow outputs (vector + SCD) created via backend interface
    bool resourcesReady = false;
    uint32_t sharedEffectContextId = 0; // backend context id for shared resources
    FfxResource opticalFlowVectorRes{}; // wrapped resource handle supplied to dispatch
    FfxResource opticalFlowSCDRes{};
    FfxResourceInternal opticalFlowVectorInternal{}; // backend internal ids (for map/unmap)
    FfxResourceInternal opticalFlowSCDInternal{};
};
static OpticalFlowIntegration g_of; // single-device assumption for this minimal layer

// Simple implicit layer that overlays "Hello world" in the top-left (or right by adjusting coordinates)
// of any swapchain image by drawing a tiny CPU-side RGBA8 bitmap copied via vkCmdCopyBufferToImage
// after the app's render pass ends (hooking Present). Kept intentionally very small.

// Environment toggle: test_vk=1 enables overlay
static bool g_enabled = [](){ const char* v = std::getenv("test_vk"); return v && std::strcmp(v, "0") != 0; }();
static bool g_of_enabled = [](){ const char* v = std::getenv("TEST_VK_OF"); return v && std::strcmp(v, "0") != 0; }();
// Log if either overlay OR optical flow is enabled so OF-only runs still produce diagnostics.
static void log_debug(const char* msg){ if(g_enabled || g_of_enabled) std::fprintf(stderr, "[test_vk] %s\n", msg); }

// Next layer function pointers (global simple approach)
static PFN_vkGetInstanceProcAddr g_nextGetInstanceProcAddr = nullptr;
static PFN_vkGetDeviceProcAddr   g_nextGetDeviceProcAddr   = nullptr;


// Function pointer dispatch tables
struct InstanceDispatchTable {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkCreateDevice CreateDevice;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties; // newly tracked
};

struct DeviceDispatchTable {
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkQueuePresentKHR QueuePresentKHR;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers FreeCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkCreateFence CreateFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
    PFN_vkCreateImage CreateImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    VkPhysicalDevice physicalDevice{};
    VkInstance instance{}; // owning instance
};

static std::mutex g_mutex;
static std::unordered_map<VkInstance, InstanceDispatchTable> g_instance_dispatch;
static std::unordered_map<VkDevice, DeviceDispatchTable> g_device_dispatch;
static std::unordered_map<VkQueue, VkDevice> g_queue_to_device;

struct SwapchainInfo {
    VkDevice device{};
    uint32_t width{};
    uint32_t height{};
    std::vector<VkImage> images; // filled after GetSwapchainImagesKHR
};
static std::unordered_map<VkSwapchainKHR, SwapchainInfo> g_swapchains;

static std::unordered_map<VkQueue, uint32_t> g_queue_family_index; // needed for command pool

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties* pProperties){
    PFN_vkGetPhysicalDeviceProperties realFn = nullptr;
    VkInstance owningInst = VK_NULL_HANDLE;
    for(auto &kv : g_device_dispatch){ if(kv.second.physicalDevice == physicalDevice){ owningInst = kv.second.instance; break; } }
    if(owningInst && g_nextGetInstanceProcAddr){
        realFn = (PFN_vkGetPhysicalDeviceProperties) g_nextGetInstanceProcAddr(owningInst, "vkGetPhysicalDeviceProperties");
    }
    if(!realFn && g_nextGetInstanceProcAddr){
        realFn = (PFN_vkGetPhysicalDeviceProperties) g_nextGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    }
    if(!realFn){
        void* lib = dlopen("libvulkan.so.1", RTLD_LAZY|RTLD_NOLOAD);
        if(lib) realFn = (PFN_vkGetPhysicalDeviceProperties) dlsym(lib, "vkGetPhysicalDeviceProperties");
    }
    if(!realFn){ if(pProperties) std::memset(pProperties, 0, sizeof(*pProperties)); std::fprintf(stderr, "[test_vk] getPhysProps override: resolve fail phys=%p\n", (void*)physicalDevice); return; }
    realFn(physicalDevice, pProperties);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties* pProps){
    PFN_vkGetPhysicalDeviceMemoryProperties realFn=nullptr; VkInstance owningInst=VK_NULL_HANDLE;
    for(auto &kv: g_device_dispatch){ if(kv.second.physicalDevice==physicalDevice){ owningInst=kv.second.instance; break; } }
    if(owningInst && g_nextGetInstanceProcAddr) realFn=(PFN_vkGetPhysicalDeviceMemoryProperties) g_nextGetInstanceProcAddr(owningInst, "vkGetPhysicalDeviceMemoryProperties");
    if(!realFn && g_nextGetInstanceProcAddr) realFn=(PFN_vkGetPhysicalDeviceMemoryProperties) g_nextGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceMemoryProperties");
    if(!realFn){ void* lib=dlopen("libvulkan.so.1", RTLD_LAZY|RTLD_NOLOAD); if(lib) realFn=(PFN_vkGetPhysicalDeviceMemoryProperties) dlsym(lib, "vkGetPhysicalDeviceMemoryProperties"); }
    if(!realFn){ if(pProps) std::memset(pProps,0,sizeof(*pProps)); std::fprintf(stderr,"[test_vk] getPhysMemProps override: resolve fail phys=%p\n",(void*)physicalDevice); return; }
    realFn(physicalDevice,pProps);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProps){
    PFN_vkGetPhysicalDeviceProperties2 realFn=nullptr; VkInstance inst=VK_NULL_HANDLE;
    for(auto &kv: g_device_dispatch){ if(kv.second.physicalDevice==physicalDevice){ inst=kv.second.instance; break; } }
    if(inst && g_nextGetInstanceProcAddr) realFn=(PFN_vkGetPhysicalDeviceProperties2) g_nextGetInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties2");
    if(!realFn && g_nextGetInstanceProcAddr) realFn=(PFN_vkGetPhysicalDeviceProperties2) g_nextGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties2");
    if(!realFn){ void* lib=dlopen("libvulkan.so.1", RTLD_LAZY|RTLD_NOLOAD); if(lib) realFn=(PFN_vkGetPhysicalDeviceProperties2) dlsym(lib, "vkGetPhysicalDeviceProperties2"); }
    if(!realFn){ if(pProps) std::memset(pProps,0,sizeof(*pProps)); return; }
    realFn(physicalDevice,pProps);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures){
    PFN_vkGetPhysicalDeviceFeatures realFn=nullptr; VkInstance inst=VK_NULL_HANDLE;
    for(auto &kv: g_device_dispatch){ if(kv.second.physicalDevice==physicalDevice){ inst=kv.second.instance; break; } }
    if(inst && g_nextGetInstanceProcAddr) realFn=(PFN_vkGetPhysicalDeviceFeatures) g_nextGetInstanceProcAddr(inst, "vkGetPhysicalDeviceFeatures");
    if(!realFn && g_nextGetInstanceProcAddr) realFn=(PFN_vkGetPhysicalDeviceFeatures) g_nextGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures");
    if(!realFn){ void* lib=dlopen("libvulkan.so.1", RTLD_LAZY|RTLD_NOLOAD); if(lib) realFn=(PFN_vkGetPhysicalDeviceFeatures) dlsym(lib, "vkGetPhysicalDeviceFeatures"); }
    if(!realFn){ if(pFeatures) std::memset(pFeatures,0,sizeof(*pFeatures)); return; }
    realFn(physicalDevice,pFeatures);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures){
    PFN_vkGetPhysicalDeviceFeatures2 realFn=nullptr; VkInstance inst=VK_NULL_HANDLE;
    for(auto &kv: g_device_dispatch){ if(kv.second.physicalDevice==physicalDevice){ inst=kv.second.instance; break; } }
    if(inst && g_nextGetInstanceProcAddr) realFn=(PFN_vkGetPhysicalDeviceFeatures2) g_nextGetInstanceProcAddr(inst, "vkGetPhysicalDeviceFeatures2");
    if(!realFn && g_nextGetInstanceProcAddr) realFn=(PFN_vkGetPhysicalDeviceFeatures2) g_nextGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures2");
    if(!realFn){ void* lib=dlopen("libvulkan.so.1", RTLD_LAZY|RTLD_NOLOAD); if(lib) realFn=(PFN_vkGetPhysicalDeviceFeatures2) dlsym(lib, "vkGetPhysicalDeviceFeatures2"); }
    if(!realFn){ if(pFeatures) std::memset(pFeatures,0,sizeof(*pFeatures)); return; }
    realFn(physicalDevice,pFeatures);
}

// Lazy Optical Flow backend initialization helper. We defer expensive / potentially unsafe
// backend interface acquisition (which previously happened in vkCreateDevice and appeared to
// trigger a crash inside ffxGetScratchMemorySizeVK) until after we know the application has
// created a swapchain or is presenting – implying the device is fully usable and required
// extensions/features are active.
static void initOpticalFlowIfNeeded(DeviceDispatchTable* ddt, InstanceDispatchTable* idt) {
    if(!g_of_enabled) return;
    if(!ddt || !idt) return;
    if(g_of.backendInterface.fpGetSDKVersion) return; // already initialized

    std::fprintf(stderr, "[test_vk] OF lazy init: starting (dispatch=%p phys=%p inst=%p)\n", (void*)ddt, (void*)ddt->physicalDevice, (void*)ddt->instance);

    // Validate physical device by enumerating (best effort; failure is non-fatal)
    bool physValid = false;
    if(idt->EnumeratePhysicalDevices){
        uint32_t count=0; if(idt->EnumeratePhysicalDevices((VkInstance)g_instance_dispatch.begin()->first, &count, nullptr)==VK_SUCCESS && count>0){
            std::vector<VkPhysicalDevice> phys(count);
            if(idt->EnumeratePhysicalDevices((VkInstance)g_instance_dispatch.begin()->first, &count, phys.data())==VK_SUCCESS){
                for(auto p: phys) if(p==ddt->physicalDevice) { physValid=true; break; }
            }
        }
    }
    if(!physValid){
        std::fprintf(stderr, "[test_vk] OF lazy init: physical device validation skipped/failed (continuing)\n");
    }

    // Light validation: ensure enumerate function resolves; non-fatal if missing.
    if(ddt->instance){
        auto instIt = g_instance_dispatch.find(ddt->instance);
        if(instIt != g_instance_dispatch.end() && instIt->second.EnumerateDeviceExtensionProperties){
            uint32_t extCount=0; instIt->second.EnumerateDeviceExtensionProperties(ddt->physicalDevice, nullptr, &extCount, nullptr);
        } else if(g_nextGetInstanceProcAddr){
            auto fpEnumDevExt = (PFN_vkEnumerateDeviceExtensionProperties) g_nextGetInstanceProcAddr(ddt->instance, "vkEnumerateDeviceExtensionProperties");
            if(fpEnumDevExt){ uint32_t extCount=0; fpEnumDevExt(ddt->physicalDevice, nullptr, &extCount, nullptr); }
        }
    }
    // Build the device context from the dispatch table and associated VkDevice (found by reverse lookup).
    VkDevice foundDevice = VK_NULL_HANDLE; for(auto &kv : g_device_dispatch){ if(&kv.second == ddt){ foundDevice = kv.first; break; } }
    if(foundDevice == VK_NULL_HANDLE){ std::fprintf(stderr, "[test_vk] OF lazy init: no device handle found\n"); g_of_enabled=false; return; }
    VkDeviceContext devCtx{ foundDevice, ddt->physicalDevice, ddt->GetDeviceProcAddr };
    std::fprintf(stderr, "[test_vk] OF lazy init: creating ffxDevice (VkDevice=%p)\n", (void*)devCtx.vkDevice);
    g_of.ffxDevice = ffxGetDeviceVK(&devCtx);
    std::fprintf(stderr, "[test_vk] OF lazy init: ffxDevice=%p\n", (void*)g_of.ffxDevice);

    // Query required scratch size via backend helper now that enumeration override should prevent crash.
    size_t scratchSize = ffxGetScratchMemorySizeVK(ddt->physicalDevice, FFX_OPTICALFLOW_CONTEXT_COUNT);
    std::fprintf(stderr, "[test_vk] OF scratch size (backend) = %zu\n", scratchSize);
    g_of.scratch.assign(scratchSize, 0);
    FfxErrorCode ifaceResult = ffxGetInterfaceVK(&g_of.backendInterface, g_of.ffxDevice, g_of.scratch.data(), g_of.scratch.size(), FFX_OPTICALFLOW_CONTEXT_COUNT);
    if(ifaceResult != FFX_OK){
        std::fprintf(stderr, "[test_vk] OF interface create failed err=%d – disabling OF\n", ifaceResult);
        g_of_enabled=false;
        return;
    }
    // Ensure frame generation path is disabled (not implemented in this minimal build).
    g_of.backendInterface.fpSwapChainConfigureFrameGeneration = nullptr;
    log_debug("OF lazy init: backend interface OK (adaptive scratch)");
}

// Create optical flow context when extent known & backend interface ready.
static void createOpticalFlowContextIfNeeded() {
    if(!g_of_enabled) return;
    if(g_of.contextCreated) return;
    if(!g_of.backendInterface.fpGetSDKVersion) return; // backend not ready
    if(g_of.extent.width == 0 || g_of.extent.height == 0) return; // need dimensions
    FfxOpticalflowContextDescription desc{};
    desc.backendInterface = g_of.backendInterface; // copy value
    desc.flags = 0;
    desc.resolution.width = g_of.extent.width;
    desc.resolution.height = g_of.extent.height;
    FfxErrorCode ec = ffxOpticalflowContextCreate(&g_of.context, &desc);
    if(ec != FFX_OK){
        std::fprintf(stderr, "[test_vk] OF: context create failed ec=%d (disabling)\n", ec);
        g_of_enabled=false; return;
    }
    g_of.contextCreated = true;
    std::fprintf(stderr, "[test_vk] OF: context created %ux%u\n", g_of.extent.width, g_of.extent.height);
}

static void createOpticalFlowSharedResourcesIfNeeded(){
    if(!g_of_enabled) return;
    if(!g_of.contextCreated) return;
    if(g_of.resourcesReady) return;
    if(!g_of.backendInterface.fpCreateBackendContext || !g_of.backendInterface.fpCreateResource || !g_of.backendInterface.fpGetResource){
        std::fprintf(stderr, "[test_vk] OF: backend missing resource callbacks\n"); return; }
    if(g_of.sharedEffectContextId==0){
        FfxErrorCode ec = g_of.backendInterface.fpCreateBackendContext(&g_of.backendInterface, FFX_EFFECT_SHAREDRESOURCES, nullptr, &g_of.sharedEffectContextId);
        if(ec != FFX_OK){ std::fprintf(stderr, "[test_vk] OF: create shared backend ctx failed ec=%d\n", ec); return; }
    }
    FfxOpticalflowSharedResourceDescriptions shared{};
    if(ffxOpticalflowGetSharedResourceDescriptions(&g_of.context, &shared)!=FFX_OK){ std::fprintf(stderr, "[test_vk] OF: get shared desc failed\n"); return; }
    FfxErrorCode e1 = g_of.backendInterface.fpCreateResource(&g_of.backendInterface, &shared.opticalFlowVector, g_of.sharedEffectContextId, &g_of.opticalFlowVectorInternal);
    FfxErrorCode e2 = g_of.backendInterface.fpCreateResource(&g_of.backendInterface, &shared.opticalFlowSCD, g_of.sharedEffectContextId, &g_of.opticalFlowSCDInternal);
    if(e1!=FFX_OK || e2!=FFX_OK){ std::fprintf(stderr, "[test_vk] OF: create shared resources failed (%d,%d)\n", e1, e2); return; }
    g_of.opticalFlowVectorRes = g_of.backendInterface.fpGetResource(&g_of.backendInterface, g_of.opticalFlowVectorInternal);
    g_of.opticalFlowSCDRes = g_of.backendInterface.fpGetResource(&g_of.backendInterface, g_of.opticalFlowSCDInternal);
    g_of.resourcesReady = g_of.opticalFlowVectorRes.resource && g_of.opticalFlowSCDRes.resource;
    std::fprintf(stderr, "[test_vk] OF: shared resources ready=%d vecIdx=%d scdIdx=%d\n", (int)g_of.resourcesReady, g_of.opticalFlowVectorInternal.internalIndex, g_of.opticalFlowSCDInternal.internalIndex);
}

static void dispatchOpticalFlowIfPossible(VkCommandBuffer cmd, VkImage swapImage){
    if(!g_of_enabled) return;
    if(!g_of.contextCreated || !g_of.resourcesReady) return;
    FfxResourceDescription colorDesc{}; // minimal description for swapchain image
    colorDesc.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    colorDesc.format = ffxGetSurfaceFormatVK(VK_FORMAT_B8G8R8A8_UNORM); // assumption; TODO detect actual
    colorDesc.width = g_of.extent.width; colorDesc.height = g_of.extent.height; colorDesc.depth=1; colorDesc.mipCount=1; colorDesc.flags=FFX_RESOURCE_FLAGS_NONE; colorDesc.usage=FFX_RESOURCE_USAGE_READ_ONLY;
    FfxResource colorRes = ffxGetResourceVK(reinterpret_cast<void*>(swapImage), colorDesc, L"SWAPCHAIN_COLOR", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    FfxOpticalflowDispatchDescription dis{}; dis.commandList = ffxGetCommandListVK(cmd); dis.color = colorRes; dis.opticalFlowVector = g_of.opticalFlowVectorRes; dis.opticalFlowSCD = g_of.opticalFlowSCDRes; dis.reset = g_of.pendingReset; dis.backbufferTransferFunction=0; dis.minMaxLuminance.x=0.f; dis.minMaxLuminance.y=1000.f;
    FfxErrorCode ec = ffxOpticalflowContextDispatch(&g_of.context, &dis);
    if(ec!=FFX_OK){ std::fprintf(stderr, "[test_vk] OF: dispatch failed ec=%d (disabling)\n", ec); g_of_enabled=false; return; }
    g_of.pendingReset=false;
    static uint32_t frameCounter=0; ++frameCounter;
    if((frameCounter & 15u)==0){ // periodic scene change diagnostics
        if(g_of.backendInterface.fpMapResource && g_of.backendInterface.fpUnmapResource){
            void* data=nullptr; if(g_of.backendInterface.fpMapResource(&g_of.backendInterface, g_of.opticalFlowSCDInternal, &data)==FFX_OK && data){
                uint32_t* u32 = (uint32_t*)data; std::fprintf(stderr, "[test_vk] OF: SCD raw first3=[%u %u %u] idx=%d\n", u32[0], u32[1], u32[2], g_of.opticalFlowSCDInternal.internalIndex); g_of.backendInterface.fpUnmapResource(&g_of.backendInterface, g_of.opticalFlowSCDInternal);
            }
        }
    }
}

// Very small monochrome bitmap font for phrase "HELLO WORLD" (fixed phrase) 8 px tall.
// Each character 6 px wide (5 + 1 space). 11 chars => 66px width.
static const uint32_t HELLO_H = 8;
static const uint32_t HELLO_W = 66;
// Bit patterns (LSB left) for 5x8 glyphs of "HELLO WORLD" with a space after each.
static std::array<uint8_t, 11*8> hello_pattern{}; // generated at runtime

static void initHelloPattern() {
    // Glyph definitions for H,E,L,O,W,R,D (5x8). 1 = white.
    auto glyph = [](char c){
        switch(c){
            case 'H': return std::array<uint8_t,8>{0x11,0x11,0x11,0x1F,0x11,0x11,0x11,0x11};
            case 'E': return std::array<uint8_t,8>{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10,0x1F};
            case 'L': return std::array<uint8_t,8>{0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x1F};
            case 'O': return std::array<uint8_t,8>{0x0E,0x11,0x11,0x11,0x11,0x11,0x11,0x0E};
            case 'W': return std::array<uint8_t,8>{0x11,0x11,0x11,0x15,0x15,0x15,0x0A,0x0A};
            case 'R': return std::array<uint8_t,8>{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11,0x11};
            case 'D': return std::array<uint8_t,8>{0x1E,0x11,0x11,0x11,0x11,0x11,0x11,0x1E};
            default: return std::array<uint8_t,8>{0,0,0,0,0,0,0,0};
        }
    };
    const char* text = "HELLO WORLD"; // length 11
    for(int ci=0; ci<11; ++ci){
        char c = text[ci];
        auto g = (c==' ')? std::array<uint8_t,8>{0,0,0,0,0,0,0,0} : glyph(c);
        for(int row=0; row<8; ++row){
            hello_pattern[ci*8 + row] = g[row];
        }
    }
}

static std::vector<uint32_t> makeHelloImage(){
    static bool inited=false; if(!inited){ initHelloPattern(); inited=true; }
    std::vector<uint32_t> img(HELLO_W*HELLO_H, 0x00000000);
    for(uint32_t ci=0; ci<11; ++ci){
        for(uint32_t row=0; row<HELLO_H; ++row){
            uint8_t bits = hello_pattern[ci*8 + row];
            for(uint32_t col=0; col<5; ++col){
                if(bits & (1 << (4-col))){
                    uint32_t x = ci*6 + col; // 1 px spacing
                    img[row*HELLO_W + x] = 0xFFFFFFFF; // white RGBA
                }
            }
        }
    }
    return img;
}

// We hook QueuePresentKHR to inject a copy operation. For simplicity assume the swapchain image is already in GENERAL or we ignore layout (unsafe but minimal). Real code would query and transition layouts.

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
    // Walk pNext chain to get link info
    const VkLayerInstanceCreateInfo* chainInfo = reinterpret_cast<const VkLayerInstanceCreateInfo*>(pCreateInfo->pNext);
    while(chainInfo && chainInfo->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
        if(chainInfo->function == VK_LAYER_LINK_INFO) {
            g_nextGetInstanceProcAddr = chainInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
            // Advance the link so next layers see their proper link info
            const_cast<VkLayerInstanceCreateInfo*>(chainInfo)->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;
            break;
        }
        chainInfo = reinterpret_cast<const VkLayerInstanceCreateInfo*>(chainInfo->pNext);
    }
    if(!g_nextGetInstanceProcAddr) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateInstance fpCreate = (PFN_vkCreateInstance) g_nextGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if(!fpCreate) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = fpCreate(pCreateInfo, pAllocator, pInstance);
    if(r != VK_SUCCESS) return r;
    log_debug("vkCreateInstance chain ok");

    InstanceDispatchTable table{};
    table.GetInstanceProcAddr = g_nextGetInstanceProcAddr;
    table.CreateInstance = fpCreate;
    table.DestroyInstance = (PFN_vkDestroyInstance) g_nextGetInstanceProcAddr(*pInstance, "vkDestroyInstance");
    table.CreateDevice = (PFN_vkCreateDevice) g_nextGetInstanceProcAddr(*pInstance, "vkCreateDevice");
    table.EnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices) g_nextGetInstanceProcAddr(*pInstance, "vkEnumeratePhysicalDevices");
    table.GetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties) g_nextGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceProperties");
    table.GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties) g_nextGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceMemoryProperties");
    table.GetDeviceProcAddr = nullptr; // resolved per-device
    table.EnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties) g_nextGetInstanceProcAddr(*pInstance, "vkEnumerateDeviceExtensionProperties");

    std::lock_guard<std::mutex> lock(g_mutex);
    g_instance_dispatch[*pInstance] = table;
    return r;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_instance_dispatch.find(instance);
    if (it != g_instance_dispatch.end()) {
        auto fp = it->second.DestroyInstance;
        g_instance_dispatch.erase(it);
        fp(instance, pAllocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    // Extract link info from pNext
    const VkLayerDeviceCreateInfo* chainInfo = reinterpret_cast<const VkLayerDeviceCreateInfo*>(pCreateInfo->pNext);
    while(chainInfo && chainInfo->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
        if(chainInfo->function == VK_LAYER_LINK_INFO){
            g_nextGetDeviceProcAddr = chainInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
            const_cast<VkLayerDeviceCreateInfo*>(chainInfo)->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;
            break;
        }
        chainInfo = reinterpret_cast<const VkLayerDeviceCreateInfo*>(chainInfo->pNext);
    }
    if(!g_nextGetDeviceProcAddr || !g_nextGetInstanceProcAddr) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateDevice fp = (PFN_vkCreateDevice) g_nextGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice");
    if(!fp) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = fp(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (r != VK_SUCCESS) return r;

    DeviceDispatchTable d{};
    d.GetDeviceProcAddr = g_nextGetDeviceProcAddr;
    d.DestroyDevice = (PFN_vkDestroyDevice) d.GetDeviceProcAddr(*pDevice, "vkDestroyDevice");
    d.GetDeviceQueue = (PFN_vkGetDeviceQueue) d.GetDeviceProcAddr(*pDevice, "vkGetDeviceQueue");
    d.QueuePresentKHR = (PFN_vkQueuePresentKHR) d.GetDeviceProcAddr(*pDevice, "vkQueuePresentKHR");
    d.QueueSubmit = (PFN_vkQueueSubmit) d.GetDeviceProcAddr(*pDevice, "vkQueueSubmit");
    d.AllocateCommandBuffers = (PFN_vkAllocateCommandBuffers) d.GetDeviceProcAddr(*pDevice, "vkAllocateCommandBuffers");
    d.FreeCommandBuffers = (PFN_vkFreeCommandBuffers) d.GetDeviceProcAddr(*pDevice, "vkFreeCommandBuffers");
    d.BeginCommandBuffer = (PFN_vkBeginCommandBuffer) d.GetDeviceProcAddr(*pDevice, "vkBeginCommandBuffer");
    d.EndCommandBuffer = (PFN_vkEndCommandBuffer) d.GetDeviceProcAddr(*pDevice, "vkEndCommandBuffer");
    d.ResetCommandBuffer = (PFN_vkResetCommandBuffer) d.GetDeviceProcAddr(*pDevice, "vkResetCommandBuffer");
    d.CreateFence = (PFN_vkCreateFence) d.GetDeviceProcAddr(*pDevice, "vkCreateFence");
    d.WaitForFences = (PFN_vkWaitForFences) d.GetDeviceProcAddr(*pDevice, "vkWaitForFences");
    d.DestroyFence = (PFN_vkDestroyFence) d.GetDeviceProcAddr(*pDevice, "vkDestroyFence");
    d.CreateBuffer = (PFN_vkCreateBuffer) d.GetDeviceProcAddr(*pDevice, "vkCreateBuffer");
    d.GetBufferMemoryRequirements = (PFN_vkGetBufferMemoryRequirements) d.GetDeviceProcAddr(*pDevice, "vkGetBufferMemoryRequirements");
    d.AllocateMemory = (PFN_vkAllocateMemory) d.GetDeviceProcAddr(*pDevice, "vkAllocateMemory");
    d.BindBufferMemory = (PFN_vkBindBufferMemory) d.GetDeviceProcAddr(*pDevice, "vkBindBufferMemory");
    d.CreateCommandPool = (PFN_vkCreateCommandPool) d.GetDeviceProcAddr(*pDevice, "vkCreateCommandPool");
    d.DestroyCommandPool = (PFN_vkDestroyCommandPool) d.GetDeviceProcAddr(*pDevice, "vkDestroyCommandPool");
    d.CmdCopyBufferToImage = (PFN_vkCmdCopyBufferToImage) d.GetDeviceProcAddr(*pDevice, "vkCmdCopyBufferToImage");
    d.CreateImage = (PFN_vkCreateImage) d.GetDeviceProcAddr(*pDevice, "vkCreateImage");
    d.GetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements) d.GetDeviceProcAddr(*pDevice, "vkGetImageMemoryRequirements");
    d.BindImageMemory = (PFN_vkBindImageMemory) d.GetDeviceProcAddr(*pDevice, "vkBindImageMemory");
    d.MapMemory = (PFN_vkMapMemory) d.GetDeviceProcAddr(*pDevice, "vkMapMemory");
    d.UnmapMemory = (PFN_vkUnmapMemory) d.GetDeviceProcAddr(*pDevice, "vkUnmapMemory");
    d.DestroyImage = (PFN_vkDestroyImage) d.GetDeviceProcAddr(*pDevice, "vkDestroyImage");
    d.DestroyBuffer = (PFN_vkDestroyBuffer) d.GetDeviceProcAddr(*pDevice, "vkDestroyBuffer");
    d.CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR) d.GetDeviceProcAddr(*pDevice, "vkCreateSwapchainKHR");
    d.DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR) d.GetDeviceProcAddr(*pDevice, "vkDestroySwapchainKHR");
    d.GetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR) d.GetDeviceProcAddr(*pDevice, "vkGetSwapchainImagesKHR");
    d.physicalDevice = physicalDevice;
    // Determine owning instance by scanning known instances
    for(auto &instPair : g_instance_dispatch){
        auto &instTable = instPair.second;
        if(!instTable.EnumeratePhysicalDevices) continue;
        uint32_t count=0;
        if(instTable.EnumeratePhysicalDevices(instPair.first, &count, nullptr)!=VK_SUCCESS || count==0) continue;
        std::vector<VkPhysicalDevice> phys(count);
        if(instTable.EnumeratePhysicalDevices(instPair.first, &count, phys.data())!=VK_SUCCESS) continue;
        for(auto p: phys){ if(p==physicalDevice){ d.instance = instPair.first; break; } }
        if(d.instance) break;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_device_dispatch[*pDevice] = d;
    log_debug("vkCreateDevice intercepted");
    if(g_enabled || g_of_enabled){
        std::fprintf(stderr, "[test_vk] flags: overlay=%d opticalflow=%d\n", (int)g_enabled, (int)g_of_enabled);
    }
    if(g_of_enabled){
    std::fprintf(stderr, "[test_vk] OF init: deferring backend initialization to lazy path\n");
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    if(g_of.contextCreated){
        ffxOpticalflowContextDestroy(&g_of.context);
        g_of.contextCreated=false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_device_dispatch.find(device);
    if (it != g_device_dispatch.end()) {
        auto fp = it->second.DestroyDevice;
        g_device_dispatch.erase(it);
        fp(device, pAllocator);
    }
}

// Utility: find memory type index
static uint32_t findMemoryType(const InstanceDispatchTable& inst, VkPhysicalDevice phys, uint32_t typeBits, VkMemoryPropertyFlags wanted){
    VkPhysicalDeviceMemoryProperties mem{};
    inst.GetPhysicalDeviceMemoryProperties(phys, &mem);
    for(uint32_t i=0;i<mem.memoryTypeCount;++i){
        if((typeBits & (1u<<i)) && (mem.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    }
    return 0; // fallback
}

// Present hook adds copy of phrase into each swapchain image about to be presented.
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    // Regular pass-through if disabled
    VkDevice device = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto qit = g_queue_to_device.find(queue);
        if(qit != g_queue_to_device.end()) device = qit->second; else if(!g_device_dispatch.empty()) device = g_device_dispatch.begin()->first;
    }
    if(device==VK_NULL_HANDLE){
        // Fallback just call first dispatch if any
        for (auto &kv : g_device_dispatch) return kv.second.QueuePresentKHR(queue, pPresentInfo);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    DeviceDispatchTable* ddt=nullptr; InstanceDispatchTable* idt=nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ddt = &g_device_dispatch[device];
        if(!g_instance_dispatch.empty()) idt = &g_instance_dispatch.begin()->second;
    }
    if(!ddt || !idt || !ddt->QueuePresentKHR) return VK_ERROR_INITIALIZATION_FAILED;

    static bool s_loggedStateOnce = false;
    if(!s_loggedStateOnce){
        s_loggedStateOnce = true;
        std::fprintf(stderr, "[test_vk] Present state: overlay=%d of_enabled=%d of_ctx=%d backend=%p extent=%ux%u\n",
            (int)g_enabled, (int)g_of_enabled, (int)g_of.contextCreated, (void*)g_of.backendInterface.fpGetSDKVersion,
            g_of.extent.width, g_of.extent.height);
    }
    // Attempt lazy backend initialization before any context creation attempt.
    if(g_of_enabled && !g_of.backendInterface.fpGetSDKVersion){
        initOpticalFlowIfNeeded(ddt, idt);
    }
    if(g_of_enabled){
        // After swapchain extent known (captured earlier) we can attempt context creation
        createOpticalFlowContextIfNeeded();
        createOpticalFlowSharedResourcesIfNeeded();
    }
    if(!g_enabled && !g_of_enabled) return ddt->QueuePresentKHR(queue, pPresentInfo);
    if(g_enabled) log_debug("vkQueuePresentKHR overlay path");

    // Build list of lit pixel positions for phrase (scaled) so we don't overwrite background.
    static const char* text = "HELLO WORLD"; // 11 chars incl space
    const uint32_t CHAR_W = 6; // 5 bits + 1 spacing
    const uint32_t GLYPH_W_BITS = 5;
    const uint32_t GLYPH_H = HELLO_H; // 8
    const uint32_t SCALE = 4; // enlarge for visibility
    const int32_t MARGIN_X = 8;
    const int32_t MARGIN_Y = 8;
    // Recreate hello_pattern if needed (makeHelloImage ensures it)
    makeHelloImage(); // ensures pattern init side-effect
    struct Px { uint16_t x,y; };
    std::vector<Px> lit;
    lit.reserve(500);
    for(uint32_t ci=0; ci<11; ++ci){
        char c = text[ci];
        if(c==' ' ) continue;
        for(uint32_t row=0; row<GLYPH_H; ++row){
            uint8_t bits = hello_pattern[ci*8 + row];
            for(uint32_t col=0; col<GLYPH_W_BITS; ++col){
                if(bits & (1 << (4-col))){
                    for(uint32_t sy=0; sy<SCALE; ++sy){
                        for(uint32_t sx=0; sx<SCALE; ++sx){
                            Px p{ (uint16_t)(ci*CHAR_W*SCALE + col*SCALE + sx), (uint16_t)(row*SCALE + sy) };
                            lit.push_back(p);
                        }
                    }
                }
            }
        }
    }
    if(lit.empty()) return ddt->QueuePresentKHR(queue, pPresentInfo);
    VkDeviceSize bufSize = lit.size()*sizeof(uint32_t);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bufSize; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer stagingBuf{}; if(ddt->CreateBuffer(device, &bci, nullptr, &stagingBuf) != VK_SUCCESS) return ddt->QueuePresentKHR(queue, pPresentInfo);
    VkMemoryRequirements memReq{}; ddt->GetBufferMemoryRequirements(device, stagingBuf, &memReq);
    uint32_t typeIndex = findMemoryType(*idt, ddt->physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = memReq.size; mai.memoryTypeIndex = typeIndex;
    VkDeviceMemory stagingMem{}; if(ddt->AllocateMemory(device, &mai, nullptr, &stagingMem)!=VK_SUCCESS){ ddt->DestroyBuffer(device, stagingBuf, nullptr); return ddt->QueuePresentKHR(queue, pPresentInfo);}    
    ddt->BindBufferMemory(device, stagingBuf, stagingMem, 0);
    void* mapped=nullptr; if(ddt->MapMemory(device, stagingMem, 0, bufSize, 0, &mapped)==VK_SUCCESS){
        uint32_t* dst = reinterpret_cast<uint32_t*>(mapped);
        for(size_t i=0;i<lit.size();++i) dst[i] = 0xFFFFFFFF; // solid white pixels
        ddt->UnmapMemory(device, stagingMem);
    }

    // Command pool per queue family (allocate transient each frame for minimalism)
    uint32_t qFam = 0; {
        std::lock_guard<std::mutex> lock(g_mutex); auto qfit = g_queue_family_index.find(queue); if(qfit!=g_queue_family_index.end()) qFam = qfit->second; }
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpci.queueFamilyIndex = qFam; cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VkCommandPool cpool{}; if(ddt->CreateCommandPool(device, &cpci, nullptr, &cpool)!=VK_SUCCESS){ ddt->DestroyBuffer(device, stagingBuf, nullptr); ddt->FreeCommandBuffers(device, cpool, 0, nullptr); return ddt->QueuePresentKHR(queue, pPresentInfo);}    
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cbai.commandPool = cpool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cmd{}; if(ddt->AllocateCommandBuffers(device, &cbai, &cmd)!=VK_SUCCESS){ ddt->DestroyCommandPool(device, cpool, nullptr); ddt->DestroyBuffer(device, stagingBuf, nullptr); return ddt->QueuePresentKHR(queue, pPresentInfo);}    
    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; ddt->BeginCommandBuffer(cmd, &cbbi);
    // Dispatch optical flow once per present (first swapchain image) before overlay so overlay can later reflect stats.
    if(g_of_enabled && g_of.contextCreated && g_of.resourcesReady && pPresentInfo->swapchainCount>0){
        VkSwapchainKHR sw = pPresentInfo->pSwapchains[0];
        uint32_t imgIndex = pPresentInfo->pImageIndices[0];
        SwapchainInfo sci{}; bool have=false; { std::lock_guard<std::mutex> lock(g_mutex); auto sit=g_swapchains.find(sw); if(sit!=g_swapchains.end()){ sci = sit->second; have=true; } }
        if(have && imgIndex < sci.images.size()){
            dispatchOpticalFlowIfPossible(cmd, sci.images[imgIndex]);
        }
    }

    // For each swapchain/image in present
    for(uint32_t i=0;i<pPresentInfo->swapchainCount;++i){
        VkSwapchainKHR sw = pPresentInfo->pSwapchains[i];
        uint32_t imgIndex = pPresentInfo->pImageIndices[i];
        SwapchainInfo sci{}; bool have=false; {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto sit = g_swapchains.find(sw); if(sit!=g_swapchains.end()){ sci = sit->second; have=true; }
        }
        if(!have || imgIndex >= sci.images.size()) continue;
        VkImage img = sci.images[imgIndex];
        // For each lit pixel create copy region
        std::vector<VkBufferImageCopy> copies; copies.reserve(lit.size());
        for(size_t pi=0; pi<lit.size(); ++pi){
            VkBufferImageCopy c{};
            c.bufferOffset = pi*sizeof(uint32_t);
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c.imageSubresource.mipLevel = 0;
            c.imageSubresource.baseArrayLayer = 0;
            c.imageSubresource.layerCount = 1;
            c.imageOffset = { (int32_t)(MARGIN_X + lit[pi].x), (int32_t)(MARGIN_Y + lit[pi].y), 0};
            c.imageExtent = {1,1,1};
            copies.push_back(c);
        }
        if(!copies.empty()) ddt->CmdCopyBufferToImage(cmd, stagingBuf, img, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, (uint32_t)copies.size(), copies.data());
    }
    ddt->EndCommandBuffer(cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; ddt->CreateFence(device, &fci, nullptr, &fence);
    ddt->QueueSubmit(queue, 1, &si, fence);
    ddt->WaitForFences(device, 1, &fence, VK_TRUE, 1'000'000'000ULL);
    ddt->DestroyFence(device, fence, nullptr);
    ddt->FreeCommandBuffers(device, cpool, 1, &cmd);
    ddt->DestroyCommandPool(device, cpool, nullptr);
    ddt->DestroyBuffer(device, stagingBuf, nullptr);
    ddt->FreeCommandBuffers(device, cpool, 0, nullptr); // no-op
    // Not freeing memory? Free: (lack of DestroyMemory – we only allocated via AllocateMemory)
    // Vulkan has vkFreeMemory; obtain pointer
    auto pfnFreeMemory = (PFN_vkFreeMemory) ddt->GetDeviceProcAddr(device, "vkFreeMemory");
    if(pfnFreeMemory) pfnFreeMemory(device, stagingMem, nullptr);

    return ddt->QueuePresentKHR(queue, pPresentInfo);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue){
    DeviceDispatchTable* ddt=nullptr; {
        std::lock_guard<std::mutex> lock(g_mutex); auto it = g_device_dispatch.find(device); if(it!=g_device_dispatch.end()) ddt = &it->second; }
    if(ddt && ddt->GetDeviceQueue){ ddt->GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue); }
    if(pQueue && *pQueue){ std::lock_guard<std::mutex> lock(g_mutex); g_queue_to_device[*pQueue]=device; g_queue_family_index[*pQueue]=queueFamilyIndex; }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain){
    DeviceDispatchTable* ddt=nullptr; { std::lock_guard<std::mutex> lock(g_mutex); auto it=g_device_dispatch.find(device); if(it!=g_device_dispatch.end()) ddt=&it->second; }
    if(!ddt||!ddt->CreateSwapchainKHR) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = ddt->CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    if(r==VK_SUCCESS){ std::lock_guard<std::mutex> lock(g_mutex); g_swapchains[*pSwapchain] = {device, pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height, {}}; }
    if(r==VK_SUCCESS && g_of_enabled){
        if(g_of.extent.width != pCreateInfo->imageExtent.width || g_of.extent.height != pCreateInfo->imageExtent.height){ g_of.pendingReset = true; }
        g_of.extent = {pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height};
        log_debug("Optical Flow: swapchain extent captured");
        // Try context create here as well (some apps may present late)
        if(!g_of.contextCreated && g_of.backendInterface.fpGetSDKVersion){
            log_debug("Optical Flow: context creation suppressed at swapchain (stability phase)");
        }
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator){
    DeviceDispatchTable* ddt=nullptr; { std::lock_guard<std::mutex> lock(g_mutex); auto it=g_device_dispatch.find(device); if(it!=g_device_dispatch.end()) ddt=&it->second; g_swapchains.erase(swapchain);}    
    if(ddt && ddt->DestroySwapchainKHR) ddt->DestroySwapchainKHR(device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pCount, VkImage* pImages){
    DeviceDispatchTable* ddt=nullptr; { std::lock_guard<std::mutex> lock(g_mutex); auto it=g_device_dispatch.find(device); if(it!=g_device_dispatch.end()) ddt=&it->second; }
    if(!ddt||!ddt->GetSwapchainImagesKHR) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = ddt->GetSwapchainImagesKHR(device, swapchain, pCount, pImages);
    if(r==VK_SUCCESS && pImages){ std::lock_guard<std::mutex> lock(g_mutex); auto &info = g_swapchains[swapchain]; info.images.assign(pImages, pImages + *pCount); }
    return r;
}

// Exported layer negotiation functions
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0) return (PFN_vkVoidFunction) vkQueuePresentKHR;
    if (std::strcmp(pName, "vkGetDeviceQueue") == 0) return (PFN_vkVoidFunction) vkGetDeviceQueue;
    if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0) return (PFN_vkVoidFunction) vkCreateSwapchainKHR;
    if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0) return (PFN_vkVoidFunction) vkDestroySwapchainKHR;
    if (std::strcmp(pName, "vkGetSwapchainImagesKHR") == 0) return (PFN_vkVoidFunction) vkGetSwapchainImagesKHR;
    auto it = g_device_dispatch.find(device);
    if (it != g_device_dispatch.end() && it->second.GetDeviceProcAddr)
        return it->second.GetDeviceProcAddr(device, pName);
    if(g_nextGetDeviceProcAddr) return g_nextGetDeviceProcAddr(device, pName);
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0) return (PFN_vkVoidFunction) vkGetInstanceProcAddr;
    if (std::strcmp(pName, "vkCreateInstance") == 0) return (PFN_vkVoidFunction) vkCreateInstance;
    if (std::strcmp(pName, "vkCreateDevice") == 0) return (PFN_vkVoidFunction) vkCreateDevice;
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0) return (PFN_vkVoidFunction) vkQueuePresentKHR;
    if (std::strcmp(pName, "vkGetDeviceQueue") == 0) return (PFN_vkVoidFunction) vkGetDeviceQueue;
    if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0) return (PFN_vkVoidFunction) vkCreateSwapchainKHR;
    if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0) return (PFN_vkVoidFunction) vkDestroySwapchainKHR;
    if (std::strcmp(pName, "vkGetSwapchainImagesKHR") == 0) return (PFN_vkVoidFunction) vkGetSwapchainImagesKHR;
    if (std::strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0) return (PFN_vkVoidFunction) vkEnumerateDeviceExtensionProperties;
    if(g_nextGetInstanceProcAddr) return g_nextGetInstanceProcAddr(instance, pName);
    return nullptr;
}

// Layer properties
static const VkLayerProperties layerProps = {
    "VK_LAYER_LUNARG_test_vk", // layerName
    VK_MAKE_VERSION(1, 0, 0),   // specVersion
    VK_MAKE_VERSION(0, 1, 0),   // implementationVersion
    "Test VK Hello overlay layer" // description
};

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    if (pProperties == nullptr) {
        *pPropertyCount = 1;
        return VK_SUCCESS;
    }
    if (*pPropertyCount >= 1) {
        pProperties[0] = layerProps;
        *pPropertyCount = 1;
        return VK_SUCCESS;
    }
    return VK_INCOMPLETE;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    return vkEnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

// Forwarder for device extension enumeration. Needed because the integrated FidelityFX backend
// calls vkEnumerateDeviceExtensionProperties directly; without exporting this symbol from the
// layer, the loader's handle validation could reject the physical device pointer (abort observed).
// We resolve the next function via the stored instance dispatch or fall back to the global symbol.
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties){
    // Short-circuit: layer-name queries aren't about a physical device. Just delegate to any instance.
    if(pLayerName != nullptr){
        for(auto &instPair : g_instance_dispatch){
            auto fp = instPair.second.EnumerateDeviceExtensionProperties;
            if(fp) return fp(physicalDevice, pLayerName, pPropertyCount, pProperties);
        }
        // Fallback global
    }

    // Locate owning instance for physicalDevice by enumerating each known instance's physical devices.
    VkInstance owningInst = VK_NULL_HANDLE;
    PFN_vkEnumerateDeviceExtensionProperties nextFn = nullptr;
    for(auto &instPair : g_instance_dispatch){
        auto &idt = instPair.second;
        if(!idt.EnumeratePhysicalDevices) continue;
        uint32_t count = 0;
        if(idt.EnumeratePhysicalDevices(instPair.first, &count, nullptr) != VK_SUCCESS || count == 0) continue;
        std::vector<VkPhysicalDevice> phys(count);
        if(idt.EnumeratePhysicalDevices(instPair.first, &count, phys.data()) != VK_SUCCESS) continue;
        for(auto p : phys){ if(p == physicalDevice){ owningInst = instPair.first; nextFn = idt.EnumerateDeviceExtensionProperties; break; } }
        if(owningInst) break;
    }

    if(!nextFn && g_nextGetInstanceProcAddr && owningInst){
        // Attempt to get the next function directly for the owning instance (should normally be stored already).
        nextFn = (PFN_vkEnumerateDeviceExtensionProperties) g_nextGetInstanceProcAddr(owningInst, "vkEnumerateDeviceExtensionProperties");
    }

    if(!nextFn && g_nextGetInstanceProcAddr){
        // Absolute fallback: ask global (may return loader trampoline; acceptable if it chains properly).
        nextFn = (PFN_vkEnumerateDeviceExtensionProperties) g_nextGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties");
    }

    if(!nextFn){
        void* lib = dlopen("libvulkan.so.1", RTLD_LAZY|RTLD_NOLOAD);
        if(lib) nextFn = (PFN_vkEnumerateDeviceExtensionProperties) dlsym(lib, "vkEnumerateDeviceExtensionProperties");
    }

    if(!nextFn){
        std::fprintf(stderr, "[test_vk] enumerateDeviceExt: resolve fail phys=%p owningInst=%p\n", (void*)physicalDevice, (void*)owningInst);
        if(pPropertyCount){ *pPropertyCount = 0; }
        return VK_ERROR_INITIALIZATION_FAILED; // Added braces to avoid misleading-indentation warning
    }
    return nextFn(physicalDevice, pLayerName, pPropertyCount, pProperties);
}

// Loader negotiation function (outside any other function scope)
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct){
    if(!pVersionStruct) return VK_ERROR_INITIALIZATION_FAILED;
    if(pVersionStruct->loaderLayerInterfaceVersion > 2) pVersionStruct->loaderLayerInterfaceVersion = 2;
    // We set our entry points; loader provides no next pointers here— they come via link info in create chains.
    pVersionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
