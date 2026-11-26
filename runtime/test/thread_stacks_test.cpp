// rt::StackSampler (runtime/src/thread_stacks.cpp, LEONIDA_STACKS): the call chain of a thread blocked in a known
// function, of a thread whose stack keeps changing while it is sampled, and of the thread a fatal fault happens on
// (rt::print_fault_stacks). No game data.
#include <windows.h>
#include <intrin.h>
#include <io.h>
#include <fcntl.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "../src/thread_stacks.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

// The function an address lies in, by its unwind entry (immune to incremental-link thunks). The optimiser can move
// part of a function into a separate chunk whose unwind info chains to the function's own entry: follow the chain.
DWORD64 function_of(DWORD64 address) {
    DWORD64 base = 0;
    const auto* entry = RtlLookupFunctionEntry(address, &base, nullptr);
    if (!entry) return 0;
    for (int depth = 0; depth < 32; ++depth) {
        if (entry->UnwindData & 1) {  // An indirect entry names another RUNTIME_FUNCTION.
            entry = reinterpret_cast<const RUNTIME_FUNCTION*>(base + (entry->UnwindData & ~DWORD(1)));
            continue;
        }
        const auto* info = reinterpret_cast<const std::uint8_t*>(base + entry->UnwindData);
        if (!((info[0] >> 3) & UNW_FLAG_CHAININFO)) break;
        const unsigned codes = (info[2] + 1u) & ~1u;  // The code array is padded to an even count.
        entry = reinterpret_cast<const RUNTIME_FUNCTION*>(info + 4 + codes * 2);
    }
    return base + entry->BeginAddress;
}

// The function that called this one.
__declspec(noinline) DWORD64 here() { return function_of(reinterpret_cast<DWORD64>(_ReturnAddress())); }

// Frame index of the first frame in `function`, or -1.
int find(const std::uint64_t* frames, size_t count, DWORD64 function) {
    for (size_t i = 0; i < count; ++i)
        if (function && function_of(frames[i]) == function) return static_cast<int>(i);
    return -1;
}

std::atomic<DWORD64> g_blocked_fn{0}, g_blocked_main_fn{0}, g_spin_fn{0};
std::atomic<bool> g_stop{false};
std::atomic<unsigned> g_sink{0};

// A frame larger than a page (probed by __chkstk), blocked in WaitForSingleObject.
__declspec(noinline) int blocked_here(HANDLE ready, HANDLE release) {
    volatile char pad[64 * 1024];
    pad[0] = 1;
    pad[sizeof(pad) - 1] = 2;
    g_blocked_fn = here();
    SetEvent(ready);
    WaitForSingleObject(release, INFINITE);
    return pad[0] + pad[sizeof(pad) - 1];  // Keeps the frame alive across the wait, and the call from a tail call.
}

HANDLE g_ready = nullptr;
HANDLE g_release = nullptr;

__declspec(noinline) DWORD WINAPI blocked_main(void*) {
    g_blocked_main_fn = here();
    const int value = blocked_here(g_ready, g_release);
    return static_cast<DWORD>(value) + 1;
}

__declspec(noinline) unsigned recurse(unsigned depth) {
    volatile unsigned local[16];
    local[0] = depth;
    local[15] = depth * 3;
    if (depth == 0) return local[15];
    return recurse(depth - 1) + local[0];
}

__declspec(noinline) DWORD WINAPI spin_main(void*) {
    g_spin_fn = here();
    while (!g_stop.load(std::memory_order_relaxed)) g_sink.fetch_add(recurse(g_sink.load() % 48 + 1));
    return 0;
}

void blocked_thread() {
    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = CreateThread(nullptr, 0, blocked_main, nullptr, 0, nullptr);
    check(thread && WaitForSingleObject(g_ready, 5000) == WAIT_OBJECT_0, "helper thread reached its wait");
    rt::StackSampler sampler;
    std::uint64_t frames[rt::StackSampler::kMaxFrames] = {};
    size_t count = 0;
    int blocked = -1, outer = -1;
    for (int attempt = 0; attempt < 100 && (blocked < 0 || outer < 0); ++attempt) {
        count = sampler.sample(thread, frames);
        blocked = find(frames, count, g_blocked_fn);
        outer = find(frames, count, g_blocked_main_fn);
        if (blocked < 0 || outer < 0) Sleep(10);
    }
    for (size_t i = 0; i < count; ++i) std::printf("   #%zu 0x%llx\n", i, static_cast<unsigned long long>(frames[i]));
    check(blocked > 0, "the chain holds blocked_here below the wait it called");
    check(outer > blocked, "and blocked_main above it, through the 64 KiB frame");
    SetEvent(g_release);
    DWORD code = 0;
    check(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0 && GetExitCodeThread(thread, &code) && code == 4,
          "the sampled thread resumed and finished normally");
    CloseHandle(thread);
    CloseHandle(g_ready);
    CloseHandle(g_release);
}

// The live stack changes after every sample's resume: each chain must come from the copy and reach spin_main.
void moving_thread() {
    HANDLE thread = CreateThread(nullptr, 0, spin_main, nullptr, 0, nullptr);
    while (!g_spin_fn) Sleep(1);
    rt::StackSampler sampler;
    std::uint64_t frames[rt::StackSampler::kMaxFrames] = {};
    int sampled = 0, reached = 0, deep = 0;
    for (int i = 0; i < 300; ++i) {
        const size_t count = sampler.sample(thread, frames);
        if (!count) continue;
        ++sampled;
        // Deeper than kMaxFrames recursion stops at the cap without reaching spin_main; that is not a failure.
        if (count == rt::StackSampler::kMaxFrames && find(frames, count, g_spin_fn) < 0) {
            ++deep;
            continue;
        }
        if (find(frames, count, g_spin_fn) >= 0) ++reached;
        else if (sampled - reached - deep <= 2) {
            std::printf("   chain %d misses spin_main (0x%llx):\n", i, static_cast<unsigned long long>(g_spin_fn.load()));
            for (size_t f = 0; f < count; ++f)
                std::printf("      #%zu 0x%llx in 0x%llx\n", f, static_cast<unsigned long long>(frames[f]),
                            static_cast<unsigned long long>(function_of(frames[f])));
        }
    }
    g_stop = true;
    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    std::printf("   %d samples, %d reach spin_main, %d at the frame cap\n", sampled, reached, deep);
    check(sampled == 300, "every sample of a running thread succeeded");
    check(reached + deep == sampled, "every chain of a moving stack unwinds to its thread function");
}

// rt::print_fault_stacks: the fault it was called for and the call chain of the thread it happened on. The function
// writes to stderr, so the capture points fd 2 at a file for the duration of the fault and the file is read back.
std::atomic<DWORD64> g_fault_fn{0}, g_fault_caller_fn{0};

__declspec(noinline) int fault_here() {
    g_fault_fn = here();
    volatile const int* none = nullptr;
    return none[3];  // a read of 0xc
}

LONG WINAPI fault_filter(EXCEPTION_POINTERS* info) {
    rt::print_fault_stacks(info->ExceptionRecord, info->ContextRecord, "test");
    return EXCEPTION_EXECUTE_HANDLER;
}

// The __try in a function of its own: MSVC refuses __try in a function whose own objects need unwinding.
__declspec(noinline) int run_fault() {
    int value = 0;
    __try {
        value = fault_here();
    } __except (fault_filter(GetExceptionInformation())) {
        value = -1;
    }
    return value;
}

// "thread_stacks_test.exe+0x1a2b" -> 0x1a2b, or -1 for a line naming another module (a host DLL frame).
long long frame_rva(const std::string& line) {
    const std::string name = "thread_stacks_test.exe+0x";
    const size_t at = line.find(name);
    if (at == std::string::npos) return -1;
    return std::stoll(line.substr(at + name.size()), nullptr, 16);
}

void fault_stacks() {
    char dir[MAX_PATH] = {};
    const DWORD len = GetTempPathA(sizeof dir, dir);
    check(len > 0 && len < sizeof dir, "the temp path resolves");
    if (!len) return;
    const std::string path = std::string(dir) + "leonida_thread_stacks_fault_" + std::to_string(GetCurrentProcessId()) +
                             ".log";
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(file != INVALID_HANDLE_VALUE, "the fault capture's file opens");
    if (file == INVALID_HANDLE_VALUE) return;
    const int saved = _dup(_fileno(stderr));
    // Binary: the capture must hold the bytes the diagnostics wrote, with their own line endings.
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(file), _O_BINARY);
    _dup2(fd, _fileno(stderr));
    g_fault_caller_fn = here();  // fault_stacks itself: the chain must reach it above the fault
    const int value = run_fault();
    _dup2(saved, _fileno(stderr));
    _close(saved);
    _close(fd);  // the last CRT reference to `file`, so this closes the handle
    check(value == -1, "the guarded call ended in its handler");

    std::ifstream in(path, std::ios::binary);
    const std::string capture{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    in.close();
    std::remove(path.c_str());
    if (capture.size() < 40) std::printf("   capture: [%s]\n", capture.c_str());
    check(capture.find("[rt] fault test: read of 0xc at thread_stacks_test.exe+0x") != std::string::npos ||
              capture.find("[rt] fault test: read of 0xc at host:thread_stacks_test.exe+0x") != std::string::npos,
          "the fault names the access and the instruction");

    // The chain of the faulting thread starts at the faulting instruction itself and unwinds out to the caller.
    const size_t chain = capture.find("(faulting thread):\n");
    check(chain != std::string::npos, "the capture holds the faulting thread's chain");
    if (chain == std::string::npos) std::printf("   capture (%zu bytes):\n%s\n", capture.size(), capture.c_str());
    if (chain == std::string::npos) return;
    const size_t fault_line = capture.find("[rt] fault test:");
    const size_t at = fault_line != std::string::npos ? capture.find(" at ", fault_line) : std::string::npos;
    const std::string where =
        at != std::string::npos ? capture.substr(at + 4, capture.find('\n', at) - at - 4) : std::string();
    check(!where.empty() && capture.find("#0 " + where, chain) != std::string::npos,
          "frame #0 is the faulting instruction the fault line names");

    const DWORD64 base = reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr));
    int in_fault_here = -1, in_caller = -1, frame = 0;
    size_t pos = chain;
    while ((pos = capture.find("   #", pos)) != std::string::npos) {
        const size_t end = capture.find('\n', pos);
        if (const long long rva = frame_rva(capture.substr(pos, end - pos)); rva >= 0) {
            const DWORD64 function = function_of(base + static_cast<DWORD64>(rva));
            if (function == g_fault_fn && in_fault_here < 0) in_fault_here = frame;
            if (function == g_fault_caller_fn && in_caller < 0) in_caller = frame;
            ++frame;
        }
        pos = end == std::string::npos ? capture.size() : end;
    }
    check(in_fault_here >= 0, "the chain reaches the function the fault happened in");
    check(in_caller > in_fault_here, "and unwinds out of it to the function that called it");
    check(capture.find("[rt] stacks test: ") != std::string::npos, "the other threads are snapshotted too");
}

void parse() {
    std::vector<unsigned> t;
    check(rt::parse_stack_times("90,180", t) && t == std::vector<unsigned>{90, 180}, "parse 90,180");
    check(rt::parse_stack_times("180,90,90", t) && t == std::vector<unsigned>{90, 180}, "parse sorts and dedupes");
    check(!rt::parse_stack_times("", t) && t.empty(), "empty text refused");
    check(!rt::parse_stack_times("1,,2", t) && !rt::parse_stack_times("1,", t) && !rt::parse_stack_times("x", t) &&
              !rt::parse_stack_times("-5", t) && !rt::parse_stack_times("1.5", t),
          "bad tokens refused");
}

}  // namespace

int main() {
    parse();
    blocked_thread();
    moving_thread();
    fault_stacks();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
