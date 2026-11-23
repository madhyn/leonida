// The PC form of a resource description the retail title builds for the console, for the Xbox-ABI device proxy.
//
// The title's D3D12_RESOURCE_DESC carries console values the PC device cannot take: the layout word is 0x101 (tiled)
// or 0x1 (row-major buffer), and the flags have bits above the PC ones set (docs/retail_boot_chain.md, #6982). Slot 76
// replaces the layout word the same way before it forwards to CreateCommittedResource; slot 25 and the slot 127
// stand-in need the same cleaning. The title's own description is never written: a caller gets a copy.
#pragma once

#include <windows.h>
#include <d3d12.h>

#include <cstdint>

namespace rt::d3d12_xs {

// The D3D12_RESOURCE_FLAGS bits the PC runtime defines; every other bit is a console one.
constexpr std::uint32_t kPcResourceFlagsMask =
    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE |
    D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
    D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY | D3D12_RESOURCE_FLAG_VIDEO_ENCODE_REFERENCE_ONLY |
    D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;

// Not constexpr: 0x101 is outside the unfixed enum's value range, which clang-cl rejects in a constant expression.
inline const auto kXboxTiledResourceLayout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);

// The console allows a depth-stencil texture to allow unordered access too (measured locally at a title's call sites:
// a 2D R32G8X24_TYPELESS texture with ALLOW_DEPTH_STENCIL | ALLOW_UNORDERED_ACCESS, sized through slot 25), but the PC
// runtime refuses that pair, and Vulkan has no depth format with storage support to make it with. Stand-in: such a
// texture is a depth-stencil texture without unordered access. Its unordered-access views write no descriptor (the PC
// layer refuses a UAV of a texture without the flag), so what a shader writes through them goes nowhere and their
// reads see whatever the descriptor held.
inline std::uint32_t without_depth_stencil_uav(D3D12_RESOURCE_DIMENSION dimension, std::uint32_t flags) {
    if (dimension != D3D12_RESOURCE_DIMENSION_BUFFER && (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        flags &= ~static_cast<std::uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    return flags;
}

// The PC flags of a console description: the PC bits only, without unordered access on a depth-stencil texture.
inline std::uint32_t pc_resource_flags(D3D12_RESOURCE_DIMENSION dimension, std::uint32_t xbox_flags) {
    return without_depth_stencil_uav(dimension, xbox_flags & kPcResourceFlagsMask);
}

// Strip Xbox-only flags and translate the Xbox tiled layout. Preserve all other layouts so the host can validate them.
inline D3D12_RESOURCE_DESC pc_resource_desc(const D3D12_RESOURCE_DESC& xbox) {
    D3D12_RESOURCE_DESC pc = xbox;
    if (pc.Layout == kXboxTiledResourceLayout) {
        pc.Layout = pc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                   : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }
    pc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(pc_resource_flags(xbox.Dimension, static_cast<std::uint32_t>(xbox.Flags)));
    return pc;
}

}  // namespace rt::d3d12_xs
