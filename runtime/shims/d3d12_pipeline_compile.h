// Timing and scheduling for Vulkan pipeline creation on the D3D12 device.
#pragma once

#include "../src/vulkan_replay.h"
#include <windows.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace rt::d3d12 {
inline bool trace_pipeline_compile() {
    static const bool on = [] {
        wchar_t v[2]{};
        return GetEnvironmentVariableW(L"LEONIDA_TRACE_PIPELINE_COMPILE", v, 2) == 1 && v[0] == L'1';
    }();
    return on;
}

inline bool async_replay_compile() {
    static const bool on = [] {
        wchar_t v[2]{};
        return GetEnvironmentVariableW(L"LEONIDA_ASYNC_PIPELINES", v, 2) == 1 && v[0] == L'1';
    }();
    return on && !leonida::strict_replay();
}

// Absolute monotonic timestamps let trace readers distinguish time in the ICD from time queued for the worker.
using CompileClock = std::chrono::steady_clock;
inline long long compile_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(CompileClock::now().time_since_epoch()).count();
}

inline long long compile_wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

template<class Build> VkPipeline replay_pipeline(const char* trigger, Build&& get) {
    if (!trace_pipeline_compile()) return get();
    const auto start = compile_us();
    auto handle = get();
    auto elapsed = compile_us() - start;
    if (elapsed >= 1000)
        std::fprintf(stderr, "[rt] pipeline-use trigger=%s tid=%lu us=%lld ready=%d end_ms=%lld\n",
                     trigger, GetCurrentThreadId(), elapsed, int(handle != VK_NULL_HANDLE), compile_wall_ms());
    return handle;
}

struct CompileTrace {
    VkPipelineCreationFeedback feedback{};
    VkPipelineCreationFeedbackCreateInfo chain{VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
                                               nullptr, &feedback, 0, nullptr};
    long long started = 0;
    const char* key = "-";

    template<class Info> void begin(Info& info, bool supported, const char* trigger, const char* shaders,
                                    std::uint32_t variant, std::size_t bytes, const std::string& description_key) {
        if (!trace_pipeline_compile()) return;
        if (!description_key.empty()) key = description_key.c_str();
        if (supported) {
            chain.pNext = info.pNext;
            info.pNext = &chain;
        }
        std::fprintf(stderr, "[rt] pipeline-compile-begin trigger=%s tid=%lu bytes=%zu variant=%u begin_ms=%lld key=%s shaders=%s\n",
                     trigger, GetCurrentThreadId(), bytes, variant, compile_wall_ms(), key, shaders);
        started = compile_us();
    }

    void end(const char* trigger, const char* shaders, std::uint32_t variant, std::size_t bytes, VkResult result) const {
        if (!started) return;
        const auto ended = compile_us();
        const int hit = !(feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) ? -1 :
                        int((feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0);
        std::fprintf(stderr, "[rt] pipeline-compile trigger=%s tid=%lu start_us=%lld us=%lld hit=%d bytes=%zu "
                             "variant=%u result=%d end_ms=%lld key=%s shaders=%s\n", trigger, GetCurrentThreadId(), started,
                             ended - started, hit, bytes, variant, int(result), compile_wall_ms(), key, shaders);
    }
};


// Demanded work has priority zero, speculative work is ordered by module size, and
// an idle checkpoint runs last. With two or more workers (LEONIDA_COMPILE_THREADS, default
// 2..6 by hardware concurrency; 1 keeps the single-worker path), at least one worker is
// reserved so demanded builds never stall behind speculative warmup compiles, and an idle
// checkpoint waits until no compile jobs are running. Jobs own their object until they
// finish. No Vulkan call runs with the queue lock held. The singleton outlives individual
// D3D12 devices.
class PipelineCompiler {
public:
    static PipelineCompiler& instance() {
        static auto* pool = new PipelineCompiler;
        return *pool;
    }
    static unsigned configured_worker_count(unsigned hw = std::thread::hardware_concurrency()) {
        char text[16]{};
        const DWORD n = GetEnvironmentVariableA("LEONIDA_COMPILE_THREADS", text, sizeof(text));
        if (n > 0 && n < sizeof(text)) {
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(text, &end, 10);
            if (end != text && *end == '\0' && parsed >= 1 && parsed <= 32) return static_cast<unsigned>(parsed);
        }
        return hw >= 18 ? 6u : hw >= 12 ? 4u : hw >= 6 ? 3u : 2u;
    }
    explicit PipelineCompiler(unsigned workers = 0)
        : workers_(workers ? workers : configured_worker_count()) {}
    ~PipelineCompiler() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        for (auto& t : threads_)
            if (t.joinable()) t.join();
    }
    PipelineCompiler(const PipelineCompiler&) = delete;
    PipelineCompiler& operator=(const PipelineCompiler&) = delete;

    unsigned worker_count() const noexcept { return workers_; }

    void submit(std::size_t priority, std::function<void()> work) {
        std::lock_guard lock(mutex_);
        if (!started_) {
            threads_.reserve(workers_);
            for (unsigned i = 0; i < workers_; ++i) threads_.emplace_back([this] { run(); });
            started_ = true;
        }
        jobs_.emplace(priority, std::move(work));
        wake_.notify_one();
    }
    bool has_compile_waiter() {
        std::lock_guard lock(mutex_);
        return (!jobs_.empty() && jobs_.begin()->first != SIZE_MAX) || compile_running_ > 0;
    }
    // Returns when no job is queued or running. A job owns its pipeline state, and through it the device, until it
    // finishes; a caller that counts references (rt::d3d12_xs::device_proxy_ref_count) waits here first.
    void wait_idle() {
        std::unique_lock wait(mutex_);
        idle_.wait(wait, [this] { return jobs_.empty() && running_ == 0; });
    }
private:
    bool can_take_locked(std::size_t priority) const noexcept {
        if (priority == 0) return true;
        if (priority == SIZE_MAX) return running_ == 0;
        return workers_ <= 1 || speculative_running_ + 1 < workers_;
    }
    void run() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            std::size_t priority = 0;
            std::function<void()> job;
            {
                std::unique_lock wait(mutex_);
                wake_.wait(wait, [this] {
                    return stopping_ || (!jobs_.empty() && can_take_locked(jobs_.begin()->first));
                });
                if (stopping_) return;
                auto first = jobs_.begin();
                priority = first->first;
                job = std::move(first->second);
                jobs_.erase(first);
                ++running_;
                if (priority != SIZE_MAX) ++compile_running_;
                if (priority != 0 && priority != SIZE_MAX) ++speculative_running_;
            }
            job();
            job = nullptr;  // what the job captured goes before the pool reports itself idle
            {
                std::lock_guard done(mutex_);
                --running_;
                if (priority != SIZE_MAX) --compile_running_;
                if (priority != 0 && priority != SIZE_MAX) --speculative_running_;
            }
            wake_.notify_all();
            idle_.notify_all();
        }
    }
    unsigned workers_ = 1;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::size_t running_ = 0;
    std::size_t compile_running_ = 0;
    std::size_t speculative_running_ = 0;
    std::multimap<std::size_t, std::function<void()>> jobs_;
    std::vector<std::thread> threads_;
    bool started_ = false;
    bool stopping_ = false;
};
}  // namespace rt::d3d12
