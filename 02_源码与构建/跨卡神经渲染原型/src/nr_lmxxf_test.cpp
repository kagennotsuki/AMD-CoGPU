// 阶段 B：在 RX 9070 XT 上直接驱动 LmxxfNrRuntime.dll 跑神经网络推理
//
// 目标：不依赖游戏、不依赖 OptiScaler，只用 D3D12 + 这个运行时的 C ABI，
//       跑通 PrepareFrame -> RecordInputs -> EnqueueHip -> RecordOutputs -> Retire，
//       并从 GetTimings 读出 network_ms（对照游戏里 14.9~17.1ms 的量级）。

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <algorithm>
#include <vector>

#include "LmxxfNrApi.h"

using Microsoft::WRL::ComPtr;

static const char* St(int32_t s) {
    switch (s) {
        case LMXXF_NR_OK: return "OK";
        case LMXXF_NR_UNSUPPORTED_ABI: return "UNSUPPORTED_ABI";
        case LMXXF_NR_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case LMXXF_NR_NOT_IMPLEMENTED: return "NOT_IMPLEMENTED";
        case LMXXF_NR_UNAVAILABLE: return "UNAVAILABLE";
        case LMXXF_NR_FAILED: return "FAILED";
        default: return "?";
    }
}

static bool Ok(int32_t s, const char* what) {
    std::printf("  %-28s -> %s (%d)\n", what, St(s), s);
    return s == LMXXF_NR_OK;
}

int main(int argc, char** argv) {
    UINT32 W = 1280, H = 720;            // 先用 720p 试通，成功再上 1440p
    int frames = 30;
    if (argc > 1) W = (UINT32)atoi(argv[1]);
    if (argc > 2) H = (UINT32)atoi(argv[2]);
    if (argc > 3) frames = atoi(argv[3]);
    std::printf("target %ux%u, frames=%d\n", W, H, frames);

    // ---- 1. 在 adapter 1（9070 XT）上建设备 ----
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) { std::printf("factory failed\n"); return 1; }
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(1, &adapter) != S_OK) { std::printf("adapter 1 missing\n"); return 1; }
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    std::printf("adapter 1: %ls\n", desc.Description);

    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        std::printf("device failed\n");
        return 1;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) { std::printf("queue failed\n"); return 1; }

    // ---- 2. 载入运行时 ----
    const wchar_t* kGameDir =
        L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2";
    std::wstring dllPath = std::wstring(kGameDir) + L"\\LmxxfNrRuntime.dll";
    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (!mod) { std::printf("LoadLibraryW failed: %lu\n", GetLastError()); return 1; }
    std::printf("loaded LmxxfNrRuntime.dll\n");

    using GetApiFn = int32_t(*)(uint32_t, LmxxfNrApi*);
    auto getApi = (GetApiFn)GetProcAddress(mod, "LmxxfNrGetApi");
    if (!getApi) { std::printf("LmxxfNrGetApi missing\n"); return 1; }

    LmxxfNrApi api{};
    api.struct_size = (uint32_t)sizeof(LmxxfNrApi);
    int32_t s = getApi(LMXXF_NR_ABI_VERSION, &api);
    std::printf("LmxxfNrGetApi -> %s (%d)  abi=%u\n", St(s), s, api.abi_version);
    if (s == LMXXF_NR_INVALID_ARGUMENT) {
        api.struct_size = LMXXF_NR_API_V1_SIZE;
        s = getApi(LMXXF_NR_ABI_VERSION, &api);
        std::printf("retry with V1_SIZE -> %s (%d)\n", St(s), s);
    }
    if (s != LMXXF_NR_OK) return 1;

    // ---- 3. 能力查询 ----
    LmxxfNrCapabilities caps{};
    caps.struct_size = (uint32_t)sizeof(caps);
    if (!Ok(api.QueryCapabilities(&caps), "QueryCapabilities")) return 1;
    std::printf("  caps: max %ux%u  history=%u overlap=%u graph=%u hip_ready=%u gfx1201=%u\n",
                caps.max_input_width, caps.max_input_height, caps.history_supported,
                caps.overlap_supported, caps.graph_supported, caps.hip_ready, caps.gfx1201_target);

    // ---- 4. 建会话 ----
    LmxxfNrCreateInfo ci{};
    ci.struct_size = (uint32_t)sizeof(ci);
    ci.device = dev.Get();
    ci.queue = queue.Get();
    ci.assets_directory = kGameDir;      // .pak 与 dll 同目录
    ci.flags = 0;
    void* ctx = nullptr;
    if (!Ok(api.Create(&ci, &ctx), "Create")) return 1;
    if (!Ok(api.PrepareSession(ctx), "PrepareSession")) return 1;

    // ---- 5. 资源 ----
    D3D12_HEAP_PROPERTIES defHeap{};
    defHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    defHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;   // AMD 必须 UNKNOWN
    defHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    defHeap.CreationNodeMask = 1;
    defHeap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = W;
    texDesc.Height = H;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> color;
    if (FAILED(dev->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
                                            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&color)))) {
        std::printf("color texture failed\n");
        return 1;
    }

    D3D12_RESOURCE_DESC expDesc{};
    expDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    expDesc.Width = 1;
    expDesc.Height = 1;
    expDesc.DepthOrArraySize = 1;
    expDesc.MipLevels = 1;
    expDesc.Format = DXGI_FORMAT_R16_FLOAT;
    expDesc.SampleDesc.Count = 1;
    expDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ComPtr<ID3D12Resource> exposure;
    if (FAILED(dev->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &expDesc,
                                            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&exposure)))) {
        std::printf("warning: exposure texture failed (continue without)\n");
        exposure = nullptr;
    }
    std::printf("resources ok\n");

    // ---- 6. 命令对象 ----
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return 1;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                     IID_PPV_ARGS(&list)))) return 1;
    list->Close();
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return 1;
    UINT64 fenceValue = 0;

    std::vector<float> netMs;
    for (int f = 0; f < frames; ++f) {
        LmxxfNrJob job{};
        job.struct_size = (uint32_t)sizeof(job);

        LmxxfNrFrameInfo fi{};
        fi.struct_size = (uint32_t)sizeof(fi);
        fi.session_id = 1;
        fi.frame_id = (uint64_t)f + 1;
        fi.list_generation = 1;
        fi.command_list = list.Get();
        fi.color_width = W;
        fi.color_height = H;
        fi.color = color.Get();
        fi.color_state = (uint32_t)D3D12_RESOURCE_STATE_COMMON;
        fi.flags = 0;
        fi.transfer_strength = 1.0f;
        fi.color_strength = 1.0f;
        fi.debug_view = 0;
        fi.model_scale = 1.0f;
        fi.exposure = exposure.Get();
        fi.exposure_state = (uint32_t)D3D12_RESOURCE_STATE_COMMON;
        fi.pre_exposure = 1.0f;
        fi.exposure_scale = 1.0f;

        int32_t s1 = api.PrepareFrame(ctx, &fi, &job);
        if (s1 != LMXXF_NR_OK) { std::printf("frame %d PrepareFrame -> %s\n", f, St(s1)); break; }

        // producer
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        int32_t s2 = api.RecordInputs(ctx, job.handle, list.Get());
        if (s2 != LMXXF_NR_OK) { std::printf("frame %d RecordInputs -> %s\n", f, St(s2)); break; }
        list->Close();
        ID3D12CommandList* l1[] = { list.Get() };
        queue->ExecuteCommandLists(1, l1);

        // HIP 计算（等待 producer 完成后跑网络）
        int32_t s3 = api.EnqueueHip(ctx, job.handle, queue.Get());
        if (s3 != LMXXF_NR_OK) { std::printf("frame %d EnqueueHip -> %s\n", f, St(s3)); break; }

        // consumer
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        int32_t s4 = api.RecordOutputs(ctx, job.handle, list.Get());
        if (s4 != LMXXF_NR_OK) { std::printf("frame %d RecordOutputs -> %s\n", f, St(s4)); break; }
        list->Close();
        ID3D12CommandList* l2[] = { list.Get() };
        queue->ExecuteCommandLists(1, l2);
        queue->Signal(fence.Get(), ++fenceValue);
        while (fence->GetCompletedValue() < fenceValue) Sleep(0);

        int32_t s5 = api.Retire(ctx, job.handle);
        if (s5 != LMXXF_NR_OK) { std::printf("frame %d Retire -> %s\n", f, St(s5)); break; }

        if (api.GetTimings) {
            LmxxfNrTimings t{};
            t.struct_size = (uint32_t)sizeof(t);
            if (api.GetTimings(ctx, &t) == LMXXF_NR_OK && t.valid) {
                netMs.push_back(t.network_ms);
                if (f % 5 == 0 || f == frames - 1)
                    std::printf("  frame %2d: network_ms = %.2f (timed frame %llu)\n",
                                f, t.network_ms, (unsigned long long)t.frame_id);
            }
        }
    }

    // ---- 7. 汇总 ----
    if (!netMs.empty()) {
        std::vector<float> tmp = netMs;
        std::sort(tmp.begin(), tmp.end());
        float med = tmp[tmp.size() / 2];
        std::printf("\nRESULT: %zu timed frames, median network_ms = %.2f, min %.2f, max %.2f\n",
                    tmp.size(), med, tmp.front(), tmp.back());
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

    api.Drain(ctx);
    api.Destroy(ctx);
    FreeLibrary(mod);
    return 0;
}
