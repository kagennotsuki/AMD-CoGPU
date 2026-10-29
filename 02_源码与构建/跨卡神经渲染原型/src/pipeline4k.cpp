// M2 链路测试：4K 帧 → GPU 缩到 1080p → 跨卡 → 9070 XT 跑 lmxxf → 回传 → 放大回 4K
//
// 目的：在进游戏之前，把"缩放 + 跨卡 + 推理 + 放大"这一整条链的耗时测出来。
//
// 用法：pipeline4k.exe [iterations]

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
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

// ---------- 与本 DLL 版本匹配的 API 表（160 字节，19 个函数指针）----------
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

// ---------- 计算着色器 ----------
static const char* kHLSL = R"(
Texture2D<float4>   gSrc : register(t0);
RWTexture2D<float4> gDst : register(u0);

// 4K -> 1080p：2x2 盒式下采样
[numthreads(8, 8, 1)]
void DownscaleCS(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uint2 s = id.xy * 2;
    float4 c = gSrc.Load(int3(s.x,     s.y,     0))
             + gSrc.Load(int3(s.x + 1, s.y,     0))
             + gSrc.Load(int3(s.x,     s.y + 1, 0))
             + gSrc.Load(int3(s.x + 1, s.y + 1, 0));
    gDst[id.xy] = c * 0.25;
}

// 1080p -> 4K：最近邻放大
[numthreads(8, 8, 1)]
void UpscaleCS(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uint sw, sh;
    gSrc.GetDimensions(sw, sh);
    uint2 s = min(id.xy >> 1, uint2(sw - 1, sh - 1));
    gDst[id.xy] = gSrc.Load(int3(s.x, s.y, 0));
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

static HRESULT CompileCS(const char* entry, ID3DBlob** blob) {
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(kHLSL, strlen(kHLSL), "inline.hlsl", nullptr, nullptr,
                            entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &err);
    if (FAILED(hr) && err) std::printf("compile %s failed: %s\n", entry, (const char*)err->GetBufferPointer());
    return hr;
}

int main(int argc, char** argv) {
    int iters = (argc > 1) ? atoi(argv[1]) : 12;
    const UINT W4K = 3840, H4K = 2160;
    const UINT W1K = 1920, H1K = 1080;
    std::printf("chain test: %ux%u -> %ux%u -> back, %d iterations\n", W4K, H4K, W1K, H1K, iters);

    // ---------- 两张卡 ----------
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return 1;
    ComPtr<IDXGIAdapter1> adA, adB;
    factory->EnumAdapters1(0, &adA);
    factory->EnumAdapters1(1, &adB);
    if (!adA || !adB) { std::printf("need 2 adapters\n"); return 1; }

    ComPtr<ID3D12Device> devA, devB;
    if (FAILED(D3D12CreateDevice(adA.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&devA)))) return 1;
    if (FAILED(D3D12CreateDevice(adB.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&devB)))) return 1;
    std::printf("devices ok\n");

    D3D12_COMMAND_QUEUE_DESC qd{};
    ComPtr<ID3D12CommandQueue> qA, qB;
    devA->CreateCommandQueue(&qd, IID_PPV_ARGS(&qA));
    devB->CreateCommandQueue(&qd, IID_PPV_ARGS(&qB));

    // ---------- lmxxf 会话（在 9070 XT 上）----------
    const wchar_t* kAssets =
        L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2\\LmxxfNrRuntime.pak";
    std::wstring dllPath(kAssets);
    dllPath = dllPath.substr(0, dllPath.find_last_of(L'\\')) + L"\\LmxxfNrRuntime.dll";
    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (!mod) { std::printf("lmxxf load failed\n"); return 1; }
    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)GetProcAddress(mod, "LmxxfNrGetApi");
    ApiTable api{};
    api.struct_size = (uint32_t)sizeof(ApiTable);
    if (!getApi || getApi(1, &api) != 0) { std::printf("GetApi failed\n"); return 1; }
    void* ctx = nullptr;
    LmxxfNrCreateInfo ci{};
    ci.struct_size = (uint32_t)sizeof(ci);
    ci.device = devB.Get();
    ci.queue = qB.Get();
    ci.assets_directory = kAssets;
    if (api.Create(&ci, &ctx) != 0) { std::printf("Create failed\n"); return 1; }
    if (api.PrepareSession(ctx) != 0) { std::printf("PrepareSession failed\n"); return 1; }
    std::printf("lmxxf session ready on 9070 XT\n");

    // ---------- 堆 ----------
    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1;
    dh.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES rh = dh; rh.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_HEAP_PROPERTIES uh = dh; uh.Type = D3D12_HEAP_TYPE_UPLOAD;

    auto makeTex = [](ID3D12Device* d, const D3D12_HEAP_PROPERTIES& hp, UINT w, UINT h,
                      D3D12_RESOURCE_STATES st, ComPtr<ID3D12Resource>& out) -> HRESULT {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, st, nullptr, IID_PPV_ARGS(&out));
    };

    auto makeBuf = [](ID3D12Device* d, const D3D12_HEAP_PROPERTIES& hp, UINT64 bytes,
                      D3D12_RESOURCE_STATES st, ComPtr<ID3D12Resource>& out) -> HRESULT {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr, IID_PPV_ARGS(&out));
    };

    // A 侧资源
    ComPtr<ID3D12Resource> src4K, small1080, smallUp, rb1080, up1080;
    if (FAILED(makeTex(devA.Get(), dh, W4K, H4K, D3D12_RESOURCE_STATE_COPY_DEST, src4K))) { std::printf("src4K failed\n"); return 1; }
    if (FAILED(makeTex(devA.Get(), dh, W1K, H1K, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, small1080))) { std::printf("small failed\n"); return 1; }
    if (FAILED(makeTex(devA.Get(), dh, W1K, H1K, D3D12_RESOURCE_STATE_COPY_DEST, smallUp))) { std::printf("smallUp failed\n"); return 1; }

    D3D12_RESOURCE_DESC d1k{};
    d1k.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d1k.Width = W1K; d1k.Height = H1K; d1k.DepthOrArraySize = 1; d1k.MipLevels = 1;
    d1k.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d1k.SampleDesc.Count = 1;
    d1k.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d1k.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp1k{};
    UINT rows = 0; UINT64 rowSize = 0, bytes1k = 0;
    devA->GetCopyableFootprints(&d1k, 0, 1, 0, &fp1k, &rows, &rowSize, &bytes1k);
    if (FAILED(makeBuf(devA.Get(), rh, bytes1k, D3D12_RESOURCE_STATE_COPY_DEST, rb1080))) { std::printf("rb failed\n"); return 1; }
    if (FAILED(makeBuf(devA.Get(), uh, bytes1k, D3D12_RESOURCE_STATE_GENERIC_READ, up1080))) { std::printf("up failed\n"); return 1; }

    // B 侧：lmxxf 的 color 纹理 + 上传/回读
    ComPtr<ID3D12Resource> colorB, upB, rbB;
    if (FAILED(makeTex(devB.Get(), dh, W1K, H1K, D3D12_RESOURCE_STATE_COMMON, colorB))) { std::printf("colorB failed\n"); return 1; }
    if (FAILED(makeBuf(devB.Get(), uh, bytes1k, D3D12_RESOURCE_STATE_GENERIC_READ, upB))) { std::printf("upB failed\n"); return 1; }
    if (FAILED(makeBuf(devB.Get(), rh, bytes1k, D3D12_RESOURCE_STATE_COPY_DEST, rbB))) { std::printf("rbB failed\n"); return 1; }
    std::printf("resources ok (1080p staging %llu bytes, rowPitch %u)\n",
                (unsigned long long)bytes1k, fp1k.Footprint.RowPitch);

    // ---------- 根签名 / PSO / 描述符 ----------
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1; ranges[0].BaseShaderRegister = 0; ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1; ranges[1].BaseShaderRegister = 0; ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp.DescriptorTable.NumDescriptorRanges = 2;
    rp.DescriptorTable.pDescriptorRanges = ranges;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 1; rsd.pParameters = &rp; rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> sig, sigErr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr))) {
        std::printf("serialize root sig failed\n"); return 1;
    }
    ComPtr<ID3D12RootSignature> rs;
    if (FAILED(devA->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rs)))) {
        std::printf("create root sig failed\n"); return 1;
    }

    auto makePso = [&](const char* entry, ComPtr<ID3D12PipelineState>& pso) -> bool {
        ComPtr<ID3DBlob> cs;
        if (FAILED(CompileCS(entry, &cs))) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rs.Get();
        pd.CS.pShaderBytecode = cs->GetBufferPointer();
        pd.CS.BytecodeLength = cs->GetBufferSize();
        return SUCCEEDED(devA->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));
    };
    ComPtr<ID3D12PipelineState> psoDown, psoUp;
    if (!makePso("DownscaleCS", psoDown)) { std::printf("PSO down failed\n"); return 1; }
    if (!makePso("UpscaleCS", psoUp)) { std::printf("PSO up failed\n"); return 1; }
    std::printf("shaders compiled (down + up)\n");

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 8;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    if (FAILED(devA->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) return 1;
    UINT inc = devA->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();

    auto srv = [&](ID3D12Resource* r, UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu; h.ptr += (SIZE_T)slot * inc;
        devA->CreateShaderResourceView(r, &sd, h);
    };
    auto uav = [&](ID3D12Resource* r, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu; h.ptr += (SIZE_T)slot * inc;
        devA->CreateUnorderedAccessView(r, nullptr, &ud, h);
    };
    // slot0=SRV(4K 或 1080), slot1=UAV(1080 或 4K)
    srv(src4K.Get(), 0);
    uav(small1080.Get(), 1);
    uav(src4K.Get(), 2);
    srv(smallUp.Get(), 4);
    uav(src4K.Get(), 5);

    // ---------- 命令对象 ----------
    ComPtr<ID3D12CommandAllocator> alA, alB;
    ComPtr<ID3D12GraphicsCommandList> clA, clB;
    ComPtr<ID3D12Fence> feA, feB;
    devA->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alA));
    devA->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alA.Get(), nullptr, IID_PPV_ARGS(&clA));
    clA->Close();
    devA->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&feA));
    devB->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alB));
    devB->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alB.Get(), nullptr, IID_PPV_ARGS(&clB));
    clB->Close();
    devB->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&feB));
    UINT64 vA = 0, vB = 0;

    auto waitA = [&]() { qA->Signal(feA.Get(), ++vA); while (feA->GetCompletedValue() < vA) Sleep(0); };
    auto waitB = [&]() { qB->Signal(feB.Get(), ++vB); while (feB->GetCompletedValue() < vB) Sleep(0); };

    std::vector<uint8_t> host((size_t)bytes1k);
    std::vector<double> totals;

    for (int i = 0; i < iters + 2; ++i) {
        auto t0 = Clock::now();

        // 1) A: 4K -> 1080（compute），再拷到回读缓冲
        alA->Reset();
        clA->Reset(alA.Get(), nullptr);
        ID3D12DescriptorHeap* heaps[] = { heap.Get() };
        clA->SetDescriptorHeaps(1, heaps);
        clA->SetComputeRootSignature(rs.Get());
        clA->SetComputeRootDescriptorTable(0, gpu);
        BarrierT(clA.Get(), src4K.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        clA->SetPipelineState(psoDown.Get());
        clA->Dispatch((W1K + 7) / 8, (H1K + 7) / 8, 1);
        BarrierT(clA.Get(), small1080.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION cd{};
        cd.pResource = rb1080.Get();
        cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        cd.PlacedFootprint = fp1k;
        D3D12_TEXTURE_COPY_LOCATION cs{};
        cs.pResource = small1080.Get();
        cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cs.SubresourceIndex = 0;
        clA->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);
        BarrierT(clA.Get(), small1080.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        clA->Close();
        { ID3D12CommandList* l[] = { clA.Get() }; qA->ExecuteCommandLists(1, l); }
        waitA();
        {
            HRESULT rr = devA->GetDeviceRemovedReason();
            if (FAILED(rr)) { std::printf("device A removed after downscale: 0x%08X\n", (unsigned)rr); return 1; }
        }
        auto t1 = Clock::now();

        // 2) CPU 读回
        {
            D3D12_RANGE rr{ 0, (SIZE_T)bytes1k };
            void* m = nullptr;
            if (FAILED(rb1080->Map(0, &rr, &m)) || !m) { std::printf("map rb failed\n"); return 1; }
            memcpy(host.data(), m, (size_t)bytes1k);
            D3D12_RANGE nw{ 0, 0 };
            rb1080->Unmap(0, &nw);
        }
        // 3) 送入 9070 XT 的 color 纹理
        {
            D3D12_RANGE nr{ 0, 0 };
            void* m = nullptr;
            upB->Map(0, &nr, &m);
            memcpy(m, host.data(), (size_t)bytes1k);
            D3D12_RANGE w{ 0, (SIZE_T)bytes1k };
            upB->Unmap(0, &w);
            alB->Reset();
            clB->Reset(alB.Get(), nullptr);
            D3D12_TEXTURE_COPY_LOCATION d{};
            d.pResource = colorB.Get();
            d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            d.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION s{};
            s.pResource = upB.Get();
            s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            s.PlacedFootprint = fp1k;
            clB->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            clB->Close();
            { ID3D12CommandList* l[] = { clB.Get() }; qB->ExecuteCommandLists(1, l); }
            waitB();
        }
        auto t2 = Clock::now();

        // 4) 9070 XT 推理
        LmxxfNrJob job{};
        job.struct_size = (uint32_t)sizeof(job);
        FrameInfo120 fi120{};
        LmxxfNrFrameInfo& fi = fi120.base;
        fi.struct_size = (uint32_t)sizeof(FrameInfo120);
        fi.session_id = 1;
        fi.frame_id = (uint64_t)i + 1;
        fi.list_generation = 1;
        fi.command_list = clB.Get();
        fi.color_width = W1K;
        fi.color_height = H1K;
        fi.color = colorB.Get();
        fi.color_state = (uint32_t)D3D12_RESOURCE_STATE_COMMON;
        fi.transfer_strength = 1.0f;
        fi.color_strength = 1.0f;
        fi.model_scale = 1.0f;
        fi.pre_exposure = 1.0f;
        fi.exposure_scale = 1.0f;
        int32_t r = api.PrepareFrame(ctx, &fi, &job);
        if (r != 0) { std::printf("PrepareFrame %d\n", r); break; }
        alB->Reset();
        clB->Reset(alB.Get(), nullptr);
        r = api.RecordInputs(ctx, job.handle, clB.Get());
        if (r != 0) { std::printf("RecordInputs %d\n", r); break; }
        clB->Close();
        { ID3D12CommandList* l[] = { clB.Get() }; qB->ExecuteCommandLists(1, l); }
        waitB();
        r = api.ExecuteAfterProducer(ctx, job.handle, qB.Get());
        if (r != 0) { std::printf("ExecuteAfterProducer %d\n", r); break; }
        alB->Reset();
        clB->Reset(alB.Get(), nullptr);
        r = api.RecordOutputs(ctx, job.handle, clB.Get());
        if (r != 0) { std::printf("RecordOutputs %d\n", r); break; }
        clB->Close();
        { ID3D12CommandList* l[] = { clB.Get() }; qB->ExecuteCommandLists(1, l); }
        waitB();
        api.Retire(ctx, job.handle);
        auto t3 = Clock::now();

        // 5) 回读 1080p（用 color 纹理当结果）
        alB->Reset();
        clB->Reset(alB.Get(), nullptr);
        D3D12_TEXTURE_COPY_LOCATION d2{};
        d2.pResource = rbB.Get();
        d2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        d2.PlacedFootprint = fp1k;
        D3D12_TEXTURE_COPY_LOCATION s2{};
        s2.pResource = colorB.Get();
        s2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s2.SubresourceIndex = 0;
        clB->CopyTextureRegion(&d2, 0, 0, 0, &s2, nullptr);
        clB->Close();
        { ID3D12CommandList* l[] = { clB.Get() }; qB->ExecuteCommandLists(1, l); }
        waitB();
        {
            D3D12_RANGE rr{ 0, (SIZE_T)bytes1k };
            void* m = nullptr;
            if (FAILED(rbB->Map(0, &rr, &m)) || !m) { std::printf("map rbB failed\n"); return 1; }
            memcpy(host.data(), m, (size_t)bytes1k);
            D3D12_RANGE nw{ 0, 0 };
            rbB->Unmap(0, &nw);
        }
        auto t4 = Clock::now();

        // 6) A: 上传 1080 → 放大回 4K
        {
            D3D12_RANGE nr{ 0, 0 };
            void* m = nullptr;
            up1080->Map(0, &nr, &m);
            memcpy(m, host.data(), (size_t)bytes1k);
            D3D12_RANGE w{ 0, (SIZE_T)bytes1k };
            up1080->Unmap(0, &w);

            alA->Reset();
            clA->Reset(alA.Get(), nullptr);
            ID3D12DescriptorHeap* heaps2[] = { heap.Get() };
            clA->SetDescriptorHeaps(1, heaps2);
            clA->SetComputeRootSignature(rs.Get());
            D3D12_TEXTURE_COPY_LOCATION dd{};
            dd.pResource = smallUp.Get();
            dd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dd.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION ss{};
            ss.pResource = up1080.Get();
            ss.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            ss.PlacedFootprint = fp1k;
            clA->CopyTextureRegion(&dd, 0, 0, 0, &ss, nullptr);
            BarrierT(clA.Get(), smallUp.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            // 放大写入 src4K（当输出目标，slot2 = UAV(4K)）
            clA->SetPipelineState(psoUp.Get());
            D3D12_GPU_DESCRIPTOR_HANDLE t2h = gpu; t2h.ptr += (SIZE_T)4 * inc;
            clA->SetComputeRootDescriptorTable(0, t2h);
            BarrierT(clA.Get(), src4K.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            clA->Dispatch((W4K + 7) / 8, (H4K + 7) / 8, 1);
            BarrierT(clA.Get(), src4K.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            clA->Close();
            { ID3D12CommandList* l[] = { clA.Get() }; qA->ExecuteCommandLists(1, l); }
            waitA();
        }
        auto t5 = Clock::now();

        if (i >= 2) {
            double total = std::chrono::duration<double, std::milli>(t5 - t0).count();
            double down = std::chrono::duration<double, std::milli>(t1 - t0).count();
            double toB = std::chrono::duration<double, std::milli>(t2 - t1).count();
            double infer = std::chrono::duration<double, std::milli>(t3 - t2).count();
            double back = std::chrono::duration<double, std::milli>(t4 - t3).count();
            double up = std::chrono::duration<double, std::milli>(t5 - t4).count();
            totals.push_back(total);
            std::printf("iter %2d: total %6.2f ms  [down %.2f | toB %.2f | infer %.2f | back %.2f | up %.2f]\n",
                        i, total, down, toB, infer, back, up);
        }
    }

    if (!totals.empty()) {
        std::vector<double> s = totals;
        std::sort(s.begin(), s.end());
        double sum = 0; for (double x : s) sum += x;
        std::printf("\nRESULT: avg %.2f ms, median %.2f ms  ->  serial %.1f fps\n",
                    sum / s.size(), s[s.size() / 2], 1000.0 / s[s.size() / 2]);
    }
    char st[1024] = { 0 };
    if (api.GetStatus) { api.GetStatus(ctx, st, 1024); std::printf("lmxxf: %s\n", st); }
    if (api.Drain) api.Drain(ctx);
    if (api.Destroy) api.Destroy(ctx);
    FreeLibrary(mod);
    return 0;
}
