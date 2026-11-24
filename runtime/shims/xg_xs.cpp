// xg_xs.dll: the console's texture layout library. Titles ask it which swizzle mode a texture should use, how large it
// is, where each mip and slice lies, and to copy linear data into tiled memory and back. The game executable
// (1.0.2.1) imports two functions from it.
//
// Measured from the call sites in the executable, read statically (shapes and checks only):
//   HRESULT XGSuggestSwizzleMode(const desc*, 0, uint32_t* mode)
//     2 call sites, both in the game's helper that fills a texture description before XGCreateTextureComputer. The
//     out pointer is a 32-bit field of that description, which the helper first sets to 0 and, when the mode is
//     known, writes itself as 0x100 | mode. The HRESULT is only passed back up; no caller branches on it.
//   HRESULT XGCreateTextureComputer(const desc*, computer** out)
//     4 call sites. Two test the HRESULT and take their failure path when it is negative; two ignore it and call
//     through *out at once. On the computer the callers use vtable slot 1 after they are done (3 sites), slot 2 with
//     one out pointer to a large stack structure (2 sites), slot 8 with six arguments in a loop over subresources
//     (1 site) and slot 10 with seven arguments (1 site).
//
// The computer's methods, their arguments and the layout structure slot 2 fills are taken from Marvisak/VIctory
// (GPL-3.0), crates/victory-hle/src/modules/xg.rs at commit d2a4bae1f, which serves the same title: 0 AddRef,
// 1 Release, 2 the layout, 6 an element's byte offset, 8 a copy into and 9 a copy out of a subresource, 10 the 64 KiB
// tiling. They match the slots measured above. The layouts come from runtime/src/console_texture_layout.h, ported
// from the same project.
//
// Kept from the measured callers: a description this shim cannot lay out still gets a computer in *out (with a
// negative HRESULT), because two callers use *out without checking the HRESULT. That computer answers every slot
// with E_NOTIMPL and writes nothing. COM says *out is null on failure; this breaks that on purpose.
//
// Not modelled: compression metadata planes (HTILE, CMASK, FMASK, DCC), addressing and copies for the pipe/bank XOR
// modes (_X, _T) and MSAA (they need the GPU's pipe configuration; the host renders those targets), and the slots no
// caller reaches (size, alignment, mip offset, element coordinate, view descriptor), which log once and return
// E_NOTIMPL.
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <utility>

#include "../src/console_texture_layout.h"
#include "shims.h"

namespace {

namespace ct = leonida::console_texture;

constexpr HRESULT kNotImpl = static_cast<HRESULT>(rt::shims::kENotImpl);
constexpr HRESULT kInvalidArg = static_cast<HRESULT>(0x80070057u);
constexpr HRESULT kFail = static_cast<HRESULT>(0x80004005u);
constexpr size_t kSlots = 32;

// D3D12_RESOURCE_DESC, whose layout field carries 0x100 | swizzle mode on the console (1 row major, 0 "pick one").
struct ResourceDesc {
    uint32_t dimension;
    uint64_t alignment;
    uint64_t width;
    uint32_t height;
    uint16_t depth_or_array;
    uint16_t mips;
    uint32_t format;
    uint32_t samples;
    uint32_t quality;
    uint32_t layout;
    uint32_t flags;
};
static_assert(sizeof(ResourceDesc) == 0x38);

constexpr uint32_t kSwizzled = 0x100;
constexpr uint32_t kRowMajor = 1;
constexpr uint32_t kAllowRenderTarget = 1;
constexpr uint32_t kAllowDepthStencil = 2;
constexpr uint32_t kTexture1d = 2, kTexture2d = 3, kTexture3d = 4;

// The layout structure slot 2 fills. Offsets 0x00, 0x08, 0x10 and 0x1c of a mip are the ones the title reads; the
// rest follow the library's older public shape (VIctory's reading).
constexpr size_t kMips = 15;
struct MipOut {
    uint64_t size;
    uint64_t offset;
    uint64_t slice_size;
    uint32_t pitch_texels;
    uint32_t pitch_bytes;
    uint32_t alignment;
    uint32_t padded[3];  // width, height in elements, depth or array size
    uint32_t elements[2];
    uint32_t texels[2];
    uint32_t depth_or_array;
    uint32_t samples;
    uint32_t swizzle;
    uint64_t bank_rotation[2];
    uint32_t slice_depth;
    uint32_t in_tail;
    uint32_t tail_position;  // where a tail mip starts in elements: x bits 0-10, y 11-21
};
struct PlaneOut {
    uint32_t usage;  // 1 data, 7 depth, 8 stencil (metadata planes are not made)
    uint64_t size;
    uint64_t slice_size;
    uint64_t offset;
    uint64_t alignment;
    uint32_t element_bytes;
    uint32_t standard_mips;
    uint32_t tail_mips;
    MipOut mips[kMips];
};
struct LayoutOut {
    uint64_t size;
    uint64_t alignment;
    uint32_t mips;
    uint32_t planes;
    PlaneOut plane[4];
    uint32_t dimension;
};
static_assert(sizeof(MipOut) == 0x70);
static_assert(sizeof(PlaneOut) == 0x6c8);
static_assert(sizeof(LayoutOut) == 0x1b40);

struct PackedMips {
    uint8_t standard;
    uint8_t packed;
    uint32_t tiles;
    uint32_t start;
};
struct SubresourceTiling {
    uint32_t width;
    uint16_t height;
    uint16_t depth;
    uint32_t start;
};
constexpr uint64_t kTile = 64u << 10;

// Prints once per call site kind; `slot` is the vtable index for a computer method, or -1 for an export.
void warn_once(std::atomic<bool>& warned, const char* what, int slot, const void* caller, const char* why) {
    if (warned.exchange(true, std::memory_order_relaxed)) return;
    char name[64];
    if (slot < 0) std::snprintf(name, sizeof(name), "%s", what);
    else std::snprintf(name, sizeof(name), "%s%d", what, slot);
    std::fprintf(stderr, "[rt] xg_xs: %s: %s (caller %s, logged once)\n", name, why,
                 rt::shims::module_offset(reinterpret_cast<uint64_t>(caller)).c_str());
}

// The mode the console's library picks for a description: linear for 1D, the XOR render and depth modes for targets,
// else 4 KiB standard. VIctory's reading of the library's older optimal-mode function (any size, 2D, 3D and cubes
// alike); revisit if streamed textures come out scrambled.
uint32_t suggested(const ResourceDesc& d) {
    if (d.dimension == kTexture1d) return ct::kLinear;
    if (d.flags & kAllowDepthStencil) return ct::kSw64kbZX;
    if (d.flags & kAllowRenderTarget) return ct::kSw64kbRX;
    return ct::kSw4kbS;
}

std::optional<ct::Texture> texture_of(const ResourceDesc& d) {
    ct::TextureDesc t;
    switch (d.dimension) {
    case kTexture1d: t.dimension = ct::Dimension::tex1d; break;
    case kTexture2d: t.dimension = ct::Dimension::tex2d; break;
    case kTexture3d: t.dimension = ct::Dimension::tex3d; break;
    default: return std::nullopt;
    }
    if (d.layout == 0) t.swizzle = suggested(d);
    else if (d.layout == kRowMajor) t.swizzle = ct::kLinear;
    else if ((d.layout & ~0xffu) == kSwizzled) t.swizzle = d.layout & 0xffu;
    else return std::nullopt;
    if (d.width > std::numeric_limits<uint32_t>::max()) return std::nullopt;
    t.width = static_cast<uint32_t>(d.width);
    t.height = d.height;
    t.depth_or_array = d.depth_or_array;
    t.mips = d.mips;
    t.format = d.format;
    t.samples = d.samples ? d.samples : 1;
    auto texture = ct::Texture::make(t);
    // The layout structure has room for 15 mips.
    if (!texture || texture->planes[0].layout.mips.size() > kMips) return std::nullopt;
    return texture;
}

// ---- The computer that cannot lay its description out: every slot E_NOTIMPL, nothing written. ----

ULONG __stdcall stub_add_ref(void*) { return 1; }
ULONG __stdcall stub_release(void*) { return 1; }

template <size_t Slot>
HRESULT __stdcall stub_slot(void*) {
    static std::atomic<bool> warned{false};
    warn_once(warned, "texture computer slot ", static_cast<int>(Slot), _ReturnAddress(),
              "no layout for this description; E_NOTIMPL");
    return kNotImpl;
}

template <size_t... Slot>
constexpr auto make_stub_vtable(std::index_sequence<Slot...>) {
    return std::array<void*, sizeof...(Slot)>{
        (Slot == 0   ? reinterpret_cast<void*>(&stub_add_ref)
         : Slot == 1 ? reinterpret_cast<void*>(&stub_release)
                     : reinterpret_cast<void*>(&stub_slot<Slot>))...};
}
const auto kStubVtable = make_stub_vtable(std::make_index_sequence<kSlots>{});
struct StubComputer {
    const void* const* vtable;
};
StubComputer g_stub{kStubVtable.data()};

// ---- The computer with a layout. ----

struct Computer {
    const void* const* vtable;
    std::atomic<ULONG> refs{1};
    ResourceDesc desc{};
    uint32_t mips = 0;
    ct::Texture texture;
};

ULONG __stdcall add_ref(Computer* c) { return c->refs.fetch_add(1, std::memory_order_relaxed) + 1; }

ULONG __stdcall release(Computer* c) {
    const ULONG left = c->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) delete c;
    return left;
}

// The mode a tail mip is reported in: the 256-byte one of its kind (3 for the render kind, which GFX10 lacks), the
// depth mode itself.
uint32_t tail_swizzle(uint32_t s) {
    switch (s) {
    case 2: case 6: case 10: case 18: case 22: case 26: return ct::kSw256bD;
    case 23: case 27: return 3;
    case 24: return s;
    default: return ct::kSw256bS;
    }
}

MipOut mip_out(const ResourceDesc& desc, const ct::Layout& l, size_t i) {
    const ct::Mip& m = l.mips[i];
    const uint64_t bpe = l.desc.element_bytes;
    const uint32_t block = l.desc.block;
    const bool three_d = desc.dimension == kTexture3d;
    const uint32_t array = desc.depth_or_array ? desc.depth_or_array : 1u;
    MipOut out{};
    out.pitch_texels = m.pitch * block;
    out.pitch_bytes = m.pitch * l.desc.element_bytes;
    out.padded[0] = m.pitch;
    out.padded[1] = m.rows;
    out.padded[2] = three_d ? m.depth_pitch : array;
    out.elements[0] = m.width;
    out.elements[1] = m.height;
    const uint64_t w = desc.width >> i;
    out.texels[0] = w ? static_cast<uint32_t>(w) : 1u;
    out.texels[1] = (desc.height >> i) ? (desc.height >> i) : 1u;
    out.depth_or_array = three_d ? m.depth : array;
    out.samples = l.desc.samples;
    out.slice_depth = l.block[2];
    const uint64_t slice = uint64_t{m.pitch} * m.rows * bpe;
    const uint64_t depth = three_d ? m.depth_pitch : 1u;
    if (!m.tail) {
        out.offset = m.offset;
        out.slice_size = slice * l.desc.samples;
        out.size = out.slice_size * depth;
        out.alignment = static_cast<uint32_t>(l.alignment);
        out.swizzle = l.desc.swizzle;
    } else {
        out.in_tail = 1;
        out.alignment = 256;
        out.swizzle = tail_swizzle(l.desc.swizzle);
        if (l.thick) {  // a 3D tail mip is placed by position, not by offset
            out.slice_size = l.alignment / l.block[2];
            out.tail_position = m.tail->x | (m.tail->y << 11);
        } else {
            out.offset = m.tail->offset;
            out.slice_size = slice;
        }
        out.size = out.slice_size * depth;
    }
    return out;
}

HRESULT __stdcall layout(Computer* c, LayoutOut* out) {
    if (!out) return kInvalidArg;
    std::memset(out, 0, sizeof(*out));
    out->size = c->texture.size;
    out->alignment = c->texture.alignment;
    out->mips = c->mips;
    out->planes = static_cast<uint32_t>(c->texture.planes.size());
    out->dimension = c->desc.dimension;
    // The library calls a depth plane "depth" only beside stencil or without mips.
    const bool depth = (c->desc.flags & kAllowDepthStencil) && (c->texture.planes.size() > 1 || c->mips == 1);
    for (size_t p = 0; p < c->texture.planes.size() && p < 4; ++p) {
        const ct::Plane& src = c->texture.planes[p];
        const ct::Layout& l = src.layout;
        PlaneOut& plane = out->plane[p];
        plane.usage = src.stencil ? 8u : depth ? 7u : 1u;
        plane.size = l.size;
        plane.slice_size = l.slice_size;
        plane.offset = src.offset;
        plane.alignment = l.alignment;
        plane.element_bytes = l.desc.element_bytes;
        plane.standard_mips = l.first_tail_mip;
        plane.tail_mips = c->mips - l.first_tail_mip;
        for (size_t i = 0; i < l.mips.size() && i < kMips; ++i) plane.mips[i] = mip_out(c->desc, l, i);
    }
    return S_OK;
}

// The plane, its mip and array slice for a subresource index, and the tiled bytes and the linear bytes a copy touches.
struct CopyArgs {
    const ct::Plane* plane;
    uint32_t mip, slice;
    size_t tiled, linear;
};

std::optional<CopyArgs> copy_args(const Computer* c, uint32_t plane, uint32_t subresource, uint32_t row,
                                  uint32_t depth) {
    if (plane >= c->texture.planes.size() || c->mips == 0) return std::nullopt;
    const ct::Plane& p = c->texture.planes[plane];
    const ct::Layout& l = p.layout;
    const uint32_t mip = subresource % c->mips, slice = subresource / c->mips;
    if (mip >= l.mips.size()) return std::nullopt;
    const ct::Mip& m = l.mips[mip];
    const uint64_t bytes = uint64_t{m.width} * l.desc.element_bytes;
    const uint64_t depths = l.subresource_depth(mip);
    // Every factor is at most 2^32, so each product fits in 64 bits; the sum is checked against size_t below.
    const uint64_t linear = (depths - 1) * depth + uint64_t{m.height - 1} * row + bytes;
    const uint64_t tiled = p.offset + l.size;
    if (linear > std::numeric_limits<size_t>::max() || tiled > std::numeric_limits<size_t>::max())
        return std::nullopt;
    return CopyArgs{&p, mip, slice, static_cast<size_t>(tiled), static_cast<size_t>(linear)};
}

HRESULT result(ct::Error e) {
    switch (e) {
    case ct::Error::ok: return S_OK;
    case ct::Error::unsupported: return kNotImpl;
    default: return kInvalidArg;
    }
}

void warn_copy(const Computer* c, const char* what, uint32_t subresource, ct::Error e) {
    static std::atomic<int> warned{0};
    if (warned.fetch_add(1, std::memory_order_relaxed) >= 8) return;
    std::fprintf(stderr,
                 "[rt] xg_xs: %s subresource %u failed (%s): dim=%u %llux%u depth/array=%u mips=%u format=%u "
                 "layout=0x%x\n",
                 what, subresource,
                 e == ct::Error::unsupported ? "addressing not modelled" : e == ct::Error::too_small ? "too small"
                                                                                                     : "invalid",
                 c->desc.dimension, static_cast<unsigned long long>(c->desc.width), c->desc.height,
                 c->desc.depth_or_array, c->desc.mips, c->desc.format, c->desc.layout);
}

// (tiled base, plane, subresource, linear, row pitch, depth pitch).
HRESULT __stdcall copy_into(Computer* c, uint8_t* tiled, uint32_t plane, uint32_t subresource, const uint8_t* linear,
                            uint32_t row, uint32_t depth) {
    const auto a = copy_args(c, plane, subresource, row, depth);
    if (!a || !tiled || !linear) return kInvalidArg;
    const size_t offset = static_cast<size_t>(a->plane->offset);
    const ct::Error e = a->plane->layout.tile(tiled + offset, a->tiled - offset, a->mip, a->slice, linear, a->linear,
                                              row, depth);
    if (e != ct::Error::ok) warn_copy(c, "copy into", subresource, e);
    return result(e);
}

// (linear, plane, subresource, sample, tiled base, row pitch, depth pitch).
HRESULT __stdcall copy_from(Computer* c, uint8_t* linear, uint32_t plane, uint32_t subresource, uint32_t /*sample*/,
                            const uint8_t* tiled, uint32_t row, uint32_t depth) {
    const auto a = copy_args(c, plane, subresource, row, depth);
    if (!a || !tiled || !linear) return kInvalidArg;
    const size_t offset = static_cast<size_t>(a->plane->offset);
    const ct::Error e = a->plane->layout.detile(tiled + offset, a->tiled - offset, a->mip, a->slice, linear, a->linear,
                                                row, depth);
    if (e != ct::Error::ok) warn_copy(c, "copy out of", subresource, e);
    return result(e);
}

// (total tiles, packed mips, tile shape in texels, in: room / out: count, first, subresource tilings), in 64 KiB
// tiles of plane 0, for 2D 64 KiB modes only (others fail, as on the console). The mip tail is the packed part, tile 0
// of each slice; tilings list the standard mips of every slice in turn. With a tail the console's library lists two
// slices at most and zeroes all but the first (VIctory's reading; the title reads slice 0 only).
HRESULT __stdcall tiling(Computer* c, uint32_t* tiles, PackedMips* packed, uint32_t* shape, uint32_t* count,
                         uint32_t first, SubresourceTiling* out) {
    const ct::Layout& l = c->texture.planes[0].layout;
    if (l.alignment != kTile) return kFail;
    if (l.thick) return kNotImpl;
    const uint32_t standard = l.first_tail_mip;
    const bool tail = standard < c->mips;
    const uint64_t tail_tiles = standard == 0 ? l.slice_size / kTile
                                : tail        ? l.mips[standard - 1].offset / kTile
                                              : 0;
    const uint32_t block = l.desc.block;
    if (tiles) *tiles = static_cast<uint32_t>(l.size / kTile);
    if (packed) {
        packed->standard = static_cast<uint8_t>(standard);
        packed->packed = static_cast<uint8_t>(c->mips - standard);
        packed->tiles = static_cast<uint32_t>(tail_tiles);
        packed->start = 0;
    }
    if (shape) {
        shape[0] = l.block[0] * block;
        shape[1] = l.block[1] * block;
        shape[2] = l.block[2];
    }
    if (count) {
        const uint32_t slices = tail ? std::min(l.slices, 2u) : l.slices;
        const uint64_t total = uint64_t{standard} * slices;
        const uint32_t wanted =
            static_cast<uint32_t>(std::min<uint64_t>(total > first ? total - first : 0, *count));
        for (uint32_t i = 0; i < wanted && out; ++i) {
            const uint32_t slice = (first + i) / standard, mip = (first + i) % standard;
            const ct::Mip& m = l.mips[mip];
            SubresourceTiling entry{};
            if (!(tail && slice > 0)) {
                entry.width = m.pitch / l.block[0];
                entry.height = static_cast<uint16_t>(m.rows / l.block[1]);
                entry.depth = 1;
                entry.start = static_cast<uint32_t>((uint64_t{slice} * l.slice_size + m.offset) / kTile);
            }
            out[i] = entry;
        }
        *count = out ? wanted : 0;
    }
    return S_OK;
}

// (plane, mip, x, y, z or slice, sample, bit position): an element's byte offset from the texture's start, 0 if
// outside. VIctory measured the title reading texels of a 128x4 4KB_S texture on the CPU through it.
uint64_t __stdcall element_offset(Computer* c, uint32_t plane, uint32_t mip, uint64_t x, uint32_t y, uint32_t z,
                                  uint32_t /*sample*/, uint32_t* bit) {
    if (bit) *bit = 0;
    if (plane >= c->texture.planes.size()) return 0;
    const ct::Plane& p = c->texture.planes[plane];
    const uint32_t x32 = x > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max()
                                                                  : static_cast<uint32_t>(x);
    uint64_t at = 0;
    if (p.layout.offset(mip, x32, y, z, at) != ct::Error::ok) return 0;
    return p.offset + at;
}

template <size_t Slot>
HRESULT __stdcall unused_slot(Computer*) {
    static std::atomic<bool> warned{false};
    warn_once(warned, "texture computer slot ", static_cast<int>(Slot), _ReturnAddress(),
              "method not implemented; E_NOTIMPL");
    return kNotImpl;
}

template <size_t... Slot>
constexpr auto make_vtable(std::index_sequence<Slot...>) {
    return std::array<void*, sizeof...(Slot)>{
        (Slot == 0    ? reinterpret_cast<void*>(&add_ref)
         : Slot == 1  ? reinterpret_cast<void*>(&release)
         : Slot == 2  ? reinterpret_cast<void*>(&layout)
         : Slot == 6  ? reinterpret_cast<void*>(&element_offset)
         : Slot == 8  ? reinterpret_cast<void*>(&copy_into)
         : Slot == 9  ? reinterpret_cast<void*>(&copy_from)
         : Slot == 10 ? reinterpret_cast<void*>(&tiling)
                      : reinterpret_cast<void*>(&unused_slot<Slot>))...};
}
const auto kVtable = make_vtable(std::make_index_sequence<kSlots>{});

HRESULT __stdcall XGCreateTextureComputer(const ResourceDesc* desc, void** out) {
    if (!out) return E_POINTER;
    std::optional<ct::Texture> texture;
    if (desc) texture = texture_of(*desc);
    if (!texture) {
        static std::atomic<int> warned{0};
        if (warned.fetch_add(1, std::memory_order_relaxed) < 8) {
            if (desc)
                std::fprintf(stderr,
                             "[rt] xg_xs: XGCreateTextureComputer: no layout for dim=%u %llux%u depth/array=%u "
                             "mips=%u format=%u samples=%u layout=0x%x flags=0x%x (caller %s)\n",
                             desc->dimension, static_cast<unsigned long long>(desc->width), desc->height,
                             desc->depth_or_array, desc->mips, desc->format, desc->samples, desc->layout,
                             desc->flags,
                             rt::shims::module_offset(reinterpret_cast<uint64_t>(_ReturnAddress())).c_str());
            else
                std::fprintf(stderr, "[rt] xg_xs: XGCreateTextureComputer: null description\n");
        }
        *out = &g_stub;  // see the top of the file: two measured callers use *out without checking the HRESULT
        return kInvalidArg;
    }
    auto* c = new (std::nothrow) Computer;
    if (!c) {
        *out = &g_stub;
        return E_OUTOFMEMORY;
    }
    c->vtable = kVtable.data();
    c->desc = *desc;
    c->mips = static_cast<uint32_t>(texture->planes[0].layout.mips.size());
    c->texture = std::move(*texture);
    *out = c;
    return S_OK;
}

// (desc, flags, out layout); the title passes flags 0 and puts the result in the description's layout field.
HRESULT __stdcall XGSuggestSwizzleMode(const ResourceDesc* desc, uint32_t /*flags*/, uint32_t* mode) {
    if (!desc || !mode) return kInvalidArg;
    *mode = kSwizzled | suggested(*desc);
    return S_OK;
}

}  // namespace

SHIM_TABLE(xg_xs,
    {"xg_xs.dll", "XGCreateTextureComputer", reinterpret_cast<void*>(&XGCreateTextureComputer)},
    {"xg_xs.dll", "XGSuggestSwizzleMode", reinterpret_cast<void*>(&XGSuggestSwizzleMode)},
);
