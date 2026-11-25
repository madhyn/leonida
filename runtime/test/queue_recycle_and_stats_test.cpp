// Pure-CPU mock-Vulkan tests for runtime/src/vulkan_backend.h and runtime/src/vulkan_replay.h:
//   1. VulkanQueue::execute_now() bounds statistics->mesh to lists.size(), ignoring stale retained prepared_ entries;
//   2. VulkanQueue::recycle_finished() frees completed predicate/predicated_copy/counted_dispatch/indirect_clamp
//      batches across ALL finished CommandSlots, not only the single reused slot in acquire() (tested with both
//      PredicateReducer::Batch and IndirectDispatchCounter::Batch).
// No GPU, Vulkan loader, or game files required.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "../src/vulkan_backend.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "line %d: CHECK failed: %s\n", __LINE__, #cond);         \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

namespace {

struct MockVk {
    std::mutex mutex;
    std::uint64_t next_handle = 1;
    std::uint64_t completed_timeline = 0;
    int create_query_pool_calls = 0;
    int create_mesh_stats_pool_calls = 0;
    int create_mesh_prims_pool_calls = 0;
    int live_descriptor_pools = 0;
    int live_buffers = 0;
    int live_memories = 0;
    int dispatch_indirect_calls = 0;
    std::unordered_map<std::uintptr_t, std::unique_ptr<std::uint8_t[]>> host_maps;
};

MockVk* g_vk = nullptr;

template <typename Handle>
Handle make_handle(std::uint64_t id) {
    return reinterpret_cast<Handle>(static_cast<std::uintptr_t>(id));
}

VKAPI_ATTR VkResult VKAPI_CALL m_create_cmd_pool(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*,
                                                 VkCommandPool* out) {
    *out = make_handle<VkCommandPool>(++g_vk->next_handle);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL m_destroy_cmd_pool(VkDevice, VkCommandPool, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL m_alloc_cmd_bufs(VkDevice, const VkCommandBufferAllocateInfo* info,
                                                VkCommandBuffer* out) {
    for (std::uint32_t i = 0; i < info->commandBufferCount; ++i)
        out[i] = make_handle<VkCommandBuffer>(++g_vk->next_handle);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL m_begin_cmd_buf(VkCommandBuffer, const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL m_end_cmd_buf(VkCommandBuffer) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL m_reset_cmd_buf(VkCommandBuffer, VkCommandBufferResetFlags) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL m_queue_submit(VkQueue, std::uint32_t, const VkSubmitInfo*, VkFence) {
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL m_queue_wait_idle(VkQueue) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL m_device_wait_idle(VkDevice) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL m_create_sem(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*,
                                            VkSemaphore* out) {
    *out = make_handle<VkSemaphore>(++g_vk->next_handle);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL m_destroy_sem(VkDevice, VkSemaphore, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL m_get_sem_val(VkDevice, VkSemaphore, std::uint64_t* value) {
    *value = g_vk->completed_timeline;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL m_wait_sems(VkDevice, const VkSemaphoreWaitInfo*, std::uint64_t) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL m_signal_sem(VkDevice, const VkSemaphoreSignalInfo*) { return VK_SUCCESS; }

VKAPI_ATTR VkResult VKAPI_CALL m_create_qpool(VkDevice, const VkQueryPoolCreateInfo* info, const VkAllocationCallbacks*,
                                              VkQueryPool* out) {
    ++g_vk->create_query_pool_calls;
    if (info->queryType == VK_QUERY_TYPE_PIPELINE_STATISTICS &&
        info->pipelineStatistics == leonida::kMeshStatisticsFlags) {
        ++g_vk->create_mesh_stats_pool_calls;
    }
    if (info->queryType == VK_QUERY_TYPE_MESH_PRIMITIVES_GENERATED_EXT) {
        ++g_vk->create_mesh_prims_pool_calls;
    }
    *out = make_handle<VkQueryPool>(++g_vk->next_handle);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL m_destroy_qpool(VkDevice, VkQueryPool, const VkAllocationCallbacks*) {}

VKAPI_ATTR VkResult VKAPI_CALL m_create_buf(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*,
                                            VkBuffer* out) {
    ++g_vk->live_buffers;
    *out = make_handle<VkBuffer>(++g_vk->next_handle);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL m_destroy_buf(VkDevice, VkBuffer buf, const VkAllocationCallbacks*) {
    if (buf) --g_vk->live_buffers;
}
VKAPI_ATTR void VKAPI_CALL m_buf_req(VkDevice, VkBuffer, VkMemoryRequirements* req) {
    req->size = 4096;
    req->alignment = 256;
    req->memoryTypeBits = 0x1u;
}
VKAPI_ATTR VkResult VKAPI_CALL m_alloc_mem(VkDevice, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*,
                                           VkDeviceMemory* out) {
    ++g_vk->live_memories;
    const auto id = ++g_vk->next_handle;
    g_vk->host_maps[static_cast<std::uintptr_t>(id)] = std::make_unique<std::uint8_t[]>(size_t(info->allocationSize));
    std::memset(g_vk->host_maps[static_cast<std::uintptr_t>(id)].get(), 0, size_t(info->allocationSize));
    *out = make_handle<VkDeviceMemory>(id);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL m_free_mem(VkDevice, VkDeviceMemory mem, const VkAllocationCallbacks*) {
    if (!mem) return;
    --g_vk->live_memories;
    g_vk->host_maps.erase(reinterpret_cast<std::uintptr_t>(mem));
}
VKAPI_ATTR VkResult VKAPI_CALL m_bind_buf(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL m_map_mem(VkDevice, VkDeviceMemory mem, VkDeviceSize off, VkDeviceSize,
                                         VkMemoryMapFlags, void** ppData) {
    const auto it = g_vk->host_maps.find(reinterpret_cast<std::uintptr_t>(mem));
    if (it == g_vk->host_maps.end()) return VK_ERROR_MEMORY_MAP_FAILED;
    *ppData = it->second.get() + off;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL m_unmap_mem(VkDevice, VkDeviceMemory) {}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL m_bda(VkDevice, const VkBufferDeviceAddressInfo* info) {
    return 0x10000000ull + reinterpret_cast<std::uintptr_t>(info->buffer) * 0x1000ull;
}

VKAPI_ATTR VkResult VKAPI_CALL m_create_dpool(VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*,
                                              VkDescriptorPool* out) {
    ++g_vk->live_descriptor_pools;
    *out = make_handle<VkDescriptorPool>(++g_vk->next_handle);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL m_destroy_dpool(VkDevice, VkDescriptorPool p, const VkAllocationCallbacks*) {
    if (p) --g_vk->live_descriptor_pools;
}
VKAPI_ATTR VkResult VKAPI_CALL m_alloc_dsets(VkDevice, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* out) {
    for (std::uint32_t i = 0; i < info->descriptorSetCount; ++i)
        out[i] = make_handle<VkDescriptorSet>(++g_vk->next_handle);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL m_update_dsets(VkDevice, std::uint32_t, const VkWriteDescriptorSet*, std::uint32_t,
                                          const VkCopyDescriptorSet*) {}
VKAPI_ATTR VkResult VKAPI_CALL m_create_dsl(VkDevice, const VkDescriptorSetLayoutCreateInfo*,
                                            const VkAllocationCallbacks*, VkDescriptorSetLayout* out) {
    *out = make_handle<VkDescriptorSetLayout>(++g_vk->next_handle);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL m_destroy_dsl(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL m_create_pl(VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*,
                                           VkPipelineLayout* out) {
    *out = make_handle<VkPipelineLayout>(++g_vk->next_handle);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL m_destroy_pl(VkDevice, VkPipelineLayout, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL m_create_sm(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*,
                                           VkShaderModule* out) {
    *out = make_handle<VkShaderModule>(++g_vk->next_handle);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL m_destroy_sm(VkDevice, VkShaderModule, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL m_create_cp(VkDevice, VkPipelineCache, std::uint32_t count,
                                           const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*,
                                           VkPipeline* out) {
    for (std::uint32_t i = 0; i < count; ++i) out[i] = make_handle<VkPipeline>(++g_vk->next_handle);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL m_create_gp(VkDevice, VkPipelineCache, std::uint32_t count,
                                           const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*,
                                           VkPipeline* out) {
    for (std::uint32_t i = 0; i < count; ++i) out[i] = make_handle<VkPipeline>(++g_vk->next_handle);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL m_destroy_p(VkDevice, VkPipeline, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL m_create_iv(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*,
                                           VkImageView* out) {
    *out = make_handle<VkImageView>(++g_vk->next_handle);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL m_destroy_iv(VkDevice, VkImageView, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL m_create_img(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*,
                                            VkImage* out) {
    *out = make_handle<VkImage>(++g_vk->next_handle);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL m_img_req(VkDevice, VkImage, VkMemoryRequirements* req) {
    req->size = 4096;
    req->alignment = 256;
    req->memoryTypeBits = 0x1u;
}
VKAPI_ATTR VkResult VKAPI_CALL m_bind_img(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL m_destroy_img(VkDevice, VkImage, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL m_destroy_fb(VkDevice, VkFramebuffer, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL m_destroy_rp(VkDevice, VkRenderPass, const VkAllocationCallbacks*) {}

VKAPI_ATTR void VKAPI_CALL m_cmd_barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
                                         std::uint32_t, const VkMemoryBarrier*, std::uint32_t,
                                         const VkBufferMemoryBarrier*, std::uint32_t, const VkImageMemoryBarrier*) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_clear_color(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*,
                                             std::uint32_t, const VkImageSubresourceRange*) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_clear_ds(VkCommandBuffer, VkImage, VkImageLayout, const VkClearDepthStencilValue*,
                                          std::uint32_t, const VkImageSubresourceRange*) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_copy_buf(VkCommandBuffer, VkBuffer, VkBuffer, std::uint32_t, const VkBufferCopy*) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_copy_b2i(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, std::uint32_t,
                                          const VkBufferImageCopy*) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_copy_i2b(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, std::uint32_t,
                                          const VkBufferImageCopy*) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_copy_img(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout,
                                          std::uint32_t, const VkImageCopy*) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_blit(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, std::uint32_t,
                                      const VkImageBlit*, VkFilter) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_resolve(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout,
                                         std::uint32_t, const VkImageResolve*) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_fill(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_update(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, const void*) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_reset_qp(VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_begin_q(VkCommandBuffer, VkQueryPool, std::uint32_t, VkQueryControlFlags) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_end_q(VkCommandBuffer, VkQueryPool, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_write_ts(VkCommandBuffer, VkPipelineStageFlagBits, VkQueryPool, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_copy_qp(VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t, VkBuffer,
                                         VkDeviceSize, VkDeviceSize, VkQueryResultFlags) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_bind_p(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_push(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, std::uint32_t,
                                      std::uint32_t, const void*) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_bind_ds(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, std::uint32_t,
                                         std::uint32_t, const VkDescriptorSet*, std::uint32_t, const std::uint32_t*) {
}
VKAPI_ATTR void VKAPI_CALL m_cmd_disp(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_disp_ind(VkCommandBuffer, VkBuffer, VkDeviceSize) { ++g_vk->dispatch_indirect_calls; }
VKAPI_ATTR void VKAPI_CALL m_cmd_begin_ren(VkCommandBuffer, const VkRenderingInfo*) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_end_ren(VkCommandBuffer) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_clear_att(VkCommandBuffer, std::uint32_t, const VkClearAttachment*, std::uint32_t,
                                           const VkClearRect*) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_bind_vb2(VkCommandBuffer, std::uint32_t, std::uint32_t, const VkBuffer*,
                                          const VkDeviceSize*, const VkDeviceSize*, const VkDeviceSize*) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_bind_ib(VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_set_vp(VkCommandBuffer, std::uint32_t, std::uint32_t, const VkViewport*) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_set_sc(VkCommandBuffer, std::uint32_t, std::uint32_t, const VkRect2D*) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_set_topo(VkCommandBuffer, VkPrimitiveTopology) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_set_restart(VkCommandBuffer, VkBool32) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_set_stencil(VkCommandBuffer, VkStencilFaceFlags, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_set_blend(VkCommandBuffer, const float[4]) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_draw(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_draw_idx(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t,
                                          std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_draw_mesh(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_begin_cond(VkCommandBuffer, const VkConditionalRenderingBeginInfoEXT*) {}
VKAPI_ATTR void VKAPI_CALL m_cmd_end_cond(VkCommandBuffer) {}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mock_gdpa(VkDevice, const char* name) {
#define E(fn, impl) if (std::strcmp(name, #fn) == 0) return reinterpret_cast<PFN_vkVoidFunction>(impl)
    E(vkCreateCommandPool, m_create_cmd_pool);
    E(vkDestroyCommandPool, m_destroy_cmd_pool);
    E(vkAllocateCommandBuffers, m_alloc_cmd_bufs);
    E(vkBeginCommandBuffer, m_begin_cmd_buf);
    E(vkEndCommandBuffer, m_end_cmd_buf);
    E(vkResetCommandBuffer, m_reset_cmd_buf);
    E(vkQueueSubmit, m_queue_submit);
    E(vkQueueWaitIdle, m_queue_wait_idle);
    E(vkDeviceWaitIdle, m_device_wait_idle);
    E(vkCreateSemaphore, m_create_sem);
    E(vkDestroySemaphore, m_destroy_sem);
    E(vkGetSemaphoreCounterValue, m_get_sem_val);
    E(vkWaitSemaphores, m_wait_sems);
    E(vkSignalSemaphore, m_signal_sem);
    E(vkCreateQueryPool, m_create_qpool);
    E(vkDestroyQueryPool, m_destroy_qpool);
    E(vkCreateBuffer, m_create_buf);
    E(vkDestroyBuffer, m_destroy_buf);
    E(vkGetBufferMemoryRequirements, m_buf_req);
    E(vkAllocateMemory, m_alloc_mem);
    E(vkFreeMemory, m_free_mem);
    E(vkBindBufferMemory, m_bind_buf);
    E(vkMapMemory, m_map_mem);
    E(vkUnmapMemory, m_unmap_mem);
    E(vkGetBufferDeviceAddress, m_bda);
    E(vkCreateDescriptorPool, m_create_dpool);
    E(vkDestroyDescriptorPool, m_destroy_dpool);
    E(vkAllocateDescriptorSets, m_alloc_dsets);
    E(vkUpdateDescriptorSets, m_update_dsets);
    E(vkCreateDescriptorSetLayout, m_create_dsl);
    E(vkDestroyDescriptorSetLayout, m_destroy_dsl);
    E(vkCreatePipelineLayout, m_create_pl);
    E(vkDestroyPipelineLayout, m_destroy_pl);
    E(vkCreateShaderModule, m_create_sm);
    E(vkDestroyShaderModule, m_destroy_sm);
    E(vkCreateComputePipelines, m_create_cp);
    E(vkCreateGraphicsPipelines, m_create_gp);
    E(vkDestroyPipeline, m_destroy_p);
    E(vkCreateImageView, m_create_iv);
    E(vkDestroyImageView, m_destroy_iv);
    E(vkCreateImage, m_create_img);
    E(vkGetImageMemoryRequirements, m_img_req);
    E(vkBindImageMemory, m_bind_img);
    E(vkDestroyImage, m_destroy_img);
    E(vkDestroyFramebuffer, m_destroy_fb);
    E(vkDestroyRenderPass, m_destroy_rp);
    E(vkCmdPipelineBarrier, m_cmd_barrier);
    E(vkCmdClearColorImage, m_cmd_clear_color);
    E(vkCmdClearDepthStencilImage, m_cmd_clear_ds);
    E(vkCmdCopyBuffer, m_cmd_copy_buf);
    E(vkCmdCopyBufferToImage, m_cmd_copy_b2i);
    E(vkCmdCopyImageToBuffer, m_cmd_copy_i2b);
    E(vkCmdCopyImage, m_cmd_copy_img);
    E(vkCmdBlitImage, m_cmd_blit);
    E(vkCmdResolveImage, m_cmd_resolve);
    E(vkCmdFillBuffer, m_cmd_fill);
    E(vkCmdUpdateBuffer, m_cmd_update);
    E(vkCmdResetQueryPool, m_cmd_reset_qp);
    E(vkCmdBeginQuery, m_cmd_begin_q);
    E(vkCmdEndQuery, m_cmd_end_q);
    E(vkCmdWriteTimestamp, m_cmd_write_ts);
    E(vkCmdCopyQueryPoolResults, m_cmd_copy_qp);
    E(vkCmdBindPipeline, m_cmd_bind_p);
    E(vkCmdPushConstants, m_cmd_push);
    E(vkCmdBindDescriptorSets, m_cmd_bind_ds);
    E(vkCmdDispatch, m_cmd_disp);
    E(vkCmdDispatchIndirect, m_cmd_disp_ind);
    E(vkCmdBeginRendering, m_cmd_begin_ren);
    E(vkCmdEndRendering, m_cmd_end_ren);
    E(vkCmdClearAttachments, m_cmd_clear_att);
    E(vkCmdBindVertexBuffers2, m_cmd_bind_vb2);
    E(vkCmdBindIndexBuffer, m_cmd_bind_ib);
    E(vkCmdSetViewport, m_cmd_set_vp);
    E(vkCmdSetScissor, m_cmd_set_sc);
    E(vkCmdSetPrimitiveTopology, m_cmd_set_topo);
    E(vkCmdSetPrimitiveRestartEnable, m_cmd_set_restart);
    E(vkCmdSetStencilReference, m_cmd_set_stencil);
    E(vkCmdSetBlendConstants, m_cmd_set_blend);
    E(vkCmdDraw, m_cmd_draw);
    E(vkCmdDrawIndexed, m_cmd_draw_idx);
    E(vkCmdDrawMeshTasksEXT, m_cmd_draw_mesh);
    E(vkCmdBeginConditionalRenderingEXT, m_cmd_begin_cond);
    E(vkCmdEndConditionalRenderingEXT, m_cmd_end_cond);
#undef E
    return nullptr;
}

VkPhysicalDeviceMemoryProperties make_mem() {
    VkPhysicalDeviceMemoryProperties mem{};
    mem.memoryHeapCount = 1;
    mem.memoryHeaps[0] = {VkDeviceSize(1) << 30, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    mem.memoryTypeCount = 1;
    mem.memoryTypes[0] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          0};
    return mem;
}

}  // namespace

int main() {
    using namespace leonida;
    const VkDevice dummy_dev = make_handle<VkDevice>(1);
    const VkQueue dummy_q = make_handle<VkQueue>(2);

    // 1. Stale prepared_ tail must not leak mesh statistics flags into later smaller ExecuteCommandLists batches.
    {
        MockVk vk;
        g_vk = &vk;
        VulkanContext ctx(mock_gdpa, dummy_dev, false);
        CHECK(ctx.loaded());
        ctx.mesh_queries = true;
        ctx.set_occlusion_resolve_memory(make_mem());

        constexpr QueryHeapId heap_id = 10;
        QueryPool qpool{};
        qpool.heap_type = d3d12_query_heap::pipeline_statistics;
        qpool.pool = make_handle<VkQueryPool>(100);
        qpool.count = 8;
        qpool.ended.assign(8, 0);
        ctx.add_query_pool(heap_id, qpool);

        constexpr std::uintptr_t vertex_pso = 1, mesh_pso = 2;
        GraphicsPipeline vp{};
        vp.pipeline = make_handle<VkPipeline>(201);
        vp.layout = make_handle<VkPipelineLayout>(202);
        vp.push_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        ctx.add_graphics_pipeline(vertex_pso, vp);

        GraphicsPipeline mp{};
        mp.pipeline = make_handle<VkPipeline>(203);
        mp.layout = make_handle<VkPipelineLayout>(204);
        mp.push_stages = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT;
        mp.mesh = true;
        ctx.add_graphics_pipeline(mesh_pso, mp);

        VulkanQueue queue(ctx, dummy_q, 0, VulkanQueue::QueueKind::direct);
        CHECK(queue.ok());

        // Batch A (2 lists): list[0] is empty; list[1] runs a mesh draw inside a pipeline-statistics query so
        // prepared_[1].statistics_mesh contains 1.
        CommandList empty_list;
        CHECK(empty_list.close());

        CommandList mesh_list;
        CHECK(mesh_list.begin_query({heap_id, d3d12_query::pipeline_statistics, 0}));
        Draw mesh_draw{};
        mesh_draw.pipeline = mesh_pso;
        mesh_draw.mesh = true;
        mesh_draw.count = 1;
        mesh_draw.instances = 1;
        mesh_draw.first = 1;
        mesh_draw.viewport = {0.f, 0.f, 16.f, 16.f, 0.f, 1.f};
        mesh_draw.scissor = {0, 0, 16, 16};
        CHECK(mesh_list.draw(mesh_draw));
        CHECK(mesh_list.end_query({heap_id, d3d12_query::pipeline_statistics, 0}));
        CHECK(mesh_list.close());

        const CommandList* batch_a[2] = {&empty_list, &mesh_list};
        CHECK(queue.execute(batch_a) == ReplayResult::success);
        CHECK(queue.prepared_entries() == 2);
        CHECK(vk.create_mesh_stats_pool_calls == 1);
        CHECK(vk.create_mesh_prims_pool_calls == 1);

        // Batch B (1 list): overlapping vertex-only pipeline-statistics queries (no mesh draw at all).
        // Before the fix, execute_now() looped over all of prepared_ (including stale prepared_[1]),
        // saw mesh == 1 from Batch A, and needlessly created both mesh_pool and primitives_pool!
        CommandList vertex_list;
        CHECK(vertex_list.begin_query({heap_id, d3d12_query::pipeline_statistics, 1}));
        CHECK(vertex_list.begin_query({heap_id, d3d12_query::pipeline_statistics, 2}));
        Draw v_draw{};
        v_draw.pipeline = vertex_pso;
        v_draw.count = 3;
        v_draw.instances = 1;
        v_draw.topology = 4;  // triangle list
        v_draw.viewport = {0.f, 0.f, 16.f, 16.f, 0.f, 1.f};
        v_draw.scissor = {0, 0, 16, 16};
        CHECK(vertex_list.draw(v_draw));
        CHECK(vertex_list.end_query({heap_id, d3d12_query::pipeline_statistics, 1}));
        CHECK(vertex_list.end_query({heap_id, d3d12_query::pipeline_statistics, 2}));
        CHECK(vertex_list.close());

        const CommandList* batch_b[1] = {&vertex_list};
        CHECK(queue.execute(batch_b) == ReplayResult::success);
        CHECK(vk.create_mesh_stats_pool_calls == 1);
        CHECK(vk.create_mesh_prims_pool_calls == 1);
    }

    // 2. VulkanQueue::recycle_finished() must release predicate/counted_dispatch batches across ALL completed slots,
    //    not just the single slot returned by acquire().
    {
        MockVk vk;
        g_vk = &vk;
        const auto mem = make_mem();
        VulkanContext ctx(mock_gdpa, dummy_dev, false, /*conditional_rendering=*/true);
        CHECK(ctx.loaded());
        ctx.configure_predication(mock_gdpa, mem, 1u << 20, 0);

        constexpr ResourceId pred_buf_id = 42;
        ResourceBinding pred_buf{};
        pred_buf.buffer = make_handle<VkBuffer>(500);
        pred_buf.size = 64;
        ctx.add_resource(pred_buf_id, pred_buf);

        VulkanQueue queue(ctx, dummy_q, 0, VulkanQueue::QueueKind::direct);
        CHECK(queue.ok());

        CommandList pred_list;
        CHECK(pred_list.set_predication({pred_buf_id, 0, d3d12_predication::equal_zero}));
        CHECK(pred_list.set_predication({0, 0, d3d12_predication::equal_zero}));
        CHECK(pred_list.close());

        CommandList plain_list;
        CHECK(plain_list.close());

        // Submit 4 predicated batches while timeline stays at 0 -> allocates 4 CommandSlots, each holding a
        // PredicateReducer::Batch (1 VkDescriptorPool + 1 VkBuffer + 1 VkDeviceMemory each).
        const CommandList* pred_span[1] = {&pred_list};
        for (int i = 0; i < 4; ++i) {
            CHECK(queue.execute(pred_span) == ReplayResult::success);
        }
        CHECK(vk.live_descriptor_pools == 4);
        CHECK(vk.live_buffers == 4);
        CHECK(vk.live_memories == 4);

        // All 4 submissions complete on the GPU.
        vk.completed_timeline = 4;

        // One subsequent non-predicated submission calls acquire() -> recycle_finished().
        // All 4 finished slots' PredicateReducer::Batches must be released immediately (not 1 freed + 3 leaked!).
        const CommandList* plain_span[1] = {&plain_list};
        CHECK(queue.execute(plain_span) == ReplayResult::success);
        CHECK(vk.live_descriptor_pools == 0);
        CHECK(vk.live_buffers == 0);
        CHECK(vk.live_memories == 0);
    }

    // 3. Same recycle_finished() check for IndirectDispatchCounter::Batch (VkBuffer + VkDeviceMemory per slot).
    {
        MockVk vk;
        g_vk = &vk;
        const auto mem = make_mem();
        VkPhysicalDeviceLimits limits{};
        limits.maxComputeWorkGroupCount[0] = 65535u;
        VulkanContext ctx(mock_gdpa, dummy_dev, false);
        CHECK(ctx.loaded());
        ctx.configure_counted_dispatch(mock_gdpa, mem, limits);

        constexpr std::uintptr_t cp_id = 7;
        ComputePipeline cp{};
        cp.pipeline = make_handle<VkPipeline>(300);
        cp.layout = make_handle<VkPipelineLayout>(301);
        ctx.add_pipeline(cp_id, cp);

        ResourceBinding args_buf{};
        args_buf.buffer = make_handle<VkBuffer>(600);
        args_buf.size = 64;
        ctx.add_resource(1, args_buf);
        ResourceBinding count_buf{};
        count_buf.buffer = make_handle<VkBuffer>(601);
        count_buf.size = 16;
        ctx.add_resource(2, count_buf);

        VulkanQueue queue(ctx, dummy_q, 0, VulkanQueue::QueueKind::direct);
        CHECK(queue.ok());

        CommandList ind_list;
        ExecuteIndirect ind{};
        ind.kind = IndirectKind::dispatch;
        ind.argument_buffer = 1;
        ind.count_buffer = 2;
        ind.stride = 12;
        ind.max_count = 2;
        ind.pipeline = cp_id;
        CHECK(ind_list.execute_indirect(ind));
        CHECK(ind_list.close());

        CommandList plain_list;
        CHECK(plain_list.close());

        const CommandList* ind_span[1] = {&ind_list};
        for (int i = 0; i < 4; ++i) {
            CHECK(queue.execute(ind_span) == ReplayResult::success);
        }
        CHECK(vk.live_buffers == 4);
        CHECK(vk.live_memories == 4);

        vk.completed_timeline = 4;
        const CommandList* plain_span[1] = {&plain_list};
        CHECK(queue.execute(plain_span) == ReplayResult::success);
        CHECK(vk.live_buffers == 0);
        CHECK(vk.live_memories == 0);
    }

    std::printf("queue_recycle_and_stats_test: all checks passed\n");
    return 0;
}
