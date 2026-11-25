// Unit tests for rt::d3d12::PipelineCompiler (shims/d3d12_pipeline_compile.h):
// parallel demanded builds, demand-lane reservation against speculative warmup,
// idle checkpoint ordering, single-worker priority order, and LEONIDA_COMPILE_THREADS parsing.
#include "d3d12_pipeline_compile.h"

#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <vector>

namespace {

int g_failed = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok) ++g_failed;
}

void test_parallel_demand() {
    rt::d3d12::PipelineCompiler pool(2);
    std::mutex m;
    std::condition_variable cv;
    int arrived = 0;
    std::atomic<int> completed{0};
    auto barrier_job = [&] {
        std::unique_lock lock(m);
        ++arrived;
        cv.notify_all();
        cv.wait(lock, [&] { return arrived == 2; });
        completed.fetch_add(1, std::memory_order_relaxed);
    };
    pool.submit(0, barrier_job);
    pool.submit(0, barrier_job);
    pool.wait_idle();
    check(completed.load() == 2, "two demanded jobs run concurrently on a 2-worker pool");
}

void test_demand_lane_reserved() {
    rt::d3d12::PipelineCompiler pool(3);
    std::mutex m;
    std::condition_variable cv;
    int warmup_started = 0;
    bool release_warmup = false;
    std::atomic<bool> third_warmup_started{false};
    std::atomic<bool> demand_finished{false};

    auto blocking_warmup = [&] {
        std::unique_lock lock(m);
        ++warmup_started;
        cv.notify_all();
        cv.wait(lock, [&] { return release_warmup; });
    };
    pool.submit(1024, blocking_warmup);
    pool.submit(2048, blocking_warmup);
    pool.submit(4096, [&] { third_warmup_started.store(true, std::memory_order_release); });

    {
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return warmup_started == 2; });
    }
    std::mutex dm;
    std::condition_variable dcv;
    bool demand_done = false;
    pool.submit(0, [&] {
        demand_finished.store(true, std::memory_order_release);
        std::lock_guard lock(dm);
        demand_done = true;
        dcv.notify_all();
    });
    {
        std::unique_lock lock(dm);
        dcv.wait(lock, [&] { return demand_done; });
    }
    check(demand_finished.load(std::memory_order_acquire),
          "demanded job completes immediately on the reserved lane while warmup fills N-1 workers");
    check(!third_warmup_started.load(std::memory_order_acquire),
          "speculative warmup stays capped at N-1 concurrent jobs");

    {
        std::lock_guard lock(m);
        release_warmup = true;
    }
    cv.notify_all();
    pool.wait_idle();
    check(third_warmup_started.load(std::memory_order_acquire),
          "queued speculative warmup finishes once earlier warmup jobs release");
}

void test_checkpoint_waits_for_idle() {
    rt::d3d12::PipelineCompiler pool(2);
    std::mutex m;
    std::condition_variable cv;
    bool compile_started = false;
    bool allow_compile_exit = false;
    std::atomic<bool> compile_done{false};
    std::atomic<bool> checkpoint_saw_compile_done{false};

    pool.submit(0, [&] {
        {
            std::lock_guard lock(m);
            compile_started = true;
        }
        cv.notify_all();
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return allow_compile_exit; });
        compile_done.store(true, std::memory_order_release);
    });

    {
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return compile_started; });
    }
    check(pool.has_compile_waiter(), "has_compile_waiter is true while a compile job is running");
    pool.submit(SIZE_MAX, [&] {
        checkpoint_saw_compile_done.store(compile_done.load(std::memory_order_acquire), std::memory_order_release);
    });
    {
        std::lock_guard lock(m);
        allow_compile_exit = true;
    }
    cv.notify_all();
    pool.wait_idle();
    check(checkpoint_saw_compile_done.load(std::memory_order_acquire),
          "idle checkpoint (SIZE_MAX) runs only after active compile jobs finish");
    check(!pool.has_compile_waiter(), "has_compile_waiter is false once the pool is idle");
}

void test_single_worker_strict_order() {
    rt::d3d12::PipelineCompiler pool(1);
    std::mutex m;
    std::condition_variable cv;
    bool gate_entered = false;
    bool release_gate = false;
    std::vector<std::size_t> order;

    pool.submit(0, [&] {
        {
            std::lock_guard lock(m);
            gate_entered = true;
        }
        cv.notify_all();
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return release_gate; });
    });
    {
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return gate_entered; });
    }
    pool.submit(SIZE_MAX, [&] { order.push_back(SIZE_MAX); });
    pool.submit(4096, [&] { order.push_back(4096); });
    pool.submit(512, [&] { order.push_back(512); });
    pool.submit(0, [&] { order.push_back(0); });
    {
        std::lock_guard lock(m);
        release_gate = true;
    }
    cv.notify_all();
    pool.wait_idle();
    const bool exact = order.size() == 4 && order[0] == 0 && order[1] == 512 && order[2] == 4096 &&
                       order[3] == SIZE_MAX;
    check(exact, "single-worker mode drains priority 0, smallest warmup, then SIZE_MAX checkpoint");
}

void test_worker_count_config() {
    SetEnvironmentVariableA("LEONIDA_COMPILE_THREADS", nullptr);
    check(rt::d3d12::PipelineCompiler::configured_worker_count(4) == 2, "hw=4 defaults to 2 workers");
    check(rt::d3d12::PipelineCompiler::configured_worker_count(8) == 3, "hw=8 defaults to 3 workers");
    check(rt::d3d12::PipelineCompiler::configured_worker_count(12) == 4, "hw=12 defaults to 4 workers");
    check(rt::d3d12::PipelineCompiler::configured_worker_count(20) == 6, "hw=20 defaults to 6 workers");

    SetEnvironmentVariableA("LEONIDA_COMPILE_THREADS", "1");
    check(rt::d3d12::PipelineCompiler::configured_worker_count(20) == 1, "LEONIDA_COMPILE_THREADS=1 selects 1 worker");
    SetEnvironmentVariableA("LEONIDA_COMPILE_THREADS", "8");
    check(rt::d3d12::PipelineCompiler::configured_worker_count(4) == 8, "LEONIDA_COMPILE_THREADS=8 selects 8 workers");
    SetEnvironmentVariableA("LEONIDA_COMPILE_THREADS", "0");
    check(rt::d3d12::PipelineCompiler::configured_worker_count(12) == 4, "LEONIDA_COMPILE_THREADS=0 falls back to default");
    SetEnvironmentVariableA("LEONIDA_COMPILE_THREADS", "99");
    check(rt::d3d12::PipelineCompiler::configured_worker_count(12) == 4, "LEONIDA_COMPILE_THREADS=99 falls back to default");
    SetEnvironmentVariableA("LEONIDA_COMPILE_THREADS", "4abc");
    check(rt::d3d12::PipelineCompiler::configured_worker_count(12) == 4, "LEONIDA_COMPILE_THREADS=4abc falls back to default");
    SetEnvironmentVariableA("LEONIDA_COMPILE_THREADS", nullptr);
}

}  // namespace

int main() {
    test_worker_count_config();
    test_parallel_demand();
    test_demand_lane_reserved();
    test_checkpoint_waits_for_idle();
    test_single_worker_strict_order();
    std::printf("pipeline_compiler: %s\n", g_failed ? "FAILED" : "all checks passed");
    return g_failed ? 1 : 0;
}
