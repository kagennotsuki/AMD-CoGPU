// Probe: LmxxfNrRuntime capabilities + Create/PrepareSession (no inference).
// ASCII only on purpose (non-ASCII source breaks this project's MSVC build).
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

struct LmxxfNrCapabilities {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t max_input_width;
    uint32_t max_input_height;
    uint32_t history_supported;
    uint32_t overlap_supported;
    uint32_t graph_supported;
    uint32_t hip_ready;
    uint32_t gfx1201_target;
};

struct LmxxfNrCreateInfo {
    uint32_t struct_size;
    void* device;
    void* queue;
    const wchar_t* assets_directory;
    uint32_t flags;
};

struct LmxxfNrApi {
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t (*QueryCapabilities)(LmxxfNrCapabilities*);
    int32_t (*Create)(const LmxxfNrCreateInfo*, void**);
    int32_t (*Destroy)(void*);
    int32_t (*PrepareSession)(void*);
    int32_t (*PrepareFrame)(void*, const void*, void*);
    int32_t (*RecordInputs)(void*, void*, void*);
    int32_t (*EnqueueHip)(void*, void*, void*);
    int32_t (*RecordOutputs)(void*, void*, void*);
    int32_t (*ExecuteAfterProducer)(void*, void*, void*);
    int32_t (*CancelUnsubmitted)(void*, void*);
    int32_t (*Poll)(void*, void*, uint32_t*);
    int32_t (*Retire)(void*, void*);
    int32_t (*ResetHistory)(void*);
    int32_t (*Drain)(void*);
    int32_t (*GetStatus)(void*, char*, uint32_t);
    int32_t (*GetLastError)(char*, uint32_t);
    int32_t (*GetTimings)(void*, void*);
};

static std::wstring ExeDir() {
    wchar_t p[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    size_t q = s.find_last_of(L'\\');
    return (q == std::wstring::npos) ? std::wstring() : s.substr(0, q + 1);
}

int main() {
    printf("=== lmxxf caps probe ===\n");

    std::wstring dir = ExeDir();
    std::wstring dll = dir + L"LmxxfNrRuntime.dll";
    HMODULE mod = LoadLibraryW(dll.c_str());
    if (!mod) { printf("[FAIL] LoadLibrary err=%lu (%ls)\n", GetLastError(), dll.c_str()); return 1; }
    printf("[ ok ] loaded LmxxfNrRuntime.dll\n");
    auto getApi = (int32_t(*)(uint32_t, LmxxfNrApi*))GetProcAddress(mod, "LmxxfNrGetApi");
    if (!getApi) { printf("[FAIL] no LmxxfNrGetApi\n"); return 1; }

    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(fac.GetAddressOf())))) { printf("[FAIL] factory\n"); return 1; }
    ComPtr<IDXGIAdapter1> ad, fallback;
    for (UINT i = 0; i < 8; ++i) {
        ComPtr<IDXGIAdapter1> a1;
        if (fac->EnumAdapters1(i, a1.GetAddressOf()) != S_OK) break;
        DXGI_ADAPTER_DESC d{};
        a1->GetDesc(&d);
        printf("       adapter[%u] = %ls  DeviceId=0x%04X\n", i, d.Description, d.DeviceId);
        if (d.DeviceId == 0x7550 && !ad) ad = a1;
        if (i == 1 && !fallback) fallback = a1;
    }
    if (!ad) ad = fallback;
    if (!ad) { printf("[FAIL] no adapter\n"); return 1; }
    { DXGI_ADAPTER_DESC d{}; ad->GetDesc(&d); printf("[ ok ] use %ls (0x%04X)\n", d.Description, d.DeviceId); }

    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(dev.GetAddressOf())))) {
        printf("[FAIL] D3D12 device\n"); return 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    ComPtr<ID3D12CommandQueue> q;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(q.GetAddressOf())))) { printf("[FAIL] queue\n"); return 1; }

    // accepted ABI table size
    struct Probe { uint32_t size; uint32_t abi; void* p[30]; } probe{};
    uint32_t accepted = 0;
    for (uint32_t sz = 128; sz <= 240; sz += 8) {
        memset(&probe, 0, sizeof(probe));
        probe.size = sz;
        if (getApi(1, (LmxxfNrApi*)&probe) == 0) { accepted = sz; printf("       GetApi(size=%u) -> OK\n", sz); }
    }
    if (!accepted) { printf("[FAIL] 128..240 all rejected\n"); return 1; }
    LmxxfNrApi api{};
    memset(&probe, 0, sizeof(probe)); probe.size = accepted;
    getApi(1, (LmxxfNrApi*)&probe);
    memcpy(&api, &probe, sizeof(api));
    printf("       use struct_size=%u abi_version=%u\n", accepted, api.abi_version);

    if (api.QueryCapabilities) {
        LmxxfNrCapabilities caps{};
        caps.struct_size = (uint32_t)sizeof(LmxxfNrCapabilities);
        int32_t r = api.QueryCapabilities(&caps);
        printf("\n=== QueryCapabilities -> %d ===\n", r);
        if (r == 0) {
            printf("  max_input_width  = %u\n", caps.max_input_width);
            printf("  max_input_height = %u\n", caps.max_input_height);
            printf("  history_supported= %u\n", caps.history_supported);
            printf("  overlap_supported= %u\n", caps.overlap_supported);
            printf("  graph_supported  = %u\n", caps.graph_supported);
            printf("  hip_ready        = %u\n", caps.hip_ready);
            printf("  gfx1201_target   = %u\n", caps.gfx1201_target);
        } else {
            char eb[256] = { 0 }; if (api.GetLastError) api.GetLastError(eb, 256);
            printf("  error: %s\n", eb);
        }
    }

    wchar_t envbuf[MAX_PATH] = { 0 };
    DWORD envn = GetEnvironmentVariableW(L"LMXXF_ASSETS", envbuf, MAX_PATH);
    std::wstring assets = (envn > 0 && envn < MAX_PATH) ? std::wstring(envbuf) : dir;
    printf("\n=== Create (assets=%ls) ===\n", assets.c_str());
    LmxxfNrCreateInfo ci{};
    ci.struct_size = (uint32_t)sizeof(LmxxfNrCreateInfo);
    ci.device = dev.Get();
    ci.queue = q.Get();
    ci.assets_directory = assets.c_str();
    ci.flags = 0;
    void* ctx = nullptr;
    int32_t r = api.Create(&ci, &ctx);
    printf("   Create -> %d\n", r);
    if (r != 0 || !ctx) {
        char eb[256] = { 0 }; if (api.GetLastError) api.GetLastError(eb, 256);
        printf("   error: %s\n", eb);
        return 1;
    }
    r = api.PrepareSession(ctx);
    printf("   PrepareSession -> %d\n", r);
    if (r != 0) {
        char eb[256] = { 0 }; if (api.GetLastError) api.GetLastError(eb, 256);
        printf("   error: %s\n", eb);
    }
    if (api.GetStatus) {
        char sb[1024] = { 0 };
        api.GetStatus(ctx, sb, 1024);
        printf("   GetStatus: %s\n", sb[0] ? sb : "(empty)");
    }
    api.Destroy(ctx);
    printf("\n=== done ===\n");
    return 0;
}
