// 阶段 C：最小闭环 —— 7900 XT 出帧 → CPU 中转 → 9070 XT 跑 lmxxf → 回传
//
// 每帧分段（串行，最保守的估法）：
//   1) A: src buffer -> readback，CPU 读回
//   2) CPU -> B 的 upload buffer -> 拷进 B 的 color 纹理
//   3) B: PrepareFrame/RecordInputs/Execute/ExecuteAfterProducer/RecordOutputs/Retire
//   4) B: private_output -> readback，CPU 读回
//   5) CPU -> A 的 upload -> 拷进 A 的 result buffer
//
// 输出：整体每帧耗时 + 各段耗时 + 由网络耗时算出的流水线上限。

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
#include <chrono>

#include "LmxxfNrApi.h"

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::high_resolution_clock;

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
    LmxxfNrFrameInfo base;
    uint64_t spare0;
    uint64_t spare1;
};
static_assert(sizeof(FrameInfo120) == 120, "frame info must be 120 bytes");

struct Ctx {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
};

static void SubmitAndWait(Ctx& c) {
    c.list->Close();
    ID3D12CommandList* l[] = { c.list.Get() };
    c.queue->ExecuteCommandLists(1, l);
    c.queue->Signal(c.fence.Get(), ++c.value);
    while (c.fence->GetCompletedValue() < c.value) Sleep(0);
}

static int InitCtx(Ctx& c, ID3D12Device* dev) {
    c.dev = dev;
    D3D12_COMMAND_QUEUE_DESC qd{};
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&c.queue)))) return 1;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.alloc)))) return 1;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.alloc.Get(), nullptr,
                                      IID_PPV_ARGS(&c.list)))) return 1;
    c.list->Close();
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence)))) return 1;
    return 0;
}

int main(int argc, char** argv) {
    UINT32 W = 1920, H = 1080;
    int frames = 20;
    if (argc > 1) W = (UINT32)atoi(argv[1]);
    if (argc > 2) H = (UINT32)atoi(argv[2]);
    if (argc > 3) frames = atoi(argv[3]);
    const UINT64 payload = (UINT64)W * H * 4;
    std::printf("pipeline %ux%u, payload %.2f MB, frames=%d\n", W, H, payload / 1048576.0, frames);

    const wchar_t* kAssets =
        L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2\\LmxxfNrRuntime.pak";

    // ---- 两张卡 ----
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return 1;
    ComPtr<IDXGIAdapter1> adA, adB;
    factory->EnumAdapters1(0, &adA);
    factory->EnumAdapters1(1, &adB);
    if (!adA || !adB) { std::printf("need two adapters\n"); return 1; }

    ComPtr<ID3D12Device> devA, devB;
    if (FAILED(D3D12CreateDevice(adA.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&devA)))) return 1;
    if (FAILED(D3D12CreateDevice(adB.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&devB)))) return 1;
    std::printf("devices ok (A=7900XT render, B=9070XT compute)\n");

    Ctx a, b;
    if (InitCtx(a, devA.Get()) || InitCtx(b, devB.Get())) { std::printf("ctx failed\n"); return 1; }

    // ---- 载入运行时（在 B 上）----
    std::wstring dllPath = std::wstring(kAssets);
    dllPath = dllPath.substr(0, dllPath.find_last_of(L'\\')) + L"\\LmxxfNrRuntime.dll";
    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (!mod) { std::printf("runtime load failed\n"); return 1; }
    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)GetProcAddress(mod, "LmxxfNrGetApi");
    ApiTable api{};
    api.struct_size = (uint32_t)sizeof(ApiTable);
    if (!getApi || getApi(1, &api) != 0) { std::printf("GetApi failed\n"); return 1; }

    void* ctx = nullptr;
    LmxxfNrCreateInfo ci{};
    ci.struct_size = (uint32_t)sizeof(ci);
    ci.device = devB.Get();
    ci.queue = b.queue.Get();
    ci.assets_directory = kAssets;
    if (api.Create(&ci, &ctx) != 0) { std::printf("Create failed\n"); return 1; }
    if (api.PrepareSession(ctx) != 0) { std::printf("PrepareSession failed\n"); return 1; }
    std::printf("lmxxf session on 9070 XT\n");

    // ---- 堆属性（AMD 要求 CPUPageProperty = UNKNOWN）----
    D3D12_HEAP_PROPERTIES defHeap{};
    defHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    defHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    defHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    defHeap.CreationNodeMask = 1;
    defHeap.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES upHeap = defHeap;
    upHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES rbHeap = defHeap;
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = payload;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // A 侧：源 buffer / 回读 / 回传上传 / 结果 buffer
    ComPtr<ID3D12Resource> srcA, rbA, upA, resA;
    devA->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&srcA));
    devA->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbA));
    devA->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upA));
    devA->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resA));

    // B 侧：上传 / color 纹理 / 回读
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

    ComPtr<ID3D12Resource> colorB, upB, rbB;
    if (FAILED(devB->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
                                             D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&colorB)))) {
        std::printf("color tex failed\n");
        return 1;
    }
    // 上传 -> 纹理的数量与行距
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rowSize = 0, texBytes = 0;
    devB->GetCopyableFootprints(&texDesc, 0, 1, 0, &fp, &rows, &rowSize, &texBytes);

    D3D12_RESOURCE_DESC upDesc = bufDesc;
    upDesc.Width = texBytes;
    devB->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &upDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upB));
    devB->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &upDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbB));
    std::printf("tex footprint: %ux%u rowPitch=%u total=%llu bytes\n",
                fp.Footprint.Width, fp.Footprint.Height, fp.Footprint.RowPitch,
                (unsigned long long)texBytes);

    void* host = VirtualAlloc(nullptr, (SIZE_T)payload, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!host) return 1;

    std::vector<double> totals, segTransfer, segNet;
    for (int f = 0; f < frames + 2; ++f) {
        auto t0 = Clock::now();

        // 1) A: src -> readback，CPU 读回
        a.alloc->Reset();
        a.list->Reset(a.alloc.Get(), nullptr);
        a.list->CopyBufferRegion(rbA.Get(), 0, srcA.Get(), 0, payload);
        SubmitAndWait(a);
        {
            D3D12_RANGE rr{ 0, (SIZE_T)payload };
            void* m = nullptr;
            rbA->Map(0, &rr, &m);
            std::memcpy(host, m, (size_t)payload);
            D3D12_RANGE nw{ 0, 0 };
            rbA->Unmap(0, &nw);
        }

        // 2) CPU -> B upload -> color 纹理
        {
            D3D12_RANGE nr{ 0, 0 };
            void* m = nullptr;
            upB->Map(0, &nr, &m);
            std::memcpy(m, host, (size_t)payload);
            D3D12_RANGE w{ 0, (SIZE_T)texBytes };
            upB->Unmap(0, &w);

            b.alloc->Reset();
            b.list->Reset(b.alloc.Get(), nullptr);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = colorB.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = upB.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = fp;
            b.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            SubmitAndWait(b);
        }
        auto t1 = Clock::now();

        // 3) B: 一帧推理
        LmxxfNrJob job{};
        job.struct_size = (uint32_t)sizeof(job);
        FrameInfo120 f120{};
        LmxxfNrFrameInfo& fi = f120.base;
        fi.struct_size = (uint32_t)sizeof(FrameInfo120);
        fi.session_id = 1;
        fi.frame_id = (uint64_t)f + 1;
        fi.list_generation = 1;
        fi.command_list = b.list.Get();
        fi.color_width = W;
        fi.color_height = H;
        fi.color = colorB.Get();
        fi.color_state = (uint32_t)D3D12_RESOURCE_STATE_COMMON;
        fi.transfer_strength = 1.0f;
        fi.color_strength = 1.0f;
        fi.model_scale = 1.0f;
        fi.pre_exposure = 1.0f;
        fi.exposure_scale = 1.0f;

        int32_t r = api.PrepareFrame(ctx, &fi, &job);
        if (r != 0) { std::printf("f%d PrepareFrame -> %d\n", f, r); break; }
        b.alloc->Reset();
        b.list->Reset(b.alloc.Get(), nullptr);
        r = api.RecordInputs(ctx, job.handle, b.list.Get());
        if (r != 0) { std::printf("f%d RecordInputs -> %d\n", f, r); break; }
        SubmitAndWait(b);
        r = api.ExecuteAfterProducer(ctx, job.handle, b.queue.Get());
        if (r != 0) { std::printf("f%d ExecuteAfterProducer -> %d\n", f, r); break; }
        b.alloc->Reset();
        b.list->Reset(b.alloc.Get(), nullptr);
        r = api.RecordOutputs(ctx, job.handle, b.list.Get());
        if (r != 0) { std::printf("f%d RecordOutputs -> %d\n", f, r); break; }
        SubmitAndWait(b);
        api.Retire(ctx, job.handle);
        auto t2 = Clock::now();

        // 4) B: 回读（用 upload buffer 的等尺寸搬运代替真实输出，计时等价）
        b.alloc->Reset();
        b.list->Reset(b.alloc.Get(), nullptr);
        b.list->CopyBufferRegion(rbB.Get(), 0, upB.Get(), 0, texBytes);
        SubmitAndWait(b);
        {
            D3D12_RANGE rr{ 0, (SIZE_T)texBytes };
            void* m = nullptr;
            rbB->Map(0, &rr, &m);
            std::memcpy(host, m, (size_t)payload);
            D3D12_RANGE nw{ 0, 0 };
            rbB->Unmap(0, &nw);
        }

        // 5) CPU -> A upload -> 结果 buffer
        {
            D3D12_RANGE nr{ 0, 0 };
            void* m = nullptr;
            upA->Map(0, &nr, &m);
            std::memcpy(m, host, (size_t)payload);
            D3D12_RANGE w{ 0, (SIZE_T)payload };
            upA->Unmap(0, &w);

            a.alloc->Reset();
            a.list->Reset(a.alloc.Get(), nullptr);
            a.list->CopyBufferRegion(resA.Get(), 0, upA.Get(), 0, payload);
            SubmitAndWait(a);
        }
        auto t3 = Clock::now();

        if (f >= 2) {
            double total = std::chrono::duration<double, std::milli>(t3 - t0).count();
            double tr1 = std::chrono::duration<double, std::milli>(t1 - t0).count();
            double net = std::chrono::duration<double, std::milli>(t2 - t1).count();
            double tr2 = total - tr1 - net;
            totals.push_back(total);
            segTransfer.push_back(tr1 + tr2);
            segNet.push_back(net);
        }
    }

    auto stats = [](std::vector<double> v, const char* name) {
        if (v.empty()) return;
        std::sort(v.begin(), v.end());
        double avg = 0;
        for (double x : v) avg += x;
        avg /= v.size();
        std::printf("  %-22s avg %6.2f ms   median %6.2f   min %6.2f   max %6.2f\n",
                    name, avg, v[v.size() / 2], v.front(), v.back());
    };

    std::printf("\n=== 闭环分段（串行，%zu 帧有效样本）===\n", totals.size());
    stats(totals, "整帧总耗时");
    stats(segNet, "网络推理段");
    stats(segTransfer, "两段传输合计");

    if (!totals.empty()) {
        std::sort(totals.begin(), totals.end());
        double med = totals[totals.size() / 2];
        std::printf("\n串行每帧 %.2f ms -> %.1f fps\n", med, 1000.0 / med);
        double netMed = segNet.empty() ? 0 : segNet[segNet.size() / 2];
        std::printf("若做流水线（传输与计算重叠）：周期≈max(网络 %.2f, 传输 %.2f) -> %.1f fps\n",
                    netMed, med - netMed, 1000.0 / (netMed > (med - netMed) ? netMed : (med - netMed)));
    }

    char st[1024] = {0};
    if (api.GetStatus) { api.GetStatus(ctx, st, 1024); std::printf("status: %s\n", st); }

    VirtualFree(host, 0, MEM_RELEASE);
    if (api.Drain) api.Drain(ctx);
    if (api.Destroy) api.Destroy(ctx);
    FreeLibrary(mod);
    return 0;
}
