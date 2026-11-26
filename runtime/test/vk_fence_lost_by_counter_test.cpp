// A device loss the driver shows only through a timeline counter (runtime/src/vulkan_backend.h, VulkanFence::completed):
// vkGetSemaphoreCounterValue succeeds with UINT64_MAX and no call has returned VK_ERROR_DEVICE_LOST yet.
//   1. UINT64_MAX on a fence no Signal asked that value of: the context is marked lost and the loss is said once,
//      without vkDeviceWaitIdle (which can block for minutes under the submit lock on such a device).
//   2. A second read on the lost device asks the driver nothing and says nothing more.
//   3. UINT64_MAX on a fence a Signal asked that value of is the value: no loss, nothing said, no vkDeviceWaitIdle.
//   4. An ordinary counter value never reaches vkDeviceWaitIdle.
//   5. The reading does not take the submit lock: a thread holding it (the releaser, mid-submission) cannot stall it.
// The driver functions are interposed on a real device's table. Skips with exit code 77 when no Vulkan 1.3 device is
// available. No game data.
#include "vk_test_device.h"
#include "../src/vulkan_backend.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); std::_Exit(1); } \
} while (0)

namespace {

namespace driver {
PFN_vkGetDeviceProcAddr real_get_device = nullptr;
PFN_vkGetSemaphoreCounterValue real_get_counter = nullptr;

std::atomic<bool> counter_reads_max{false};
std::atomic<bool> idle_reports_loss{false};
std::atomic<unsigned> idle_calls{0};
std::atomic<unsigned> counter_calls{0};

VKAPI_ATTR VkResult VKAPI_CALL get_semaphore_counter_value(VkDevice device, VkSemaphore sem, std::uint64_t* value) {
    counter_calls.fetch_add(1, std::memory_order_relaxed);
    if (counter_reads_max.load(std::memory_order_acquire)) {
        *value = UINT64_MAX;
        return VK_SUCCESS;
    }
    return real_get_counter(device, sem, value);
}

VKAPI_ATTR VkResult VKAPI_CALL device_wait_idle(VkDevice) {
    idle_calls.fetch_add(1, std::memory_order_relaxed);
    return idle_reports_loss.load(std::memory_order_acquire) ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_device(VkDevice device, const char* name) {
    const std::string_view n = name;
    if (n == "vkGetSemaphoreCounterValue") return reinterpret_cast<PFN_vkVoidFunction>(get_semaphore_counter_value);
    if (n == "vkDeviceWaitIdle") return reinterpret_cast<PFN_vkVoidFunction>(device_wait_idle);
    return real_get_device(device, name);
}

void install(PFN_vkGetDeviceProcAddr device_proc, VkDevice device) {
    real_get_device = device_proc;
    real_get_counter =
        reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(device_proc(device, "vkGetSemaphoreCounterValue"));
}
}  // namespace driver

}  // namespace

int main() {
    using namespace leonida;

    vktest::Device d;
    if (!d.create(false)) {
        std::fprintf(stderr, "vk_fence_lost_by_counter_test: no Vulkan 1.3 device, skipped\n");
        return 77;
    }
    driver::install(d.device_proc, d.device);

    {   // 4, then 3: an ordinary value, then UINT64_MAX on a fence a Signal asked that value of
        VulkanContext context(driver::get_device, d.device, true);
        EXPECT(context.loaded());
        VulkanFence fence(context, 7);
        const unsigned lost_before = device_lost_reports.load();
        EXPECT(fence.completed() == 7);
        EXPECT(driver::idle_calls.load() == 0);

        context.holds().signaled(fence.semaphore(), UINT64_MAX);  // what a queue's Signal(UINT64_MAX) records
        driver::counter_reads_max.store(true, std::memory_order_release);
        EXPECT(fence.completed() == UINT64_MAX);    // the value asked for; the layer adds nothing to it
        driver::counter_reads_max.store(false, std::memory_order_release);
        EXPECT(driver::idle_calls.load() == 0);
        EXPECT(!context.is_device_lost());
        EXPECT(device_lost_reports.load() == lost_before);
    }
    {   // 1 and 2: UINT64_MAX that no Signal asked for is a loss, decided without vkDeviceWaitIdle
        VulkanContext context(driver::get_device, d.device, true);
        EXPECT(context.loaded());
        VulkanFence fence(context, 0);
        const unsigned lost_before = device_lost_reports.load();
        context.holds().signaled(fence.semaphore(), 5);  // a Signal below UINT64_MAX was submitted

        driver::counter_reads_max.store(true, std::memory_order_release);
        driver::idle_reports_loss.store(true, std::memory_order_release);
        EXPECT(fence.completed() == UINT64_MAX);
        EXPECT(context.is_device_lost());
        EXPECT(driver::idle_calls.load() == 0);
        EXPECT(device_lost_reports.load() == lost_before + 1);

        const unsigned counter_before = driver::counter_calls.load();
        EXPECT(fence.completed() == UINT64_MAX);    // lost: answered without asking the driver again
        context.mark_device_lost();
        EXPECT(driver::counter_calls.load() == counter_before);
        EXPECT(driver::idle_calls.load() == 0);
        EXPECT(device_lost_reports.load() == lost_before + 1);
        driver::counter_reads_max.store(false, std::memory_order_release);
        driver::idle_reports_loss.store(false, std::memory_order_release);
    }
    {   // 5: with the submit lock held by another thread, the reading still answers and marks the loss
        VulkanContext context(driver::get_device, d.device, true);
        EXPECT(context.loaded());
        VulkanFence fence(context, 3);
        std::atomic<bool> locked{false}, release{false};
        std::thread holder([&] {
            std::lock_guard lock(context.submit_mutex());
            locked.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        });
        while (!locked.load(std::memory_order_acquire)) std::this_thread::yield();
        driver::counter_reads_max.store(true, std::memory_order_release);
        EXPECT(fence.completed() == UINT64_MAX);
        driver::counter_reads_max.store(false, std::memory_order_release);
        EXPECT(context.is_device_lost());
        EXPECT(driver::idle_calls.load() == 0);
        release.store(true, std::memory_order_release);
        holder.join();
    }
    std::puts("vk_fence_lost_by_counter: ok");
    return 0;
}
