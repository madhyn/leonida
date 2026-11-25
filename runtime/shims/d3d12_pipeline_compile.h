// Timing and scheduling for Vulkan pipeline creation on the D3D12 device.
#pragma once

#include "../src/vulkan_replay.h"
#include <windows.h>
#include <algorithm>
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
// checkpoint waits until no other job is running. No Vulkan call runs with the queue lock held.
//
// Lifetime. A job names its owner (the PSO it builds, or the device for a cache checkpoint) and the device whose
// Vulkan objects it uses, and holds no reference to either: a reference kept the device alive past the title's last
// Release until the worker reached the job, so a released device's builds ran on (a test that then destroyed its
// VkDevice crashed).
//   - A PSO's last Release does not wait for its running build, which can be a dispatch-worker build of a module the
//     driver takes minutes over: after_running() drops its queued jobs and hands the PSO's deletion to the worker,
//     to run when that build returns (PipelineState::last_release). The PSO lets its device reference go at once.
//   - A device's teardown waits: cancel() drops its queued jobs (by owner or device) and waits for its running ones
//     and what runs after them, so an in-flight vkCreate*Pipelines finishes before the device's objects go.
//   - The workers run while a device is alive (retain/release): the last device's release stops and joins them.
//   - stop_for_exit() is for a process that ends with its device alive (main.cpp: the title's exit, a console Ctrl+C
//     or close). After it nothing more is queued: submit() refuses, and the callers build in place or not at all.
class PipelineCompiler {
public:
    static PipelineCompiler& instance() {
        static auto* pool = new PipelineCompiler;  // never destroyed: static destruction must not race the workers
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

    struct Stats {
        std::uint64_t submitted = 0;
        std::uint64_t completed = 0;  // jobs the workers ran
        std::uint64_t dropped = 0;    // queued jobs thrown away by cancel, after_running, release or stop_for_exit
        std::uint64_t refused = 0;    // submissions after stop_for_exit
        std::uint64_t waited = 0;     // cancel calls that waited for a running job
        std::uint64_t deferred = 0;   // after_running calls that left something to run after a running job
    };

    // False, with nothing queued, after stop_for_exit.
    bool submit(const void* owner, const void* device, std::size_t priority, std::function<void()> work) {
        std::lock_guard lock(mutex_);
        if (closed_) {
            ++stats_.refused;
            return false;
        }
        if (workers_.empty()) {
            worker_target_ = configured_worker_count();
            const std::uint64_t generation = ++generation_;
            workers_.reserve(worker_target_);
            for (unsigned i = 0; i < worker_target_; ++i)
                workers_.emplace_back([this, generation] { run(generation); });
        }
        jobs_.emplace(priority, Job{owner, device, std::move(work)});
        ++stats_.submitted;
        wake_.notify_one();
        return true;
    }

    bool closed() {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    bool has_compile_waiter() {
        std::lock_guard lock(mutex_);
        return (!jobs_.empty() && jobs_.begin()->first != SIZE_MAX) || compile_running_ > 0;
    }
    // Returns when no job is queued or running. A job owns its pipeline state, and through it the device, until it
    // finishes; a caller that counts references (rt::d3d12_xs::device_proxy_ref_count) waits here first.
    void wait_idle() {
        std::unique_lock wait(mutex_);
        done_.wait(wait, [this] { return jobs_.empty() && running_.empty(); });
    }

    // Drops the queued jobs whose owner or device is `key` and waits for the running ones, with what runs after them
    // (after_running), unless one is on this thread. Jobs a running one queues for `key` meanwhile are dropped too.
    // Returns how many were dropped.
    std::size_t cancel(const void* key) {
        std::vector<Job> dropped;
        {
            std::unique_lock lock(mutex_);
            const auto me = std::this_thread::get_id();
            bool counted = false;
            for (;;) {
                drop_locked(dropped, [key](const Job& job) { return job.owner == key || job.device == key; });
                const bool busy = std::any_of(running_.begin(), running_.end(), [&](const Running& r) {
                    return (r.owner == key || r.device == key) && r.thread != me;
                });
                if (!busy) break;
                if (!counted) ++stats_.waited;
                counted = true;
                done_.wait(lock);
            }
        }
        return dropped.size();  // the dropped jobs' captures are destroyed here, outside the lock
    }

    // Drops the queued jobs filed under `owner`. When one of its jobs is running, `last` is run on the worker after it
    // returns, before the job counts as done (a device's cancel waits for both), and the answer is true; otherwise
    // false, and `last` is not kept. On the worker inside that job itself, `last` runs after the job returns too.
    bool after_running(const void* owner, std::function<void()> last) {
        std::vector<Job> dropped;
        std::lock_guard lock(mutex_);
        drop_locked(dropped, [owner](const Job& job) { return job.owner == owner; });
        for (Running& r : running_) {
            if (r.owner != owner || r.finished) continue;
            r.after = r.after ? [first = std::move(r.after), then = std::move(last)] { first(); then(); }
                              : std::move(last);
            ++stats_.deferred;
            return true;
        }
        return false;
    }

    // True on the worker while it runs a job whose owner or device is `key` (not what runs after it).
    bool running_here(const void* key) {
        std::lock_guard lock(mutex_);
        const auto me = std::this_thread::get_id();
        return std::any_of(running_.begin(), running_.end(), [&](const Running& r) {
            return r.thread == me && !r.finished && (r.owner == key || r.device == key);
        });
    }

    // A device holds the worker while it lives. The last release drops whatever is still queued, stops the worker
    // and joins it; a release on the worker itself lets it return after the job it is in.
    void retain() {
        std::lock_guard lock(mutex_);
        ++users_;
    }
    void release() {
        std::vector<Job> dropped;
        std::vector<std::thread> workers;
        {
            std::lock_guard lock(mutex_);
            if (users_ && --users_) return;
            workers = stop_locked(dropped);
        }
        finish(std::move(workers), true);
    }

    // Process exit with a device still alive: drops the queue, refuses new jobs and waits up to `wait` for running
    // ones, so ExitProcess does not end the workers inside the driver. False when a job was still running.
    bool stop_for_exit(std::chrono::milliseconds wait) {
        std::vector<Job> dropped;
        std::vector<std::thread> workers;
        bool idle = true;
        {
            std::unique_lock lock(mutex_);
            closed_ = true;
            workers = stop_locked(dropped);
            const auto me = std::this_thread::get_id();
            idle = done_.wait_for(lock, wait, [&] {
                return std::none_of(running_.begin(), running_.end(), [&](const Running& r) { return r.thread != me; });
            });
        }
        finish(std::move(workers), idle);
        return idle;
    }

    Stats stats() {
        std::lock_guard lock(mutex_);
        return stats_;
    }
    bool worker_running() {
        std::lock_guard lock(mutex_);
        return !workers_.empty();
    }
    // Tests: called on the worker with each job's owner, once the job counts as running and before its work.
    void set_job_probe(std::function<void(const void* owner)> probe) {
        std::lock_guard lock(mutex_);
        probe_ = std::move(probe);
    }

private:
    struct Job {
        const void* owner = nullptr;
        const void* device = nullptr;
        std::function<void()> work;
    };
    struct Running {
        const void* owner;
        const void* device;
        std::thread::id thread;
        std::function<void()> after;  // after_running
        bool finished = false;        // the job returned; `after` may be running
    };

    template<class Match> void drop_locked(std::vector<Job>& dropped, const Match& match) {
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            if (match(it->second)) {
                dropped.push_back(std::move(it->second));
                it = jobs_.erase(it);
                ++stats_.dropped;
            } else {
                ++it;
            }
        }
    }

    std::vector<std::thread> stop_locked(std::vector<Job>& dropped) {
        drop_locked(dropped, [](const Job&) { return true; });
        ++generation_;  // the workers return when they next look: after any job they are running
        wake_.notify_all();
        return std::move(workers_);
    }

    static void finish(std::vector<std::thread> workers, bool join) {
        const auto me = std::this_thread::get_id();
        for (auto& w : workers) {
            if (!w.joinable()) continue;
            if (join && w.get_id() != me) w.join();
            else w.detach();
        }
    }

    bool can_take_locked(std::size_t priority) const noexcept {
        if (priority == 0) return true;
        if (priority == SIZE_MAX) return running_.empty();
        return worker_target_ <= 1 || speculative_running_ + 1 < worker_target_;
    }

    Running& mine(std::thread::id me) {
        return *std::find_if(running_.begin(), running_.end(), [me](const Running& r) { return r.thread == me; });
    }

    void run(std::uint64_t generation) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const auto me = std::this_thread::get_id();
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock, [&] {
                return generation_ != generation || (!jobs_.empty() && can_take_locked(jobs_.begin()->first));
            });
            if (generation_ != generation) return;
            auto first = jobs_.begin();
            const std::size_t priority = first->first;
            Job job = std::move(first->second);
            jobs_.erase(first);
            if (priority != SIZE_MAX) ++compile_running_;
            if (priority != 0 && priority != SIZE_MAX) ++speculative_running_;
            running_.push_back({job.owner, job.device, me});
            const auto probe = probe_;
            lock.unlock();
            try {
                if (probe) probe(job.owner);
                job.work();
            } catch (...) {
                std::fprintf(stderr, "[rt] pipeline compile worker: a job threw\n");
            }
            job.work = nullptr;  // its captures go before the job counts as done
            lock.lock();
            ++stats_.completed;
            if (priority != SIZE_MAX) --compile_running_;
            if (priority != 0 && priority != SIZE_MAX) --speculative_running_;
            Running& self = mine(me);
            self.finished = true;
            if (std::function<void()> after = std::move(self.after)) {
                lock.unlock();
                try {
                    after();
                } catch (...) {
                    std::fprintf(stderr, "[rt] pipeline compile worker: work after a job threw\n");
                }
                after = nullptr;
                lock.lock();
            }
            running_.erase(std::find_if(running_.begin(), running_.end(), [me](const Running& r) {
                return r.thread == me;
            }));
            wake_.notify_all();
            done_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    std::multimap<std::size_t, Job> jobs_;
    std::vector<Running> running_;  // one per worker; stopped workers may still be finishing one
    std::vector<std::thread> workers_;
    unsigned worker_target_ = 1;
    std::size_t compile_running_ = 0;
    std::size_t speculative_running_ = 0;
    std::uint64_t generation_ = 0;  // workers run while this is the value they started with
    unsigned users_ = 0;            // live devices (retain/release)
    bool closed_ = false;           // stop_for_exit: nothing more is queued
    Stats stats_;
    std::function<void(const void*)> probe_;
};
}  // namespace rt::d3d12
