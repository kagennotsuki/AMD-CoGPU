// 验证 lmxxf 创建参数（不涉及游戏，零风险）
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

struct ApiTable {
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t (*QueryCapabilities)(void*);
    int32_t (*Create)(const void*, void**);
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
    void* extra0;
    void* extra1;
};

struct CreateInfo40 {
    uint32_t struct_size;
    uint32_t pad0;
    void* device;
    void* queue;
    const wchar_t* assets_directory;
    void* reserved0;
};

int main() {
    SetConsoleOutputCP(65001);
    const wchar_t* kPak = L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2\\LmxxfNrRuntime.pak";
    std::printf("sizeof(ApiTable)=%zu  sizeof(CreateInfo40)=%zu\n", sizeof(ApiTable), sizeof(CreateInfo40));

    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(fac.GetAddressOf())))) { std::printf("factory 失败\n"); return 1; }
    ComPtr<IDXGIAdapter1> ad;
    if (fac->EnumAdapters1(1, ad.GetAddressOf()) != S_OK) { std::printf("适配器1缺失\n"); return 1; }
    DXGI_ADAPTER_DESC d{};
    ad->GetDesc(&d);
    std::wprintf(L"适配器: %ls\n", d.Description);

    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(dev.GetAddressOf())))) {
        std::printf("设备失败\n"); return 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    ComPtr<ID3D12CommandQueue> q;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(q.GetAddressOf()));

    std::wstring dir(kPak);
    dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
    std::wstring dllp = dir + L"LmxxfNrRuntime.dll";
    HMODULE mod = LoadLibraryW(dllp.c_str());
    if (!mod) { std::printf("DLL 加载失败\n"); return 1; }
    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)GetProcAddress(mod, "LmxxfNrGetApi");

    ApiTable api{};
    api.struct_size = (uint32_t)sizeof(ApiTable);
    int rc = getApi ? getApi(1, &api) : -1;
    std::printf("GetApi rc=%d\n", rc);

    CreateInfo40 ci{};
    ci.struct_size = (uint32_t)sizeof(CreateInfo40);
    ci.device = dev.Get();
    ci.queue = q.Get();
    ci.assets_directory = kPak;

    void* ctx = nullptr;
    int rcCreate = api.Create(&ci, &ctx);
    std::printf("Create rc=%d ctx=%p\n", rcCreate, ctx);
    if (rcCreate != 0) {
        char e[512] = { 0 };
        if (api.GetLastError) api.GetLastError(e, 512);
        std::printf("  错误: %s\n", e);
        return 1;
    }
    int rcPrep = api.PrepareSession(ctx);
    std::printf("PrepareSession rc=%d\n", rcPrep);
    if (api.GetStatus) {
        char st[1024] = { 0 };
        api.GetStatus(ctx, st, 1024);
        std::printf("状态: %s\n", st);
    }
    return 0;
}
