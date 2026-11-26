// Copies between two formats of one D3D12 copy family (depth <-> colour of the same size, BC <-> its uncompressed
// equivalent), on the Vulkan-backed device and on host D3D12 (the layer's own adapter and WARP). Bytes read back
// compared.
#include "vk_test_device.h"
#include "host_adapter.h"
#include "host_wine.h"
#include "../shims/d3d12_com.h"
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
template<class T>
struct Ref {
    T* p = nullptr;
    ~Ref() { if (p) p->Release(); }
    T* operator->() const { return p; }
};

D3D12_RESOURCE_DESC tex(DXGI_FORMAT f, UINT w, UINT h, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    return d;
}

D3D12_RESOURCE_DESC mips(D3D12_RESOURCE_DESC d, UINT16 levels) {
    d.MipLevels = levels;
    return d;
}

D3D12_RESOURCE_DESC buf(UINT64 bytes) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

struct Case {
    const char* name;
    D3D12_RESOURCE_DESC src, dst;
    bool whole;  // CopyResource, else CopyTextureRegion
    bool depth_values;  // also marks the depth <-> colour cases, counted apart from the BC ones (both fail the test)
    UINT x = 0, y = 0;  // CopyTextureRegion destination offset
    bool has_box = false;
    D3D12_BOX box{};
    UINT dst_mip = 0;
    bool check_edge_bytes = false;
};

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    return x;
}

unsigned char upload_byte(UINT64 index) {
    return static_cast<unsigned char>(index * 7 + 3);
}

// Pack the destination footprint rows for the copied subresource, or every mip and layer for CopyResource.
bool run(ID3D12Device* dev, const Case& c, std::vector<unsigned char>& out, HRESULT& close_hr,
         const std::vector<unsigned char>& packed_upload = {}, bool check_replay = false) {
    D3D12_COMMAND_QUEUE_DESC qd{};
    Ref<ID3D12CommandQueue> q;
    Ref<ID3D12CommandAllocator> a;
    Ref<ID3D12GraphicsCommandList> l;
    Ref<ID3D12Fence> fence;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q.p))) ||
        FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a.p))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.p, nullptr, IID_PPV_ARGS(&l.p))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))))
        return false;
    const UINT ns = UINT(c.src.MipLevels) * c.src.DepthOrArraySize, nd = UINT(c.dst.MipLevels) * c.dst.DepthOrArraySize;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> sf(ns), df(nd);
    std::vector<UINT> source_rows(ns), rows(nd);
    std::vector<UINT64> source_row_bytes(ns), row_bytes(nd);
    UINT64 sbytes = 0, dbytes = 0;
    dev->GetCopyableFootprints(&c.src, 0, ns, 0, sf.data(), source_rows.data(), source_row_bytes.data(), &sbytes);
    dev->GetCopyableFootprints(&c.dst, 0, nd, 0, df.data(), rows.data(), row_bytes.data(), &dbytes);
    if (!packed_upload.empty()) {
        UINT64 payload = 0;
        if (source_rows != rows || source_row_bytes != row_bytes) return false;
        for (UINT i = 0; i < ns; ++i) payload += source_rows[i] * source_row_bytes[i];
        if (payload != packed_upload.size()) return false;
    }
    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT}, up{D3D12_HEAP_TYPE_UPLOAD}, rb{D3D12_HEAP_TYPE_READBACK};
    Ref<ID3D12Resource> src, dst, upload, readback;
    const D3D12_RESOURCE_DESC ub = buf(sbytes), rbd = buf(dbytes);
    if (FAILED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &c.src, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&src.p))) ||
        FAILED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &c.dst, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&dst.p))) ||
        FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ub, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&upload.p))) ||
        FAILED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &rbd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&readback.p))))
        return false;
    unsigned char* p = nullptr;
    if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
    for (UINT64 i = 0; i < sbytes; ++i) p[i] = upload_byte(i);
    if (c.depth_values)  // normal floats in [0, 1): a host driver may flush denormals
        for (UINT64 i = 0; i + 4 <= sbytes; i += 4) {
            const float v = float((i / 4) % 1000) / 1000.0f;
            std::memcpy(p + i, &v, 4);
        }
    if (c.check_edge_bytes) {
        // The edge fixtures copy whole R32G32_UINT source rows, one eight-byte texel per BC1 block.
        // Count the data bytes, excluding padding, so each row contains a different pattern.
        const UINT64 bytes_per_row = c.src.Width * 8;
        for (UINT y = 0; y < c.src.Height; ++y) {
            auto* row = p + sf[0].Offset + UINT64(y) * sf[0].Footprint.RowPitch;
            for (UINT64 x = 0; x < bytes_per_row; ++x) row[x] = upload_byte(y * bytes_per_row + x);
        }
    }
    if (!packed_upload.empty()) {
        const unsigned char* input = packed_upload.data();
        for (UINT i = 0; i < ns; ++i) {
            for (UINT y = 0; y < source_rows[i]; ++y) {
                auto* row = p + sf[i].Offset + UINT64(y) * sf[i].Footprint.RowPitch;
                std::memcpy(row, input, size_t(source_row_bytes[i]));
                input += source_row_bytes[i];
            }
        }
    }
    upload->Unmap(0, nullptr);
    if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
    std::memset(p, 0xcd, size_t(dbytes));
    readback->Unmap(0, nullptr);

    for (UINT i = 0; i < ns; ++i) {
        D3D12_TEXTURE_COPY_LOCATION from{upload.p, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, to{src.p};
        from.PlacedFootprint = sf[i];
        to.SubresourceIndex = i;
        l->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    D3D12_RESOURCE_BARRIER b = transition(src.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l->ResourceBarrier(1, &b);
    if (c.whole) {
        l->CopyResource(dst.p, src.p);
    } else {
        D3D12_TEXTURE_COPY_LOCATION s{src.p}, d{dst.p};
        d.SubresourceIndex = c.dst_mip;
        l->CopyTextureRegion(&d, c.x, c.y, 0, &s, c.has_box ? &c.box : nullptr);
    }
    b = transition(dst.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l->ResourceBarrier(1, &b);
    const UINT first = c.whole ? 0 : c.dst_mip, end = c.whole ? nd : c.dst_mip + 1;
    for (UINT i = first; i < end; ++i) {
        D3D12_TEXTURE_COPY_LOCATION rs{dst.p}, rd{readback.p, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        rs.SubresourceIndex = i;
        rd.PlacedFootprint = df[i];
        l->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
    }
    close_hr = l->Close();
    if (FAILED(close_hr)) return false;
    ID3D12CommandList* lists[] = {l.p};
    q->ExecuteCommandLists(1, lists);
    if (check_replay) {
        const auto result = rt::d3d12::last_execute_result(q.p);
        if (result != leonida::ReplayResult::success) {
            std::fprintf(stderr, "%s: Vulkan replay returned %s (%d)\n", c.name,
                         result == leonida::ReplayResult::invalid_copy ? "invalid_copy" : "error", static_cast<int>(result));
            return false;
        }
    }
    if (FAILED(q->Signal(fence.p, 1))) return false;
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!e) return false;
    const bool complete = SUCCEEDED(fence->SetEventOnCompletion(1, e)) && WaitForSingleObject(e, 10000) == WAIT_OBJECT_0;
    CloseHandle(e);
    if (!complete || fence->GetCompletedValue() != 1 || FAILED(dev->GetDeviceRemovedReason())) return false;
    if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
    // Compare the bytes inside the rows and leave footprint padding out.
    out.clear();
    for (UINT i = first; i < end; ++i)
        for (UINT r = 0; r < rows[i]; ++r) {
            const unsigned char* row = p + df[i].Offset + r * df[i].Footprint.RowPitch;
            out.insert(out.end(), row, row + row_bytes[i]);
        }
    readback->Unmap(0, nullptr);
    return true;
}

void drain(ID3D12Device* host) {
    Ref<ID3D12InfoQueue> iq;
    if (FAILED(host->QueryInterface(IID_PPV_ARGS(&iq.p)))) return;
    for (UINT64 i = 0; i < iq->GetNumStoredMessages(); ++i) {
        SIZE_T n = 0;
        iq->GetMessage(i, nullptr, &n);
        std::vector<char> m(n);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(m.data());
        iq->GetMessage(i, msg, &n);
        if (msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) std::fprintf(stderr, "    host debug: %s\n", msg->pDescription);
    }
    iq->ClearStoredMessages();
}

bool check_bc_mip_tails(ID3D12Device* ours, IDXGIFactory4* factory) {
    Ref<IDXGIAdapter> adapter;
    Ref<ID3D12Device> warp;
    if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter.p))) ||
        FAILED(D3D12CreateDevice(adapter.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&warp.p)))) {
        std::fprintf(stderr, "BC mip tails require a native D3D12 WARP device\n");
        return false;
    }
    const auto adapter_name = hostadapter::description(adapter.p);
    struct Format { const char* name; DXGI_FORMAT src, dst; UINT texel_bytes; };
    const Format formats[] = {
        {"BC1", DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_BC1_UNORM, 8},
        {"BC7", DXGI_FORMAT_R32G32B32A32_UINT, DXGI_FORMAT_BC7_UNORM, 16},
    };
    constexpr UINT16 levels = 4;
    unsigned matched = 0, subresources = 0;
    size_t bytes = 0;
    for (const auto& format : formats) {
        for (const UINT width : {8u, 1u}) {
            const UINT height = width == 8 ? 1 : 8;
            for (UINT16 layers = 1; layers <= 2; ++layers) {
                Case c{format.name, mips(tex(format.src, width, height), levels),
                       mips(tex(format.dst, 4 * width, 4 * height), levels), true, false};
                c.src.DepthOrArraySize = c.dst.DepthOrArraySize = layers;
                // One source texel becomes one BC block. Build the expected packed rows from the source
                // dimensions alone, in D3D12 subresource order. Mips, layers and rows have distinct data.
                std::vector<unsigned char> expected;
                const UINT count = UINT(levels) * layers;
                for (UINT sub = 0; sub < count; ++sub) {
                    const UINT mip = sub % levels;
                    const UINT row_bytes = std::max(1u, width >> mip) * format.texel_bytes;
                    for (UINT row = 0; row < std::max(1u, height >> mip); ++row)
                        for (UINT x = 0; x < row_bytes; ++x)
                            expected.push_back(static_cast<unsigned char>(13 + sub * 43 + row * 91 + x * 19 + (x >> 2) * 7));
                }
                std::vector<unsigned char> native, vulkan;
                HRESULT wh = E_FAIL, vh = E_FAIL;
                const bool warp_ok = run(warp.p, c, native, wh, expected) && native == expected;
                const bool vk_ok = run(ours, c, vulkan, vh, expected, true) && vulkan == expected;
                if (!warp_ok || !vk_ok) {
                    std::fprintf(stderr, "%s %ux%u, %u layers: expected %zu bytes; WARP (%s) %s (%zu bytes, "
                                 "Close 0x%08lx), Vulkan %s (%zu bytes, Close 0x%08lx)\n", format.name, width, height,
                                 UINT(layers), expected.size(), adapter_name.c_str(), warp_ok ? "matches" : "failed",
                                 native.size(), static_cast<unsigned long>(wh), vk_ok ? "matches" : "failed",
                                 vulkan.size(), static_cast<unsigned long>(vh));
                    drain(warp.p);
                    continue;
                }
                ++matched;
                subresources += count;
                bytes += expected.size();
            }
        }
    }
    std::printf("BC mip tails: %u/8 cases, %u subresources, %zu bytes match WARP (%s) and the upload\n",
                matched, subresources, bytes, adapter_name.c_str());
    return matched == 8;
}
}  // namespace

int main() {
    vktest::Device vk;
    const auto status = vk.initialize();
    if (status == vktest::DeviceStatus::unavailable) return 77;
    if (status != vktest::DeviceStatus::ready) return 1;
    Ref<ID3D12Debug> debug;
    const bool debug_on = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug.p)));
    if (debug_on) debug->EnableDebugLayer();
    std::printf("host debug layer: %s\n", debug_on ? "enabled" : "NOT available");
    Ref<ID3D12Device> ours;
    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = vk.instance_proc;
    info.instance = vk.instance;
    info.physical = vk.physical;
    info.device = vk.device;
    info.queue = vk.queue;
    info.queue_family = vk.family;
    if (FAILED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&ours.p)))) return 1;

    constexpr auto ds = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    const Case cases[] = {
        {"CopyResource D32_FLOAT -> R32_FLOAT", tex(DXGI_FORMAT_D32_FLOAT, 16, 16, ds), tex(DXGI_FORMAT_R32_FLOAT, 16, 16), true, true},
        {"CopyTextureRegion D32_FLOAT -> R32_FLOAT", tex(DXGI_FORMAT_D32_FLOAT, 16, 16, ds), tex(DXGI_FORMAT_R32_FLOAT, 16, 16), false, true},
        {"CopyResource R32_FLOAT -> D32_FLOAT", tex(DXGI_FORMAT_R32_FLOAT, 16, 16), tex(DXGI_FORMAT_D32_FLOAT, 16, 16, ds), true, true},
        {"CopyResource D16_UNORM -> R16_UNORM", tex(DXGI_FORMAT_D16_UNORM, 16, 16, ds), tex(DXGI_FORMAT_R16_UNORM, 16, 16), true, true},
        {"CopyResource R16_UNORM -> D16_UNORM", tex(DXGI_FORMAT_R16_UNORM, 16, 16), tex(DXGI_FORMAT_D16_UNORM, 16, 16, ds), true, true},
        {"CopyTextureRegion D32_FLOAT -> R32_UINT", tex(DXGI_FORMAT_D32_FLOAT, 16, 16, ds), tex(DXGI_FORMAT_R32_UINT, 16, 16), false, true},
        // A depth-stencil side is copied whole (no box, offset 0), as D3D12 requires of CopyTextureRegion.
        {"CopyTextureRegion R16_UNORM -> D16_UNORM", tex(DXGI_FORMAT_R16_UNORM, 16, 16), tex(DXGI_FORMAT_D16_UNORM, 16, 16, ds), false, true},
        {"CopyResource D16_UNORM -> R16_UINT", tex(DXGI_FORMAT_D16_UNORM, 16, 16, ds), tex(DXGI_FORMAT_R16_UINT, 16, 16), true, true},
        {"CopyTextureRegion R16_SNORM -> D16_UNORM", tex(DXGI_FORMAT_R16_SNORM, 16, 16), tex(DXGI_FORMAT_D16_UNORM, 16, 16, ds), false, true},
        {"CopyResource D32_FLOAT -> R32_FLOAT, 3 mips", mips(tex(DXGI_FORMAT_D32_FLOAT, 16, 16, ds), 3), mips(tex(DXGI_FORMAT_R32_FLOAT, 16, 16), 3), true, true},
        {"negative control: CopyResource D32_FLOAT -> R16_UNORM", tex(DXGI_FORMAT_D32_FLOAT, 16, 16, ds), tex(DXGI_FORMAT_R16_UNORM, 16, 16), true, true},
        {"CopyTextureRegion R32G32_UINT 4x4 -> BC1 16x16", tex(DXGI_FORMAT_R32G32_UINT, 4, 4), tex(DXGI_FORMAT_BC1_UNORM, 16, 16), false, false},
        {"CopyTextureRegion BC1 16x16 -> R32G32_UINT 4x4", tex(DXGI_FORMAT_BC1_UNORM, 16, 16), tex(DXGI_FORMAT_R32G32_UINT, 4, 4), false, false},
        {"negative control: CopyResource R32_FLOAT -> R16_FLOAT", tex(DXGI_FORMAT_R32_FLOAT, 16, 16), tex(DXGI_FORMAT_R16_FLOAT, 16, 16), true, false},
        {"negative control: CopyResource R32_FLOAT 16x16 -> R32_FLOAT 8x8", tex(DXGI_FORMAT_R32_FLOAT, 16, 16), tex(DXGI_FORMAT_R32_FLOAT, 8, 8), true, false},
        {"CopyTextureRegion BC7 16x16 -> R32G32B32A32_UINT 4x4", tex(DXGI_FORMAT_BC7_UNORM, 16, 16), tex(DXGI_FORMAT_R32G32B32A32_UINT, 4, 4), false, false},
        {"CopyTextureRegion BC1 box 4..12 x 8..16 -> R16G16B16A16_UINT 8x8 at (3, 1)", tex(DXGI_FORMAT_BC1_UNORM, 16, 16), tex(DXGI_FORMAT_R16G16B16A16_UINT, 8, 8), false, false, 3, 1, true, {4, 8, 0, 12, 16, 1}},
        {"CopyTextureRegion BC1 10x6 (padded box) -> R32G32_UINT 3x2", tex(DXGI_FORMAT_BC1_UNORM, 10, 6), tex(DXGI_FORMAT_R32G32_UINT, 3, 2), false, false},
        {.name = "CopyTextureRegion R32G32_UINT 3x2 -> BC1 10x6",
         .src = tex(DXGI_FORMAT_R32G32_UINT, 3, 2), .dst = tex(DXGI_FORMAT_BC1_UNORM, 10, 6),
         .whole = false, .depth_values = false, .check_edge_bytes = true},
        {.name = "CopyTextureRegion R32G32_UINT 2x1 -> BC1 12x8 mip 1 (6x4)",
         .src = tex(DXGI_FORMAT_R32G32_UINT, 2, 1), .dst = mips(tex(DXGI_FORMAT_BC1_UNORM, 12, 8), 2),
         .whole = false, .depth_values = false, .dst_mip = 1, .check_edge_bytes = true},
        {"CopyTextureRegion R32G32_UINT 2x2 -> BC1 16x16 at (8, 4)", tex(DXGI_FORMAT_R32G32_UINT, 2, 2), tex(DXGI_FORMAT_BC1_UNORM, 16, 16), false, false, 8, 4},
        {"negative control: CopyTextureRegion R32_UINT 4x4 -> BC1 16x16", tex(DXGI_FORMAT_R32_UINT, 4, 4), tex(DXGI_FORMAT_BC1_UNORM, 16, 16), false, false},
        {"negative control: CopyTextureRegion R32G32_UINT 4x4 -> BC1 16x16 at (12, 0)", tex(DXGI_FORMAT_R32G32_UINT, 4, 4), tex(DXGI_FORMAT_BC1_UNORM, 16, 16), false, false, 12, 0},
        {"CopyResource BC7 16x16 -> R32G32B32A32_UINT 4x4", tex(DXGI_FORMAT_BC7_UNORM, 16, 16), tex(DXGI_FORMAT_R32G32B32A32_UINT, 4, 4), true, false},
        {"CopyResource BC1 12x8 -> R32G32_UINT 3x2", tex(DXGI_FORMAT_BC1_UNORM, 12, 8), tex(DXGI_FORMAT_R32G32_UINT, 3, 2), true, false},
        {"CopyResource R32G32B32A32_UINT 4x4 -> BC7 16x16, 3 mips", mips(tex(DXGI_FORMAT_R32G32B32A32_UINT, 4, 4), 3), mips(tex(DXGI_FORMAT_BC7_UNORM, 16, 16), 3), true, false},
        {"CopyResource BC7 16x16 -> R32G32B32A32_UINT 4x4, 3 mips", mips(tex(DXGI_FORMAT_BC7_UNORM, 16, 16), 3), mips(tex(DXGI_FORMAT_R32G32B32A32_UINT, 4, 4), 3), true, false},
        {"negative control: CopyResource BC1 12x8 -> R32G32_UINT 3x2, 2 mips", mips(tex(DXGI_FORMAT_BC1_UNORM, 12, 8), 2), mips(tex(DXGI_FORMAT_R32G32_UINT, 3, 2), 2), true, false},
        {"CopyResource R32G32_UINT 3x2 -> BC1 12x8, 2 mips", mips(tex(DXGI_FORMAT_R32G32_UINT, 3, 2), 2), mips(tex(DXGI_FORMAT_BC1_UNORM, 12, 8), 2), true, false},
        {"CopyResource R32G32B32A32_UINT 4x4 -> BC7 16x16", tex(DXGI_FORMAT_R32G32B32A32_UINT, 4, 4), tex(DXGI_FORMAT_BC7_UNORM, 16, 16), true, false},
    };

    if (hostwine::host_is_wine()) return 77;  // vkd3d runs copies WARP refuses, so the controls can't hold
    Ref<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory.p)))) return 1;
    const bool mip_tails_ok = check_bc_mip_tails(ours.p, factory.p);
    unsigned differ = 0, differ_depth = 0, compared = 0;
    for (const bool warp : {false, true}) {
        for (const Case& c : cases) {
            // Check the full output of the partial-edge fixtures against the upload bytes on every available
            // Vulkan device. Compare the same cases against native D3D12 using WARP when available.
            if (!warp && (c.check_edge_bytes || std::strncmp(c.name, "negative", 8) == 0)) continue;
            std::vector<unsigned char> h, o;
            HRESULT hh = S_OK, oh = S_OK;
            if (c.check_edge_bytes) {
                std::vector<unsigned char> expected(size_t(c.src.Width) * c.src.Height * 8);
                for (size_t i = 0; i < expected.size(); ++i) expected[i] = upload_byte(i);
                const bool ours_ok = run(ours.p, c, o, oh);
                if (!ours_ok || o != expected) {
                    std::fprintf(stderr, "%s: expected %zu upload bytes (read %zu bytes, Close 0x%08lx)\n",
                                 c.name, expected.size(), o.size(), (unsigned long)oh);
                    ++differ;
                    continue;
                }
                std::printf("%s: matches %zu upload bytes\n", c.name, expected.size());
            }
            // A fresh device per case: an invalid copy removes the host device.
            Ref<IDXGIAdapter> adapter;
            Ref<ID3D12Device> host;
            std::string adapter_name;
            if (!hostadapter::reference(factory.p, vk, warp, &adapter.p, adapter_name)) continue;
            const char* hn = adapter_name.c_str();
            if (FAILED(D3D12CreateDevice(adapter.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&host.p)))) continue;
            const bool host_ok = run(host.p, c, h, hh);
            drain(host.p);
            if (!host_ok) {
                std::printf("%s: %s refuses it (Close 0x%08lx, removed reason 0x%08lx), skipped\n", c.name, hn,
                            (unsigned long)hh, (unsigned long)host->GetDeviceRemovedReason());
                if (c.check_edge_bytes) continue;  // the layer's output was already checked against the upload
                // The layer must refuse it too (at Close or execution), never write out of bounds (validation).
                const unsigned before = vk.validation_errors();
                run(ours.p, c, o, oh);
                std::printf("  ours: Close 0x%08lx, %u new Vulkan validation errors\n", (unsigned long)oh,
                            vk.validation_errors() - before);
                continue;
            }
            const bool ours_ok = c.check_edge_bytes || run(ours.p, c, o, oh);
            ++compared;
            if (ours_ok && o == h) {
                std::printf("%s: matches %s\n", c.name, hn);
                continue;
            }
            ++(c.depth_values ? differ_depth : differ);
            size_t first = 0;
            while (first < o.size() && first < h.size() && o[first] == h[first]) ++first;
            bool untouched = !o.empty();
            for (unsigned char x : o) untouched = untouched && x == 0xcd;
            std::printf("%s: DIFFERS from %s (ours %s%s, Close 0x%08lx; first differing byte %zu: ours 0x%02x host 0x%02x)\n",
                        c.name, hn, ours_ok ? "submitted" : "refused", untouched ? ", readback untouched: the copy was refused at execution" : "", (unsigned long)oh, first,
                        first < o.size() ? o[first] : 0, first < h.size() ? h[first] : 0);
        }
    }
    std::printf("%u compared, %u differ (BC <-> uncompressed, controls), %u depth <-> colour differ\n", compared,
                differ, differ_depth);
    if (vk.validation_errors()) {
        std::printf("%u Vulkan validation errors\n", vk.validation_errors());
        return 1;
    }
    return !mip_tails_ok || differ || differ_depth ? 1 : 0;
}
