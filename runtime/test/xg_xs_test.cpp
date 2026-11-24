// Checks the registered xg_xs.dll shim (runtime/shims/xg_xs.cpp) and the console texture layout model behind it
// (runtime/src/console_texture_layout.h). Synthetic textures only; no game files.
//
// Both exports are resolved through rt::shims::find, so this is the table the runtime installs. The layout cases and
// their expected numbers are those of the project the model is ported from (Marvisak/VIctory, GPL-3.0,
// crates/victory-formats/src/texture.rs and crates/victory-hle/src/modules/xg.rs tests at d2a4bae1f): hand-checked
// addresses, mip tail placement, a two-plane depth-stencil, and a bijection plus round trip over every element.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/console_texture_layout.h"
#include "shims.h"

namespace {

namespace ct = leonida::console_texture;

using CreateFn = HRESULT(__stdcall*)(const void*, void**);
using SuggestFn = HRESULT(__stdcall*)(const void*, uint32_t, uint32_t*);
using ReleaseFn = ULONG(__stdcall*)(void*);
using LayoutFn = HRESULT(__stdcall*)(void*, void*);
using OffsetFn = uint64_t(__stdcall*)(void*, uint32_t, uint32_t, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t*);
using IntoFn = HRESULT(__stdcall*)(void*, uint8_t*, uint32_t, uint32_t, const uint8_t*, uint32_t, uint32_t);
using FromFn = HRESULT(__stdcall*)(void*, uint8_t*, uint32_t, uint32_t, uint32_t, const uint8_t*, uint32_t, uint32_t);
using TilingFn = HRESULT(__stdcall*)(void*, uint32_t*, void*, uint32_t*, uint32_t*, uint32_t, void*);
constexpr HRESULT kNotImpl = static_cast<HRESULT>(rt::shims::kENotImpl);
constexpr HRESULT kInvalidArg = static_cast<HRESULT>(0x80070057u);
constexpr HRESULT kFail = static_cast<HRESULT>(0x80004005u);

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++g_failures;
    }
}

bool all_bytes(const uint8_t* p, size_t n, uint8_t value) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] != value) return false;
    return true;
}

ct::Desc desc2d(uint32_t w, uint32_t h, uint32_t mips, uint32_t bpe, uint32_t swizzle) {
    ct::Desc d;
    d.width = w;
    d.height = h;
    d.mips = mips;
    d.element_bytes = bpe;
    d.swizzle = swizzle;
    return d;
}

uint64_t offset_of(const ct::Layout& l, uint32_t mip, uint32_t x, uint32_t y, uint32_t z) {
    uint64_t at = ~uint64_t{0};
    return l.offset(mip, x, y, z, at) == ct::Error::ok ? at : ~uint64_t{0};
}

// The D3D12-style description the title passes (0x38 bytes).
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

ResourceDesc rdesc(uint64_t w, uint32_t h, uint16_t mips, uint32_t format, uint32_t layout) {
    ResourceDesc d{};
    d.dimension = 3;
    d.width = w;
    d.height = h;
    d.depth_or_array = 1;
    d.mips = mips;
    d.format = format;
    d.samples = 1;
    d.layout = layout;
    return d;
}

void test_layout_model() {
    // Sizes past the hardware's 16384 texels per side are refused instead of overflowing.
    for (uint32_t s : {ct::kLinear, ct::kSw64kbDX}) {
        const auto largest = ct::Layout::make(desc2d(ct::kMaxExtent, ct::kMaxExtent, 15, 16, s));
        check(largest && largest->size >= uint64_t{ct::kMaxExtent} * ct::kMaxExtent * 16, "largest texture lays out");
        check(!ct::Layout::make(desc2d(ct::kMaxExtent + 1, 1, 1, 16, s)), "width past 16384 is refused");
        check(!ct::Layout::make(desc2d(1, 0xffffffffu, 1, 16, s)), "height past 16384 is refused");
        auto deep = desc2d(1, 1, 1, 16, s);
        deep.depth = 0xffffffffu;
        check(!ct::Layout::make(deep), "depth past 16384 is refused");
    }
    // Blocks hold their size in elements, and the pattern covers exactly the block's coordinates.
    for (uint32_t log2 : {8u, 12u, 16u}) {
        const uint32_t s = log2 == 8 ? ct::kSw256bS : log2 == 12 ? ct::kSw4kbS : ct::kSw64kbD;
        for (uint32_t e = 0; e < 5; ++e) {
            const auto l = ct::Layout::make(desc2d(1, 1, 1, 1u << e, s));
            check(l && (uint64_t{l->block[0]} * l->block[1] * l->block[2] << e) == (uint64_t{1} << log2),
                  "block holds its size in elements");
            check(l && l->pattern()[0].size() == l->block[0] && l->pattern()[1].size() == l->block[1],
                  "pattern covers the block");
        }
    }
    {
        const auto l = ct::Layout::make(desc2d(64, 64, 1, 16, ct::kSw64kbD));
        check(l && l->block[0] == 64 && l->block[1] == 64 && l->block[2] == 1, "64 KiB, 16-byte elements: 64x64");
        check(l && l->size == 65536 && l->alignment == 65536, "64 KiB, 16-byte elements: one block");
    }
    // Hand-checked: in a 64 KiB D block of 4-byte elements, x bits go to address bits 2, 3, 6, 9, 11, 13, 15 and y
    // bits to 4, 5, 7, 8, 10, 12, 14; a 4 KiB S block of 16-byte elements is 16x16 with y first.
    {
        const auto l = ct::Layout::make(desc2d(128, 128, 1, 4, ct::kSw64kbD));
        check(l && offset_of(*l, 0, 1, 0, 0) == (1u << 2), "64KB_D x=1");
        check(l && offset_of(*l, 0, 4, 0, 0) == (1u << 6), "64KB_D x=4");
        check(l && offset_of(*l, 0, 0, 4, 0) == (1u << 7), "64KB_D y=4");
        check(l && offset_of(*l, 0, 0, 8, 0) == (1u << 8), "64KB_D y=8");
        check(l && offset_of(*l, 0, 64, 64, 0) == ((1u << 15) | (1u << 14)), "64KB_D (64,64)");
        const auto s = ct::Layout::make(desc2d(32, 32, 1, 16, ct::kSw4kbS));
        check(s && s->block[0] == 16 && s->block[1] == 16, "4KB_S 16-byte block is 16x16");
        check(s && offset_of(*s, 0, 0, 1, 0) == (1u << 4), "4KB_S y=1");
        check(s && offset_of(*s, 0, 1, 0, 0) == (1u << 6), "4KB_S x=1");
        check(s && offset_of(*s, 0, 16, 0, 0) == 4096, "4KB_S second block");
        check(s && offset_of(*s, 0, 0, 16, 0) == 8192, "4KB_S third block");
    }
    // A 256x256 BC7 chain in 64 KiB D: one tail block holding mips 1-8, then mip 0; mip 1 sits at x = 32 elements,
    // 32 KiB into the tail. 4 KiB blocks hold fewer tail mips.
    {
        const auto l = ct::Layout::make(desc2d(64, 64, 9, 16, ct::kSw64kbD));
        check(l && l->first_tail_mip == 1 && l->size == 2 * 65536 && l->mips[0].offset == 65536, "tail comes first");
        check(l && l->mips[1].tail && l->mips[1].tail->offset == 32768 && l->mips[1].tail->x == 32 &&
                  l->mips[1].tail->y == 0,
              "mip 1 tail position");
        check(l && offset_of(*l, 1, 0, 0, 0) == 32768, "mip 1 element 0 address");
        const auto s = ct::Layout::make(desc2d(64, 64, 9, 16, ct::kSw4kbS));
        check(s && s->first_tail_mip == 3 && s->mips[2].offset == 4096 && s->mips[1].offset == 8192 &&
                  s->mips[0].offset == 8192 + 16384,
              "4 KiB tail holds fewer mips");
    }
    // Depth-stencil is two planes: 4-byte depth in 128x128 blocks, 1-byte stencil in 256x256 blocks.
    {
        ct::TextureDesc t;
        t.width = 1920;
        t.height = 1080;
        t.format = 20;
        t.swizzle = ct::kSw64kbZX;
        const auto tex = ct::Texture::make(t);
        check(tex && tex->planes.size() == 2, "D32_FLOAT_S8X24 has two planes");
        check(tex && tex->planes[0].layout.size == 1920ull * 1152 * 4, "depth plane size");
        check(tex && tex->planes[1].offset == tex->planes[0].layout.size, "stencil follows depth");
        check(tex && tex->planes[1].layout.size == 2048ull * 1280, "stencil plane size");
        check(tex && !tex->planes[0].layout.addressable(), "XOR modes are not addressed");
    }
    // Bad descriptions and buffers are refused.
    {
        check(!ct::Layout::make(desc2d(0, 4, 1, 4, ct::kSw4kbS)), "zero width is refused");
        check(!ct::Layout::make(desc2d(4, 4, 1, 12, ct::kSw4kbS)), "12-byte elements are refused");
        check(!ct::Layout::make(desc2d(4, 4, 1, 4, 3)), "mode 3 is refused");
        const auto l = ct::Layout::make(desc2d(64, 64, 1, 4, ct::kSw4kbS));
        std::vector<uint8_t> out(64 * 64 * 4), small(100);
        check(l && l->detile(small.data(), small.size(), 0, 0, out.data(), out.size(), 256, 0) == ct::Error::too_small,
              "a short tiled buffer is too small");
        uint64_t at = 0;
        check(l && l->offset(0, 64, 0, 0, at) == ct::Error::invalid, "an element outside is invalid");
        const auto lin = ct::Layout::make(desc2d(4, 2, 1, 4, ct::kLinear));
        std::vector<uint8_t> tiled(lin ? lin->size : 0), linear(32);
        check(lin && lin->detile(tiled.data(), tiled.size(), 0, 0, linear.data(), linear.size(), SIZE_MAX, 0) ==
                         ct::Error::too_small,
              "an overflowing row pitch is refused");
        auto vol = desc2d(4, 2, 1, 4, ct::kLinear);
        vol.dimension = ct::Dimension::tex3d;
        vol.depth = 3;
        const auto v = ct::Layout::make(vol);
        std::vector<uint8_t> vt(v ? v->size : 0), vl(96);
        check(v && v->detile(vt.data(), vt.size(), 0, 0, vl.data(), vl.size(), 16, SIZE_MAX) == ct::Error::too_small,
              "an overflowing depth pitch is refused");
    }
}

// Every element of every mip and slice lands on its own address inside the texture, and tiling then detiling gives
// the rows back without touching the padding around them.
void test_bijection_and_round_trip() {
    std::vector<ct::Desc> cases = {
        desc2d(64, 64, 7, 16, ct::kSw64kbD), desc2d(100, 60, 7, 4, ct::kSw64kbD), desc2d(250, 175, 8, 8, ct::kSw4kbS),
        desc2d(33, 17, 6, 1, ct::kSw4kbD),   desc2d(40, 9, 3, 2, ct::kSw64kbS),   desc2d(30, 20, 5, 4, ct::kLinear),
        desc2d(83, 31, 6, 1, ct::kSw256bS),
    };
    auto bc = desc2d(109, 71, 6, 16, ct::kSw256bD);
    bc.block = 4;
    cases.push_back(bc);
    auto array = desc2d(70, 70, 4, 16, ct::kSw4kbS);
    array.depth = 3;
    cases.push_back(array);
    auto volume = desc2d(64, 32, 7, 4, ct::kSw4kbS);
    volume.dimension = ct::Dimension::tex3d;
    volume.depth = 16;
    cases.push_back(volume);
    for (const ct::Desc& d : cases) {
        const auto l = ct::Layout::make(d);
        if (!l) {
            check(false, "round-trip case lays out");
            continue;
        }
        std::vector<bool> seen(static_cast<size_t>(l->size));
        std::vector<uint8_t> tiled(static_cast<size_t>(l->size));
        const size_t n = d.element_bytes;
        const uint32_t slices = d.dimension == ct::Dimension::tex3d ? 1 : d.depth;
        bool ok = true;
        for (uint32_t slice = 0; slice < slices; ++slice) {
            for (uint32_t mip = 0; mip < d.mips; ++mip) {
                const ct::Mip& m = l->mips[mip];
                const size_t active = size_t{m.width} * n, row = active + 13, plane = row * m.height + 11;
                std::vector<uint8_t> linear(plane * m.depth);
                for (size_t i = 0; i < linear.size(); ++i) linear[i] = static_cast<uint8_t>(i * 7 + mip * 13 + slice);
                ok &= l->tile(tiled.data(), tiled.size(), mip, slice, linear.data(), linear.size(), row, plane) ==
                      ct::Error::ok;
                std::vector<uint8_t> back(linear.size(), 0xcc);
                ok &= l->detile(tiled.data(), tiled.size(), mip, slice, back.data(), back.size(), row, plane) ==
                      ct::Error::ok;
                for (uint32_t z = 0; z < m.depth; ++z) {
                    const size_t base = z * plane;
                    for (uint32_t y = 0; y < m.height; ++y) {
                        const size_t start = base + y * row;
                        ok &= std::memcmp(&back[start], &linear[start], active) == 0;
                        ok &= all_bytes(&back[start + active], row - active, 0xcc);
                        for (uint32_t x = 0; x < m.width; ++x) {
                            uint64_t at64 = 0;
                            if (l->offset(mip, x, y, slice + z, at64) != ct::Error::ok) {
                                ok = false;
                                continue;
                            }
                            const size_t at = static_cast<size_t>(at64);
                            const size_t source = start + x * n;
                            if (at + n > seen.size() || at % n != 0 || seen[at]) {
                                ok = false;
                                continue;
                            }
                            ok &= std::memcmp(&tiled[at], &linear[source], n) == 0;
                            seen[at] = true;
                        }
                    }
                    ok &= all_bytes(&back[base + plane - 11], 11, 0xcc);
                }
            }
        }
        char what[96];
        std::snprintf(what, sizeof(what), "bijection and round trip: %ux%ux%u mips %u bpe %u mode %u", d.width,
                      d.height, d.depth, d.mips, d.element_bytes, d.swizzle);
        check(ok, what);
    }
}

void test_shim(CreateFn create, SuggestFn suggest) {
    // XGSuggestSwizzleMode follows the use: 4KB_S, then the XOR render and depth modes.
    uint32_t mode = 0;
    ResourceDesc d = rdesc(256, 256, 1, 98, 0);
    check(suggest(&d, 0, &mode) == S_OK && mode == 0x105, "suggest: a sampled texture gets 4KB_S");
    d.flags = 1;
    check(suggest(&d, 0, &mode) == S_OK && mode == 0x11b, "suggest: a render target gets 64KB_R_X");
    d.flags = 2;
    check(suggest(&d, 0, &mode) == S_OK && mode == 0x118, "suggest: a depth target gets 64KB_Z_X");
    check(suggest(nullptr, 0, &mode) == kInvalidArg, "suggest: a null description is E_INVALIDARG");

    check(create(&d, nullptr) == E_POINTER, "XGCreateTextureComputer(desc, nullptr) is E_POINTER");

    // A description with no layout still hands out a computer (two measured callers use *out without checking).
    void* stub = nullptr;
    const ResourceDesc bad = rdesc(256, 256, 1, 0, 0x10a);
    check(create(&bad, &stub) == kInvalidArg, "an unknown format is E_INVALIDARG");
    check(stub != nullptr, "and still stores a computer for the callers that do not check");
    if (stub) {
        void* const* vt = *static_cast<void* const* const*>(stub);
        std::vector<uint8_t> out(0x1b40, 0xcd);
        check(reinterpret_cast<LayoutFn>(vt[2])(stub, out.data()) == kNotImpl &&
                  all_bytes(out.data(), out.size(), 0xcd),
              "that computer's slot 2 is E_NOTIMPL and writes nothing");
        reinterpret_cast<ReleaseFn>(vt[1])(stub);
        reinterpret_cast<ReleaseFn>(vt[1])(stub);
        check(reinterpret_cast<LayoutFn>(vt[2])(stub, out.data()) == kNotImpl,
              "that computer stays usable after more Releases than it was handed out");
    }

    // Element offsets (slot 6): linear rows of 1 KiB (128 8-byte elements), 0 outside.
    {
        void* c = nullptr;
        const ResourceDesc lin = rdesc(128, 4, 1, 11, 0x100);
        check(create(&lin, &c) == S_OK && c, "a 128x4 R16G16B16A16 linear texture gets a computer");
        if (c) {
            void* const* vt = *static_cast<void* const* const*>(c);
            uint32_t bit = 7;
            check(reinterpret_cast<OffsetFn>(vt[6])(c, 0, 0, 3, 1, 0, 0, &bit) == (128 + 3) * 8 && bit == 0,
                  "slot 6: element (3, 1) of linear rows");
            check(reinterpret_cast<OffsetFn>(vt[6])(c, 0, 0, 128, 0, 0, 0, nullptr) == 0, "slot 6: 0 outside");
            check(reinterpret_cast<ReleaseFn>(vt[1])(c) == 0, "Release frees the computer");
        }
    }

    // The title's calls: the layout, then a copy in and out of each mip. 256x256 BC7, full chain, 64 KiB display.
    void* c = nullptr;
    const ResourceDesc bc7 = rdesc(256, 256, 0, 98, 0x10a);
    check(create(&bc7, &c) == S_OK && c, "a 256x256 BC7 chain gets a computer");
    if (!c) return;
    void* const* vt = *static_cast<void* const* const*>(c);
    std::vector<uint8_t> buf(0x1b40);
    check(reinterpret_cast<LayoutFn>(vt[2])(c, buf.data()) == S_OK, "slot 2 fills the layout");
    const auto u64 = [&](size_t at) { uint64_t v; std::memcpy(&v, &buf[at], 8); return v; };
    const auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, &buf[at], 4); return v; };
    // LayoutOut: size 0x00, alignment 0x08, mips 0x10, planes 0x14, plane[0] at 0x18 (PlaneOut: element_bytes 0x28,
    // standard 0x2c, tail 0x30, mips at 0x38, MipOut 0x70 bytes: offset 0x08, pitch_bytes 0x1c, elements 0x30, in_tail
    // 0x64).
    check(u64(0x00) == 131072 && u64(0x08) == 65536 && u32(0x10) == 9 && u32(0x14) == 1, "layout totals");
    const size_t plane = 0x18;
    check(u32(plane + 0x2c) == 1 && u32(plane + 0x30) == 8 && u32(plane + 0x28) == 16, "plane 0 mips and elements");
    const size_t mip0 = plane + 0x38, mip1 = mip0 + 0x70;
    check(u64(mip0 + 0x08) == 65536 && u32(mip0 + 0x1c) == 1024, "mip 0 offset and pitch");
    check(u64(mip1 + 0x08) == 32768 && u32(mip1 + 0x64) == 1, "mip 1 sits in the tail");

    std::vector<uint8_t> tiled(131072);
    bool copies = true;
    for (uint32_t mip = 0; mip < 9; ++mip) {
        const size_t m = mip0 + mip * 0x70;
        const uint32_t w = u32(m + 0x30), h = u32(m + 0x34), row = w * 16;
        std::vector<uint8_t> rows(size_t{row} * h);
        for (size_t i = 0; i < rows.size(); ++i) rows[i] = static_cast<uint8_t>(i ^ mip);
        copies &= reinterpret_cast<IntoFn>(vt[8])(c, tiled.data(), 0, mip, rows.data(), row, 0) == S_OK;
        std::vector<uint8_t> back(rows.size());
        copies &= reinterpret_cast<FromFn>(vt[9])(c, back.data(), 0, mip, 0, tiled.data(), row, 0) == S_OK;
        copies &= back == rows;
    }
    check(copies, "slots 8 and 9 copy every mip in and out");

    // Slot 10: one tile for the tail, one for mip 0.
    uint32_t tiles = 0, shape[3] = {}, count = 4;
    uint8_t packed[12] = {};
    uint8_t subs[4 * 12] = {};
    check(reinterpret_cast<TilingFn>(vt[10])(c, &tiles, packed, shape, &count, 0, subs) == S_OK, "slot 10 succeeds");
    uint32_t packed_tiles = 0, first_start = 0;
    std::memcpy(&packed_tiles, packed + 4, 4);
    std::memcpy(&first_start, subs + 8, 4);
    check(tiles == 2 && packed[0] == 1 && packed[1] == 8 && packed_tiles == 1, "slot 10 tiles and packed mips");
    check(shape[0] == 256 && shape[1] == 256 && shape[2] == 1 && count == 1 && first_start == 1,
          "slot 10 shape and mip 0's tiling");
    check(reinterpret_cast<ReleaseFn>(vt[1])(c) == 0, "Release frees the BC7 computer");

    // 4 KiB modes have no 64 KiB tiling.
    void* c4 = nullptr;
    const ResourceDesc four = rdesc(256, 256, 0, 98, 0x105);
    check(create(&four, &c4) == S_OK && c4, "a 4KB_S chain gets a computer");
    if (c4) {
        void* const* v4 = *static_cast<void* const* const*>(c4);
        check(reinterpret_cast<TilingFn>(v4[10])(c4, &tiles, nullptr, nullptr, nullptr, 0, nullptr) == kFail,
              "slot 10 of a 4 KiB mode is E_FAIL");
        check(reinterpret_cast<LayoutFn>(v4[31])(c4, buf.data()) == kNotImpl, "the last slot (31) is E_NOTIMPL");
        reinterpret_cast<ReleaseFn>(v4[1])(c4);
    }
}

}  // namespace

int main() {
    auto create = reinterpret_cast<CreateFn>(rt::shims::find("xg_xs.dll", "XGCreateTextureComputer"));
    auto suggest = reinterpret_cast<SuggestFn>(rt::shims::find("xg_xs.dll", "XGSuggestSwizzleMode"));
    check(create != nullptr, "XGCreateTextureComputer is registered");
    check(suggest != nullptr, "XGSuggestSwizzleMode is registered");
    check(rt::shims::owns_dll("xg_xs.dll"), "the registry owns xg_xs.dll");
    if (!create || !suggest) return 1;

    test_layout_model();
    test_bijection_and_round_trip();
    test_shim(create, suggest);

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
