// CPU-only unit test for three runtime/src/vulkan_replay.h fixes:
//   1. zero_clear_unavailable() checks Draw / IndirectRasterDraw color and depth/stencil attachments
//      (including coupled depth/stencil planes) so unset vkCmdClear*Image refuses the list instead of
//      dereferencing a null function pointer in detail::initialize().
//   2. AtomicCopyBuffer preparation enforces UINT64_MAX offset/size overflow checks and same-VkBuffer
//      aliasing self-overlap refusal (VUID-vkCmdCopyBuffer-pRegions-00117), matching CopyBuffer.
//   3. zero_image_aspect() requests align - 1 alignment headroom from copy_scratch so small/1x1 8B/16B
//      formats (BC1/BC7/RGBA32F) still zero-initialize when copy_scratch returns a 4-byte-aligned offset.
#include "../src/vulkan_replay.h"

#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

#define EXPECT(condition)                                                  \
    do {                                                                   \
        if (!(condition)) {                                                \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
            return 1;                                                      \
        }                                                                  \
    } while (0)

namespace {

using namespace leonida;

unsigned g_color_clears = 0;
unsigned g_depth_clears = 0;
unsigned g_draws = 0;
unsigned g_fills = 0;
unsigned g_copies = 0;
VkDeviceSize g_last_fill_offset = 0;
VkDeviceSize g_last_fill_size = 0;
VkDeviceSize g_last_copy_offset = 0;

VKAPI_ATTR void VKAPI_CALL mock_barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
                                        std::uint32_t, const VkMemoryBarrier*, std::uint32_t,
                                        const VkBufferMemoryBarrier*, std::uint32_t, const VkImageMemoryBarrier*) {}
VKAPI_ATTR void VKAPI_CALL mock_clear_color(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*,
                                            std::uint32_t, const VkImageSubresourceRange*) {
    ++g_color_clears;
}
VKAPI_ATTR void VKAPI_CALL mock_clear_depth(VkCommandBuffer, VkImage, VkImageLayout, const VkClearDepthStencilValue*,
                                            std::uint32_t, const VkImageSubresourceRange*) {
    ++g_depth_clears;
}
VKAPI_ATTR void VKAPI_CALL mock_begin_rendering(VkCommandBuffer, const VkRenderingInfo*) {}
VKAPI_ATTR void VKAPI_CALL mock_end_rendering(VkCommandBuffer) {}
VKAPI_ATTR void VKAPI_CALL mock_bind_pipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
VKAPI_ATTR void VKAPI_CALL mock_push_constants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, std::uint32_t,
                                               std::uint32_t, const void*) {}
VKAPI_ATTR void VKAPI_CALL mock_set_viewport(VkCommandBuffer, std::uint32_t, std::uint32_t, const VkViewport*) {}
VKAPI_ATTR void VKAPI_CALL mock_set_scissor(VkCommandBuffer, std::uint32_t, std::uint32_t, const VkRect2D*) {}
VKAPI_ATTR void VKAPI_CALL mock_bind_vb2(VkCommandBuffer, std::uint32_t, std::uint32_t, const VkBuffer*,
                                         const VkDeviceSize*, const VkDeviceSize*, const VkDeviceSize*) {}
VKAPI_ATTR void VKAPI_CALL mock_bind_ib(VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType) {}
VKAPI_ATTR void VKAPI_CALL mock_set_topology(VkCommandBuffer, VkPrimitiveTopology) {}
VKAPI_ATTR void VKAPI_CALL mock_set_restart(VkCommandBuffer, VkBool32) {}
VKAPI_ATTR void VKAPI_CALL mock_set_stencil_ref(VkCommandBuffer, VkStencilFaceFlags, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL mock_set_blend(VkCommandBuffer, const float[4]) {}
VKAPI_ATTR void VKAPI_CALL mock_draw(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {
    ++g_draws;
}
VKAPI_ATTR void VKAPI_CALL mock_draw_indexed(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t,
                                             std::int32_t, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL mock_copy_buffer(VkCommandBuffer, VkBuffer, VkBuffer, std::uint32_t, const VkBufferCopy*) {}
VKAPI_ATTR void VKAPI_CALL mock_fill_buffer(VkCommandBuffer, VkBuffer, VkDeviceSize offset, VkDeviceSize size,
                                            std::uint32_t) {
    ++g_fills;
    g_last_fill_offset = offset;
    g_last_fill_size = size;
}
VKAPI_ATTR void VKAPI_CALL mock_copy_buffer_to_image(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, std::uint32_t,
                                                     const VkBufferImageCopy* regions) {
    ++g_copies;
    g_last_copy_offset = regions[0].bufferOffset;
}

ResourceBinding make_image(std::uintptr_t handle, VkFormat format, VkImageAspectFlags aspects, std::uint32_t w = 4,
                           std::uint32_t h = 4) {
    ResourceBinding b{};
    b.image = reinterpret_cast<VkImage>(handle);
    b.format = format;
    b.aspects = aspects;
    b.width = w;
    b.height = h;
    b.depth = b.mip_levels = b.array_layers = b.samples = 1;
    b.color_attachment = (aspects == VK_IMAGE_ASPECT_COLOR_BIT);
    return b;
}

ResourceBinding make_buffer(std::uintptr_t handle, VkDeviceSize offset, VkDeviceSize size) {
    ResourceBinding b{};
    b.buffer = reinterpret_cast<VkBuffer>(handle);
    b.buffer_offset = offset;
    b.size = size;
    return b;
}

ReplayFunctions make_draw_functions(const GraphicsPipeline& pipeline) {
    ReplayFunctions vk{};
    vk.pipeline_barrier = mock_barrier;
    vk.begin_rendering = mock_begin_rendering;
    vk.end_rendering = mock_end_rendering;
    vk.bind_pipeline = mock_bind_pipeline;
    vk.push_constants = mock_push_constants;
    vk.set_viewport = mock_set_viewport;
    vk.set_scissor = mock_set_scissor;
    vk.bind_vertex_buffers2 = mock_bind_vb2;
    vk.bind_index_buffer = mock_bind_ib;
    vk.set_primitive_topology = mock_set_topology;
    vk.set_primitive_restart_enable = mock_set_restart;
    vk.set_stencil_reference = mock_set_stencil_ref;
    vk.set_blend_constants = mock_set_blend;
    vk.draw = mock_draw;
    vk.draw_indexed = mock_draw_indexed;
    vk.buffer_at = [](std::uint64_t, std::uint64_t&, VkBuffer&, VkDeviceSize&) { return false; };
    vk.image_view = [](const auto&, const auto&) { return reinterpret_cast<VkImageView>(std::uintptr_t{1}); };
    vk.graphics_pipeline = [&pipeline](std::uintptr_t) { return &pipeline; };
    return vk;
}

int test_draw_zero_clear_unavailable_color_and_depth() {
    GraphicsPipeline pipe{};
    pipe.pipeline = reinterpret_cast<VkPipeline>(std::uintptr_t{1});
    pipe.layout = reinterpret_cast<VkPipelineLayout>(std::uintptr_t{1});
    pipe.push_stages = VK_SHADER_STAGE_VERTEX_BIT;
    pipe.color_formats = {VK_FORMAT_R8G8B8A8_UNORM};
    pipe.depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    pipe.depth_aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

    const ResourceBinding color = make_image(1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    const ResourceBinding depth = make_image(2, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                             VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    std::unordered_map<ResourceId, ResourceBinding> res{{1, color}, {2, depth}};
    auto resolve = [&](ResourceId id) -> const ResourceBinding* {
        auto it = res.find(id);
        return it == res.end() ? nullptr : &it->second;
    };

    auto views = std::make_shared<DrawViews>();
    views->render_target_count = 1;
    views->render_targets[0] = {1, 0, 0, 1};

    Draw draw{};
    draw.pipeline = 1;
    draw.views = views;
    draw.depth = {2, 0, 0, 1};
    draw.topology = 4;  // TRIANGLELIST
    draw.viewport = {0.0f, 0.0f, 4.0f, 4.0f, 0.0f, 1.0f};
    draw.scissor = {0, 0, 4, 4};
    draw.count = 3;
    draw.instances = 1;

    CommandList list;
    EXPECT(list.draw(draw) && list.close());

    // 1. UNDEFINED color attachment with clear_color_image == nullptr must return unsupported (not crash!).
    {
        LayoutTracker tracker;
        tracker.track(1, color, VK_IMAGE_LAYOUT_UNDEFINED);
        tracker.track(2, depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        auto vk = make_draw_functions(pipe);
        vk.clear_depth_stencil_image = mock_clear_depth;
        g_color_clears = g_depth_clears = g_draws = 0;
        EXPECT(replay(list, VK_NULL_HANDLE, resolve, vk, &tracker) == ReplayResult::unsupported);
        EXPECT(g_draws == 0 && tracker.layout(1, 0) == VK_IMAGE_LAYOUT_UNDEFINED);

        // Supplying clear_color_image allows replay and zero-clears color before the draw.
        vk.clear_color_image = mock_clear_color;
        EXPECT(replay(list, VK_NULL_HANDLE, resolve, vk, &tracker) == ReplayResult::success);
        EXPECT(g_color_clears == 1 && g_depth_clears == 0 && g_draws == 1);
    }

    // 2. UNDEFINED depth/stencil attachment with clear_depth_stencil_image == nullptr must return unsupported.
    {
        LayoutTracker tracker;
        tracker.track(1, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        tracker.track(2, depth, VK_IMAGE_LAYOUT_UNDEFINED);
        auto vk = make_draw_functions(pipe);
        vk.clear_color_image = mock_clear_color;
        g_color_clears = g_depth_clears = g_draws = 0;
        EXPECT(replay(list, VK_NULL_HANDLE, resolve, vk, &tracker) == ReplayResult::unsupported);
        EXPECT(g_draws == 0 && tracker.layout(2, 0) == VK_IMAGE_LAYOUT_UNDEFINED);

        vk.clear_depth_stencil_image = mock_clear_depth;
        EXPECT(replay(list, VK_NULL_HANDLE, resolve, vk, &tracker) == ReplayResult::success);
        EXPECT(g_color_clears == 0 && g_depth_clears == 1 && g_draws == 1);
    }

    // 3. Separate depth/stencil layouts: depth initialized, stencil still UNDEFINED.
    //    prepare_draw binds all aspects of the DSV (depth | stencil), so detail::to_attachment will zero-clear
    //    the UNDEFINED stencil plane: replay must refuse when clear_depth_stencil_image is unset, and succeed
    //    once stencil is also initialized.
    {
        LayoutTracker tracker(true);
        tracker.track(1, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        tracker.track(2, depth, VK_IMAGE_LAYOUT_UNDEFINED);
        tracker.set(2, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        auto vk = make_draw_functions(pipe);
        vk.separate_depth_stencil_layouts = true;
        vk.clear_color_image = mock_clear_color;
        g_color_clears = g_depth_clears = g_draws = 0;
        EXPECT(replay(list, VK_NULL_HANDLE, resolve, vk, &tracker) == ReplayResult::unsupported);
        EXPECT(g_draws == 0);

        tracker.set(2, {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1}, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        EXPECT(replay(list, VK_NULL_HANDLE, resolve, vk, &tracker) == ReplayResult::success);
        EXPECT(g_draws == 1 && g_depth_clears == 0);
    }

    // 4. not_zeroed attachments need no clear_color_image / clear_depth_stencil_image even from UNDEFINED.
    {
        ResourceBinding color_nz = color;
        ResourceBinding depth_nz = depth;
        color_nz.not_zeroed = true;
        depth_nz.not_zeroed = true;
        std::unordered_map<ResourceId, ResourceBinding> res_nz{{1, color_nz}, {2, depth_nz}};
        auto resolve_nz = [&](ResourceId id) -> const ResourceBinding* {
            auto it = res_nz.find(id);
            return it == res_nz.end() ? nullptr : &it->second;
        };
        LayoutTracker tracker;
        tracker.track(1, color_nz, VK_IMAGE_LAYOUT_UNDEFINED);
        tracker.track(2, depth_nz, VK_IMAGE_LAYOUT_UNDEFINED);
        auto vk = make_draw_functions(pipe);
        g_color_clears = g_depth_clears = g_draws = 0;
        EXPECT(replay(list, VK_NULL_HANDLE, resolve_nz, vk, &tracker) == ReplayResult::success);
        EXPECT(g_color_clears == 0 && g_depth_clears == 0 && g_draws == 1);
    }

    return 0;
}

int test_atomic_copy_overflow_and_aliasing_overlap() {
    ReplayFunctions vk{};
    vk.pipeline_barrier = mock_barrier;
    vk.copy_buffer = mock_copy_buffer;

    // 1. Two distinct ResourceIds aliasing overlapping bytes inside the SAME VkBuffer (e.g. GPU arena):
    //    dst [0x1000, 0x1008) vs src [0x1004, 0x100c) overlaps by 4 bytes -> must be refused (VUID-00117).
    {
        std::unordered_map<ResourceId, ResourceBinding> res{
            {1, make_buffer(0x10, 0x1000, 64)},
            {2, make_buffer(0x10, 0x1004, 64)},
        };
        auto resolve = [&](ResourceId id) -> const ResourceBinding* {
            auto it = res.find(id);
            return it == res.end() ? nullptr : &it->second;
        };
        CommandList list;
        EXPECT(list.atomic_copy({1, 0, 2, 0, 8, {}}) && list.close());
        PreparedList prepared;
        EXPECT(prepare(list, resolve, vk, prepared) == ReplayResult::invalid_copy);
    }

    // 2. Non-overlapping placed buffers in the SAME VkBuffer must still succeed.
    {
        std::unordered_map<ResourceId, ResourceBinding> res{
            {1, make_buffer(0x10, 0x1000, 64)},
            {2, make_buffer(0x10, 0x1008, 64)},
        };
        auto resolve = [&](ResourceId id) -> const ResourceBinding* {
            auto it = res.find(id);
            return it == res.end() ? nullptr : &it->second;
        };
        CommandList list;
        EXPECT(list.atomic_copy({1, 0, 2, 0, 8, {}}) && list.close());
        PreparedList prepared;
        EXPECT(prepare(list, resolve, vk, prepared) == ReplayResult::success);
        EXPECT(prepared.commands[0].buffer_copies.size() == 1);
        EXPECT(prepared.commands[0].buffer_copies[0].srcOffset == 0x1008);
        EXPECT(prepared.commands[0].buffer_copies[0].dstOffset == 0x1000);
        EXPECT(prepared.commands[0].buffer_copies[0].size == 8);
    }

    // 3. UINT64_MAX overflow on buffer_offset + src_offset / dst_offset must return invalid_copy.
    {
        std::unordered_map<ResourceId, ResourceBinding> res{
            {1, make_buffer(0x10, UINT64_MAX - 8, 64)},
            {2, make_buffer(0x20, 0, 64)},
        };
        auto resolve = [&](ResourceId id) -> const ResourceBinding* {
            auto it = res.find(id);
            return it == res.end() ? nullptr : &it->second;
        };
        CommandList list;
        EXPECT(list.atomic_copy({1, 16, 2, 0, 8, {}}) && list.close());
        PreparedList prepared;
        EXPECT(prepare(list, resolve, vk, prepared) == ReplayResult::invalid_copy);
    }

    // 4. UINT64_MAX overflow on dst + bytes must return invalid_copy.
    {
        std::unordered_map<ResourceId, ResourceBinding> res{
            {1, make_buffer(0x10, UINT64_MAX - 4, 64)},
            {2, make_buffer(0x20, 0, 64)},
        };
        auto resolve = [&](ResourceId id) -> const ResourceBinding* {
            auto it = res.find(id);
            return it == res.end() ? nullptr : &it->second;
        };
        CommandList list;
        EXPECT(list.atomic_copy({1, 0, 2, 0, 8, {}}) && list.close());
        PreparedList prepared;
        EXPECT(prepare(list, resolve, vk, prepared) == ReplayResult::invalid_copy);
    }

    return 0;
}

int test_zero_image_aspect_unaligned_scratch_offset() {
    // 1x1 BC7 (16-byte block) and 1x1 BC1 (8-byte block) with a 4-byte-aligned scratch offset.
    for (const auto [format, expected_align] :
         std::vector<std::pair<VkFormat, VkDeviceSize>>{{VK_FORMAT_BC7_UNORM_BLOCK, 16},
                                                        {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8},
                                                        {VK_FORMAT_R32G32B32A32_SFLOAT, 16}}) {
        const ResourceBinding img = make_image(5, format, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ReplayFunctions vk{};
        vk.pipeline_barrier = mock_barrier;
        vk.fill_buffer = mock_fill_buffer;
        vk.copy_buffer_to_image = mock_copy_buffer_to_image;
        VkDeviceSize requested = 0;
        vk.copy_scratch = [&](VkDeviceSize bytes, VkBuffer& out_buf, VkDeviceSize& out_off) {
            requested = bytes;
            out_buf = reinterpret_cast<VkBuffer>(std::uintptr_t{0x99});
            out_off = 4;  // 4-byte aligned, NOT 8- or 16-byte aligned
            return true;
        };

        g_fills = g_copies = 0;
        g_last_fill_offset = g_last_fill_size = g_last_copy_offset = 0;
        EXPECT(detail::zero_image_aspect(vk, VK_NULL_HANDLE, img, range, VK_IMAGE_ASPECT_COLOR_BIT,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
        EXPECT(requested == expected_align + (expected_align - 1));
        EXPECT(g_fills == 1 && g_copies == 1);
        EXPECT((g_last_fill_offset % expected_align) == 0 && (g_last_fill_offset % 4) == 0);
        EXPECT(g_last_fill_size == expected_align);
        EXPECT(g_last_copy_offset == g_last_fill_offset);
    }

    // Fallback when scratch has exact fill_bytes capacity (rejects padded_request) at an aligned offset 0.
    {
        const ResourceBinding img = make_image(6, VK_FORMAT_BC7_UNORM_BLOCK, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ReplayFunctions vk{};
        vk.pipeline_barrier = mock_barrier;
        vk.fill_buffer = mock_fill_buffer;
        vk.copy_buffer_to_image = mock_copy_buffer_to_image;
        vk.copy_scratch = [&](VkDeviceSize bytes, VkBuffer& out_buf, VkDeviceSize& out_off) {
            if (bytes > 16) return false;
            out_buf = reinterpret_cast<VkBuffer>(std::uintptr_t{0x99});
            out_off = 0;
            return true;
        };
        g_fills = g_copies = 0;
        EXPECT(detail::zero_image_aspect(vk, VK_NULL_HANDLE, img, range, VK_IMAGE_ASPECT_COLOR_BIT,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
        EXPECT(g_fills == 1 && g_copies == 1 && g_last_fill_offset == 0 && g_last_fill_size == 16);
    }

    return 0;
}

}  // namespace

int main() {
    if (test_draw_zero_clear_unavailable_color_and_depth()) return 1;
    if (test_atomic_copy_overflow_and_aliasing_overlap()) return 1;
    if (test_zero_image_aspect_unaligned_scratch_offset()) return 1;
    std::printf("replay_draw_zero_and_atomic_guards_test: ok\n");
    return 0;
}
