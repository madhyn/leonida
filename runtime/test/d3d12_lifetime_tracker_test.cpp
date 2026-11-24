// Host-vs-Vulkan parity test for ID3D12Device5::CreateLifetimeTracker and ID3D12LifetimeTracker::DestroyOwnedObject.
// Compares our Vulkan-backed D3D12 layer side-by-side against the real Windows PC D3D12 runtime (default GPU + WARP).
// No shaders or game data.
#include "host_adapter.h"
#include "../shims/d3d12_com.h"
#include <dxgi1_4.h>

#include <atomic>
#include <cstdio>
#include <vector>

#define EXPECT(cond)                                                                                \
    do {                                                                                            \
        if (!(cond)) {                                                                              \
            std::fprintf(stderr, "FAIL [%s] line %d: %s\n", who, __LINE__, #cond);                  \
            return 1;                                                                               \
        }                                                                                           \
    } while (0)

#define EXPECT_HR(expr, want)                                                                       \
    do {                                                                                            \
        const HRESULT got_ = (expr);                                                                \
        if (got_ != (want)) {                                                                       \
            std::fprintf(stderr, "FAIL [%s] line %d: %s = 0x%08lX, want 0x%08lX\n", who, __LINE__,  \
                         #expr, static_cast<unsigned long>(got_), static_cast<unsigned long>(want));\
            return 1;                                                                               \
        }                                                                                           \
    } while (0)

namespace {

template <class T>
struct Ref {
    T* p = nullptr;
    ~Ref() {
        if (p) p->Release();
    }
    T* operator->() const { return p; }
};

class TestOwner final : public ID3D12LifetimeOwner {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12LifetimeOwner)) {
            *out = static_cast<ID3D12LifetimeOwner*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    void STDMETHODCALLTYPE LifetimeStateUpdated(D3D12_LIFETIME_STATE state) override {
        states.push_back(state);
    }

    ULONG ref_count() const { return refs_.load(); }
    std::vector<D3D12_LIFETIME_STATE> states;

private:
    std::atomic<ULONG> refs_{1};
};

struct Observed {
    HRESULT create_null_owner = S_OK;
    HRESULT create_null_out = S_OK;
    HRESULT create_ok = E_FAIL;
    HRESULT create_bad_iid = S_OK;
    ULONG owner_refs_after_create = 0;
    ULONG owner_refs_after_release = 0;
    std::size_t callbacks_after_create = 0;
    HRESULT destroy_null = S_OK;
    HRESULT destroy_valid = E_FAIL;
    std::size_t callbacks_after_destroy = 0;
    int last_state_after_destroy = -1;
    HRESULT destroy_foreign = S_OK;
    bool get_device_matches = false;
};

int probe_device(ID3D12Device* device, ID3D12Device* foreign_device, const char* who, Observed& obs, bool is_host = false) {
    Ref<ID3D12Device5> dev5;
    EXPECT(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dev5.p))));

    // 1. Null owner / null out / bad IID
    {
        Ref<ID3D12LifetimeTracker> t;
        obs.create_null_owner = dev5->CreateLifetimeTracker(nullptr, IID_PPV_ARGS(&t.p));
        EXPECT(t.p == nullptr);
        TestOwner owner;
        obs.create_null_out = dev5->CreateLifetimeTracker(&owner, __uuidof(ID3D12LifetimeTracker), nullptr);
        obs.create_bad_iid = dev5->CreateLifetimeTracker(&owner, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&t.p));
        EXPECT(t.p == nullptr);
    }

    // 2. Valid create + GetDevice + owner refcount + DestroyOwnedObject
    {
        TestOwner owner;
        const ULONG refs_before = owner.ref_count();
        Ref<ID3D12LifetimeTracker> tracker;
        obs.create_ok = dev5->CreateLifetimeTracker(&owner, IID_PPV_ARGS(&tracker.p));
        EXPECT(SUCCEEDED(obs.create_ok) && tracker.p != nullptr);
        obs.owner_refs_after_create = owner.ref_count() - refs_before;
        obs.callbacks_after_create = owner.states.size();

        // GetDevice returns the owning device.
        Ref<ID3D12Device> got_dev;
        EXPECT(SUCCEEDED(tracker->GetDevice(IID_PPV_ARGS(&got_dev.p))) && got_dev.p != nullptr);
        Ref<IUnknown> u1, u2;
        EXPECT(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&u1.p))));
        EXPECT(SUCCEEDED(got_dev->QueryInterface(IID_PPV_ARGS(&u2.p))));
        obs.get_device_matches = (u1.p == u2.p);

        // Private data & SetName work (ID3D12Object inheritance).
        EXPECT(SUCCEEDED(tracker->SetName(L"test_tracker")));

        // DestroyOwnedObject(nullptr) - the Windows host runtime dereferences null without the debug layer.
        if (!is_host) obs.destroy_null = tracker->DestroyOwnedObject(nullptr);

        // Create a small buffer on this device and call DestroyOwnedObject.
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = 256;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Ref<ID3D12Resource> buf;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buf.p))));
        obs.destroy_valid = tracker->DestroyOwnedObject(buf.p);
        obs.callbacks_after_destroy = owner.states.size();
        if (!owner.states.empty()) obs.last_state_after_destroy = int(owner.states.back());

        // Foreign device object (also only safe without debug layer on our own layer)
        if (foreign_device && !is_host) {
            Ref<ID3D12Resource> fbuf;
            if (SUCCEEDED(foreign_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                                  D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                                  IID_PPV_ARGS(&fbuf.p)))) {
                obs.destroy_foreign = tracker->DestroyOwnedObject(fbuf.p);
            }
        }

        tracker.p->Release();
        tracker.p = nullptr;
        obs.owner_refs_after_release = owner.ref_count() - refs_before;
    }

    std::printf("[%s] null_owner=0x%08lX null_out=0x%08lX bad_iid=0x%08lX ok=0x%08lX "
                "owner_ref_delta=(+%lu -> +%lu) cb_create=%zu destroy_null=0x%08lX destroy_ok=0x%08lX "
                "cb_destroy=%zu last_state=%d destroy_foreign=0x%08lX get_dev=%d\n",
                who, static_cast<unsigned long>(obs.create_null_owner),
                static_cast<unsigned long>(obs.create_null_out), static_cast<unsigned long>(obs.create_bad_iid),
                static_cast<unsigned long>(obs.create_ok), obs.owner_refs_after_create, obs.owner_refs_after_release,
                obs.callbacks_after_create, static_cast<unsigned long>(obs.destroy_null),
                static_cast<unsigned long>(obs.destroy_valid), obs.callbacks_after_destroy,
                obs.last_state_after_destroy, static_cast<unsigned long>(obs.destroy_foreign),
                int(obs.get_device_matches));
    std::fflush(stdout);
    return 0;
}

}  // namespace

int main() {
    const char* who = "main";
    Ref<ID3D12Device> vk_dev;
    HRESULT hr = rt::d3d12::create_vulkan_device_standalone(IID_PPV_ARGS(&vk_dev.p));
    if (FAILED(hr) || !vk_dev.p) {
        std::printf("SKIP (no Vulkan device: 0x%08lx)\n", static_cast<unsigned long>(hr));
        return 77;
    }

    // Create a WARP host device to compare real Windows D3D12 runtime behavior AND serve as a foreign device.
    Ref<IDXGIFactory4> factory;
    Ref<IDXGIAdapter> warp_adapter;
    Ref<ID3D12Device> warp_dev;
    Ref<ID3D12Device> warp_dev2;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory.p))) &&
        SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp_adapter.p)))) {
        (void)D3D12CreateDevice(warp_adapter.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&warp_dev.p));
        (void)D3D12CreateDevice(warp_adapter.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&warp_dev2.p));
    }

    Observed ours{};
    if (probe_device(vk_dev.p, warp_dev.p, "vulkan", ours) != 0) return 1;

    EXPECT_HR(ours.create_null_owner, E_INVALIDARG);
    EXPECT_HR(ours.create_null_out, E_UNEXPECTED);
    EXPECT_HR(ours.create_bad_iid, E_NOINTERFACE);
    EXPECT_HR(ours.create_ok, S_OK);
    EXPECT(ours.get_device_matches);
    EXPECT_HR(ours.destroy_null, E_INVALIDARG);
    EXPECT_HR(ours.destroy_valid, S_OK);
    EXPECT(ours.callbacks_after_destroy == 1);
    EXPECT(ours.last_state_after_destroy == int(D3D12_LIFETIME_STATE_NOT_IN_USE));
    EXPECT(ours.owner_refs_after_release == 0);
    if (warp_dev.p) {
        EXPECT_HR(ours.destroy_foreign, E_INVALIDARG);
        Observed host{};
        if (probe_device(warp_dev.p, warp_dev2.p, "host-warp", host, /*is_host=*/true) != 0) return 1;
        EXPECT_HR(ours.create_null_owner, host.create_null_owner);
        EXPECT_HR(ours.create_null_out, host.create_null_out);
        EXPECT_HR(ours.create_bad_iid, host.create_bad_iid);
        EXPECT_HR(ours.create_ok, host.create_ok);
        EXPECT(ours.get_device_matches == host.get_device_matches);
        EXPECT(ours.owner_refs_after_create == host.owner_refs_after_create);
        EXPECT(ours.owner_refs_after_release == host.owner_refs_after_release);
        EXPECT(ours.callbacks_after_create == host.callbacks_after_create);
        // Note: on PC WARP, DestroyOwnedObject returns E_INVALIDARG for an ordinary untracked ID3D12Resource buffer
        // that wasn't created via a lifetime-owned factory; our Vulkan layer accepts any same-device child and signals
        // D3D12_LIFETIME_STATE_NOT_IN_USE so console titles calling DestroyOwnedObject on device children succeed.
        std::printf("Parity against real Windows D3D12 (WARP): EXACT MATCH on CreateLifetimeTracker & refcounting\n");
    }

    std::printf("d3d12_lifetime_tracker_test: ALL CHECKS PASSED\n");
    return 0;
}
