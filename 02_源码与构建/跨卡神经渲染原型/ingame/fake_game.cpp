// 离屏"假游戏"：完整闭环验证台
//   每帧：清屏渲染 → Present 前抓帧 → 4K 下采样 1080p → CPU → 9070 XT 跑 lmxxf
//        → 回读 → CPU → 双线性放大 → 贴回下一帧的后备缓冲（一帧延迟）
//
// 目的：在不用启动游戏的前提下，验证"抓帧→跨卡推理→写回"整条链是否成立，
//       并测出流水线周期。跑通后再把这套逻辑搬回 dxgi 代理。
//
// 用法：fake_game.exe [宽 高] [帧数]

#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <condition_variable>

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

static const char* kHLSL = R"(
Texture2D<float4>   gSrc : register(t0);
SamplerState        gSmp : register(s0);
RWTexture2D<float4> gDst : register(u0);

[numthreads(8, 8, 1)]
void DownscaleCS(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uint2 s = id.xy * 2;
    float4 c = gSrc.Load(int3(s.x, s.y, 0)) + gSrc.Load(int3(s.x+1, s.y, 0))
             + gSrc.Load(int3(s.x, s.y+1, 0)) + gSrc.Load(int3(s.x+1, s.y+1, 0));
    gDst[id.xy] = c * 0.25;
}

[numthreads(8, 8, 1)]
void UpscaleCS(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    gDst[id.xy] = gSrc.SampleLevel(gSmp, uv, 0);
}
)";

static void BarrierT(ID3D12GraphicsCommandList* l, ID3D12Resource* r,
                     D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    l->ResourceBarrier(1, &br);
}

// ---------- 9070 XT 侧（工作线程）----------
struct Worker {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fe;
    UINT64 fv = 0;
    ComPtr<ID3D12Resource> color, up, rb;
    HMODULE mod = nullptr;
    ApiTable api{};
    void* ctx = nullptr;
    UINT64 bytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT w = 0, h = 0;
    bool ok = false;

    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    std::vector<uint8_t> inBuf, outBuf;
    bool hasIn = false, hasOut = false, quit = false;
    int processed = 0;
    double lastMs = 0;
};

static Worker W;

static void WaitB() { W.q->Signal(W.fe.Get(), ++W.fv); while (W.fe->GetCompletedValue() < W.fv) Sleep(0); }

static bool WorkerInit() {
    ComPtr<IDXGIFactory6> f;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IDXGIAdapter1> ad;
    if (f->EnumAdapters1(1, &ad) != S_OK) { std::printf("adapter 1 缺失\n"); return false; }
    DXGI_ADAPTER_DESC ades{};
    ad->GetDesc(&ades);
    std::printf("worker adapter: %ls\n", ades.Description);
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&W.dev)))) return false;
    D3D12_COMMAND_QUEUE_DESC qd{};
    W.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&W.q));
    W.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&W.al));
    W.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, W.al.Get(), nullptr, IID_PPV_ARGS(&W.cl));
    W.cl->Close();
    W.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&W.fe));

    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1;
    dh.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES uh = dh; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES rh = dh; rh.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W.w; td.Height = W.h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(W.dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&W.color)))) return false;

    D3D12_RESOURCE_DESC sd = td;
    sd.Flags = D3D12_RESOURCE_FLAG_NONE;
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    W.dev->GetCopyableFootprints(&sd, 0, 1, 0, &W.fp, &rows, &rowSize, &total);
    W.bytes = total;

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    W.dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&W.up));
    W.dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&W.rb));

    W.inBuf.assign((size_t)total, 0);
    W.outBuf.assign((size_t)total, 0);

    const wchar_t* kAssets =
        L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2\\LmxxfNrRuntime.pak";
    std::wstring dll(kAssets);
    dll = dll.substr(0, dll.find_last_of(L'\\')) + L"\\LmxxfNrRuntime.dll";
    W.mod = LoadLibraryW(dll.c_str());
    if (!W.mod) { std::printf("lmxxf 加载失败\n"); return false; }
    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)GetProcAddress(W.mod, "LmxxfNrGetApi");
    W.api.struct_size = (uint32_t)sizeof(ApiTable);
    if (!getApi || getApi(1, &W.api) != 0) { std::printf("GetApi 失败\n"); return false; }
    LmxxfNrCreateInfo ci{};
    ci.struct_size = (uint32_t)sizeof(ci);
    ci.device = W.dev.Get();
    ci.queue = W.q.Get();
    ci.assets_directory = kAssets;
    if (W.api.Create(&ci, &W.ctx) != 0) { std::printf("Create 失败\n"); return false; }
    if (W.api.PrepareSession(W.ctx) != 0) { std::printf("PrepareSession 失败\n"); return false; }
    std::printf("lmxxf 会话就绪 (%ux%u)\n", W.w, W.h);
    W.ok = true;
    return true;
}

static void WorkerMain() {
    std::vector<uint8_t> local;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(W.m);
            W.cv.wait(lk, [] { return W.quit || W.hasIn; });
            if (W.quit) break;
            local = W.inBuf;
            W.hasIn = false;
        }
        auto t0 = Clock::now();

        // 上传 → color
        D3D12_RANGE nr{ 0, 0 };
        void* m = nullptr;
        if (SUCCEEDED(W.up->Map(0, &nr, &m)) && m) {
            memcpy(m, local.data(), local.size());
            D3D12_RANGE wr{ 0, (SIZE_T)local.size() };
            W.up->Unmap(0, &wr);

            W.al->Reset(); W.cl->Reset(W.al.Get(), nullptr);
            D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = W.color.Get();
            d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = W.up.Get();
            s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint = W.fp;
            W.cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            W.cl->Close();
            { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
            WaitB();

            LmxxfNrJob job{}; job.struct_size = (uint32_t)sizeof(job);
            FrameInfo120 fi120{};
            LmxxfNrFrameInfo& fi = fi120.base;
            fi.struct_size = (uint32_t)sizeof(FrameInfo120);
            fi.session_id = 1;
            fi.frame_id = (uint64_t)W.processed + 1;
            fi.list_generation = 1;
            fi.command_list = W.cl.Get();
            fi.color_width = W.w; fi.color_height = W.h;
            fi.color = W.color.Get();
            fi.color_state = (uint32_t)D3D12_RESOURCE_STATE_COMMON;
            fi.transfer_strength = 1.0f; fi.color_strength = 1.0f; fi.model_scale = 1.0f;
            fi.pre_exposure = 1.0f; fi.exposure_scale = 1.0f;

            if (W.api.PrepareFrame(W.ctx, &fi, &job) == 0) {
                W.al->Reset(); W.cl->Reset(W.al.Get(), nullptr);
                if (W.api.RecordInputs(W.ctx, job.handle, W.cl.Get()) == 0) {
                    W.cl->Close();
                    { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
                    WaitB();
                    if (W.api.ExecuteAfterProducer(W.ctx, job.handle, W.q.Get()) == 0) {
                        W.al->Reset(); W.cl->Reset(W.al.Get(), nullptr);
                        if (W.api.RecordOutputs(W.ctx, job.handle, W.cl.Get()) == 0) {
                            W.cl->Close();
                            { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
                            WaitB();
                            W.api.Retire(W.ctx, job.handle);
                        }
                    }
                }
            }

            // 回读
            W.al->Reset(); W.cl->Reset(W.al.Get(), nullptr);
            D3D12_TEXTURE_COPY_LOCATION rd{}; rd.pResource = W.rb.Get();
            rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; rd.PlacedFootprint = W.fp;
            D3D12_TEXTURE_COPY_LOCATION rs{}; rs.pResource = W.color.Get();
            rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; rs.SubresourceIndex = 0;
            W.cl->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
            W.cl->Close();
            { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
            WaitB();

            D3D12_RANGE rr{ 0, (SIZE_T)W.bytes };
            void* rm = nullptr;
            if (SUCCEEDED(W.rb->Map(0, &rr, &rm)) && rm) {
                memcpy(local.data(), rm, local.size());
                D3D12_RANGE nw{ 0, 0 };
                W.rb->Unmap(0, &nw);
            }
        }
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        {
            std::lock_guard<std::mutex> lk(W.m);
            W.outBuf = local;
            W.hasOut = true;
            W.processed++;
            W.lastMs = ms;
            if (W.processed <= 3 || W.processed % 60 == 0)
                std::printf("  worker frame %d: %.2f ms\n", W.processed, ms);
        }
    }
}

int main(int argc, char** argv) {
    UINT w4k = (argc > 1) ? (UINT)atoi(argv[1]) : 1920;
    UINT h4k = (argc > 2) ? (UINT)atoi(argv[2]) : 1080;
    int frames = (argc > 3) ? atoi(argv[3]) : 300;
    UINT w1k = (w4k <= 1920 && h4k <= 1080) ? w4k : 1920;
    UINT h1k = (w4k <= 1920 && h4k <= 1080) ? h4k : 1080;
    std::printf("假游戏: 交换链 %ux%u, 网络 %ux%u, 帧数 %d\n", w4k, h4k, w1k, h1k, frames);

    W.w = w1k; W.h = h1k;

    // ---- 窗口 + 设备 A ----
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FakeGame";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"FakeGame", L"Cross-GPU NR harness", WS_OVERLAPPEDWINDOW,
                                0, 0, (int)w4k / 2, (int)h4k / 2, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    ComPtr<IDXGIFactory6> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adA;
    factory->EnumAdapters1(0, &adA);
    ComPtr<ID3D12Device> devA;
    if (FAILED(D3D12CreateDevice(adA.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&devA)))) { std::printf("设备失败\n"); return 1; }
    D3D12_COMMAND_QUEUE_DESC qd{};
    ComPtr<ID3D12CommandQueue> qA;
    devA->CreateCommandQueue(&qd, IID_PPV_ARGS(&qA));

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = w4k; scd.Height = h4k;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 3;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap1;
    if (FAILED(factory->CreateSwapChainForHwnd(qA.Get(), hwnd, &scd, nullptr, nullptr, &swap1))) { std::printf("交换链失败\n"); return 1; }
    ComPtr<IDXGISwapChain3> swap;
    swap1.As(&swap);

    // RTV
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 3;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    devA->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
    UINT rtvInc = devA->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12Resource> bbs[3];
    for (UINT i = 0; i < 3; ++i) {
        swap->GetBuffer(i, IID_PPV_ARGS(&bbs[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * rtvInc;
        devA->CreateRenderTargetView(bbs[i].Get(), nullptr, h);
    }

    // 缩放资源
    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1; dh.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES uh = dh; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES rh = dh; rh.Type = D3D12_HEAP_TYPE_READBACK;

    auto makeTex = [&](UINT w, UINT h, D3D12_RESOURCE_STATES st, ComPtr<ID3D12Resource>& out) {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return devA->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td, st, nullptr, IID_PPV_ARGS(&out));
    };
    ComPtr<ID3D12Resource> small1080, smallUp, bigOut, rb1080, up1080;
    makeTex(w1k, h1k, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, small1080);
    makeTex(w1k, h1k, D3D12_RESOURCE_STATE_COPY_DEST, smallUp);
    makeTex(w4k, h4k, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, bigOut);

    D3D12_RESOURCE_DESC d1k{};
    d1k.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d1k.Width = w1k; d1k.Height = h1k; d1k.DepthOrArraySize = 1; d1k.MipLevels = 1;
    d1k.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d1k.SampleDesc.Count = 1;
    d1k.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    UINT rows = 0; UINT64 rowSize = 0, bytes1k = 0;
    devA->GetCopyableFootprints(&d1k, 0, 1, 0, &W.fp, &rows, &rowSize, &bytes1k);
    W.bytes = bytes1k;

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes1k; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    devA->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb1080));
    devA->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up1080));

    // 根签名（SRV t0 + 静态采样器 + UAV u0）
    D3D12_DESCRIPTOR_RANGE srvR{}, uavR{};
    srvR.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srvR.NumDescriptors = 1; srvR.BaseShaderRegister = 0;
    uavR.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uavR.NumDescriptors = 1; uavR.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1; rp[0].DescriptorTable.pDescriptorRanges = &srvR;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &uavR;
    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2; rsd.pParameters = rp;
    rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &samp;
    ComPtr<ID3DBlob> sig, err;
    D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    ComPtr<ID3D12RootSignature> rs;
    devA->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rs));

    auto makePso = [&](const char* entry, ComPtr<ID3D12PipelineState>& pso) {
        ComPtr<ID3DBlob> cs, e2;
        if (FAILED(D3DCompile(kHLSL, strlen(kHLSL), "h.hlsl", nullptr, nullptr, entry, "cs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e2))) {
            std::printf("编译 %s 失败: %s\n", entry, e2 ? (const char*)e2->GetBufferPointer() : "?");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rs.Get();
        pd.CS.pShaderBytecode = cs->GetBufferPointer();
        pd.CS.BytecodeLength = cs->GetBufferSize();
        return SUCCEEDED(devA->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));
    };
    ComPtr<ID3D12PipelineState> psoDown, psoUp;
    if (!makePso("DownscaleCS", psoDown) || !makePso("UpscaleCS", psoUp)) return 1;

    D3D12_DESCRIPTOR_HEAP_DESC shd{};
    shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    shd.NumDescriptors = 16;
    shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    devA->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&heap));
    UINT inc = devA->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE ch = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gh = heap->GetGPUDescriptorHandleForHeapStart();
    auto mkSrv = [&](ID3D12Resource* r, UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = ch; h.ptr += (SIZE_T)slot * inc;
        devA->CreateShaderResourceView(r, &sd, h);
    };
    auto mkUav = [&](ID3D12Resource* r, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = ch; h.ptr += (SIZE_T)slot * inc;
        devA->CreateUnorderedAccessView(r, nullptr, &ud, h);
    };
    for (UINT i = 0; i < 3; ++i) mkSrv(bbs[i].Get(), i);   // 0..2 后备缓冲
    mkSrv(smallUp.Get(), 3);   // 写回源
    mkUav(small1080.Get(), 4); // 下采样目标
    mkUav(bigOut.Get(), 5);    // 放大目标

    ComPtr<ID3D12CommandAllocator> alA;
    ComPtr<ID3D12GraphicsCommandList> clA;
    ComPtr<ID3D12Fence> feA;
    devA->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alA));
    devA->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alA.Get(), nullptr, IID_PPV_ARGS(&clA));
    clA->Close();
    devA->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&feA));
    UINT64 vA = 0;

    if (!WorkerInit()) { std::printf("worker 初始化失败\n"); return 1; }
    W.th = std::thread(WorkerMain);

    // 预热：喂一帧全零并等它完成（lmxxf 首次权重上传约 1.5 秒），再开始计时
    {
        std::lock_guard<std::mutex> lk(W.m);
        std::vector<uint8_t> zeros(W.inBuf.size(), 0);
        W.inBuf = zeros;
        W.hasIn = true;
        W.cv.notify_one();
    }
    {
        auto tW = Clock::now();
        while (true) {
            { std::lock_guard<std::mutex> lk(W.m); if (W.processed >= 1) break; }
            if (std::chrono::duration<double>(Clock::now() - tW).count() > 15.0) { std::printf("预热超时\n"); break; }
            Sleep(10);
        }
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - tW).count();
        std::printf("预热完成，用时 %.0f ms\n", ms);
    }

    std::vector<uint8_t> host((size_t)bytes1k);
    std::vector<uint8_t> pendingWb;
    bool hasWb = false;
    int captured = 0, writebacks = 0;
    double sumSubmit = 0, sumHarvest = 0;
    int harvests = 0;

    auto t0 = Clock::now();
    for (int f = 0; f < frames; ++f) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

        UINT idx = swap->GetCurrentBackBufferIndex();
        ID3D12Resource* bb = bbs[idx].Get();

        auto tA = Clock::now();
        alA->Reset();
        clA->Reset(alA.Get(), nullptr);

        // ① 渲染当前帧（清屏成随时间变化的花纹）
        BarrierT(clA.Get(), bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += (SIZE_T)idx * rtvInc;
        float c[4] = { (float)((f % 120) / 120.0), 0.35f, 0.55f, 1.0f };
        clA->ClearRenderTargetView(rtv, c, 0, nullptr);

        // ② 写回上一帧的 NR 结果（放大后贴回）
        bool didWb = false;
        if (hasWb) {
            void* m = nullptr;
            D3D12_RANGE nr{ 0, 0 };
            if (SUCCEEDED(up1080->Map(0, &nr, &m)) && m) {
                memcpy(m, pendingWb.data(), pendingWb.size());
                D3D12_RANGE wr{ 0, (SIZE_T)bytes1k };
                up1080->Unmap(0, &wr);

                D3D12_TEXTURE_COPY_LOCATION cd{}; cd.pResource = smallUp.Get();
                cd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; cd.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION cs{}; cs.pResource = up1080.Get();
                cs.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; cs.PlacedFootprint = W.fp;
                clA->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);
                BarrierT(clA.Get(), smallUp.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                ID3D12DescriptorHeap* heaps[] = { heap.Get() };
                clA->SetDescriptorHeaps(1, heaps);
                clA->SetComputeRootSignature(rs.Get());
                D3D12_GPU_DESCRIPTOR_HANDLE st = gh; st.ptr += (SIZE_T)3 * inc;
                D3D12_GPU_DESCRIPTOR_HANDLE ut = gh; ut.ptr += (SIZE_T)5 * inc;
                clA->SetComputeRootDescriptorTable(0, st);
                clA->SetComputeRootDescriptorTable(1, ut);
                clA->SetPipelineState(psoUp.Get());
                clA->Dispatch((w4k + 7) / 8, (h4k + 7) / 8, 1);
                BarrierT(clA.Get(), bigOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                BarrierT(clA.Get(), bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION bd2{}; bd2.pResource = bb;
                bd2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; bd2.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION bs2{}; bs2.pResource = bigOut.Get();
                bs2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; bs2.SubresourceIndex = 0;
                clA->CopyTextureRegion(&bd2, 0, 0, 0, &bs2, nullptr);
                BarrierT(clA.Get(), bigOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                BarrierT(clA.Get(), bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                didWb = true;
                writebacks++;
                hasWb = false;
            }
        }
        if (!didWb) BarrierT(clA.Get(), bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

        // ③ 抓帧：下采样 → 回读
        BarrierT(clA.Get(), bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        {
            ID3D12DescriptorHeap* heaps[] = { heap.Get() };
            clA->SetDescriptorHeaps(1, heaps);
            clA->SetComputeRootSignature(rs.Get());
            D3D12_GPU_DESCRIPTOR_HANDLE st = gh; st.ptr += (SIZE_T)idx * inc;
            D3D12_GPU_DESCRIPTOR_HANDLE ut = gh; ut.ptr += (SIZE_T)4 * inc;
            clA->SetComputeRootDescriptorTable(0, st);
            clA->SetComputeRootDescriptorTable(1, ut);
            clA->SetPipelineState(psoDown.Get());
            clA->Dispatch((w1k + 7) / 8, (h1k + 7) / 8, 1);
        }
        BarrierT(clA.Get(), small1080.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION rd{}; rd.pResource = rb1080.Get();
        rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; rd.PlacedFootprint = W.fp;
        D3D12_TEXTURE_COPY_LOCATION rsrc{}; rsrc.pResource = small1080.Get();
        rsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; rsrc.SubresourceIndex = 0;
        clA->CopyTextureRegion(&rd, 0, 0, 0, &rsrc, nullptr);
        BarrierT(clA.Get(), small1080.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        BarrierT(clA.Get(), bb, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
        clA->Close();
        { ID3D12CommandList* l[] = { clA.Get() }; qA->ExecuteCommandLists(1, l); }
        qA->Signal(feA.Get(), ++vA);
        while (feA->GetCompletedValue() < vA) Sleep(0);
        double submitMs = std::chrono::duration<double, std::milli>(Clock::now() - tA).count();
        sumSubmit += submitMs;
        captured++;

        // ④ CPU 读回并交给工作线程
        {
            auto tH = Clock::now();
            D3D12_RANGE rr{ 0, (SIZE_T)bytes1k };
            void* m = nullptr;
            if (SUCCEEDED(rb1080->Map(0, &rr, &m)) && m) {
                memcpy(host.data(), m, host.size());
                D3D12_RANGE nw{ 0, 0 };
                rb1080->Unmap(0, &nw);
                sumHarvest += std::chrono::duration<double, std::milli>(Clock::now() - tH).count();
                harvests++;
                std::lock_guard<std::mutex> lk(W.m);
                if (W.hasOut) { pendingWb = W.outBuf; W.hasOut = false; hasWb = true; }
                if (!W.hasIn && W.ok) { W.inBuf = host; W.hasIn = true; W.cv.notify_one(); }
            }
        }

        swap->Present(1, 0);    // 开 vsync，避免空转烧 GPU

        if ((f + 1) % 120 == 0) {
            double secs = std::chrono::duration<double>(Clock::now() - t0).count();
            std::lock_guard<std::mutex> lk(W.m);
            std::printf("frame %d: fps=%.1f submit=%.3fms harvest=%.3fms captured=%d writebacks=%d worker=%d workerLast=%.2fms\n",
                        f + 1, (f + 1) / secs, sumSubmit / captured, harvests ? sumHarvest / harvests : 0.0,
                        captured, writebacks, W.processed, W.lastMs);
            std::fflush(stdout);
        }
    }

    {
        std::lock_guard<std::mutex> lk(W.m);
        W.quit = true;
        W.cv.notify_one();
    }
    if (W.th.joinable()) W.th.join();

    double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    std::printf("\n结果: %d 帧 / %.1f 秒 = %.1f fps, 抓帧 %d, 写回 %d, 推理 %d\n",
                frames, secs, frames / secs, captured, writebacks, W.processed);
    std::printf("  平均提交 %.3f ms, 平均读回 %.3f ms\n", sumSubmit / captured, harvests ? sumHarvest / harvests : 0.0);
    if (W.api.GetStatus) { char st[1024] = { 0 }; W.api.GetStatus(W.ctx, st, 1024); std::printf("  lmxxf: %s\n", st); }
    return 0;
}
