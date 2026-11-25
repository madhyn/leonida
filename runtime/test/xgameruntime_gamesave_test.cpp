// XGameSaveImpl through QueryApiImpl, called the way xgameruntime.lib's XGameSave* thunks call it: QueryApiImpl with
// the class id (or a later interface id), one vtable slot with `this` first. Slots follow xgameruntime-docs' layout;
// arguments, limits and error codes follow the public GDK reference. Saves go to a temporary LEONIDA_SAVE_DIR; no
// game data, nothing online.
#include <windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <XAsync.h>
#include <XAsyncProvider.h>
#include <XTaskQueue.h>

#include "shims.h"

namespace rt::shims {
long xgameruntime_gamesave_open_handles();
void xgameruntime_gamesave_test_interrupt_after(unsigned checkpoints);
}

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

using Handle = void*;
struct ContainerInfo {
    const char* name;
    const char* displayName;
    uint32_t blobCount;
    uint64_t totalSize;
    time_t lastModifiedTime;
    bool needsSync;
};
struct BlobInfo {
    const char* name;
    uint32_t size;
};
struct Blob {
    BlobInfo info;
    uint8_t* data;
};

constexpr HRESULT E_GS_INVALID_CONTAINER_NAME = static_cast<HRESULT>(0x80830001);
constexpr HRESULT E_GS_OUT_OF_LOCAL_STORAGE = static_cast<HRESULT>(0x80830003);
constexpr HRESULT E_GS_UPDATE_TOO_BIG = static_cast<HRESULT>(0x80830005);
constexpr HRESULT E_GS_PROVIDED_BUFFER_TOO_SMALL = static_cast<HRESULT>(0x80830007);
constexpr HRESULT E_GS_BLOB_NOT_FOUND = static_cast<HRESULT>(0x80830008);
constexpr HRESULT E_GS_HANDLE_EXPIRED = static_cast<HRESULT>(0x8083000D);

constexpr GUID kSaveClass = {0x704c3f58, 0xe629, 0x4cc2, {0xb1, 0x97, 0x30, 0x51, 0x1b, 0x99, 0x6f, 0xe2}};
constexpr GUID kSaveImpl2 = {0x704c3f58, 0xe629, 0x4cc2, {0xb1, 0x97, 0x30, 0x51, 0x1b, 0x99, 0x6e, 0xe2}};
constexpr GUID kSaveImpl3 = {0x1bfff3af, 0xf14a, 0x40a3, {0x8e, 0x35, 0x9a, 0xda, 0x90, 0x65, 0x93, 0xf9}};
constexpr GUID kOther = {0x973a344e, 0x24bf, 0x4d0f, {0x84, 0x57, 0x56, 0xc5, 0x34, 0x89, 0x2b, 0x29}};  // XGameImpl

using Query = HRESULT(WINAPI*)(const GUID*, REFIID, void**);
using Fn = void*;
Fn slot(void* obj, int index) { return (*static_cast<Fn* const*>(obj))[index]; }
template <class F> F at(void* obj, int index) { return reinterpret_cast<F>(slot(obj, index)); }

bool finished(XAsyncBlock* async) { return XAsyncGetStatus(async, true) != E_PENDING; }

using Init = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, const char*, bool, Handle*);
using InitAsync = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, const char*, bool, XAsyncBlock*);
using ResultHandle = HRESULT(STDMETHODCALLTYPE*)(void*, XAsyncBlock*, Handle*);
using Close = void(STDMETHODCALLTYPE*)(void*, Handle);
using Quota = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, int64_t*);
using WithAsync = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, XAsyncBlock*);
using QuotaResult = HRESULT(STDMETHODCALLTYPE*)(void*, XAsyncBlock*, int64_t*);
using ByName = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, const char*);
using ByNameAsync = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, const char*, XAsyncBlock*);
using AsyncOnly = HRESULT(STDMETHODCALLTYPE*)(void*, XAsyncBlock*);
using ContainerCb = bool(CALLBACK*)(const ContainerInfo*, void*);
using BlobCb = bool(CALLBACK*)(const BlobInfo*, void*);
using GetInfo = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, const char*, void*, ContainerCb);
using EnumContainers = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, void*, ContainerCb);
using EnumBlobs = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, void*, BlobCb);
using EnumBlobsByName = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, const char*, void*, BlobCb);
using Create = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, const char*, Handle*);
using Read = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, const char**, uint32_t*, size_t, Blob*);
using ReadAsync = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, const char**, uint32_t, XAsyncBlock*);
using ReadResult = HRESULT(STDMETHODCALLTYPE*)(void*, XAsyncBlock*, size_t, Blob*, uint32_t*);
using Write = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, const char*, const uint8_t*, size_t);
using Submit = HRESULT(STDMETHODCALLTYPE*)(void*, Handle);
using FolderAsync = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, const char*, XAsyncBlock*);
using FolderResult = HRESULT(STDMETHODCALLTYPE*)(void*, XAsyncBlock*, size_t, char*);
using FilesQuota = HRESULT(STDMETHODCALLTYPE*)(void*, Handle, const char*, int64_t*);

struct Seen {
    std::vector<std::string> names, displays;
    std::vector<uint32_t> counts;
    std::vector<uint64_t> sizes;
    std::vector<time_t> modified;
    int stop_after = 1000;
};
bool CALLBACK on_container(const ContainerInfo* info, void* context) {
    auto* seen = static_cast<Seen*>(context);
    seen->names.emplace_back(info->name);
    seen->displays.emplace_back(info->displayName ? info->displayName : "");
    seen->counts.push_back(info->blobCount);
    seen->sizes.push_back(info->totalSize);
    seen->modified.push_back(info->lastModifiedTime);
    return static_cast<int>(seen->names.size()) < seen->stop_after;
}
bool CALLBACK on_blob(const BlobInfo* info, void* context) {
    auto* seen = static_cast<Seen*>(context);
    seen->names.emplace_back(info->name);
    seen->sizes.push_back(info->size);
    return true;
}

namespace fs = std::filesystem;

struct SaveSession {
    void* save;
    Handle provider = nullptr;
    Handle container = nullptr;

    HRESULT open(const fs::path& root) {
        if (!SetEnvironmentVariableW(L"LEONIDA_SAVE_DIR", root.c_str())) return E_FAIL;
        HRESULT hr = at<Init>(save, 3)(save, reinterpret_cast<Handle>(0x1000), "SCID", false, &provider);
        if (SUCCEEDED(hr)) hr = at<Create>(save, 16)(save, provider, "story", &container);
        return hr;
    }

    ~SaveSession() {
        if (container) at<Close>(save, 17)(save, container);
        if (provider) at<Close>(save, 6)(save, provider);
    }
};

struct Edit {
    const char* name;
    const char* data;   // nullptr deletes
};

HRESULT submit_edits(void* save, Handle container, const char* display, const std::vector<Edit>& edits) {
    Handle update = nullptr;
    HRESULT hr = at<Create>(save, 23)(save, container, display, &update);
    for (const Edit& e : edits) {
        if (FAILED(hr)) break;
        hr = e.data ? at<Write>(save, 25)(save, update, e.name, reinterpret_cast<const uint8_t*>(e.data),
                                        std::strlen(e.data))
                    : at<ByName>(save, 26)(save, update, e.name);
    }
    if (SUCCEEDED(hr)) hr = at<Submit>(save, 27)(save, update);
    if (update) at<Close>(save, 24)(save, update);
    return hr;
}

HRESULT write_next(SaveSession& session, const char* display = "New story") {
    return submit_edits(session.save, session.container, display, {{"a", "NEW_A_LONG"}, {"b", nullptr}, {"c", "NEW_C"}});
}

bool interrupt_child(const fs::path& root, bool during_recovery, unsigned checkpoint) {
    std::wstring exe(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (!length || length == exe.size()) return false;
    exe.resize(length);
    std::wstring command = L"\"" + exe + L"\" " + (during_recovery ? L"--interrupt-recovery" : L"--interrupt-update") +
                           L" \"" + root.wstring() + L"\" " + std::to_wstring(checkpoint);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process))
        return false;
    CloseHandle(process.hThread);
    DWORD exit_code = 0;
    const bool stopped = WaitForSingleObject(process.hProcess, 15000) == WAIT_OBJECT_0;
    const bool interrupted = stopped && GetExitCodeProcess(process.hProcess, &exit_code) && exit_code == 83;
    if (!stopped) {
        TerminateProcess(process.hProcess, 84);
        WaitForSingleObject(process.hProcess, 5000);
    }
    CloseHandle(process.hProcess);
    return interrupted;
}

bool matches(const Blob* blobs, uint32_t count, bool next) {
    const std::string_view a = next ? "NEW_A_LONG" : "OLD_A";
    const std::string_view second = next ? "NEW_C" : "OLD_B";
    return count == 2 && std::strcmp(blobs[0].info.name, "a") == 0 &&
           std::strcmp(blobs[1].info.name, next ? "c" : "b") == 0 && blobs[0].info.size == a.size() &&
           blobs[1].info.size == second.size() && std::memcmp(blobs[0].data, a.data(), a.size()) == 0 &&
           std::memcmp(blobs[1].data, second.data(), second.size()) == 0;
}

void expect_generation(const SaveSession& session, bool next, const char* display, time_t old_modified = 0) {
    std::array<uint64_t, 128> buffer{};
    auto* blobs = reinterpret_cast<Blob*>(buffer.data());
    const size_t size = sizeof(buffer);
    uint32_t count = 0;
    check(at<Read>(session.save, 20)(session.save, session.container, nullptr, &count, size, blobs) == S_OK &&
              matches(blobs, count, next),
          "public ReadBlobData returns the complete expected generation");

    XAsyncBlock async{};
    const bool started = at<ReadAsync>(session.save, 21)(session.save, session.container, nullptr, 0, &async) == S_OK;
    count = 0;
    check(started && finished(&async) &&
              at<ReadResult>(session.save, 22)(session.save, &async, size, blobs, &count) == S_OK &&
              matches(blobs, count, next),
          "the async read path returns the same generation");

    const uint64_t size_on_disk = next ? 15 : 10;
    Seen seen;
    check(at<EnumContainers>(session.save, 14)(session.save, session.provider, &seen, on_container) == S_OK &&
              seen.names == std::vector<std::string>{"story"} && seen.displays == std::vector<std::string>{display} &&
              seen.counts == std::vector<uint32_t>{2} && seen.sizes == std::vector<uint64_t>{size_on_disk} &&
              (!old_modified || seen.modified == std::vector<time_t>{old_modified}),
          "container enumeration and display metadata describe the same generation");
    int64_t remaining = 0;
    check(at<Quota>(session.save, 7)(session.save, session.provider, &remaining) == S_OK &&
              remaining == (int64_t{1} << 30) - static_cast<int64_t>(size_on_disk),
          "quota excludes journal scratch and counts the recovered blobs");
}

bool seed_generation(SaveSession& session, const fs::path& dir, time_t& modified) {
    if (submit_edits(session.save, session.container, "Old story", {{"a", "OLD_A"}, {"b", "OLD_B"}}) != S_OK) return false;
    std::error_code ec;
    const auto past = fs::file_time_type::clock::now() - std::chrono::hours(24);
    for (const wchar_t* name : {L"a", L"b"}) {
        fs::last_write_time(dir / name, past, ec);
        if (ec) return false;
    }
    Seen seen;
    if (at<GetInfo>(session.save, 13)(session.save, session.provider, "story", &seen, on_container) != S_OK ||
        seen.modified.size() != 1 || !seen.modified.front())
        return false;
    modified = seen.modified.front();
    return true;
}

void test_interruption(void* save, const fs::path& root) {
    // Points are staging, publication, replacing a, deleting b, creating c, updating .display, and retirement.
    for (unsigned point = 1; point <= 7; ++point) {
        std::printf("Interrupted save checkpoint %u\n", point);
        const fs::path testcase = root / std::to_wstring(point);
        const fs::path user_dir = testcase / L"local-user";
        time_t modified = 0;
        {
            SaveSession seed{save};
            const bool seeded = seed.open(testcase) == S_OK && seed_generation(seed, user_dir / L"story", modified);
            check(seeded, "seed a complete original update with metadata");
            if (!seeded) continue;
        }
        const bool interrupted = interrupt_child(testcase, false, point);
        check(interrupted, "the child process exited at the requested save checkpoint");
        if (!interrupted) continue;
        if (point == 3) {
            std::ifstream a(user_dir / L"story" / L"a", std::ios::binary);
            std::ifstream b(user_dir / L"story" / L"b", std::ios::binary);
            const auto bytes = [](std::ifstream& f) {
                return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            };
            check(bytes(a) == "NEW_A_LONG" && bytes(b) == "OLD_B", "the process stopped after the first live replacement");
        }
        if (point == 6) {
            check(interrupt_child(testcase, true, 2), "a second child exited after restoring metadata and removing c");
        }
        SaveSession reopened{save};
        const bool recovered = reopened.open(testcase) == S_OK;
        check(recovered, "a fresh provider opens the interrupted save");
        if (recovered) {
            const bool next = point == 7;
            expect_generation(reopened, next, next ? "New story" : "Old story", next ? 0 : modified);
            check(!fs::exists(user_dir / L".save-update~"), "successful recovery and committed cleanup remove scratch");
        }
    }

    const fs::path first = root / L"first";
    const fs::path first_container = first / L"local-user" / L"story";
    check(interrupt_child(first, false, 3) && fs::exists(first_container / L"a"),
          "a child interrupted during its first save left a live blob");
    check(interrupt_child(first, true, 2) && !fs::exists(first_container),
          "a second interruption occurs after recovery removes the new directory");
    SaveSession first_reopen{save};
    check(first_reopen.open(first) == S_OK && !fs::exists(first_container),
          "recovering an interrupted first save removes the newly created container");
}

void test_locked_recovery(void* save, const fs::path& root) {
    const fs::path dir = root / L"local-user" / L"story";
    SaveSession session{save};
    time_t modified = 0;
    const bool seeded = session.open(root) == S_OK && seed_generation(session, dir, modified);
    check(seeded, "seed the sharing-failure recovery fixture");
    if (!seeded) return;

    // The child is the only writer, and it has already exited when the parent's live handle attempts recovery.
    const bool interrupted = interrupt_child(root, false, 3);
    check(interrupted, "child interrupted after replacing a in the sharing-failure fixture");
    if (!interrupted) return;
    const auto hold = [](const fs::path& path) {
        return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    };
    HANDLE blocked = hold(dir / L"a");
    check(blocked != INVALID_HANDLE_VALUE, "hold the changed blob without FILE_SHARE_DELETE");
    std::array<uint64_t, 128> buffer{};
    uint32_t count = 0;
    int64_t quota = 0;
    Seen seen;
    check(at<Read>(save, 20)(save, session.container, nullptr, &count, sizeof(buffer),
                           reinterpret_cast<Blob*>(buffer.data())) == E_GS_OUT_OF_LOCAL_STORAGE &&
              at<Quota>(save, 7)(save, session.provider, &quota) == E_GS_OUT_OF_LOCAL_STORAGE &&
              at<EnumContainers>(save, 14)(save, session.provider, &seen, on_container) == E_GS_OUT_OF_LOCAL_STORAGE &&
              seen.names.empty() && fs::exists(dir.parent_path() / L".save-update~" / L"active"),
          "blocked recovery retains the journal and reports storage failure to existing handles");
    if (blocked != INVALID_HANDLE_VALUE) CloseHandle(blocked);
    expect_generation(session, false, "Old story", modified);

    blocked = hold(dir / L".display");
    check(blocked != INVALID_HANDLE_VALUE && write_next(session) == E_GS_OUT_OF_LOCAL_STORAGE,
          "locking display metadata fails the whole update");
    check(!fs::exists(dir.parent_path() / L".save-update~"), "ordinary rollback retires the journal before returning");
    if (blocked != INVALID_HANDLE_VALUE) CloseHandle(blocked);
    expect_generation(session, false, "Old story", modified);
    check(write_next(session, "") == S_OK && fs::file_size(dir / L".display") == 0,
          "the same update succeeds with an explicit empty display name once unlocked");
    expect_generation(session, true, "");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 4) {
        const std::wstring_view mode(argv[1]);
        if (mode != L"--interrupt-update" && mode != L"--interrupt-recovery") return 2;
        auto query = reinterpret_cast<Query>(rt::shims::find("xgameruntime.dll", "QueryApiImpl"));
        void* save = nullptr;
        if (!query || FAILED(query(&kSaveClass, kSaveClass, &save)) || !save) return 2;
        SaveSession session{save};
        const auto checkpoint = static_cast<unsigned>(std::wcstoul(argv[3], nullptr, 10));
        if (mode == L"--interrupt-recovery") {
            rt::shims::xgameruntime_gamesave_test_interrupt_after(checkpoint);
            return session.open(argv[2]) == S_OK ? 0 : 3;
        }
        if (FAILED(session.open(argv[2]))) return 3;
        rt::shims::xgameruntime_gamesave_test_interrupt_after(checkpoint);
        return write_next(session) == S_OK ? 0 : 4;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash still shows the checks before it
    std::printf("xgameruntime_gamesave_test: XGameSaveImpl through QueryApiImpl\n");
    const std::filesystem::path root = std::filesystem::temp_directory_path() / L"leonida_gamesave_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    SetEnvironmentVariableW(L"LEONIDA_SAVE_DIR", root.c_str());

    auto query = reinterpret_cast<Query>(rt::shims::find("xgameruntime.dll", "QueryApiImpl"));
    check(query != nullptr, "QueryApiImpl is registered");
    if (!query) return 1;

    void* save = nullptr;
    check(query(&kSaveClass, kSaveClass, &save) == S_OK && save, "QueryApiImpl hands out XGameSaveImpl for its class id");
    if (!save) return 1;
    void* save2 = nullptr;
    void* save3 = nullptr;
    check(query(&kSaveClass, kSaveImpl2, &save2) == S_OK && save2 == save &&
              query(&kSaveClass, kSaveImpl3, &save3) == S_OK && save3 == save,
          "IXGameSaveImpl2 and IXGameSaveImpl3 answer with the same object");
    void* none = reinterpret_cast<void*>(1);
    check(query(&kSaveClass, kOther, &none) == E_NOINTERFACE && none == nullptr, "another interface id is refused");

    Handle user = reinterpret_cast<Handle>(0x1000);   // XUserHandle: opaque here, only checked for null
    Handle provider = nullptr;
    check(at<Init>(save, 3)(save, nullptr, "SCID", false, &provider) == E_INVALIDARG && !provider,
          "InitializeProvider without a user is refused");
    check(at<Init>(save, 3)(save, user, "SCID", true, &provider) == S_OK && provider,
          "InitializeProvider opens the offline provider (syncOnDemand is accepted)");
    check(std::filesystem::is_directory(root / L"local-user"), "the provider's folder is <LEONIDA_SAVE_DIR>\\local-user");

    int64_t quota = 0;
    check(at<Quota>(save, 7)(save, provider, &quota) == S_OK && quota == (int64_t{1} << 30),
          "GetRemainingQuota of an empty provider is the whole 1 GiB");

    // ---- containers and names ----
    Handle container = nullptr;
    check(at<Create>(save, 16)(save, provider, "bad name", &container) == E_GS_INVALID_CONTAINER_NAME && !container,
          "a container name with a space is refused");
    for (const char* bad : {"a..b", "dir/", "x.", "a/b-c/d", ""})
        check(at<Create>(save, 16)(save, provider, bad, &container) == E_GS_INVALID_CONTAINER_NAME,
              "a container name against the documented rule is refused");
    check(at<Create>(save, 16)(save, provider, std::string(257, 'a').c_str(), &container) == E_GS_INVALID_CONTAINER_NAME,
          "a container name over 256 characters is refused");
    check(at<Create>(save, 16)(save, provider, "slots/SAVE_00.sav-1", &container) == S_OK && container,
          "a name with a path part and . - in the file part is accepted");
    check(!std::filesystem::exists(root / L"local-user" / L"slots%2FSAVE_00.sav-1"),
          "a container's folder appears on its first update, not on create");

    // ---- an update ----
    Handle update = nullptr;
    check(at<Create>(save, 23)(save, container, "Story 1", &update) == S_OK && update, "CreateUpdate");
    const std::string story(5000, 'S'), profile = "profile bytes";
    check(at<Write>(save, 25)(save, update, "story", reinterpret_cast<const uint8_t*>(story.data()), story.size()) == S_OK,
          "SubmitBlobWrite buffers a blob");
    check(at<Write>(save, 25)(save, update, "story", reinterpret_cast<const uint8_t*>("x"), 1) == E_INVALIDARG,
          "a second write of the same blob in one update is refused");
    check(at<Write>(save, 25)(save, update, "profile/main", reinterpret_cast<const uint8_t*>(profile.data()),
                              profile.size()) == S_OK,
          "a blob name with a slash is accepted (it is %-encoded on disk)");
    std::vector<uint8_t> big(16u * 1024 * 1024, 1);
    check(at<Write>(save, 25)(save, update, "big", big.data(), big.size()) == E_GS_UPDATE_TOO_BIG,
          "an update past 16 MB is refused");
    check(at<Submit>(save, 27)(save, update) == S_OK, "SubmitUpdate commits");
    const auto dir = root / L"local-user" / L"slots%2FSAVE_00.sav-1";
    check(std::filesystem::file_size(dir / L"story") == story.size() && std::filesystem::exists(dir / L"profile%2Fmain"),
          "the blobs are files, in tools/saves.py's layout and encoding");
    at<Close>(save, 24)(save, update);

    // ---- enumeration ----
    Seen containers;
    check(at<EnumContainers>(save, 14)(save, provider, &containers, on_container) == S_OK &&
              containers.names.size() == 1 && containers.names[0] == "slots/SAVE_00.sav-1" &&
              containers.displays[0] == "Story 1" && containers.counts[0] == 2 &&
              containers.sizes[0] == story.size() + profile.size(),
          "EnumerateContainerInfo: name, display name, blob count and size");
    Seen info;
    check(at<GetInfo>(save, 13)(save, provider, "slots/SAVE_00.sav-1", &info, on_container) == S_OK &&
              info.names.size() == 1,
          "GetContainerInfo calls back for a container that exists");
    Seen missing;
    check(at<GetInfo>(save, 13)(save, provider, "nothing", &missing, on_container) == S_OK && missing.names.empty(),
          "GetContainerInfo of a missing container calls nothing");
    Seen blobs;
    check(at<EnumBlobs>(save, 18)(save, container, &blobs, on_blob) == S_OK && blobs.names.size() == 2 &&
              blobs.names[0] == "profile/main" && blobs.names[1] == "story" && blobs.sizes[1] == story.size(),
          "EnumerateBlobInfo lists both blobs with their decoded names and sizes");
    Seen prefixed;
    check(at<EnumBlobsByName>(save, 19)(save, container, "sto", &prefixed, on_blob) == S_OK &&
              prefixed.names.size() == 1 && prefixed.names[0] == "story",
          "EnumerateBlobInfoByName filters by prefix");

    // ---- reading ----
    const char* names[] = {"story", "profile/main"};
    uint32_t count = 2;
    std::vector<uint8_t> small(64);
    check(at<Read>(save, 20)(save, container, names, &count, small.size(), reinterpret_cast<Blob*>(small.data())) ==
              E_GS_PROVIDED_BUFFER_TOO_SMALL,
          "ReadBlobData into a buffer too small for the blobs is refused");
    std::vector<uint64_t> buffer(1024);
    count = 2;
    auto* out = reinterpret_cast<Blob*>(buffer.data());
    check(at<Read>(save, 20)(save, container, names, &count, buffer.size() * 8, out) == S_OK && count == 2 &&
              std::strcmp(out[0].info.name, "story") == 0 && out[0].info.size == story.size() &&
              std::memcmp(out[0].data, story.data(), story.size()) == 0 &&
              std::strcmp(out[1].info.name, "profile/main") == 0 &&
              std::memcmp(out[1].data, profile.data(), profile.size()) == 0,
          "ReadBlobData returns both blobs, byte for byte, pointing into the buffer");
    const char* absent[] = {"nope"};
    count = 1;
    check(at<Read>(save, 20)(save, container, absent, &count, buffer.size() * 8, out) == E_GS_BLOB_NOT_FOUND,
          "a blob that is not there is E_GS_BLOB_NOT_FOUND");
    count = 0;
    check(at<Read>(save, 20)(save, container, nullptr, &count, buffer.size() * 8, out) == S_OK && count == 2,
          "blobNames = nullptr reads every blob");

    XAsyncBlock async{};
    check(at<ReadAsync>(save, 21)(save, container, names, 1, &async) == S_OK && finished(&async),
          "ReadBlobDataAsync runs");
    size_t needed = 0;
    check(XAsyncGetResultSize(&async, &needed) == S_OK && needed > story.size(),
          "its result size is what the buffer needs (XAsyncGetResultSize)");
    std::vector<uint64_t> abuf((needed + 7) / 8);
    uint32_t acount = 0;
    auto* aout = reinterpret_cast<Blob*>(abuf.data());
    check(at<ReadResult>(save, 22)(save, &async, needed, aout, &acount) == S_OK && acount == 1 &&
              std::strcmp(aout[0].info.name, "story") == 0 && std::memcmp(aout[0].data, story.data(), story.size()) == 0,
          "ReadBlobDataResult fills the title's buffer with one blob");

    // ---- delete a blob, async submit ----
    check(at<Create>(save, 23)(save, container, nullptr, &update) == S_OK, "a second update");
    check(at<ByName>(save, 26)(save, update, "profile/main") == S_OK, "SubmitBlobDelete");
    async = XAsyncBlock{};
    check(at<WithAsync>(save, 28)(save, update, &async) == S_OK && finished(&async) &&
              at<AsyncOnly>(save, 29)(save, &async) == S_OK,
          "SubmitUpdateAsync commits the delete");
    check(!std::filesystem::exists(dir / L"profile%2Fmain") && std::filesystem::exists(dir / L"story"),
          "the deleted blob is gone, the other stays");
    Seen after;
    at<EnumContainers>(save, 14)(save, provider, &after, on_container);
    check(after.displays.size() == 1 && after.displays[0] == "Story 1", "an update without a display name keeps it");
    at<Close>(save, 24)(save, update);
    check(at<Submit>(save, 27)(save, update) == E_GS_HANDLE_EXPIRED, "a closed update is refused");

    // ---- quota ----
    async = XAsyncBlock{};
    int64_t aquota = 0;
    check(at<WithAsync>(save, 8)(save, provider, &async) == S_OK && finished(&async) &&
              at<QuotaResult>(save, 9)(save, &async, &aquota) == S_OK &&
              aquota == (int64_t{1} << 30) - static_cast<int64_t>(story.size()),
          "GetRemainingQuotaAsync subtracts what the saves hold");

    // ---- delete the container ----
    async = XAsyncBlock{};
    check(at<ByNameAsync>(save, 11)(save, provider, "slots/SAVE_00.sav-1", &async) == S_OK && finished(&async) &&
              at<AsyncOnly>(save, 12)(save, &async) == S_OK && !std::filesystem::exists(dir),
          "DeleteContainerAsync removes the container");
    at<Close>(save, 17)(save, container);

    // ---- async provider, XGameSaveFiles ----
    async = XAsyncBlock{};
    Handle provider2 = nullptr;
    check(at<InitAsync>(save, 4)(save, user, "SCID", false, &async) == S_OK && finished(&async) &&
              at<ResultHandle>(save, 5)(save, &async, &provider2) == S_OK && provider2 && provider2 != provider,
          "InitializeProviderAsync hands out a second provider");
    async = XAsyncBlock{};
    check(at<FolderAsync>(save, 30)(save, user, "SCID", &async) == S_OK && finished(&async), "FilesGetFolderWithUiAsync");
    size_t folder_size = 0;
    XAsyncGetResultSize(&async, &folder_size);
    std::string folder(folder_size, '\0');
    check(folder_size > 0 && at<FolderResult>(save, 31)(save, &async, folder.size(), folder.data()) == S_OK &&
              std::filesystem::is_directory(std::filesystem::u8path(folder.c_str())),
          "FilesGetFolderWithUiResult returns a folder that exists");
    int64_t fquota = 0;
    check(at<FilesQuota>(save, 32)(save, user, "SCID", &fquota) == S_OK && fquota == (int64_t{1} << 30),
          "FilesGetRemainingQuota of an empty folder");

    // ---- a handle that outlives its container, after the heap hands the address out again ----
    // An update and an async read keep the container handle they were made from. Once that container is closed, the
    // next container may get the same address; the stale update and read must still be refused, not write into or
    // read from the newer container (another save).
    {
        Handle first = nullptr, stale_update = nullptr;
        check(at<Create>(save, 16)(save, provider, "first", &first) == S_OK &&
                  at<Create>(save, 23)(save, first, nullptr, &stale_update) == S_OK &&
                  at<Write>(save, 25)(save, stale_update, "mine", reinterpret_cast<const uint8_t*>("first's"), 7) == S_OK,
              "an update of container \"first\" buffers a blob");
        XTaskQueueHandle manual = nullptr;
        XTaskQueueCreate(XTaskQueueDispatchMode::Manual, XTaskQueueDispatchMode::Manual, &manual);
        XAsyncBlock stale_read{};
        stale_read.queue = manual;
        check(manual && at<ReadAsync>(save, 21)(save, first, nullptr, 0, &stale_read) == S_OK,
              "a ReadBlobDataAsync of \"first\" waits on a manual queue");
        at<Close>(save, 17)(save, first);

        // Containers are made until one lands at the closed container's address (most heaps hand it back at once).
        Handle second = nullptr;
        std::vector<Handle> others;
        for (int i = 0; i < 4096 && !second; ++i) {
            Handle c = nullptr;
            if (at<Create>(save, 16)(save, provider, "second", &c) != S_OK) break;
            if (c == first) second = c;
            else others.push_back(c);
        }
        for (Handle c : others) at<Close>(save, 17)(save, c);
        if (!second) {
            std::printf("SKIP the heap never reused a closed container's address; the stale-handle checks need it\n");
        } else {
            Handle theirs = nullptr;
            check(at<Create>(save, 23)(save, second, nullptr, &theirs) == S_OK &&
                      at<Write>(save, 25)(save, theirs, "theirs", reinterpret_cast<const uint8_t*>("second's"), 8) ==
                          S_OK &&
                      at<Submit>(save, 27)(save, theirs) == S_OK,
                  "container \"second\", at the closed container's address, gets a blob of its own");
            at<Close>(save, 24)(save, theirs);
            check(at<Submit>(save, 27)(save, stale_update) == E_GS_HANDLE_EXPIRED,
                  "SubmitUpdate of an update whose container was closed is refused, though a new container has its "
                  "address");
            check(!std::filesystem::exists(root / L"local-user" / L"second" / L"mine"),
                  "the stale update wrote nothing into the container that took its container's address");
        }
        while (XTaskQueueDispatch(manual, XTaskQueuePort::Work, 0) ||
               XTaskQueueDispatch(manual, XTaskQueuePort::Completion, 0)) {}
        if (second) {
            size_t stale_size = 0;
            const HRESULT read_hr = XAsyncGetStatus(&stale_read, false);
            XAsyncGetResultSize(&stale_read, &stale_size);
            check(read_hr == E_GS_HANDLE_EXPIRED && stale_size == 0,
                  "ReadBlobDataAsync of a closed container completes with E_GS_HANDLE_EXPIRED, not with the blobs of "
                  "the container that took its address");
            at<Close>(save, 17)(save, second);
        }
        if (XAsyncGetStatus(&stale_read, false) == S_OK) {   // collect a result so the async state is released
            std::vector<uint64_t> drain(1024);
            uint32_t drained = 0;
            at<ReadResult>(save, 22)(save, &stale_read, drain.size() * 8, reinterpret_cast<Blob*>(drain.data()), &drained);
        }
        at<Close>(save, 24)(save, stale_update);
        if (manual) XTaskQueueCloseHandle(manual);
        std::filesystem::remove_all(root / L"local-user" / L"first", ec);
        std::filesystem::remove_all(root / L"local-user" / L"second", ec);
    }

    // ---- a failed commit leaves the container as it was ----
    // Another process holds blob "b" open without FILE_SHARE_DELETE, so replacing it fails in the middle of the
    // update, after "a" was already replaced. The update must fail, undo what it already did, and leave no temporary
    // or backup files. Closing the handle makes the same update succeed.
    {
        const HRESULT out_of_local = E_GS_OUT_OF_LOCAL_STORAGE;
        auto submit = [&](Handle container, const std::vector<Edit>& edits) {
            return submit_edits(save, container, nullptr, edits);
        };
        auto read_blob = [&](const std::filesystem::path& dir, const wchar_t* file) {
            std::ifstream f(dir / file, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        };
        auto file_count = [](const std::filesystem::path& dir) {
            std::error_code count_ec;
            int n = 0;
            for (const auto& e : std::filesystem::directory_iterator(dir, count_ec)) {
                (void)e;
                ++n;
            }
            return n;
        };

        Handle rc = nullptr;
        check(at<Create>(save, 16)(save, provider, "rollback", &rc) == S_OK && rc, "CreateContainer for the rollback checks");
        const std::filesystem::path rollback_dir = root / L"local-user" / L"rollback";

        // Case A: a write whose rename is blocked after an earlier write was applied.
        check(submit(rc, {{"a", "OLD_A"}, {"b", "OLD_B"}}) == S_OK, "seed blobs a and b");
        HANDLE hold = CreateFileW((rollback_dir / L"b").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        check(hold != INVALID_HANDLE_VALUE, "another handle holds blob b open");
        check(submit(rc, {{"a", "NEW_A"}, {"b", "NEW_B"}}) == out_of_local, "an update whose rename of b is blocked fails");
        check(read_blob(rollback_dir, L"a") == "OLD_A" && read_blob(rollback_dir, L"b") == "OLD_B",
              "a failed update leaves a and b with their old contents");
        check(file_count(rollback_dir) == 2, "a failed update leaves no temporary or backup files");
        CloseHandle(hold);
        check(submit(rc, {{"a", "NEW_A"}, {"b", "NEW_B"}}) == S_OK, "the same update succeeds once b is free");
        check(read_blob(rollback_dir, L"a") == "NEW_A" && read_blob(rollback_dir, L"b") == "NEW_B" && file_count(rollback_dir) == 2,
              "the successful update leaves only the new contents");

        // Case B: a delete that sorts before a blocked write must not be applied.
        check(submit(rc, {{"a", "OLD_A"}, {"b", "OLD_B"}}) == S_OK, "reseed blobs a and b");
        hold = CreateFileW((rollback_dir / L"b").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           0, nullptr);
        check(hold != INVALID_HANDLE_VALUE, "another handle holds blob b open again");
        check(submit(rc, {{"a", nullptr}, {"b", "NEW_B"}}) == out_of_local, "a mixed update with a blocked write fails");
        check(std::filesystem::exists(rollback_dir / L"a") && read_blob(rollback_dir, L"a") == "OLD_A" && read_blob(rollback_dir, L"b") == "OLD_B",
              "the delete of a is not applied when the write of b fails");
        check(file_count(rollback_dir) == 2, "the mixed failed update leaves no temporary or backup files");
        CloseHandle(hold);

        // Case C: the plain success path, a write and a delete.
        check(submit(rc, {{"a", "FINAL_A"}, {"b", nullptr}}) == S_OK, "a write and a delete succeed together");
        check(read_blob(rollback_dir, L"a") == "FINAL_A" && !std::filesystem::exists(rollback_dir / L"b") && file_count(rollback_dir) == 1,
              "the successful write and delete leave only the written blob");

        // Case D: scratch files of a commit that died midway. A temporary is dropped; a backup may be the only copy of
        // a blob's old contents, so neither an update of another blob nor a failed update of two rollback_dir removes it.
        std::ofstream(rollback_dir / L"b~", std::ios::binary) << "HALF_B";
        std::ofstream(rollback_dir / L"b~b~", std::ios::binary) << "KEPT_B";
        check(submit(rc, {{"a", "NEXT_A"}}) == S_OK, "an update of a with scratch files of b present");
        check(!std::filesystem::exists(rollback_dir / L"b~") && read_blob(rollback_dir, L"b~b~") == "KEPT_B",
              "a stale temporary is removed and a kept backup is not");
        hold = CreateFileW((rollback_dir / L"a").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           0, nullptr);
        check(hold != INVALID_HANDLE_VALUE, "another handle holds blob a open");
        check(submit(rc, {{"a", "X"}, {"b", "Y"}}) == out_of_local, "an update that fails at a, before b is reached");
        CloseHandle(hold);
        check(read_blob(rollback_dir, L"b~b~") == "KEPT_B", "the failed update keeps the backup it did not write");

        at<Close>(save, 17)(save, rc);
    }

    at<Close>(save, 6)(save, provider);
    at<Close>(save, 6)(save, provider2);
    at<Close>(save, 6)(save, provider);   // twice: ignored
    check(at<Quota>(save, 7)(save, provider, &quota) == E_GS_HANDLE_EXPIRED, "a closed provider is refused");
    test_interruption(save, root / L"interruption");
    test_locked_recovery(save, root / L"locked-recovery");
    check(rt::shims::xgameruntime_gamesave_open_handles() == 0, "every handle the test opened is closed");

    std::filesystem::remove_all(root, ec);
    std::printf("xgameruntime_gamesave_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
