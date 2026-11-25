// Portable unit and concurrency stress tests for leonida::QueueHolds (src/queue_holds.h).
// Covers Wait-before-Signal chains, barrier ordering, exception safety (no stuck per-thread re-entrancy),
// independent nested QueueHolds instances, detach/cancel_after synchronization, and high-contention pump handoff
// with no lost wakeups. No Vulkan, no Windows headers, no game data.
#include "../src/queue_holds.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

using Holds = leonida::QueueHolds<std::uint64_t>;

struct TestHolder final : public Holds::Holder {
    struct Op {
        std::uint64_t fence = 0;
        std::uint64_t wait_value = 0;
        std::uint64_t seq = 0;
        std::function<void()> on_release;
    };

    explicit TestHolder(Holds& h) : holds(h) {}

    void hold(std::uint64_t fence, std::uint64_t wait_value, std::function<void()> fn) {
        std::lock_guard lock(mutex);
        const std::uint64_t s = holds.next_seq();
        ops.push_back({fence, wait_value, s, std::move(fn)});
        holds.held_changed(1);
    }

    bool release_ready() override {
        bool released = false;
        for (;;) {
            Op op;
            {
                std::lock_guard lock(mutex);
                if (ops.empty() || holds.pending(ops.front().fence, ops.front().wait_value)) return released;
                op = std::move(ops.front());
                ops.pop_front();
                holds.held_changed(-1);
            }
            if (op.on_release) op.on_release();
            released = true;
        }
    }

    void discard_waiting_on(std::uint64_t semaphore) override {
        std::vector<Op> dropped;
        {
            std::lock_guard lock(mutex);
            for (auto it = ops.begin(); it != ops.end();) {
                if (it->fence == semaphore) {
                    dropped.push_back(std::move(*it));
                    it = ops.erase(it);
                    holds.held_changed(-1);
                } else {
                    ++it;
                }
            }
        }
        discarded.fetch_add(dropped.size(), std::memory_order_relaxed);
        if (on_discard) on_discard(semaphore, dropped.size());
    }

    std::uint64_t oldest_held_seq() override {
        std::lock_guard lock(mutex);
        return ops.empty() ? 0 : ops.front().seq;
    }

    std::size_t pending_count() const {
        std::lock_guard lock(mutex);
        return ops.size();
    }

    Holds& holds;
    mutable std::mutex mutex;
    std::deque<Op> ops;
    std::atomic<std::size_t> discarded{0};
    std::function<void(std::uint64_t, std::size_t)> on_discard;
};

int test_wait_before_signal_chain_and_barriers() {
    Holds holds;
    TestHolder queue_b(holds), queue_c(holds);
    holds.attach(&queue_b);
    holds.attach(&queue_c);

    constexpr std::uint64_t kFence1 = 1, kFence2 = 2, kUnknown = 99;
    holds.add_fence(kFence1, 0);
    holds.add_fence(kFence2, 0);
    EXPECT(holds.pending(kFence1, 1));
    EXPECT(!holds.pending(kUnknown, 10));
    EXPECT(holds.would_signal(kUnknown, 10));

    std::vector<int> log;
    queue_b.hold(kFence1, 1, [&] {
        log.push_back(1);
        holds.signaled(kFence2, 1);
        holds.pump_ready();  // re-entrant pump from inside release_ready
    });
    queue_c.hold(kFence2, 1, [&] { log.push_back(2); });

    const int owner = 0;
    holds.after_held(&owner, [&] { log.push_back(3); });
    holds.after_held(&owner, [&] { log.push_back(4); });

    // Nothing ready yet.
    holds.pump_ready();
    EXPECT(log.empty());
    EXPECT(holds.any_held());

    // Satisfy fence 1: cascades queue B -> fence 2 -> queue C -> both barriers in order.
    EXPECT(holds.would_signal(kFence1, 1));
    holds.signaled(kFence1, 1);
    EXPECT(!holds.would_signal(kFence1, 1));
    EXPECT(holds.would_signal(kFence1, 2));
    holds.pump_ready();

    EXPECT(!holds.any_held());
    EXPECT((log == std::vector<int>{1, 2, 3, 4}));

    holds.detach(&queue_c);
    holds.detach(&queue_b);
    return 0;
}

int test_exception_safety_does_not_wedge_thread() {
    Holds holds;
    TestHolder holder(holds);
    holds.attach(&holder);

    constexpr std::uint64_t kFence1 = 10, kFence2 = 11, kFence3 = 12;
    holds.add_fence(kFence1, 0);
    holds.add_fence(kFence2, 0);
    holds.add_fence(kFence3, 0);

    // 1. Exception from holder->release_ready() must not leave in_pump() or pump_active_ stuck on this thread.
    int ran = 0;
    holder.hold(kFence1, 1, [&] { throw std::runtime_error("release failure"); });
    holder.hold(kFence1, 1, [&] { ++ran; });
    holds.signaled(kFence1, 1);
    bool caught = false;
    try {
        holds.pump_ready();
    } catch (const std::runtime_error&) {
        caught = true;
    }
    EXPECT(caught);
    EXPECT(ran == 0);
    holds.pump_ready();  // must run on the exact same thread and drain the second held op
    EXPECT(ran == 1 && !holds.any_held());

    // 2. Exception from a Barrier callback must drain sibling ready barriers, rethrow the first error, and not leave
    // in_pump() or pump_active_ stuck.
    const int owner = 0;
    int barrier_ran = 0;
    holder.hold(kFence2, 1, [&] { ++ran; });
    holds.after_held(&owner, [&] { throw std::runtime_error("barrier failure"); });
    holds.after_held(&owner, [&] { ++barrier_ran; });
    holds.signaled(kFence2, 1);
    caught = false;
    try {
        holds.pump_ready();
    } catch (const std::runtime_error&) {
        caught = true;
    }
    EXPECT(caught);
    EXPECT(ran == 2 && barrier_ran == 1);
    int later_barrier_ran = 0;
    holds.after_held(&owner, [&] { ++later_barrier_ran; });
    EXPECT(later_barrier_ran == 1);

    // 3. Exception from discard_waiting_on() in remove_fence() must not leave in_pump() stuck.
    bool should_throw = true;
    holder.on_discard = [&](std::uint64_t, std::size_t) {
        if (should_throw) {
            should_throw = false;
            throw std::runtime_error("discard failure");
        }
    };
    holder.hold(kFence3, 1, [] {});
    caught = false;
    try {
        holds.remove_fence(kFence3);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    EXPECT(caught);
    EXPECT(holder.discarded.load() == 1);

    // Subsequent remove_fence and pump_ready on the same thread must both still execute.
    constexpr std::uint64_t kFence4 = 13, kFence5 = 14;
    holds.add_fence(kFence4, 0);
    holds.add_fence(kFence5, 0);
    holder.hold(kFence4, 1, [] {});
    holds.remove_fence(kFence4);
    EXPECT(holder.discarded.load() == 2 && !holds.any_held());

    int after_remove = 0;
    holder.hold(kFence5, 1, [&] { ++after_remove; });
    holds.signaled(kFence5, 1);
    holds.pump_ready();
    EXPECT(after_remove == 1 && !holds.any_held());

    holds.detach(&holder);
    return 0;
}

int test_concurrent_handoff_preserved_on_throw() {
    // 1. Thread A is inside holder_a->release_ready(); thread B registers an after_held callback and returns via
    // pump_active_ (setting dirty_); thread A then throws. Both the exception and thread B's callback must complete
    // without requiring a separate pump_ready() call.
    {
        Holds holds;
        TestHolder holder_a(holds);
        holds.attach(&holder_a);

        constexpr std::uint64_t kFenceA = 70;
        holds.add_fence(kFenceA, 0);

        std::atomic<bool> entered{false}, release_throw{false};
        holder_a.hold(kFenceA, 1, [&] {
            entered.store(true, std::memory_order_release);
            while (!release_throw.load(std::memory_order_acquire)) std::this_thread::yield();
            throw std::runtime_error("holder handoff throw");
        });
        holds.signaled(kFenceA, 1);

        std::atomic<bool> caught{false};
        std::thread thread_a([&] {
            try {
                holds.pump_ready();
            } catch (const std::runtime_error&) {
                caught.store(true, std::memory_order_release);
            }
        });
        while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();

        const int owner = 0;
        std::atomic<int> cb_count{0};
        holds.after_held(&owner, [&] { cb_count.fetch_add(1, std::memory_order_relaxed); });
        EXPECT(cb_count.load() == 0);  // handed off to thread A via dirty_

        release_throw.store(true, std::memory_order_release);
        thread_a.join();

        EXPECT(caught.load(std::memory_order_acquire));
        EXPECT(cb_count.load() == 1);
        holds.detach(&holder_a);
    }

    // 2. Thread A throws in holder_a->release_ready() after thread B concurrently signaled holder_b and handed off.
    {
        Holds holds;
        TestHolder holder_a(holds), holder_b(holds);
        holds.attach(&holder_a);
        holds.attach(&holder_b);

        constexpr std::uint64_t kFenceA = 71, kFenceB = 72;
        holds.add_fence(kFenceA, 0);
        holds.add_fence(kFenceB, 0);

        std::atomic<bool> entered{false}, release_throw{false};
        holder_a.hold(kFenceA, 1, [&] {
            entered.store(true, std::memory_order_release);
            while (!release_throw.load(std::memory_order_acquire)) std::this_thread::yield();
            throw std::runtime_error("holder_a throw");
        });
        std::atomic<int> b_ran{0};
        holder_b.hold(kFenceB, 1, [&] { b_ran.fetch_add(1, std::memory_order_relaxed); });
        holds.signaled(kFenceA, 1);

        std::atomic<bool> caught{false};
        std::thread thread_a([&] {
            try {
                holds.pump_ready();
            } catch (const std::runtime_error&) {
                caught.store(true, std::memory_order_release);
            }
        });
        while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();

        holds.signaled(kFenceB, 1);
        holds.pump_ready();  // sets dirty_ and returns via pump_active_
        EXPECT(b_ran.load() == 0);

        release_throw.store(true, std::memory_order_release);
        thread_a.join();

        EXPECT(caught.load(std::memory_order_acquire));
        EXPECT(b_ran.load() == 1);
        EXPECT(!holds.any_held());
        holds.detach(&holder_b);
        holds.detach(&holder_a);
    }

    // 3. Thread A throws inside a barrier callback after thread B concurrently registers a second barrier.
    {
        Holds holds;
        const int owner = 0;
        std::atomic<bool> entered{false}, release_throw{false}, caught{false};
        std::atomic<int> second_ran{0};

        std::thread thread_a([&] {
            try {
                holds.after_held(&owner, [&] {
                    entered.store(true, std::memory_order_release);
                    while (!release_throw.load(std::memory_order_acquire)) std::this_thread::yield();
                    throw std::runtime_error("barrier_1 throw");
                });
            } catch (const std::runtime_error&) {
                caught.store(true, std::memory_order_release);
            }
        });
        while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();

        holds.after_held(&owner, [&] { second_ran.fetch_add(1, std::memory_order_relaxed); });
        EXPECT(second_ran.load() == 0);

        release_throw.store(true, std::memory_order_release);
        thread_a.join();

        EXPECT(caught.load(std::memory_order_acquire));
        EXPECT(second_ran.load() == 1);
    }

    // 4. A holder that re-entrantly calls pump_ready() and throws without popping must not spin forever, and must
    // still drain sibling holders before rethrowing.
    {
        struct AlwaysThrowingHolder final : public Holds::Holder {
            Holds& holds;
            int calls = 0;
            explicit AlwaysThrowingHolder(Holds& h) : holds(h) {}
            bool release_ready() override {
                ++calls;
                holds.pump_ready();  // re-entrant: sets dirty_ = true
                throw std::runtime_error("always throws");
            }
            void discard_waiting_on(std::uint64_t) override {}
            std::uint64_t oldest_held_seq() override { return 0; }
        };

        Holds holds;
        AlwaysThrowingHolder bad(holds);
        TestHolder good(holds);
        holds.attach(&bad);
        holds.attach(&good);

        constexpr std::uint64_t kFence = 73;
        holds.add_fence(kFence, 0);
        int good_ran = 0;
        good.hold(kFence, 1, [&] { ++good_ran; });
        holds.signaled(kFence, 1);

        bool caught = false;
        try {
            holds.pump_ready();
        } catch (const std::runtime_error&) {
            caught = true;
        }
        EXPECT(caught);
        EXPECT(bad.calls == 1);
        EXPECT(good_ran == 1);
        EXPECT(!holds.any_held());

        holds.detach(&good);
        holds.detach(&bad);
    }
    return 0;
}

int test_nested_distinct_instances_are_independent() {
    Holds holds_a, holds_b;
    TestHolder holder_a(holds_a), holder_b(holds_b);
    holds_a.attach(&holder_a);
    holds_b.attach(&holder_b);

    constexpr std::uint64_t kFenceA = 1, kFenceB = 2, kFenceBDrop = 3;
    holds_a.add_fence(kFenceA, 0);
    holds_b.add_fence(kFenceB, 0);
    holds_b.add_fence(kFenceBDrop, 0);

    int b_released = 0, b_barrier = 0;
    holder_b.hold(kFenceB, 1, [&] { ++b_released; });
    holder_b.hold(kFenceBDrop, 1, [] {});
    const int owner_b = 0;
    holds_b.after_held(&owner_b, [&] { ++b_barrier; });

    // Pumping holds_a triggers holds_b.pump_ready() and holds_b.remove_fence() on the SAME thread.
    // A global thread_local in_pump flag would falsely suppress both calls on holds_b.
    holder_a.hold(kFenceA, 1, [&] {
        holds_b.signaled(kFenceB, 1);
        holds_b.pump_ready();
        holds_b.remove_fence(kFenceBDrop);
    });

    holds_a.signaled(kFenceA, 1);
    holds_a.pump_ready();

    EXPECT(b_released == 1);
    EXPECT(holder_b.discarded.load() == 1);
    EXPECT(b_barrier == 1);
    EXPECT(!holds_a.any_held() && !holds_b.any_held());

    holds_a.detach(&holder_a);
    holds_b.detach(&holder_b);
    return 0;
}

int test_detach_and_cancel_after_synchronize() {
    Holds holds;
    TestHolder holder(holds);
    holds.attach(&holder);

    constexpr std::uint64_t kFence = 42;
    holds.add_fence(kFence, 0);

    std::atomic<bool> in_release{false}, allow_finish{false}, detached{false};
    holder.hold(kFence, 1, [&] {
        in_release.store(true, std::memory_order_release);
        while (!allow_finish.load(std::memory_order_acquire)) std::this_thread::yield();
    });

    const int early_owner = 0, cancelled_owner = 1, kept_owner = 2;
    std::atomic<int> early_runs{0}, cancelled_runs{0}, kept_runs{0};
    holds.after_held(&early_owner, [&] { early_runs.fetch_add(1, std::memory_order_relaxed); });
    holds.cancel_after(&early_owner);  // cancelled while work is still held: must never run
    holds.after_held(&cancelled_owner, [&] { cancelled_runs.fetch_add(1, std::memory_order_relaxed); });
    holds.after_held(&kept_owner, [&] { kept_runs.fetch_add(1, std::memory_order_relaxed); });

    holds.signaled(kFence, 1);
    std::thread pumper([&] { holds.pump_ready(); });
    while (!in_release.load(std::memory_order_acquire)) std::this_thread::yield();

    std::thread detacher([&] {
        holds.detach(&holder);
        detached.store(true, std::memory_order_release);
    });
    // Yield a few times: detach must stay blocked while release_ready() is in flight.
    for (int i = 0; i < 64; ++i) std::this_thread::yield();
    EXPECT(!detached.load(std::memory_order_acquire));

    // Also cancel cancelled_owner concurrently: it waits for the running pump too.
    std::atomic<bool> cancelled{false};
    std::thread canceller([&] {
        holds.cancel_after(&cancelled_owner);
        cancelled.store(true, std::memory_order_release);
    });

    allow_finish.store(true, std::memory_order_release);
    pumper.join();
    detacher.join();
    canceller.join();
    EXPECT(detached.load(std::memory_order_acquire) && cancelled.load(std::memory_order_acquire));
    EXPECT(early_runs.load() == 0 && kept_runs.load() == 1);
    return 0;
}

int test_multithreaded_pump_handoff_no_lost_wakeups() {
    Holds holds;
    TestHolder holder_a(holds), holder_b(holds);
    holds.attach(&holder_a);
    holds.attach(&holder_b);

    constexpr std::uint64_t kFenceA = 100, kFenceB = 200;
    constexpr int kRounds = 12000;
    holds.add_fence(kFenceA, 0);
    holds.add_fence(kFenceB, 0);

    std::atomic<int> released_a{0}, released_b{0}, barriers{0};
    const int owner_a = 0, owner_b = 1;

    // Hammer concurrent signal + pump_ready + after_held from 4 threads simultaneously so threads constantly race
    // right across the end-of-pump exit boundary.
    std::thread t_hold_a([&] {
        for (int i = 1; i <= kRounds; ++i) {
            holder_a.hold(kFenceA, std::uint64_t(i), [&] { released_a.fetch_add(1, std::memory_order_relaxed); });
            holds.pump_ready();
        }
    });
    std::thread t_sig_a([&] {
        for (int i = 1; i <= kRounds; ++i) {
            holds.signaled(kFenceA, std::uint64_t(i));
            holds.pump_ready();
            if ((i & 7) == 0) holds.after_held(&owner_a, [&] { barriers.fetch_add(1, std::memory_order_relaxed); });
        }
    });
    std::thread t_hold_b([&] {
        for (int i = 1; i <= kRounds; ++i) {
            holder_b.hold(kFenceB, std::uint64_t(i), [&] { released_b.fetch_add(1, std::memory_order_relaxed); });
            holds.pump_ready();
        }
    });
    std::thread t_sig_b([&] {
        for (int i = 1; i <= kRounds; ++i) {
            holds.signaled(kFenceB, std::uint64_t(i));
            holds.pump_ready();
            if ((i & 7) == 0) holds.after_held(&owner_b, [&] { barriers.fetch_add(1, std::memory_order_relaxed); });
        }
    });

    t_hold_a.join();
    t_sig_a.join();
    t_hold_b.join();
    t_sig_b.join();

    // Without any extra pump_ready() call, every single held op and every single barrier MUST already have drained.
    EXPECT(released_a.load() == kRounds);
    EXPECT(released_b.load() == kRounds);
    EXPECT(barriers.load() == (kRounds / 8) * 2);
    EXPECT(!holds.any_held());
    EXPECT(holder_a.pending_count() == 0 && holder_b.pending_count() == 0);

    holds.detach(&holder_b);
    holds.detach(&holder_a);
    return 0;
}

}  // namespace

int main() {
    if (int r = test_wait_before_signal_chain_and_barriers()) return r;
    if (int r = test_exception_safety_does_not_wedge_thread()) return r;
    if (int r = test_concurrent_handoff_preserved_on_throw()) return r;
    if (int r = test_nested_distinct_instances_are_independent()) return r;
    if (int r = test_detach_and_cancel_after_synchronize()) return r;
    if (int r = test_multithreaded_pump_handoff_no_lost_wakeups()) return r;
    std::printf("queue_holds_test: ok\n");
    return 0;
}
