// Nitro Vulkan layer (NitroVk.dll).
//
// Registered as an implicit layer, so the Vulkan loader puts it into every
// Vulkan program, but it only does anything for games enabled in Nitro (or
// when NITRO_VULKAN=1 is set by "Launch with injection"). Everyone else gets a
// straight pass-through.
//
// Frame gen runs on Nitro's own D3D11 device: the swap chain image is copied
// into a D3D11 texture imported into Vulkan (VK_KHR_external_memory_win32),
// the generated frames come back the same way, and each one is presented
// with an extra vkAcquireNextImageKHR / vkQueuePresentKHR before the real frame.

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/bridge.h"
#include "../core/log.h"
#include "../core/overlay.h"
#include "../core/runtime.h"

namespace {

using namespace lsfgi;

constexpr uint32_t kMaxGen = 3;      // up to 4x
constexpr uint32_t kSlots = kMaxGen + 1;

void* Key(const void* dispatchable) {
    return *static_cast<void* const*>(dispatchable);
}

// ---------------------------------------------------------------------------
// Is Nitro switched on for this process?

bool g_decided = false;
bool g_enabled = false;
std::mutex g_mutex; // guards all maps below

HMODULE SelfModule() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SelfModule), &m);
    return m;
}

void DecideEnabled() {
    if (g_decided) return;
    g_decided = true;

    wchar_t env[8] = {};
    const DWORD envLen = GetEnvironmentVariableW(L"NITRO_VULKAN", env, 8);
    bool on = envLen > 0 && env[0] == L'1';
    if (envLen > 0 && env[0] == L'0') return;

    if (!on) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring key = exe;
        std::transform(key.begin(), key.end(), key.begin(), ::towlower);
        const std::wstring ini = ModuleDirectory(SelfModule()) + L"\\" + kConfigFileName;
        on = GetPrivateProfileIntW(L"VulkanGames", key.c_str(), 0, ini.c_str()) != 0;
    }
    if (!on) return; // not one of ours: stay silent, no log file

    if (LoadRuntime(SelfModule(), L"NitroVk.log") && ClaimProcess()) {
        g_enabled = true;
        Log("Vulkan layer active");
    }
}

// ---------------------------------------------------------------------------
// Per-instance / per-device / per-swapchain data

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
};

#define NITRO_DEVICE_FUNCS(X)                                                                                  \
    X(DestroyDevice) X(GetDeviceQueue) X(GetDeviceQueue2) X(QueueSubmit) X(QueuePresentKHR)                    \
    X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR)               \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer)                 \
    X(EndCommandBuffer) X(ResetCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyImage) X(CreateFence)             \
    X(DestroyFence) X(WaitForFences) X(ResetFences) X(CreateSemaphore) X(DestroySemaphore) X(CreateImage)      \
    X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory)           \
    X(DeviceWaitIdle) X(GetMemoryWin32HandlePropertiesKHR)

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    InstanceData* inst = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkSetDeviceLoaderData setLoaderData = nullptr;
    bool interop = false;
    bool luidValid = false;
    LUID luid{};
    VkPhysicalDeviceMemoryProperties memProps{};
    std::unordered_map<VkQueue, uint32_t> queueFamily;
#define X(name) PFN_vk##name name = nullptr;
    NITRO_DEVICE_FUNCS(X)
#undef X
};

bool MapFormat(VkFormat vk, DXGI_FORMAT& dxgi, VkFormat& imported) {
    switch (vk) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        dxgi = DXGI_FORMAT_B8G8R8A8_UNORM;
        imported = VK_FORMAT_B8G8R8A8_UNORM;
        return true;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        dxgi = DXGI_FORMAT_R8G8B8A8_UNORM;
        imported = VK_FORMAT_R8G8B8A8_UNORM;
        return true;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        dxgi = DXGI_FORMAT_R10G10B10A2_UNORM;
        imported = vk;
        return true;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT;
        imported = vk;
        return true;
    default: return false;
    }
}

class SwapData {
public:
    DeviceData* dev = nullptr;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    bool failed = false;

    ~SwapData() { Destroy(); }
    VkResult Present(VkQueue queue, const VkPresentInfoKHR* info);

private:
    bool Ensure(VkQueue queue, uint32_t family);
    void Destroy();
    bool Import(HANDLE handle, VkImage& image, VkDeviceMemory& memory);
    void Barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                 VkAccessFlags dst, uint32_t srcFamily, uint32_t dstFamily) const;
    void Copy(VkCommandBuffer cb, VkImage src, VkImageLayout srcLayout, VkImage dst, VkImageLayout dstLayout) const;
    bool Begin(uint32_t slot);
    VkResult Submit(VkQueue queue, uint32_t slot, uint32_t waitCount, const VkSemaphore* waits,
                    VkPipelineStageFlags stage, VkSemaphore signal);

    std::unique_ptr<Bridge> bridge_;
    uint32_t family_ = UINT32_MAX;
    VkImage in_ = VK_NULL_HANDLE;
    VkDeviceMemory inMem_ = VK_NULL_HANDLE;
    VkImage real_ = VK_NULL_HANDLE; // real frame with the menu drawn on it
    VkDeviceMemory realMem_ = VK_NULL_HANDLE;
    std::vector<VkImage> outs_;
    std::vector<VkDeviceMemory> outMem_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_[kSlots] = {};
    VkFence fence_[kSlots] = {};
    bool fenceBusy_[kSlots] = {};
    VkSemaphore acquired_[kMaxGen] = {};
    VkSemaphore copied_[kMaxGen] = {};
};

std::unordered_map<void*, std::unique_ptr<InstanceData>> g_instances;
std::unordered_map<void*, std::unique_ptr<DeviceData>> g_devices;
std::unordered_map<VkSwapchainKHR, std::unique_ptr<SwapData>> g_swapchains;

InstanceData* FindInstance(const void* dispatchable) {
    auto it = g_instances.find(Key(dispatchable));
    return it == g_instances.end() ? nullptr : it->second.get();
}

DeviceData* FindDevice(const void* dispatchable) {
    auto it = g_devices.find(Key(dispatchable));
    return it == g_devices.end() ? nullptr : it->second.get();
}

// ---------------------------------------------------------------------------
// SwapData

void SwapData::Destroy() {
    if (!dev) return;
    VkDevice d = dev->device;
    if (pool_ || in_) dev->DeviceWaitIdle(d);
    for (uint32_t i = 0; i < kSlots; ++i)
        if (fence_[i]) dev->DestroyFence(d, fence_[i], nullptr);
    for (uint32_t i = 0; i < kMaxGen; ++i) {
        if (acquired_[i]) dev->DestroySemaphore(d, acquired_[i], nullptr);
        if (copied_[i]) dev->DestroySemaphore(d, copied_[i], nullptr);
    }
    if (pool_) dev->DestroyCommandPool(d, pool_, nullptr);
    if (in_) dev->DestroyImage(d, in_, nullptr);
    if (inMem_) dev->FreeMemory(d, inMem_, nullptr);
    if (real_) dev->DestroyImage(d, real_, nullptr);
    if (realMem_) dev->FreeMemory(d, realMem_, nullptr);
    for (VkImage i : outs_) dev->DestroyImage(d, i, nullptr);
    for (VkDeviceMemory m : outMem_) dev->FreeMemory(d, m, nullptr);
    std::memset(fence_, 0, sizeof(fence_));
    std::memset(fenceBusy_, 0, sizeof(fenceBusy_));
    std::memset(acquired_, 0, sizeof(acquired_));
    std::memset(copied_, 0, sizeof(copied_));
    std::memset(cmd_, 0, sizeof(cmd_));
    pool_ = VK_NULL_HANDLE;
    in_ = VK_NULL_HANDLE;
    inMem_ = VK_NULL_HANDLE;
    real_ = VK_NULL_HANDLE;
    realMem_ = VK_NULL_HANDLE;
    outs_.clear();
    outMem_.clear();
    bridge_.reset();
}

bool SwapData::Import(HANDLE handle, VkImage& image, VkDeviceMemory& memory) {
    DXGI_FORMAT dxgi{};
    VkFormat fmt{};
    MapFormat(format, dxgi, fmt);

    VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &ext;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {extent.width, extent.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (dev->CreateImage(dev->device, &ici, nullptr, &image) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    dev->GetImageMemoryRequirements(dev->device, image, &req);
    uint32_t bits = req.memoryTypeBits;
    if (dev->GetMemoryWin32HandlePropertiesKHR) {
        VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        if (dev->GetMemoryWin32HandlePropertiesKHR(dev->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
                                                   handle, &hp) == VK_SUCCESS &&
            (bits & hp.memoryTypeBits))
            bits &= hp.memoryTypeBits;
    }
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < dev->memProps.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((bits & (1u << i)) &&
            (dev->memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;
    for (uint32_t i = 0; i < dev->memProps.memoryTypeCount && type == UINT32_MAX; ++i)
        if (bits & (1u << i)) type = i;
    if (type == UINT32_MAX) return false;

    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = image;
    VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    import.pNext = &dedicated;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    import.handle = handle;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &import;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (dev->AllocateMemory(dev->device, &ai, nullptr, &memory) != VK_SUCCESS) return false;
    return dev->BindImageMemory(dev->device, image, memory, 0) == VK_SUCCESS;
}

void SwapData::Barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                       VkAccessFlags src, VkAccessFlags dst, uint32_t srcFamily, uint32_t dstFamily) const {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = srcFamily;
    b.dstQueueFamilyIndex = dstFamily;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dev->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                            nullptr, 0, nullptr, 1, &b);
}

void SwapData::Copy(VkCommandBuffer cb, VkImage src, VkImageLayout srcLayout, VkImage dst,
                    VkImageLayout dstLayout) const {
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {extent.width, extent.height, 1};
    dev->CmdCopyImage(cb, src, srcLayout, dst, dstLayout, 1, &region);
}

bool SwapData::Begin(uint32_t slot) {
    if (fenceBusy_[slot]) {
        dev->WaitForFences(dev->device, 1, &fence_[slot], VK_TRUE, UINT64_MAX);
        dev->ResetFences(dev->device, 1, &fence_[slot]);
        fenceBusy_[slot] = false;
    }
    dev->ResetCommandBuffer(cmd_[slot], 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return dev->BeginCommandBuffer(cmd_[slot], &bi) == VK_SUCCESS;
}

VkResult SwapData::Submit(VkQueue queue, uint32_t slot, uint32_t waitCount, const VkSemaphore* waits,
                          VkPipelineStageFlags stage, VkSemaphore signal) {
    dev->EndCommandBuffer(cmd_[slot]);
    std::vector<VkPipelineStageFlags> stages(waitCount, stage);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = waitCount;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_[slot];
    si.signalSemaphoreCount = signal ? 1 : 0;
    si.pSignalSemaphores = &signal;
    const VkResult r = dev->QueueSubmit(queue, 1, &si, fence_[slot]);
    if (r == VK_SUCCESS) fenceBusy_[slot] = true;
    return r;
}

bool SwapData::Ensure(VkQueue queue, uint32_t family) {
    DXGI_FORMAT dxgi{};
    VkFormat imported{};
    if (!MapFormat(format, dxgi, imported)) {
        Log("Vulkan swap chain format %d not supported", static_cast<int>(format));
        return false;
    }
    if (bridge_ && family == family_ &&
        bridge_->Matches(extent.width, extent.height, dxgi, G().multiplier.load()))
        return true;
    Destroy();

    bridge_ = std::make_unique<Bridge>();
    BridgeParams p;
    p.adapter = dev->luidValid ? &dev->luid : nullptr;
    p.width = extent.width;
    p.height = extent.height;
    p.format = dxgi;
    p.share = ShareMode::NtHandle;
    std::string err;
    if (!bridge_->Init(p, err)) {
        Log("Vulkan frame generation setup failed: %s", err.c_str());
        return false;
    }
    if (!Import(bridge_->InputHandle(), in_, inMem_) || !Import(bridge_->RealOutputHandle(), real_, realMem_)) {
        Log("Could not import the shared input texture into Vulkan");
        return false;
    }
    const int n = bridge_->MaxOutputs();
    outs_.assign(static_cast<size_t>(n), VK_NULL_HANDLE);
    outMem_.assign(static_cast<size_t>(n), VK_NULL_HANDLE);
    for (int k = 0; k < n; ++k)
        if (!Import(bridge_->OutputHandle(k), outs_[k], outMem_[k])) {
            Log("Could not import a shared output texture into Vulkan");
            return false;
        }

    family_ = family;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    if (dev->CreateCommandPool(dev->device, &pci, nullptr, &pool_) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = kSlots;
    if (dev->AllocateCommandBuffers(dev->device, &cai, cmd_) != VK_SUCCESS) return false;
    for (VkCommandBuffer cb : cmd_)
        if (dev->setLoaderData) dev->setLoaderData(dev->device, cb); // dispatchable object made by a layer

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < kSlots; ++i)
        if (dev->CreateFence(dev->device, &fci, nullptr, &fence_[i]) != VK_SUCCESS) return false;
    for (uint32_t i = 0; i < kMaxGen; ++i)
        if (dev->CreateSemaphore(dev->device, &sci, nullptr, &acquired_[i]) != VK_SUCCESS ||
            dev->CreateSemaphore(dev->device, &sci, nullptr, &copied_[i]) != VK_SUCCESS)
            return false;

    // Shared images live in GENERAL layout for their whole life.
    if (!Begin(0)) return false;
    Barrier(cmd_[0], in_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, 0, VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED);
    Barrier(cmd_[0], real_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, 0, VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED);
    for (VkImage o : outs_)
        Barrier(cmd_[0], o, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, 0, VK_QUEUE_FAMILY_IGNORED,
                VK_QUEUE_FAMILY_IGNORED);
    if (Submit(queue, 0, 0, nullptr, 0, VK_NULL_HANDLE) != VK_SUCCESS) return false;

    Log("Vulkan frame generation ready: %ux%u, format %d, %dx", extent.width, extent.height,
        static_cast<int>(format), G().multiplier.load());
    return true;
}

VkResult SwapData::Present(VkQueue queue, const VkPresentInfoKHR* info) {
    const bool generate = G().fgEnabled.load();
    const bool menu = OverlayVisible();
    if (!generate && bridge_) bridge_->Reset();
    if (!generate && !menu) {
        NoteFrame(0, "Vulkan", extent.width, extent.height);
        return dev->QueuePresentKHR(queue, info);
    }
    auto famIt = dev->queueFamily.find(queue);
    const uint32_t family = famIt == dev->queueFamily.end() ? 0 : famIt->second;
    if (!Ensure(queue, family)) {
        Destroy();
        failed = true;
        return dev->QueuePresentKHR(queue, info);
    }

    const uint32_t idx = info->pImageIndices[0];
    const uint32_t ext = VK_QUEUE_FAMILY_EXTERNAL;

    // 1. real swap chain image -> shared input texture
    if (!Begin(0)) return dev->QueuePresentKHR(queue, info);
    VkCommandBuffer cb = cmd_[0];
    Barrier(cb, images[idx], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
    Barrier(cb, in_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, ext,
            family);
    Copy(cb, images[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, in_, VK_IMAGE_LAYOUT_GENERAL);
    Barrier(cb, images[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_READ_BIT, 0, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
    Barrier(cb, in_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0, family,
            ext);
    VkResult r = Submit(queue, 0, info->waitSemaphoreCount, info->pWaitSemaphores,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) return r;
    dev->WaitForFences(dev->device, 1, &fence_[0], VK_TRUE, UINT64_MAX);
    dev->ResetFences(dev->device, 1, &fence_[0]);
    fenceBusy_[0] = false;

    // 2. run LSFG on Nitro's D3D11 device (and draw the menu, if open)
    const bool drawMenu =
        menu && OverlayBegin(bridge_->Device(), bridge_->Context(), FindMainWindow(), extent.width, extent.height);
    const FramePlan plan = PlanFrame(nullptr); // refresh rate of the foreground window's monitor
    const int n = bridge_->Run(generate, drawMenu ? Bridge::OverlayFn([](ID3D11DeviceContext* c, ID3D11Texture2D* t) { OverlayDraw(c, t); }) : Bridge::OverlayFn{},
                               plan.gens, plan.denom);
    NoteFrame(n, "Vulkan", extent.width, extent.height);

    // 3. present each generated frame in its own swap chain image
    for (int k = 0; k < n; ++k) {
        uint32_t gi = 0;
        r = dev->AcquireNextImageKHR(dev->device, swapchain, 100000000ull, acquired_[k], VK_NULL_HANDLE, &gi);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) break;
        const uint32_t slot = 1 + static_cast<uint32_t>(k);
        if (!Begin(slot)) break;
        cb = cmd_[slot];
        Barrier(cb, outs_[k], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
                ext, family);
        Barrier(cb, images[gi], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        Copy(cb, outs_[k], VK_IMAGE_LAYOUT_GENERAL, images[gi], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        Barrier(cb, images[gi], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        Barrier(cb, outs_[k], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, 0,
                family, ext);
        if (Submit(queue, slot, 1, &acquired_[k], VK_PIPELINE_STAGE_TRANSFER_BIT, copied_[k]) != VK_SUCCESS) break;

        VkPresentInfoKHR gen{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        gen.waitSemaphoreCount = 1;
        gen.pWaitSemaphores = &copied_[k];
        gen.swapchainCount = 1;
        gen.pSwapchains = &swapchain;
        gen.pImageIndices = &gi;
        r = dev->QueuePresentKHR(queue, &gen);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return r;
    }

    // 4. menu open: put the real frame with the menu on it into the game's image
    if (drawMenu && Begin(0)) {
        cb = cmd_[0];
        Barrier(cb, real_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_READ_BIT, ext,
                family);
        Barrier(cb, images[idx], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        Copy(cb, real_, VK_IMAGE_LAYOUT_GENERAL, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        Barrier(cb, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        Barrier(cb, real_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, 0, family,
                ext);
        if (Submit(queue, 0, 0, nullptr, 0, VK_NULL_HANDLE) == VK_SUCCESS) {
            dev->WaitForFences(dev->device, 1, &fence_[0], VK_TRUE, UINT64_MAX);
            dev->ResetFences(dev->device, 1, &fence_[0]);
            fenceBusy_[0] = false;
        }
    }

    // 5. present the real frame (its wait semaphores were consumed in step 1)
    VkPresentInfoKHR real = *info;
    real.waitSemaphoreCount = 0;
    real.pWaitSemaphores = nullptr;
    return dev->QueuePresentKHR(queue, &real);
}

// ---------------------------------------------------------------------------
// Layer entry points

VKAPI_ATTR VkResult VKAPI_CALL Nitro_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* alloc,
                                                    VkInstance* instance) {
    auto* chain = const_cast<VkLayerInstanceCreateInfo*>(static_cast<const VkLayerInstanceCreateInfo*>(ci->pNext));
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                      chain->function == VK_LAYER_LINK_INFO))
        chain = const_cast<VkLayerInstanceCreateInfo*>(static_cast<const VkLayerInstanceCreateInfo*>(chain->pNext));
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    const VkResult r = create(ci, alloc, instance);
    if (r != VK_SUCCESS) return r;

    auto data = std::make_unique<InstanceData>();
    data->instance = *instance;
    data->gipa = gipa;
    auto get = [&](const char* name) { return gipa(*instance, name); };
    data->DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(get("vkDestroyInstance"));
    data->GetPhysicalDeviceProperties2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get("vkGetPhysicalDeviceProperties2"));
    if (!data->GetPhysicalDeviceProperties2)
        data->GetPhysicalDeviceProperties2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get("vkGetPhysicalDeviceProperties2KHR"));
    data->GetPhysicalDeviceMemoryProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get("vkGetPhysicalDeviceMemoryProperties"));
    data->EnumerateDeviceExtensionProperties =
        reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(get("vkEnumerateDeviceExtensionProperties"));
    data->GetPhysicalDeviceSurfaceCapabilitiesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
        get("vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));

    std::lock_guard<std::mutex> lock(g_mutex);
    DecideEnabled();
    g_instances[Key(*instance)] = std::move(data);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Nitro_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc) {
    PFN_vkDestroyInstance destroy = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_instances.find(Key(instance));
        if (it == g_instances.end()) return;
        destroy = it->second->DestroyInstance;
        g_instances.erase(it);
    }
    destroy(instance, alloc);
}

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& e : list)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

VKAPI_ATTR VkResult VKAPI_CALL Nitro_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
                                                  const VkAllocationCallbacks* alloc, VkDevice* device) {
    auto* link = const_cast<VkLayerDeviceCreateInfo*>(static_cast<const VkLayerDeviceCreateInfo*>(ci->pNext));
    VkLayerDeviceCreateInfo* loaderData = nullptr;
    VkLayerDeviceCreateInfo* linkInfo = nullptr;
    for (; link; link = const_cast<VkLayerDeviceCreateInfo*>(static_cast<const VkLayerDeviceCreateInfo*>(link->pNext))) {
        if (link->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) continue;
        if (link->function == VK_LAYER_LINK_INFO && !linkInfo) linkInfo = link;
        if (link->function == VK_LOADER_DATA_CALLBACK && !loaderData) loaderData = link;
    }
    if (!linkInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = linkInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = linkInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    linkInfo->u.pLayerInfo = linkInfo->u.pLayerInfo->pNext;

    InstanceData* inst = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        inst = FindInstance(phys);
    }
    if (!inst) return VK_ERROR_INITIALIZATION_FAILED;
    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst->instance, "vkCreateDevice"));

    // Add the extensions needed to import D3D11 textures, when available.
    std::vector<const char*> exts(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
    bool interop = false;
    if (g_enabled && inst->EnumerateDeviceExtensionProperties) {
        uint32_t count = 0;
        inst->EnumerateDeviceExtensionProperties(phys, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> avail(count);
        inst->EnumerateDeviceExtensionProperties(phys, nullptr, &count, avail.data());
        interop = HasExtension(avail, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
        if (interop) {
            for (const char* want : {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
                                     VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
                                     VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME}) {
                const bool present = std::any_of(exts.begin(), exts.end(),
                                                 [&](const char* e) { return std::strcmp(e, want) == 0; });
                if (!present && HasExtension(avail, want)) exts.push_back(want);
            }
        } else {
            Log("This GPU driver lacks VK_KHR_external_memory_win32 - Vulkan frame gen disabled");
        }
    }
    VkDeviceCreateInfo patched = *ci;
    patched.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    patched.ppEnabledExtensionNames = exts.data();
    const VkResult r = create(phys, &patched, alloc, device);
    if (r != VK_SUCCESS) return r;

    auto data = std::make_unique<DeviceData>();
    data->device = *device;
    data->phys = phys;
    data->inst = inst;
    data->gdpa = gdpa;
    data->setLoaderData = loaderData ? loaderData->u.pfnSetDeviceLoaderData : nullptr;
    data->interop = interop;
#define X(name) data->name = reinterpret_cast<PFN_vk##name>(gdpa(*device, "vk" #name));
    NITRO_DEVICE_FUNCS(X)
#undef X
    if (inst->GetPhysicalDeviceProperties2) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &id;
        inst->GetPhysicalDeviceProperties2(phys, &props);
        if (id.deviceLUIDValid) {
            std::memcpy(&data->luid, id.deviceLUID, sizeof(LUID));
            data->luidValid = true;
        }
    }
    if (inst->GetPhysicalDeviceMemoryProperties) inst->GetPhysicalDeviceMemoryProperties(phys, &data->memProps);

    std::lock_guard<std::mutex> lock(g_mutex);
    g_devices[Key(*device)] = std::move(data);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Nitro_DestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
    PFN_vkDestroyDevice destroy = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        DeviceData* d = FindDevice(device);
        if (!d) return;
        for (auto it = g_swapchains.begin(); it != g_swapchains.end();)
            it = it->second->dev == d ? g_swapchains.erase(it) : std::next(it);
        destroy = d->DestroyDevice;
        g_devices.erase(Key(device));
    }
    destroy(device, alloc);
}

VKAPI_ATTR void VKAPI_CALL Nitro_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    std::lock_guard<std::mutex> lock(g_mutex);
    DeviceData* d = FindDevice(device);
    if (!d) return;
    d->GetDeviceQueue(device, family, index, queue);
    if (*queue) d->queueFamily[*queue] = family;
}

VKAPI_ATTR void VKAPI_CALL Nitro_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    std::lock_guard<std::mutex> lock(g_mutex);
    DeviceData* d = FindDevice(device);
    if (!d || !d->GetDeviceQueue2) return;
    d->GetDeviceQueue2(device, info, queue);
    if (*queue) d->queueFamily[*queue] = info->queueFamilyIndex;
}

VKAPI_ATTR VkResult VKAPI_CALL Nitro_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* ci,
                                                        const VkAllocationCallbacks* alloc, VkSwapchainKHR* sc) {
    std::lock_guard<std::mutex> lock(g_mutex);
    DeviceData* d = FindDevice(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (!g_enabled || !d->interop) return d->CreateSwapchainKHR(device, ci, alloc, sc);

    VkSwapchainCreateInfoKHR patched = *ci;
    bool usable = true;
    if (d->inst->GetPhysicalDeviceSurfaceCapabilitiesKHR) {
        VkSurfaceCapabilitiesKHR caps{};
        if (d->inst->GetPhysicalDeviceSurfaceCapabilitiesKHR(d->phys, ci->surface, &caps) == VK_SUCCESS) {
            const VkImageUsageFlags need = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            usable = (caps.supportedUsageFlags & need) == need;
            uint32_t want = ci->minImageCount + kMaxGen; // room for the extra acquires
            if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
            if (usable) patched.minImageCount = want;
        }
    }
    if (usable) patched.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    const VkResult r = d->CreateSwapchainKHR(device, &patched, alloc, sc);
    if (r != VK_SUCCESS || !usable) return r;

    auto data = std::make_unique<SwapData>();
    data->dev = d;
    data->swapchain = *sc;
    data->format = ci->imageFormat;
    data->extent = ci->imageExtent;
    uint32_t count = 0;
    d->GetSwapchainImagesKHR(device, *sc, &count, nullptr);
    data->images.resize(count);
    d->GetSwapchainImagesKHR(device, *sc, &count, data->images.data());
    g_swapchains[*sc] = std::move(data);
    return r;
}

VKAPI_ATTR void VKAPI_CALL Nitro_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR sc,
                                                     const VkAllocationCallbacks* alloc) {
    std::lock_guard<std::mutex> lock(g_mutex);
    DeviceData* d = FindDevice(device);
    if (!d) return;
    g_swapchains.erase(sc);
    d->DestroySwapchainKHR(device, sc, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL Nitro_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    std::unique_lock<std::mutex> lock(g_mutex);
    DeviceData* d = FindDevice(queue);
    if (!d) return VK_ERROR_DEVICE_LOST;
    SwapData* s = nullptr;
    if (g_enabled && info->swapchainCount == 1 && !InHook()) {
        auto it = g_swapchains.find(info->pSwapchains[0]);
        if (it != g_swapchains.end() && !it->second->failed) s = it->second.get();
    }
    if (!s) {
        lock.unlock();
        return d->QueuePresentKHR(queue, info);
    }
    InHook() = true;
    PollHotkeys();
    const VkResult r = s->Present(queue, info);
    InHook() = false;
    return r;
}

PFN_vkVoidFunction DeviceHook(const char* name) {
    struct Entry {
        const char* name;
        PFN_vkVoidFunction fn;
    };
    static const Entry kEntries[] = {
        {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(&Nitro_DestroyDevice)},
        {"vkGetDeviceQueue", reinterpret_cast<PFN_vkVoidFunction>(&Nitro_GetDeviceQueue)},
        {"vkGetDeviceQueue2", reinterpret_cast<PFN_vkVoidFunction>(&Nitro_GetDeviceQueue2)},
        {"vkCreateSwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(&Nitro_CreateSwapchainKHR)},
        {"vkDestroySwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(&Nitro_DestroySwapchainKHR)},
        {"vkQueuePresentKHR", reinterpret_cast<PFN_vkVoidFunction>(&Nitro_QueuePresentKHR)},
    };
    for (const auto& e : kEntries)
        if (std::strcmp(name, e.name) == 0) return e.fn;
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Nitro_GetDeviceProcAddr(VkDevice device, const char* name);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Nitro_GetInstanceProcAddr(VkInstance instance, const char* name) {
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&Nitro_GetInstanceProcAddr);
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&Nitro_GetDeviceProcAddr);
    if (std::strcmp(name, "vkCreateInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&Nitro_CreateInstance);
    if (std::strcmp(name, "vkDestroyInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&Nitro_DestroyInstance);
    if (std::strcmp(name, "vkCreateDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&Nitro_CreateDevice);
    if (PFN_vkVoidFunction f = DeviceHook(name)) return f;
    if (!instance) return nullptr;
    PFN_vkGetInstanceProcAddr next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (InstanceData* d = FindInstance(instance)) next = d->gipa;
    }
    return next ? next(instance, name) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Nitro_GetDeviceProcAddr(VkDevice device, const char* name) {
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&Nitro_GetDeviceProcAddr);
    if (PFN_vkVoidFunction f = DeviceHook(name)) return f;
    PFN_vkGetDeviceProcAddr next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (DeviceData* d = FindDevice(device)) next = d->gdpa;
    }
    return next ? next(device, name) : nullptr;
}

} // namespace

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
    if (!v || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    if (v->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    v->pfnGetInstanceProcAddr = &Nitro_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = &Nitro_GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
