#pragma once

// Imports TitleHostRegions into Vulkan via VK_EXT_external_memory_host so CPU-allocated
// title memory ranges have valid VkBuffers and GPU buffer device addresses (BDA), and
// safely retires/destroys imported buffers when those ranges are unmapped or freed.

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "title_host_regions.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace leonida {

struct VulkanTitleRegionFunctions {
    PFN_vkCreateBuffer vkCreateBuffer = nullptr;
    PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory vkAllocateMemory = nullptr;
    PFN_vkFreeMemory vkFreeMemory = nullptr;
    PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
    PFN_vkGetBufferDeviceAddress vkGetBufferDeviceAddress = nullptr;
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
    PFN_vkGetMemoryHostPointerPropertiesEXT vkGetMemoryHostPointerPropertiesEXT = nullptr;
};

class VulkanTitleRegions {
public:
    VulkanTitleRegions() = default;
    VulkanTitleRegions(const VulkanTitleRegions&) = delete;
    VulkanTitleRegions& operator=(const VulkanTitleRegions&) = delete;

    ~VulkanTitleRegions() { destroy_all(); }

    void configure(VkDevice device, const VulkanTitleRegionFunctions& fn, VkDeviceSize host_pointer_alignment,
                   const VkPhysicalDeviceMemoryProperties& memory_properties,
                   TitleHostRegions* regions = &title_host_regions()) {
        destroy_all();
        std::lock_guard<std::mutex> lock(mutex_);
        device_ = device;
        fn_ = fn;
        alignment_ = host_pointer_alignment;
        memory_properties_ = memory_properties;
        regions_ = regions;
    }

    bool supported() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return supported_locked();
    }

    // Imports any pending TitleHostRegions entries into Vulkan via VK_EXT_external_memory_host.
    void sync_pending() {
        TitleHostRegions* regions = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!supported_locked()) return;
            regions = regions_;
        }

        const auto pending = regions->pending_imports();
        for (const auto& region : pending) {
            import_region(regions, region);
        }
        collect_retired(true);
    }

    // Destroys Vulkan buffers/memory for any retired regions (after waiting for GPU idle if needed).
    void collect_retired(bool wait_idle = true) {
        TitleHostRegions* regions = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            regions = regions_;
            if (!regions || device_ == VK_NULL_HANDLE) return;
        }

        const auto retired = regions->take_retired();
        if (retired.empty()) return;

        std::vector<ImportedEntry> to_destroy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            to_destroy.reserve(retired.size());
            for (const auto& r : retired) {
                const auto it = entries_.find(r.handle);
                if (it != entries_.end()) {
                    to_destroy.push_back(it->second);
                    entries_.erase(it);
                }
            }
        }

        if (to_destroy.empty()) return;
        if (wait_idle && fn_.vkDeviceWaitIdle) {
            fn_.vkDeviceWaitIdle(device_);
        }
        for (const auto& entry : to_destroy) {
            destroy_entry(entry);
        }
    }

    // Resolves a title host VA into an imported VkBuffer, byte offset, and optional host pointer.
    bool lookup(std::uint64_t va, std::uint64_t& size, VkBuffer& buffer, VkDeviceSize& offset,
                const std::uint8_t** host_ptr = nullptr) {
        if (!va) return false;
        TitleHostRegions* regions = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!supported_locked()) return false;
            regions = regions_;
        }

        auto hit = regions->find(va);
        if (!hit.found) return false;
        if (hit.gpu_bda == 0 || hit.handle == 0) {
            sync_pending();
            hit = regions->find(va);
            if (!hit.found || hit.gpu_bda == 0 || hit.handle == 0) return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(hit.handle);
        if (it == entries_.end() || it->second.buffer == VK_NULL_HANDLE) return false;
        if (va < it->second.aligned_start) return false;
        const std::uint64_t buf_offset = va - it->second.aligned_start;
        if (buf_offset >= it->second.aligned_size) return false;

        buffer = it->second.buffer;
        offset = buf_offset;
        const std::uint64_t remaining = hit.size - hit.offset;
        size = size ? std::min<std::uint64_t>(size, remaining) : remaining;
        if (host_ptr) *host_ptr = reinterpret_cast<const std::uint8_t*>(va);
        return true;
    }

    // Resolves a title host VA to its imported GPU BDA, lazily importing if still pending.
    std::optional<std::uint64_t> resolve_bda(std::uint64_t va) {
        if (!va) return std::nullopt;
        TitleHostRegions* regions = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!supported_locked()) return std::nullopt;
            regions = regions_;
        }

        auto hit = regions->find(va);
        if (!hit.found) return std::nullopt;
        if (hit.gpu_bda == 0) {
            sync_pending();
            return regions->resolve_bda(va);
        }
        return hit.gpu_bda + hit.offset;
    }

    // Makes prior CPU writes to imported title host regions visible to GPU shaders and transfer/vertex/index stages.
    void record_host_write_barrier(VkCommandBuffer cb) const {
        if (!cb) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!supported_locked() || entries_.empty() || !fn_.vkCmdPipelineBarrier) return;

        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                                VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                                VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        fn_.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier,
                                 0, nullptr, 0, nullptr);
    }

    void destroy_all() {
        std::vector<ImportedEntry> remaining;
        VkDevice device = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            device = device_;
            remaining.reserve(entries_.size());
            for (const auto& [handle, entry] : entries_) {
                (void)handle;
                remaining.push_back(entry);
            }
            entries_.clear();
        }
        if (device != VK_NULL_HANDLE && !remaining.empty()) {
            if (fn_.vkDeviceWaitIdle) fn_.vkDeviceWaitIdle(device);
            for (const auto& entry : remaining) {
                if (entry.buffer != VK_NULL_HANDLE && fn_.vkDestroyBuffer) {
                    fn_.vkDestroyBuffer(device, entry.buffer, nullptr);
                }
                if (entry.memory != VK_NULL_HANDLE && fn_.vkFreeMemory) {
                    fn_.vkFreeMemory(device, entry.memory, nullptr);
                }
            }
        }
    }

private:
    struct ImportedEntry {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::uint64_t aligned_start = 0;
        std::uint64_t aligned_size = 0;
        std::uint64_t base_bda = 0;
    };

    bool supported_locked() const {
        return device_ != VK_NULL_HANDLE && regions_ != nullptr && alignment_ != 0 && fn_.vkCreateBuffer &&
               fn_.vkDestroyBuffer && fn_.vkGetBufferMemoryRequirements && fn_.vkAllocateMemory && fn_.vkFreeMemory &&
               fn_.vkBindBufferMemory && fn_.vkGetBufferDeviceAddress && fn_.vkGetMemoryHostPointerPropertiesEXT;
    }

    void destroy_entry(const ImportedEntry& entry) const {
        if (entry.buffer != VK_NULL_HANDLE && fn_.vkDestroyBuffer) {
            fn_.vkDestroyBuffer(device_, entry.buffer, nullptr);
        }
        if (entry.memory != VK_NULL_HANDLE && fn_.vkFreeMemory) {
            fn_.vkFreeMemory(device_, entry.memory, nullptr);
        }
    }

    std::uint32_t choose_memory_type(std::uint32_t allowed_types) const {
        constexpr VkMemoryPropertyFlags kPreferred =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        std::uint32_t fallback = memory_properties_.memoryTypeCount;
        for (std::uint32_t i = 0; i < memory_properties_.memoryTypeCount; ++i) {
            if (!(allowed_types & (1u << i))) continue;
            if ((memory_properties_.memoryTypes[i].propertyFlags & kPreferred) == kPreferred) {
                return i;
            }
            if (fallback == memory_properties_.memoryTypeCount) {
                fallback = i;
            }
        }
        return fallback;
    }

    void import_region(TitleHostRegions* regions, const TitleHostRegions::Region& region) {
        if (!region.host_va || !region.size) return;

        const std::uint64_t align = alignment_;
        const std::uint64_t aligned_start = region.host_va & ~(align - 1);
        const std::uint64_t region_end = region.host_va + region.size;
        if (region_end < region.host_va) return;
        const std::uint64_t aligned_end = (region_end + (align - 1)) & ~(align - 1);
        if (aligned_end <= aligned_start) return;
        const std::uint64_t aligned_size = aligned_end - aligned_start;

#ifdef _WIN32
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(aligned_start), &mbi, sizeof(mbi))) return;
        if (mbi.State != MEM_COMMIT) return;
        constexpr DWORD kBadProtect = PAGE_NOACCESS | PAGE_GUARD | PAGE_WRITECOMBINE | PAGE_NOCACHE;
        if (mbi.Protect & kBadProtect) return;
        const std::uint64_t mbi_end =
            reinterpret_cast<std::uint64_t>(mbi.BaseAddress) + static_cast<std::uint64_t>(mbi.RegionSize);
        if (mbi_end < aligned_end) return;
#endif

        VkMemoryHostPointerPropertiesEXT host_props{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (fn_.vkGetMemoryHostPointerPropertiesEXT(device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                    reinterpret_cast<void*>(aligned_start),
                                                    &host_props) != VK_SUCCESS) {
            return;
        }

        VkExternalMemoryBufferCreateInfo ext_buf{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        ext_buf.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;

        VkBufferCreateInfo buf_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buf_info.pNext = &ext_buf;
        buf_info.size = aligned_size;
        buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkBuffer buffer = VK_NULL_HANDLE;
        if (fn_.vkCreateBuffer(device_, &buf_info, nullptr, &buffer) != VK_SUCCESS) {
            return;
        }

        VkMemoryRequirements reqs{};
        fn_.vkGetBufferMemoryRequirements(device_, buffer, &reqs);
        const std::uint32_t valid_types = reqs.memoryTypeBits & host_props.memoryTypeBits;
        const std::uint32_t mem_type = choose_memory_type(valid_types);
        if (mem_type >= memory_properties_.memoryTypeCount) {
            fn_.vkDestroyBuffer(device_, buffer, nullptr);
            return;
        }

        VkImportMemoryHostPointerInfoEXT import_info{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
        import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        import_info.pHostPointer = reinterpret_cast<void*>(aligned_start);

        VkMemoryAllocateFlagsInfo flags_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags_info.pNext = &import_info;
        flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

        VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc_info.pNext = &flags_info;
        alloc_info.allocationSize = std::max<VkDeviceSize>(reqs.size, aligned_size);
        alloc_info.memoryTypeIndex = mem_type;

        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (fn_.vkAllocateMemory(device_, &alloc_info, nullptr, &memory) != VK_SUCCESS) {
            fn_.vkDestroyBuffer(device_, buffer, nullptr);
            return;
        }

        if (fn_.vkBindBufferMemory(device_, buffer, memory, 0) != VK_SUCCESS) {
            fn_.vkDestroyBuffer(device_, buffer, nullptr);
            fn_.vkFreeMemory(device_, memory, nullptr);
            return;
        }

        VkBufferDeviceAddressInfo bda_info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        bda_info.buffer = buffer;
        const std::uint64_t base_bda = fn_.vkGetBufferDeviceAddress(device_, &bda_info);
        if (!base_bda) {
            fn_.vkDestroyBuffer(device_, buffer, nullptr);
            fn_.vkFreeMemory(device_, memory, nullptr);
            return;
        }

        std::uint64_t handle = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handle = next_handle_++;
            entries_.emplace(handle, ImportedEntry{buffer, memory, aligned_start, aligned_size, base_bda});
        }
        const std::uint64_t region_bda = base_bda + (region.host_va - aligned_start);
        regions->mark_imported(region.host_va, region.size, region_bda, handle);
    }

    mutable std::mutex mutex_;
    VkDevice device_ = VK_NULL_HANDLE;
    VulkanTitleRegionFunctions fn_{};
    VkDeviceSize alignment_ = 0;
    VkPhysicalDeviceMemoryProperties memory_properties_{};
    TitleHostRegions* regions_ = nullptr;
    std::uint64_t next_handle_ = 1;
    std::unordered_map<std::uint64_t, ImportedEntry> entries_;
};

}  // namespace leonida
