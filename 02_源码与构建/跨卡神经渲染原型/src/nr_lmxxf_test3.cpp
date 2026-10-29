// 阶段 B v3：frame-info 用 120 字节（这个 DLL 的 ABI 比上游头文件多 16 字节）
// 每步用 Poll 打印 job 状态，定位 EnqueueHip 的状态机要求。
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#include "LmxxfNrApi.h"

using Microsoft::WRL::ComPtr;

struct ApiTable {
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t (*QueryCapabilities)(LmxxfNrCapabilities*);
    int32_t (*Create)(const LmxxfNrCreateInfo*, void**);
    int32_t (*Destroy)(void*);
    int32_t (*PrepareSession)(void*);
    int32_t (*PrepareFrame)(void*, const LmxxfNrFrameInfo*, LmxxfNrJob*);
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
    int32_t (*GetTimings)(void*, LmxxfNrTimings*);
    void* extra0;
    void* extra1;
};

struct FrameInfo120 {
    LmxxfNrFrameInfo base;   // 104 字节
    uint64_t spare0;
    uint64_t spare1;         // 合计 120
};
static_assert(sizeof(FrameInfo120) == 120, "frame info must be 120 bytes");

static const char* St(int32_t s) {
    switch (s) {
        case 0: return "OK";
        case 1: return "UNSUPPORTED_ABI";
        case 2: return "INVALID_ARGUMENT";
        case 3: return "NOT_IMPLEMENTED";
        case 4: return "UNAVAILABLE";
        case 5: return "FAILED";
        default: return "?";
    }
}
static const char* JobSt(uint32_t s) {
    switch (s) {
        case 0: return "NONE";
        case 1: return "PREPARED";
        case 2: return "PRODUCER_SUBMITTED";
        case 3: return "NR_ENQUEUED";
        case 4: return "NR_COMPLETE";
        case 5: return "CONSUMER_COMPLETE";
        case 6: return "RETIRED";
        default: return "?";
    }
}

int main(int argc, char** argv) {
    UINT32 W = 1920, H = 1080;
    int frames = 6;
    if (argc > 1) W = (UINT32)atoi(argv[1]);
    if (argc > 2) H = (UINT32)atoi(argv[2]);
    if (argc > 3) frames = atoi(argv[3]);
    std::printf("target %ux%u, frames=%d\n", W, H, frames);

    const wchar_t* kAssets =
        L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2\\LmxxfNrRuntime.pak";
    std::wstring dllPath = std::wstring(kAssets);
    dllPath = dllPath.substr(0, dllPath.find_last_of(L'\\')) + L"\\LmxxfNrRuntime.dll";

    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (!mod) { std::printf("load failed %lu\n", GetLastError()); return 1; }
    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)GetProcAddress(mod, "LmxxfNrGetApi");
    if (!getApi) return 1;

    ApiTable api{};
    api.struct_size = (uint32_t)sizeof(ApiTable);
    if (getApi(1, &api) != 0) { std::printf("GetApi failed\n"); return 1; }
    std::printf("api table %u bytes abi=%u\n", api.struct_size, api.abi_version);

    ComPtr<IDXGIFactory6> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    factory->EnumAdapters1(1, &adapter);
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    std::printf("adapter 1: %ls\n", ad.Description);

    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return 1;
    D3D12_COMMAND_QUEUE_DESC qd{};
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return 1;

    void* ctx = nullptr;
    LmxxfNrCreateInfo ci{};
    ci.struct_size = (uint32_t)sizeof(ci);
    ci.device = dev.Get();
    ci.queue = queue.Get();
    ci.assets_directory = kAssets;
    ci.flags = 0;
    int32_t r = api.Create(&ci, &ctx);
    std::printf("Create -> %s\n", St(r));
    if (r != 0) return 1;
    std::printf("PrepareSession -> %s\n", St(api.PrepareSession(ctx)));

    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1;
    dh.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = H;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> color;
    if (FAILED(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td,
                                            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&color)))) {
        std::printf("color failed\n");
        return 1;
    }

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
    list->Close();
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    UINT64 fv = 0;

    auto stateOf = [&](void* h) -> const char* {
        uint32_t s = 0xFFFFFFFF;
        if (api.Poll && h) { api.Poll(ctx, h, &s); return JobSt(s); }
        return "?";
    };

    std::vector<float> times;
    for (int f = 0; f < frames; ++f) {
        LmxxfNrJob job{};
        job.struct_size = (uint32_t)sizeof(job);

        FrameInfo120 f120{};
        LmxxfNrFrameInfo& fi = f120.base;
        fi.struct_size = (uint32_t)sizeof(FrameInfo120);
        fi.session_id = 1;
        fi.frame_id = (uint64_t)f + 1;
        fi.list_generation = 1;
        fi.command_list = list.Get();
        fi.color_width = W;
        fi.color_height = H;
        fi.color = color.Get();
        fi.color_state = (uint32_t)D3D12_RESOURCE_STATE_COMMON;
        fi.transfer_strength = 1.0f;
        fi.color_strength = 1.0f;
        fi.model_scale = 1.0f;
        fi.pre_exposure = 1.0f;
        fi.exposure_scale = 1.0f;

        r = api.PrepareFrame(ctx, &fi, &job);
        if (r != 0) {
            char e[512] = {0};
            if (api.GetLastError) api.GetLastError(e, 512);
            std::printf("f%d PrepareFrame -> %s err=%s\n", f, St(r), e);
            break;
        }

        // 记录 producer
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        r = api.RecordInputs(ctx, job.handle, list.Get());
        std::printf("f%d RecordInputs -> %s  state=%s\n", f, St(r), stateOf(job.handle));
        if (r != 0) {
            char e[512] = {0};
            if (api.GetLastError) api.GetLastError(e, 512);
            std::printf("   err=%s\n", e);
            break;
        }
        list->Close();
        ID3D12CommandList* l1[] = { list.Get() };
        queue->ExecuteCommandLists(1, l1);
        std::printf("f%d after execute  state=%s\n", f, stateOf(job.handle));

        // 试 ExecuteAfterProducer（若运行时有这个语义）
        if (api.ExecuteAfterProducer) {
            r = api.ExecuteAfterProducer(ctx, job.handle, queue.Get());
            std::printf("f%d ExecuteAfterProducer -> %s  state=%s\n", f, St(r), stateOf(job.handle));
        }

        // ExecuteAfterProducer 已把 HIP 计算排入并完成（state=NR_COMPLETE），
        // 此时再调 EnqueueHip 会因状态不符被拒，因此只在没有该入口时回退。
        if (!api.ExecuteAfterProducer) {
            r = api.EnqueueHip(ctx, job.handle, queue.Get());
            std::printf("f%d EnqueueHip(fallback) -> %s  state=%s\n", f, St(r), stateOf(job.handle));
            if (r != 0) break;
        }

        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        r = api.RecordOutputs(ctx, job.handle, list.Get());
        std::printf("f%d RecordOutputs -> %s  state=%s\n", f, St(r), stateOf(job.handle));
        if (r != 0) break;
        list->Close();
        ID3D12CommandList* l2[] = { list.Get() };
        queue->ExecuteCommandLists(1, l2);
        queue->Signal(fence.Get(), ++fv);
        while (fence->GetCompletedValue() < fv) Sleep(0);

        r = api.Retire(ctx, job.handle);
        std::printf("f%d Retire -> %s\n", f, St(r));

        if (api.GetTimings) {
            LmxxfNrTimings t{};
            t.struct_size = (uint32_t)sizeof(t);
            if (api.GetTimings(ctx, &t) == 0 && t.valid) {
                times.push_back(t.network_ms);
                std::printf("   network_ms=%.2f\n", t.network_ms);
            }
        }
    }

    if (!times.empty()) {
        std::vector<float> s2 = times;
        std::sort(s2.begin(), s2.end());
        std::printf("\nRESULT: %zu timed frames, median network_ms=%.2f (min %.2f max %.2f)\n",
                    s2.size(), s2[s2.size() / 2], s2.front(), s2.back());
    }
    if (api.GetStatus) {
        char st[1024] = {0};
        api.GetStatus(ctx, st, 1024);
        std::printf("status: %s\n", st);
    }
    if (api.Drain) api.Drain(ctx);
    if (api.Destroy) api.Destroy(ctx);
    FreeLibrary(mod);
    return 0;
}
