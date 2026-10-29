// 阶段 B（修正版）：这个 DLL 的 LmxxfNrApi 表是 160 字节（19 个函数指针），
// 上游头文件是 144（17 个）。按偏移建表，并对各结构体尺寸做协商探测。
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

#include "LmxxfNrApi.h"   // 复用结构体定义（Caps/CreateInfo/FrameInfo/Job/Timings）

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

int main(int argc, char** argv) {
    UINT32 W = 1280, H = 720;
    int frames = 10;
    if (argc > 1) W = (UINT32)atoi(argv[1]);
    if (argc > 2) H = (UINT32)atoi(argv[2]);
    if (argc > 3) frames = atoi(argv[3]);
    std::printf("target %ux%u, frames=%d\n", W, H, frames);

    const wchar_t* kGameDir = L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2";
    const wchar_t* kAssets = L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2\\LmxxfNrRuntime.pak";
    std::wstring dllPath = std::wstring(kGameDir) + L"\\LmxxfNrRuntime.dll";
    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (!mod) { std::printf("load failed %lu\n", GetLastError()); return 1; }

    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)GetProcAddress(mod, "LmxxfNrGetApi");
    if (!getApi) return 1;

    ApiTable api{};
    api.struct_size = (uint32_t)sizeof(ApiTable);
    int32_t s = getApi(1, &api);
    std::printf("GetApi -> %s   table %u bytes, abi=%u\n", St(s), api.struct_size, api.abi_version);
    if (s != 0) return 1;

    LmxxfNrCapabilities caps{};
    bool capsOk = false;
    for (uint32_t sz : { (uint32_t)sizeof(caps), 44u, 40u, 36u, 32u }) {
        std::memset(&caps, 0, sizeof(caps));
        caps.struct_size = sz;
        int32_t r = api.QueryCapabilities(&caps);
        std::printf("  QueryCapabilities(struct_size=%u) -> %s\n", sz, St(r));
        if (r == 0) { capsOk = true; break; }
    }
    if (capsOk) {
        std::printf("  caps: max %ux%u history=%u graph=%u hip_ready=%u gfx1201=%u\n",
                    caps.max_input_width, caps.max_input_height, caps.history_supported,
                    caps.graph_supported, caps.hip_ready, caps.gfx1201_target);
    }

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
    bool created = false;
    for (uint32_t sz : { (uint32_t)sizeof(LmxxfNrCreateInfo), 32u, 24u }) {
        LmxxfNrCreateInfo ci{};
        ci.struct_size = sz;
        ci.device = dev.Get();
        ci.queue = queue.Get();
        ci.assets_directory = kAssets;   // 必须指向 .pak 文件本身（日志证实）
        ci.flags = 0;
        int32_t r = api.Create(&ci, &ctx);
        std::printf("  Create(struct_size=%u) -> %s\n", sz, St(r));
        if (r == 0) { created = true; break; }
    }
    if (!created) {
        char err[512] = {0};
        if (api.GetLastError) api.GetLastError(err, 512);
        std::printf("create failed; last error: %s\n", err);
        return 1;
    }
    std::printf("  PrepareSession -> %s\n", St(api.PrepareSession(ctx)));

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
    std::printf("resources ok\n");

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
    list->Close();
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    UINT64 fv = 0;

    // --- 扫描 PrepareFrame 接受的 frame-info 尺寸 ---
    uint32_t fiSize = 0;
    for (uint32_t sz : { 104u, 96u, 112u, 120u, 128u, 136u, 144u, 88u, 80u, 152u, 160u }) {
        LmxxfNrFrameInfo probe{};
        probe.struct_size = sz;
        probe.session_id = 1;
        probe.frame_id = 1;
        probe.list_generation = 1;
        probe.command_list = list.Get();
        probe.color_width = W;
        probe.color_height = H;
        probe.color = color.Get();
        probe.color_state = (uint32_t)D3D12_RESOURCE_STATE_COMMON;
        probe.transfer_strength = 1.0f;
        probe.color_strength = 1.0f;
        probe.model_scale = 1.0f;
        probe.pre_exposure = 1.0f;
        probe.exposure_scale = 1.0f;
        LmxxfNrJob j{};
        j.struct_size = (uint32_t)sizeof(j);
        int32_t r = api.PrepareFrame(ctx, &probe, &j);
        std::printf("  PrepareFrame(struct_size=%u) -> %s\n", sz, St(r));
        if (r == 0) {
            fiSize = sz;
            if (api.CancelUnsubmitted) api.CancelUnsubmitted(ctx, j.handle);
            break;
        }
    }
    if (fiSize == 0) {
        char err[512] = {0};
        if (api.GetLastError) api.GetLastError(err, 512);
        std::printf("no acceptable frame-info size; last error: %s\n", err);
        return 1;
    }
    std::printf("  => using frame-info struct_size = %u\n", fiSize);
    std::vector<float> times;
    for (int f = 0; f < frames; ++f) {
        LmxxfNrJob job{};
        job.struct_size = (uint32_t)sizeof(job);

        LmxxfNrFrameInfo fi{};
        fi.struct_size = fiSize;
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

        int32_t r = api.PrepareFrame(ctx, &fi, &job);
        if (r != 0) {
            char err[512] = {0};
            if (api.GetLastError) api.GetLastError(err, 512);
            std::printf("frame %d PrepareFrame -> %s  err=%s\n", f, St(r), err);
            break;
        }

        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        r = api.RecordInputs(ctx, job.handle, list.Get());
        if (r != 0) { std::printf("frame %d RecordInputs -> %s\n", f, St(r)); break; }
        list->Close();
        ID3D12CommandList* l1[] = { list.Get() };
        queue->ExecuteCommandLists(1, l1);

        r = api.EnqueueHip(ctx, job.handle, queue.Get());
        if (r != 0) { std::printf("frame %d EnqueueHip -> %s\n", f, St(r)); break; }

        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        r = api.RecordOutputs(ctx, job.handle, list.Get());
        if (r != 0) { std::printf("frame %d RecordOutputs -> %s\n", f, St(r)); break; }
        list->Close();
        ID3D12CommandList* l2[] = { list.Get() };
        queue->ExecuteCommandLists(1, l2);
        queue->Signal(fence.Get(), ++fv);
        while (fence->GetCompletedValue() < fv) Sleep(0);

        r = api.Retire(ctx, job.handle);
        if (r != 0) { std::printf("frame %d Retire -> %s\n", f, St(r)); break; }

        if (api.GetTimings) {
            LmxxfNrTimings t{};
            t.struct_size = (uint32_t)sizeof(t);
            if (api.GetTimings(ctx, &t) == 0 && t.valid) {
                times.push_back(t.network_ms);
                std::printf("  frame %2d network_ms=%.2f\n", f, t.network_ms);
            }
        }
    }

    if (!times.empty()) {
        std::vector<float> s2 = times;
        std::sort(s2.begin(), s2.end());
        std::printf("\nRESULT: %zu timed frames, median network_ms = %.2f (min %.2f max %.2f)\n",
                    s2.size(), s2[s2.size() / 2], s2.front(), s2.back());
    } else {
        char err[512] = {0};
        if (api.GetLastError) api.GetLastError(err, 512);
        std::printf("\nno timings. last error: %s\n", err);
    }
    if (api.GetStatus) {
        char st[512] = {0};
        api.GetStatus(ctx, st, 512);
        std::printf("status: %s\n", st);
    }
    if (api.Drain) api.Drain(ctx);
    if (api.Destroy) api.Destroy(ctx);
    FreeLibrary(mod);
    return 0;
}
