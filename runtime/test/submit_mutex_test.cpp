// leonida::SubmitMutex (src/vulkan_device_fault.h), on the CPU: the submit lock's owner and hold tracking, its waiter
// count, let_submit_finish (the handoff a pipeline create makes to submits before entering the driver) and the
// compile gate between pipeline creates and queue submissions, with warm-up creates that do not hold submissions
// (RT-COMPILE-GATE-WARMUP-FREE). Windows only: the owner is a Win32 thread id. No GPU, no game data.
#include "../src/vulkan_device_fault.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

// For checks inside threads, where EXPECT's return would leave the thread, not the test.
std::atomic<bool> failed_void{false};
#define EXPECT_VOID(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); failed_void = true; } \
} while (0)

using leonida::SubmitMutex;
using namespace std::chrono_literals;

std::int64_t now_ms() { return leonida::diagnostic_ms(); }

// Polls `done` for up to two seconds.
template<class Done> bool eventually(Done&& done) {
    for (int i = 0; i < 2000; ++i) {
        if (done()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return done();
}

int owner_tracking() {
    SubmitMutex m;
    EXPECT(m.owner() == 0 && m.waiting() == 0);
    const std::int64_t before = now_ms();
    m.lock();
    EXPECT(m.owner() == GetCurrentThreadId());
    EXPECT(m.since_ms() >= before && m.since_ms() <= now_ms());
    // The holder's own thread: nothing to let through, no wait (it would deadlock on the lock it holds).
    EXPECT(m.let_submit_finish(true) == 0);
    EXPECT(m.let_submit_finish(false) == 0);
    m.unlock();
    EXPECT(m.owner() == 0);
    EXPECT(m.try_lock());
    EXPECT(m.owner() == GetCurrentThreadId());
    m.unlock();
    // Free: no wait at all.
    EXPECT(m.let_submit_finish(true) == 0);
    std::printf("owner and hold tracking, holder's own thread returns at once\n");
    return 0;
}

int waits_for_holder() {
    SubmitMutex m;
    std::atomic<bool> held{false}, released{false};
    std::thread holder([&] {
        m.lock();
        held = true;
        std::this_thread::sleep_for(200ms);
        released = true;
        m.unlock();
    });
    EXPECT(eventually([&] { return held.load(); }));
    EXPECT(m.owner() != 0 && m.owner() != GetCurrentThreadId());
    const std::int64_t waited = m.let_submit_finish(false);
    const bool after_release = released.load();
    holder.join();
    EXPECT(after_release);
    EXPECT(waited >= 100);
    std::printf("let_submit_finish(false) waited %lld ms for another thread's hold\n", static_cast<long long>(waited));
    return 0;
}

int waits_for_queued_submitter() {
    // A holds the lock, B queues behind it in lock(); the worker's handoff returns only after B has had its turn.
    SubmitMutex m;
    std::atomic<bool> a_held{false}, b_done{false};
    std::atomic<std::int64_t> b_released_at{0};
    std::thread a([&] {
        m.lock();
        a_held = true;
        std::this_thread::sleep_for(150ms);
        m.unlock();
    });
    EXPECT(eventually([&] { return a_held.load(); }));
    std::thread b([&] {
        m.lock();
        std::this_thread::sleep_for(100ms);
        b_released_at = now_ms();
        b_done = true;
        m.unlock();
    });
    EXPECT(eventually([&] { return m.waiting() == 1; }));  // B is counted while it waits
    const std::int64_t waited = m.let_submit_finish(true);
    const bool b_had_turn = b_done.load();
    const std::int64_t returned_at = now_ms();
    a.join();
    b.join();
    EXPECT(b_had_turn);
    EXPECT(returned_at >= b_released_at.load());
    EXPECT(m.waiting() == 0 && m.owner() == 0);
    std::printf("let_submit_finish(true) let a queued submitter go first (%lld ms)\n", static_cast<long long>(waited));
    return 0;
}

int handoff_is_capped() {
    // Two threads keep the lock busy back to back (one always queued): the worker still gets its turn after the cap.
    SubmitMutex m;
    std::atomic<bool> stop{false};
    auto churn = [&] {
        while (!stop) {
            m.lock();
            std::this_thread::sleep_for(10ms);
            m.unlock();
        }
    };
    std::thread a(churn), b(churn);
    EXPECT(eventually([&] { return m.owner() != 0 && m.waiting() != 0; }));
    const std::int64_t waited = m.let_submit_finish(true, 100);
    stop = true;
    a.join();
    b.join();
    std::printf("capped handoff: %lld ms\n", static_cast<long long>(waited));
    EXPECT(waited >= 90 && waited < 600);
    std::printf("let_submit_finish(true, 100) under a steady stream of submits returned after %lld ms\n",
                static_cast<long long>(waited));
    return 0;
}

int compile_gate() {
    if (!SubmitMutex::compile_gate_enabled()) {
        std::printf("compile gate off (LEONIDA_COMPILE_GATE=0): not checked\n");
        return 0;
    }
    SubmitMutex m;
    // Nothing in the driver: a submission passes at once.
    {
        const std::int64_t start = now_ms();
        const SubmitMutex::SubmitTurn turn(m);
        EXPECT(now_ms() - start < 50);
    }
    // A submission waits for the create in the driver; a create arriving meanwhile waits behind the submission.
    std::atomic<bool> creating{false};
    std::atomic<std::int64_t> create_left_at{0}, submit_entered_at{0}, submit_left_at{0}, second_entered_at{0};
    std::atomic<std::int64_t> second_waited{-1};
    std::thread create([&] {
        const SubmitMutex::CompileTurn turn(m);
        creating = true;
        std::this_thread::sleep_for(250ms);
        create_left_at = now_ms();
    });
    EXPECT(eventually([&] { return creating.load(); }));
    std::thread submit([&] {
        m.lock();  // as a submitter does: the submit lock, then the gate around vkQueueSubmit
        {
            const SubmitMutex::SubmitTurn turn(m);
            submit_entered_at = now_ms();
            std::this_thread::sleep_for(100ms);
            submit_left_at = now_ms();
        }
        m.unlock();
    });
    EXPECT(eventually([&] { return m.gate_waiter() != 0; }));  // waiting at the gate, with the submit lock held
    EXPECT(m.owner() == m.gate_waiter());
    std::thread second([&] {
        const SubmitMutex::CompileTurn turn(m);
        second_entered_at = now_ms();
        second_waited = turn.waited_ms();
    });
    create.join();
    submit.join();
    second.join();
    EXPECT(submit_entered_at.load() >= create_left_at.load());   // never in the driver together with a create
    EXPECT(second_entered_at.load() >= submit_left_at.load());   // the waiting submission went first
    EXPECT(second_waited.load() >= 50);
    EXPECT(m.gate_waiter() == 0);
    const SubmitMutex::GateStats stats = m.gate_stats();
    EXPECT(stats.submits == 1 && stats.submit_max_ms >= 100);
    EXPECT(stats.creates == 1 && stats.create_max_ms >= 50);
    std::printf("compile gate: submission waited %lld ms for a create, a later create waited %lld ms behind it\n",
                static_cast<long long>(stats.submit_max_ms), static_cast<long long>(stats.create_max_ms));
    // Creates share the gate.
    {
        const SubmitMutex::CompileTurn first(m);
        std::atomic<std::int64_t> other_waited{-1};
        std::thread other([&] {
            const SubmitMutex::CompileTurn turn(m);
            other_waited = turn.waited_ms();
        });
        other.join();
        EXPECT(other_waited.load() == 0);
    }
    std::printf("compile gate: creates pass it together\n");
    return 0;
}

// Warm-up creates (CompileTurn speculative) do not hold submissions; demanded ones still do; at most warmup_max warm-ups
// are in the driver at once; none enters while a submission waits or is in the driver; with warm-ups held
// (LEONIDA_COMPILE_GATE_WARMUPS=1) a submission waits for them as before.
int warmups_do_not_hold_the_gate() {
    if (!SubmitMutex::compile_gate_enabled()) {
        std::printf("warm-up gate: compile gate off (LEONIDA_COMPILE_GATE=0): not checked\n");
        return 0;
    }
    SubmitMutex m;
    m.set_warmup_policy({true, 1});
    // A submission is not held by a warm-up in the driver.
    {
        std::atomic<bool> in{false}, release{false};
        std::thread warm([&] {
            const SubmitMutex::CompileTurn turn(m, true);
            in = true;
            while (!release) std::this_thread::sleep_for(1ms);
        });
        EXPECT(eventually([&] { return in.load(); }));
        EXPECT(m.speculative_compiles() == 1);
        const std::int64_t start = now_ms();
        {
            const SubmitMutex::SubmitTurn turn(m);
        }
        EXPECT(now_ms() - start < 50);
        release = true;
        warm.join();
        EXPECT(m.speculative_compiles() == 0);
    }
    // A demanded create is still waited for, also beside a warm-up: the submission waits for the demanded one only.
    {
        std::atomic<bool> warm_in{false}, demand_in{false}, release_warm{false};
        std::atomic<std::int64_t> demand_left{0}, submitted{0};
        std::thread warm([&] {
            const SubmitMutex::CompileTurn turn(m, true);
            warm_in = true;
            while (!release_warm) std::this_thread::sleep_for(1ms);
        });
        std::thread demand([&] {
            const SubmitMutex::CompileTurn turn(m);
            demand_in = true;
            std::this_thread::sleep_for(200ms);
            demand_left = now_ms();
        });
        EXPECT(eventually([&] { return warm_in.load() && demand_in.load(); }));
        std::thread submit([&] {
            const SubmitMutex::SubmitTurn turn(m);
            submitted = now_ms();
        });
        submit.join();
        demand.join();
        EXPECT(submitted.load() >= demand_left.load());  // waited for the demanded create
        EXPECT(warm_in.load() && !release_warm.load());  // and not for the warm-up, still in the driver
        release_warm = true;
        warm.join();
    }
    // At most warmup_max warm-ups in the driver; a demanded create does not wait for them.
    for (const unsigned max : {1u, 2u}) {
        m.set_warmup_policy({true, max});
        std::atomic<int> inside{0}, most{0};
        std::atomic<bool> release{false};
        std::vector<std::thread> warms;
        for (int i = 0; i < 4; ++i)
            warms.emplace_back([&] {
                const SubmitMutex::CompileTurn turn(m, true);
                const int now = ++inside;
                int seen = most.load();
                while (now > seen && !most.compare_exchange_weak(seen, now)) {
                }
                while (!release) std::this_thread::sleep_for(1ms);
                --inside;
            });
        EXPECT(eventually([&] { return inside.load() == int(max); }));
        std::this_thread::sleep_for(100ms);
        EXPECT(inside.load() == int(max));  // the others wait for a place
        std::atomic<std::int64_t> demand_waited{-1};
        std::thread demand([&] {
            const SubmitMutex::CompileTurn turn(m);
            demand_waited = turn.waited_ms();
        });
        demand.join();
        EXPECT(demand_waited.load() == 0);
        release = true;
        for (auto& t : warms) t.join();
        EXPECT(most.load() == int(max));
        EXPECT(m.speculative_compiles() == 0);
    }
    m.set_warmup_policy({true, 1});
    // A warm-up never enters while a submission is in the driver.
    {
        std::atomic<bool> submitting{false}, release{false};
        std::atomic<std::int64_t> submit_left{0}, warm_entered{0};
        std::thread submit([&] {
            const SubmitMutex::SubmitTurn turn(m);
            submitting = true;
            while (!release) std::this_thread::sleep_for(1ms);
            submit_left = now_ms();
        });
        EXPECT(eventually([&] { return submitting.load(); }));
        std::thread warm([&] {
            const SubmitMutex::CompileTurn turn(m, true);
            warm_entered = now_ms();
        });
        std::this_thread::sleep_for(100ms);
        EXPECT(warm_entered.load() == 0);
        release = true;
        submit.join();
        warm.join();
        EXPECT(warm_entered.load() >= submit_left.load());
    }
    // LEONIDA_COMPILE_GATE_WARMUPS=1: a warm-up holds submissions as a demanded create does.
    {
        m.set_warmup_policy({false, 1});
        std::atomic<bool> in{false};
        std::atomic<std::int64_t> warm_left{0}, submitted{0};
        std::thread warm([&] {
            const SubmitMutex::CompileTurn turn(m, true);
            EXPECT_VOID(!turn.speculative());
            in = true;
            std::this_thread::sleep_for(150ms);
            warm_left = now_ms();
        });
        EXPECT(eventually([&] { return in.load(); }));
        EXPECT(m.speculative_compiles() == 0);
        {
            const SubmitMutex::SubmitTurn turn(m);
            submitted = now_ms();
        }
        warm.join();
        EXPECT(submitted.load() >= warm_left.load());
    }
    // The settings.
    _putenv_s("LEONIDA_COMPILE_GATE_WARMUPS", "1");
    _putenv_s("LEONIDA_COMPILE_GATE_WARMUP_MAX", "3");
    auto policy = SubmitMutex::warmup_policy_from_environment();
    EXPECT(!policy.free && policy.max == 3);
    _putenv_s("LEONIDA_COMPILE_GATE_WARMUPS", "0");
    _putenv_s("LEONIDA_COMPILE_GATE_WARMUP_MAX", "0");
    policy = SubmitMutex::warmup_policy_from_environment();
    EXPECT(policy.free && policy.max == 1);
    _putenv_s("LEONIDA_COMPILE_GATE_WARMUP_MAX", "x");
    EXPECT(SubmitMutex::warmup_policy_from_environment().max == 1);
    _putenv_s("LEONIDA_COMPILE_GATE_WARMUPS", "");
    _putenv_s("LEONIDA_COMPILE_GATE_WARMUP_MAX", "");
    policy = SubmitMutex::warmup_policy_from_environment();
    EXPECT(policy.free && policy.max == 1);
    if (failed_void) return 1;
    std::printf("warm-up gate: submissions pass a warm-up, wait for demanded creates; warm-ups capped\n");
    return 0;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (owner_tracking() || waits_for_holder() || waits_for_queued_submitter() || handoff_is_capped() ||
        compile_gate() || warmups_do_not_hold_the_gate())
        return 1;
    std::printf("submit_mutex_test: all passed\n");
    return 0;
}
