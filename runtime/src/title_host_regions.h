#pragma once

// Thread-safe interval table mapping non-overlapping title host virtual address ranges
// [host_va, host_va + size) to their optional Vulkan-imported GPU buffer device address (BDA)
// and import handle. Regions with gpu_bda == 0 are registered by the CPU allocator / xmem
// and pending lazy import by the Vulkan device; detached/replaced regions that hold a Vulkan
// import handle are queued for deferred destruction after GPU idle.

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace leonida {

class TitleHostRegions {
public:
    struct Region {
        std::uint64_t host_va = 0;
        std::uint64_t size = 0;
        std::uint64_t gpu_bda = 0;
        std::uint64_t tag = 0;
        std::uint64_t handle = 0;
    };

    struct Retired {
        std::uint64_t host_va = 0;
        std::uint64_t size = 0;
        std::uint64_t gpu_bda = 0;
        std::uint64_t tag = 0;
        std::uint64_t handle = 0;
    };

    struct Lookup {
        bool found = false;
        std::uint64_t host_va = 0;
        std::uint64_t size = 0;
        std::uint64_t gpu_bda = 0;
        std::uint64_t offset = 0;
        std::uint64_t handle = 0;
    };

    // Registers [host_va, host_va + size) with gpu_bda = 0 (pending Vulkan import).
    // Overlapping stale entries are split or replaced; any detached entry with a Vulkan
    // import handle is recorded in the retired list. Ignores host_va == 0 or size == 0.
    void register_range(std::uint64_t host_va, std::uint64_t size, std::uint64_t tag = 0) {
        if (host_va == 0 || size == 0) {
            return;
        }
        const std::uint64_t end = clamp_end(host_va, size);
        const std::uint64_t clamped_size = end - host_va;
        if (clamped_size == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        detach_range_locked(host_va, end);
        regions_.emplace(host_va, Region{host_va, clamped_size, 0, tag, 0});
    }

    // Removes [host_va, host_va + size), splitting larger regions if partially unmapped
    // and removing fully covered regions. Detached entries with a Vulkan import handle
    // are recorded in the retired list for safe destruction after GPU idle.
    void unregister_range(std::uint64_t host_va, std::uint64_t size) {
        if (host_va == 0 || size == 0) {
            return;
        }
        const std::uint64_t end = clamp_end(host_va, size);
        if (end <= host_va) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        detach_range_locked(host_va, end);
    }

    // O(log N) lookup returning the region containing `va` and `offset = va - region.host_va`.
    Lookup find(std::uint64_t va) const {
        if (va == 0) {
            return {};
        }

        std::lock_guard<std::mutex> lock(mutex_);
        return find_locked(va);
    }

    // Resolves `va` to `region.gpu_bda + offset` if `va` falls in an imported region
    // (`gpu_bda != 0`), or `std::nullopt` otherwise.
    std::optional<std::uint64_t> resolve_bda(std::uint64_t va) const {
        if (va == 0) {
            return std::nullopt;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const Lookup hit = find_locked(va);
        if (!hit.found || hit.gpu_bda == 0) {
            return std::nullopt;
        }
        return hit.gpu_bda + hit.offset;
    }

    // Returns all currently registered regions that have not yet been imported into Vulkan (`gpu_bda == 0`).
    std::vector<Region> pending_imports() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Region> pending;
        for (const auto& [va, region] : regions_) {
            (void)va;
            if (region.gpu_bda == 0) {
                pending.push_back(region);
            }
        }
        return pending;
    }

    // Marks a previously registered region [host_va, host_va + size) as imported with `gpu_bda`
    // and Vulkan `handle`. If the region was unregistered or resized concurrently before this
    // call arrived, any non-zero `handle` is queued into `retired` so it is not leaked.
    void mark_imported(std::uint64_t host_va, std::uint64_t size, std::uint64_t gpu_bda, std::uint64_t handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = regions_.find(host_va);
        if (it == regions_.end() || it->second.size != size) {
            if (handle != 0) {
                retired_.push_back(Retired{host_va, size, gpu_bda, 0, handle});
            }
            return;
        }

        if (it->second.handle != 0 && it->second.handle != handle) {
            retired_.push_back(Retired{
                it->second.host_va,
                it->second.size,
                it->second.gpu_bda,
                it->second.tag,
                it->second.handle,
            });
        }

        it->second.gpu_bda = gpu_bda;
        it->second.handle = handle;
    }

    // Drains all retired entries that had a Vulkan import handle (`handle != 0`).
    std::vector<Retired> take_retired() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Retired> out;
        out.swap(retired_);
        return out;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return regions_.size();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return regions_.empty();
    }

private:
    static std::uint64_t clamp_end(std::uint64_t start, std::uint64_t size) {
        const std::uint64_t max_u64 = std::numeric_limits<std::uint64_t>::max();
        return (size > max_u64 - start) ? max_u64 : (start + size);
    }

    Lookup find_locked(std::uint64_t va) const {
        if (regions_.empty()) {
            return {};
        }
        auto it = regions_.upper_bound(va);
        if (it == regions_.begin()) {
            return {};
        }
        --it;
        const std::uint64_t offset = va - it->first;
        if (offset >= it->second.size) {
            return {};
        }
        return Lookup{true, it->second.host_va, it->second.size, it->second.gpu_bda, offset, it->second.handle};
    }

    void detach_range_locked(std::uint64_t start, std::uint64_t end) {
        auto it = regions_.lower_bound(start);
        if (it != regions_.begin()) {
            auto prev = std::prev(it);
            if (clamp_end(prev->second.host_va, prev->second.size) > start) {
                it = prev;
            }
        }

        while (it != regions_.end() && it->first < end) {
            const Region old = it->second;
            const std::uint64_t old_start = old.host_va;
            const std::uint64_t old_end = clamp_end(old.host_va, old.size);

            it = regions_.erase(it);

            if (old.handle != 0) {
                retired_.push_back(Retired{old.host_va, old.size, old.gpu_bda, old.tag, old.handle});
            }

            if (old_start < start) {
                const Region left{old_start, start - old_start, 0, old.tag, 0};
                regions_.emplace(left.host_va, left);
            }
            if (old_end > end) {
                const Region right{end, old_end - end, 0, old.tag, 0};
                regions_.emplace(right.host_va, right);
            }
        }
    }

    mutable std::mutex mutex_;
    std::map<std::uint64_t, Region> regions_;
    std::vector<Retired> retired_;
};

// Process-wide title host region table shared between the CPU/xmem shims and the Vulkan backend.
inline TitleHostRegions& title_host_regions() {
    static TitleHostRegions instance;
    return instance;
}

} // namespace leonida
