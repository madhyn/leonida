// The Xbox Series depth-stencil format D16_UNORM_S8_UINT (DXGI_FORMAT 118, past B4G4R4A4_UNORM) and its plane views
// R16_UNORM_X8_TYPELESS (119) and X16_TYPELESS_G8_UINT (120) on the Vulkan-backed device.
//
// The retail executable sizes a 118 texture with GetResourceAllocationInfo at start-up and asserts that the answer is
// not the invalid one; it also names 118 as a pipeline DSVFormat. The layer stores the texture as D32_SFLOAT_S8_UINT
// (NVIDIA and RADV have no Vulkan D16_UNORM_S8_UINT). This checks, on whatever GPU runs it:
//   * FORMAT_INFO gives two planes for the three formats;
//   * FORMAT_SUPPORT answers 118 as a 2D depth-stencil format and the two plane views as shader-readable, and gives
//     the caller its own format back in the structure;
//   * GetResourceAllocationInfo sizes a full-screen 118 depth buffer (not UINT64_MAX), at least two bytes a texel
//     for the depth plane;
//   * a 118 depth buffer is created with a 118 clear value, takes DSVs in its own format and in UNKNOWN, and a
//     depth-and-stencil clear through them runs to its fence.
//
// Synthetic resources only; no game files. LEONIDA_VK_VALIDATION=1 checks Vulkan usage.
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"

#include <cstdint>
#include <cstdio>

namespace {

constexpr auto kD16S8 = static_cast<DXGI_FORMAT>(118);
constexpr auto kR16X8 = static_cast<DXGI_FORMAT>(119);
constexpr auto kX16G8 = static_cast<DXGI_FORMAT>(120);

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

template <class T>
struct Ref {
    T* p = nullptr;
    ~Ref() { if (p) p->Release(); }
    T* operator->() const { return p; }
};

D3D12_RESOURCE_DESC depth_texture(UINT64 width, UINT height) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kD16S8;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    return desc;
}

void format_queries(ID3D12Device* device) {
    for (const DXGI_FORMAT format : {kD16S8, kR16X8, kX16G8}) {
        D3D12_FEATURE_DATA_FORMAT_INFO info{format};
        const HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info));
        std::printf("format %d: FORMAT_INFO hr=0x%08lx planes %u\n", static_cast<int>(format),
                    static_cast<unsigned long>(hr), static_cast<unsigned>(info.PlaneCount));
        check(SUCCEEDED(hr) && info.PlaneCount == 2, "FORMAT_INFO: two planes");
    }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{kD16S8};
    HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs));
    std::printf("format 118: FORMAT_SUPPORT hr=0x%08lx Support1=0x%08x\n", static_cast<unsigned long>(hr), fs.Support1);
    check(SUCCEEDED(hr), "FORMAT_SUPPORT answers D16_UNORM_S8_UINT");
    check((fs.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) && (fs.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D),
          "D16_UNORM_S8_UINT is a 2D depth-stencil format");
    check(fs.Format == kD16S8, "FORMAT_SUPPORT leaves the caller's format in the structure");

    for (const DXGI_FORMAT plane : {kR16X8, kX16G8}) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT view{plane};
        hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &view, sizeof(view));
        std::printf("format %d: FORMAT_SUPPORT hr=0x%08lx Support1=0x%08x\n", static_cast<int>(plane),
                    static_cast<unsigned long>(hr), view.Support1);
        check(SUCCEEDED(hr) && (view.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) && view.Format == plane,
              "a D16_UNORM_S8_UINT plane view format is shader-readable");
    }
}

void allocation(ID3D12Device* device) {
    const D3D12_RESOURCE_DESC desc = depth_texture(2560, 1440);
    const D3D12_RESOURCE_ALLOCATION_INFO info = device->GetResourceAllocationInfo(0, 1, &desc);
    std::printf("format 118 2560x1440: GetResourceAllocationInfo size %llu alignment %llu\n",
                static_cast<unsigned long long>(info.SizeInBytes), static_cast<unsigned long long>(info.Alignment));
    check(info.SizeInBytes != UINT64_MAX, "GetResourceAllocationInfo sizes a D16_UNORM_S8_UINT depth buffer");
    check(info.SizeInBytes != UINT64_MAX && info.SizeInBytes >= UINT64(2560) * 1440 * 2,
          "the size holds two bytes of depth a texel");
}

bool clear_runs(ID3D12Device* device) {
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> fence;
    Ref<ID3D12DescriptorHeap> heap;
    Ref<ID3D12Resource> depth;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                         IID_PPV_ARGS(&list.p))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))))
        return false;
    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap.p)))) return false;

    D3D12_HEAP_PROPERTIES heap_props{D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC desc = depth_texture(64, 32);
    D3D12_CLEAR_VALUE clear{};
    clear.Format = kD16S8;
    clear.DepthStencil.Depth = 1.0f;
    const HRESULT made = device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                                         IID_PPV_ARGS(&depth.p));
    check(SUCCEEDED(made), "a D16_UNORM_S8_UINT depth buffer is created with its own clear value");
    if (FAILED(made)) return false;

    const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_CPU_DESCRIPTOR_HANDLE typed = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE implicit{typed.ptr + step};
    D3D12_DEPTH_STENCIL_VIEW_DESC view{};
    view.Format = kD16S8;
    view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(depth.p, &view, typed);
    device->CreateDepthStencilView(depth.p, nullptr, implicit);
    const auto both = static_cast<D3D12_CLEAR_FLAGS>(D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL);
    list->ClearDepthStencilView(typed, both, 0.25f, 0x5a, 0, nullptr);
    const D3D12_RECT rect{4, 4, 20, 12};
    list->ClearDepthStencilView(implicit, both, 0.75f, 0x3c, 1, &rect);
    if (FAILED(list->Close())) return false;
    ID3D12CommandList* lists[] = {list.p};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence.p, 1))) return false;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const bool done = event && SUCCEEDED(fence->SetEventOnCompletion(1, event)) &&
                      WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
    if (event) CloseHandle(event);
    return done;
}

}  // namespace

int main() {
    vktest::Device d;
    const auto status = d.initialize();
    if (status == vktest::DeviceStatus::unavailable) return 77;
    if (status != vktest::DeviceStatus::ready) return 1;

    VkFormatProperties d16s8{};
    d.instance_fn<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(
        d.physical, VK_FORMAT_D16_UNORM_S8_UINT, &d16s8);
    std::printf("Vulkan D16_UNORM_S8_UINT depth attachment: %s (the layer stores D32_SFLOAT_S8_UINT either way)\n",
                (d16s8.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? "native" : "no");

    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = d.instance_proc;
    info.instance = d.instance;
    info.physical = d.physical;
    info.device = d.device;
    info.queue = d.queue;
    info.queue_family = d.family;
    info.bindless = d.bindless;
    info.bindless_partially_bound = d.partially_bound;
    info.image_view_min_lod = d.image_view_min_lod;
    info.host_image_copy = d.host_image_copy;
    ID3D12Device* device = nullptr;
    if (FAILED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&device))) || !device) {
        std::printf("FAIL Vulkan-backed D3D12 device created\n");
        return 1;
    }

    format_queries(device);
    allocation(device);
    check(clear_runs(device), "depth-and-stencil clears through a typed and an UNKNOWN DSV run to the fence");
    device->Release();

    if (g_failures) {
        std::printf("d3d12_xbox_d16s8_test: %d checks failed\n", g_failures);
        return 1;
    }
    std::printf("d3d12_xbox_d16s8_test: OK\n");
    return 0;
}
