// Portable unit and concurrency tests for leonida::label::Memory (src/gpu_label_memory.h).
// Covers in-place non-heap region expansion (preserving in-flight GPU completion value writes),
// partial vs entire page release generation invalidation, heap vs non-heap isolation, base-0 GPU arena
// tracking, alignment/overflow bounds, and concurrent track/capture/publish. No Windows/Vulkan/game data.
#include "../src/gpu_label_memory.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

using leonida::label::Memory;
using leonida::label::Target;

bool publish_ok(Memory& mem, const Target& target, std::uintptr_t expected_addr, unsigned expected_bytes,
                std::uintptr_t expected_alloc) {
    bool called = false;
    const bool ok = mem.publish(target, [&](std::uintptr_t addr, unsigned bytes, std::uintptr_t alloc) {
        called = true;
        return addr == expected_addr && bytes == expected_bytes && alloc == expected_alloc;
    });
    return ok && called;
}

bool publish_rejected(Memory& mem, const Target& target) {
    bool called = false;
    const bool ok = mem.publish(target, [&](std::uintptr_t, unsigned, std::uintptr_t) {
        called = true;
        return true;
    });
    return !ok && !called;
}

int test_expand_preserves_inflight_targets() {
    Memory mem;
    constexpr std::uintptr_t kBase = 0x100000;
    constexpr std::uintptr_t kAlloc = 0x100000;
    mem.track(kBase, 0x1000, kAlloc, false);

    const Target t4 = mem.capture(kBase + 0x10, 4);
    const Target t8 = mem.capture(kBase + 0x20, 8);
    EXPECT(t4.owner.lock() != nullptr && t8.owner.lock() != nullptr);
    EXPECT(mem.capture(kBase + 0x1000, 8).owner.lock() == nullptr);

    // Growing the same non-heap region at the same base and allocation_base must update Region::end in place
    // so in-flight Targets remain valid instead of being silently dropped.
    mem.track(kBase, 0x4000, kAlloc, false);
    EXPECT(publish_ok(mem, t4, kBase + 0x10, 4, kAlloc));
    EXPECT(publish_ok(mem, t8, kBase + 0x20, 8, kAlloc));

    // The newly covered tail is immediately capturable and publishable too.
    const Target t_tail = mem.capture(kBase + 0x3ff8, 8);
    EXPECT(publish_ok(mem, t_tail, kBase + 0x3ff8, 8, kAlloc));

    // Partial release bumps generation: pre-release targets become stale, post-release targets survive further
    // in-place expansion while the pre-release stale target remains rejected.
    EXPECT(mem.release_pages(kBase, 0x1000, false, [] { return true; }));
    EXPECT(publish_rejected(mem, t4));
    EXPECT(publish_rejected(mem, t_tail));

    const Target t_gen2 = mem.capture(kBase + 0x10, 8);
    EXPECT(t_gen2.generation == t4.generation + 1);
    mem.track(kBase, 0x8000, kAlloc, false);
    EXPECT(publish_rejected(mem, t4));
    EXPECT(publish_ok(mem, t_gen2, kBase + 0x10, 8, kAlloc));
    return 0;
}

int test_subrange_and_adjacent_page_isolation() {
    Memory mem;
    constexpr std::uintptr_t kBase = 0x200000;
    constexpr std::uintptr_t kAlloc = 0x200000;
    mem.track(kBase, 0x2000, kAlloc, false);
    const Target t0 = mem.capture(kBase + 0x100, 8);

    // Re-tracking a covered subrange does not disturb existing targets.
    mem.track(kBase + 0x1000, 0x1000, kAlloc, false);
    EXPECT(publish_ok(mem, t0, kBase + 0x100, 8, kAlloc));

    // An adjacent region at kBase + 0x2000 stays separate so partially releasing page 2 does not falsely
    // invalidate live targets in [kBase, kBase + 0x2000).
    mem.track(kBase + 0x2000, 0x1000, kAlloc, false);
    const Target t_adj = mem.capture(kBase + 0x2008, 8);
    EXPECT(publish_ok(mem, t_adj, kBase + 0x2008, 8, kAlloc));

    EXPECT(mem.release_pages(kBase + 0x2000, 0x1000, false, [] { return true; }));
    EXPECT(publish_rejected(mem, t_adj));
    EXPECT(publish_ok(mem, t0, kBase + 0x100, 8, kAlloc));
    return 0;
}

int test_allocation_base_and_heap_invalidation() {
    Memory mem;
    constexpr std::uintptr_t kBase = 0x300000;
    mem.track(kBase, 0x1000, kBase, false);
    const Target t_old = mem.capture(kBase + 8, 8);

    // Different allocation_base at the same base represents a new reservation and invalidates prior targets.
    mem.track(kBase, 0x2000, kBase + 0x80000, false);
    EXPECT(publish_rejected(mem, t_old));
    const Target t_new = mem.capture(kBase + 8, 8);
    EXPECT(publish_ok(mem, t_new, kBase + 8, 8, kBase + 0x80000));

    // Failed release callback must leave regions and generations untouched.
    EXPECT(!mem.release_pages(kBase + 0x80000, 0, true, [] { return false; }));
    EXPECT(publish_ok(mem, t_new, kBase + 8, 8, kBase + 0x80000));

    // Heap regions are isolated from release_pages and invalidated by release_heap or re-track.
    constexpr std::uintptr_t kHeap = 0x400000;
    mem.track(kHeap, 0x1000, kBase + 0x80000, true);
    const Target t_heap = mem.capture(kHeap + 16, 4);
    EXPECT(mem.release_pages(kBase + 0x80000, 0, true, [] { return true; }));
    EXPECT(publish_rejected(mem, t_new));
    EXPECT(publish_ok(mem, t_heap, kHeap + 16, 4, kBase + 0x80000));

    mem.release_heap(kHeap);
    EXPECT(publish_rejected(mem, t_heap));
    return 0;
}

int test_arena_base_zero_and_bounds() {
    Memory mem;
    constexpr std::uintptr_t kArenaHost = 0x500000;
    mem.track(0, 0x10000, kArenaHost, false);
    const Target t0 = mem.capture(0x100, 8);
    EXPECT(publish_ok(mem, t0, 0x100, 8, kArenaHost));

    // Expanding base-0 GPU arena preserves in-flight targets.
    mem.track(0, 0x20000, kArenaHost, false);
    EXPECT(publish_ok(mem, t0, 0x100, 8, kArenaHost));
    const Target t_high = mem.capture(0x1fff8, 8);
    EXPECT(publish_ok(mem, t_high, 0x1fff8, 8, kArenaHost));

    // Invalid track and capture calls are rejected cleanly.
    mem.track(0x900000, 0, 0x900000, false);
    mem.track(0x900000, 0x1000, 0, false);
    mem.track(std::numeric_limits<std::uintptr_t>::max() - 0x10, 0x100, 0x900000, false);
    EXPECT(mem.capture(0x900000, 4).owner.lock() == nullptr);
    EXPECT(mem.capture(0x100, 2).owner.lock() == nullptr);
    EXPECT(mem.capture(0x100, 16).owner.lock() == nullptr);
    EXPECT(mem.capture(0x102, 4).owner.lock() == nullptr);
    EXPECT(mem.capture(0x104, 8).owner.lock() == nullptr);
    EXPECT(mem.capture(0x1fffc, 8).owner.lock() == nullptr);  // straddles end (0x20000)
    return 0;
}

int test_concurrent_expand_and_publish() {
    Memory mem;
    constexpr std::uintptr_t kBase = 0x600000;
    constexpr std::uintptr_t kAlloc = 0x600000;
    mem.track(kBase, 0x1000, kAlloc, false);

    std::atomic<bool> failed{false};
    std::thread expander([&] {
        for (std::uintptr_t pages = 1; pages <= 256; ++pages) mem.track(kBase, pages * 0x1000, kAlloc, false);
    });
    std::vector<std::thread> workers;
    for (int w = 0; w < 3; ++w) {
        workers.emplace_back([&, w] {
            const std::uintptr_t addr = kBase + static_cast<std::uintptr_t>(w) * 16;
            for (int i = 0; i < 2000; ++i) {
                const Target t = mem.capture(addr, 8);
                if (!publish_ok(mem, t, addr, 8, kAlloc)) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    expander.join();
    for (auto& worker : workers) worker.join();
    EXPECT(!failed.load());
    return 0;
}

}  // namespace

int main() {
    if (int r = test_expand_preserves_inflight_targets()) return r;
    if (int r = test_subrange_and_adjacent_page_isolation()) return r;
    if (int r = test_allocation_base_and_heap_invalidation()) return r;
    if (int r = test_arena_base_zero_and_bounds()) return r;
    if (int r = test_concurrent_expand_and_publish()) return r;
    std::printf("gpu_label_memory_test: ok\n");
    return 0;
}
