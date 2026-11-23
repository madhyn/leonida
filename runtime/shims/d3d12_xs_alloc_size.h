// Console allocation sizes for render targets, for the Xbox-ABI device proxy's slot 25 (GetResourceAllocationInfo).
//
// Why: the retail title checks a fixed memory budget against what its first full-screen render targets used and asserts
// when more than 2 MiB is left over (docs/retail_boot_chain.md, stop 17, #6982). The PC device sizes a 2D render target
// from the Vulkan driver's memory requirements, which is about width x height x element bytes. The console lays a
// render target out in 64 KiB tiles (the RDNA2 SW_64KB layouts), so its allocation is the surface padded to whole
// tiles: for 4-byte elements a tile is 128 x 128 pixels, and 1080 rows take nine tile rows, not 8.4. The PC size is
// smaller by that padding, summed over every full-screen target.
//
// What this is: the tile shapes are the RDNA2 SW_64KB_D shapes of runtime/src/gfx10_tiling.h (Mesa addrlib patterns,
// validated against real textures in docs/textures.md). That a console render target uses these shapes and no extra
// (DCC, CMask) memory in its allocation size is an assumption: the title's own sizes are not measured. The rule only
// ever makes a size larger than the PC device's own answer, never smaller.
#pragma once

#include "../src/gfx10_tiling.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace rt::d3d12_xs {

constexpr UINT64 kConsoleTileBytes = 64 * 1024;  // one SW_64KB tile

// Bytes per element of the colour formats a render target takes, or 0 for any other format (the PC size then stays).
inline std::size_t render_target_element_bytes(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R8_SINT: case DXGI_FORMAT_A8_UNORM:
            return 1;
        case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_B5G6R5_UNORM:
            return 2;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT: case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
            return 4;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT:
            return 8;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT:
            return 16;
        default:
            return 0;
    }
}

// The console's allocation size for a single-sampled 2D render target, or 0 when the rule does not apply (not a 2D
// texture, not a render target, more than one sample, an unlisted format, an empty extent, or a size that overflows).
// Mips whose extent fits in one tile share the tail tile; every other mip is padded to whole tiles. Slices add up.
inline UINT64 console_render_target_size(const D3D12_RESOURCE_DESC& desc) {
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return 0;
    if (!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) return 0;
    if (desc.SampleDesc.Count > 1) return 0;
    const std::size_t element_bytes = render_target_element_bytes(desc.Format);
    if (!element_bytes || !desc.Width || !desc.Height || !desc.DepthOrArraySize) return 0;

    const UINT mips = desc.MipLevels ? desc.MipLevels : 1;
    UINT64 slice = 0;
    bool tail = false;
    for (UINT mip = 0; mip < mips && mip < 32; ++mip) {
        const UINT64 width = std::max<UINT64>(desc.Width >> mip, 1);
        const UINT64 height = std::max<UINT>(desc.Height >> mip, 1);
        leonida::gfx10::TiledSurface2D surface;
        if (width > UINT32_MAX ||
            !leonida::gfx10::TiledSurface2D::make(leonida::gfx10::TileMode::sw_64kb_d, element_bytes,
                                                  static_cast<std::size_t>(width), static_cast<std::size_t>(height),
                                                  surface))
            return 0;
        if (width <= surface.tile_width() && height <= surface.tile_height()) {
            tail = true;
            break;
        }
        slice += surface.tiled_size();
    }
    if (tail) slice += kConsoleTileBytes;
    const UINT64 slices = desc.DepthOrArraySize;
    if (slice > UINT64_MAX / slices) return 0;
    return slice * slices;
}

// What GetResourceAllocationInfo adds to the PC device's answer: for every description with a console size larger than
// the PC device's size for it alone, the difference. pc_size_of(i) is the PC device's size for description i alone.
// A description the PC device refuses (UINT64_MAX) or the rule does not cover adds nothing.
template <class PcSizeOf>
UINT64 console_render_target_extra(const D3D12_RESOURCE_DESC* descs, UINT count, PcSizeOf pc_size_of) {
    UINT64 extra = 0;
    for (UINT i = 0; descs && i < count; ++i) {
        const UINT64 console = console_render_target_size(descs[i]);
        if (!console) continue;
        const UINT64 pc = pc_size_of(i);
        if (pc == UINT64_MAX || pc >= console) continue;
        extra += console - pc;
    }
    return extra;
}

}  // namespace rt::d3d12_xs
